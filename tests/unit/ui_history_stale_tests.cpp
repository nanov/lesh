#include "ui/history/history.h"

#include "ui/history/blob.h"
#include "ui/history/locking.h"
#include "ui/history/log.h"
#include "ui/history/watch.h"

#include "temp_path.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace lesh::ui::history;

// MILESTONE 5 OF THE TWO-TIER HISTORY (#195, ADR-0010 §Locking and staleness):
// what happens when a SECOND SHELL is using the same two files.
//
// Four mechanisms, and each one is here because a shell without it has a bug
// somebody has actually filed:
//
//   THE DIRECTORY WATCH is fish #3565. A terminal left open all day never saves
//   - it only autosuggests - so fish's `loaded_old` latch, which is cleared by
//   saving, is never cleared, and that terminal keeps serving a mapping of a
//   `history.data` three vacuums old. `ASiblingsVacuumIsSeenWithNoWriteOnOurSide`
//   is the whole ticket in one test: the word "no write" is the assertion.
//
//   THE FILE-ID CHECK is the same question asked on the path that a shell which
//   DOES type commands takes. `open`, lock, and then ask whether the thing you
//   locked is still the thing at the path - because a vacuum between those two
//   steps leaves you appending, under a lock nobody respects, to an inode that
//   has already been unlinked.
//
//   THE GIVE-UP LATCH is fish's answer to lockless NFS, where `flock` can block
//   for minutes. One slow lock and this process never takes another. It is
//   tested with an injected duration, because a test that actually waited a
//   quarter of a second for it would be a test nobody runs.
//
//   THE REMOTE FALLBACK is fish PR #5097. An `mmap` over NFS faults when the
//   server drops the file, and the fault is a SIGBUS in a shell that did nothing
//   wrong. The bytes are read into a heap buffer instead, and the assertion that
//   matters is that NOTHING ABOVE CAN TELL: the same records come out.
//
// EVERY PROCESS-WIDE FLAG IS RESET AROUND EVERY TEST. The latch, the remote
// flag, the overrides and the lock counter are deliberately process-wide (that
// is ADR-0010's design - "no atomic beyond `abandoned_locking`"), which means a
// test that left one set would be a test that broke the next one.

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

[[nodiscard]] std::span<const std::byte> as_bytes(std::string_view text) noexcept {
	return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

[[nodiscard]] std::string as_text(std::span<const std::byte> bytes) {
	return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] std::vector<std::string> walk(const history& store) {
	std::vector<std::string> out;
	static_cast<const lesh::ui::history_source&>(store).for_each_newest_first(
		[&out](std::string_view entry) {
			out.emplace_back(entry);
			return true;
		});
	return out;
}

// `open`, with #194's PERIODIC VACUUM TURNED OFF.
//
// Nothing in this file is about the rewrite: these tests are about the file-id
// check, the directory watch and the remote fallback, and they assert about the
// two files byte for byte. #194's countdown starts at a random value in
// `[0, 25)` - a correctness property, so that a shell used for twenty commands
// still eventually vacuums - so one `run_command` in twenty-five would
// otherwise truncate the log, replace `history.data` and fire the watch, and
// this file would be right most of the time. `UiHistoryVacuum*` drives the
// rewrite directly.
[[nodiscard]] open_report open_quietly(history& store, const std::string& directory) {
	store.set_automatic_vacuum(false);
	return store.open(directory);
}

void run_command(history& store, std::string_view cmd, std::string_view cwd = "/tmp") {
	if (store.add(cmd, cwd) == add_status::rejected)
		return;
	store.resolve_pending(0);
}

void write_file(const std::string& path, std::span<const std::byte> bytes) {
	std::ofstream out{path, std::ios::binary | std::ios::trunc};
	ASSERT_TRUE(out.is_open()) << "could not create " << path;
	out.write(reinterpret_cast<const char*>(bytes.data()),
	          static_cast<std::streamsize>(bytes.size()));
	ASSERT_TRUE(out.good());
}

[[nodiscard]] std::vector<std::byte> read_file(const std::string& path) {
	std::ifstream in{path, std::ios::binary};
	const std::string text{std::istreambuf_iterator<char>(in),
	                       std::istreambuf_iterator<char>()};
	std::vector<std::byte> out(text.size());
	if (!text.empty())
		std::memcpy(out.data(), text.data(), text.size());
	return out;
}

// A record borrowing `cmd`, which the caller keeps alive.
[[nodiscard]] record record_for(std::string_view cmd, std::uint64_t when) {
	return record{.cmd = as_bytes(cmd), .when = when, .cwd = {}, .exit_code = 0,
	              .session_id = 0};
}

// #194'S VACUUM, STOOD IN FOR. The real one is another ticket; what THIS ticket
// needs from it is exactly the two syscalls at the end of ADR-0010 §Vacuum -
// build a blob into a temp file in the same directory and `rename` it over
// `history.data` - because the rename is the event the watch exists to see, and
// the new inode is what the file-id check exists to notice.
void vacuum_stand_in(const std::string& directory, std::span<const record> newest_first) {
	blob_writer writer;
	const std::string temp = directory + "/history.tmp";
	write_file(temp, writer.build(newest_first));
	ASSERT_EQ(::rename(temp.c_str(), (directory + "/history.data").c_str()), 0)
		<< std::strerror(errno);
}

// Polls the watch descriptor and drains it until a reload has happened, or the
// budget runs out.
//
// A LOOP AND NOT ONE DRAIN, for two reasons that are both properties of the
// mechanism rather than of the test. The watch is on the DIRECTORY - it has to
// be, a `rename` over a file never fires on that file - so the temp file's
// creation wakes it once before the rename does, and that first wake is a
// `stat` that finds nothing changed. And a notification is asynchronous: the
// kernel owes us the event, not the moment.
[[nodiscard]] bool drain_until_reloaded(history& store, int budget_ms = 5000) {
	const std::size_t before = store.reloads();
	const auto deadline =
		std::chrono::steady_clock::now() + std::chrono::milliseconds{budget_ms};
	while (std::chrono::steady_clock::now() < deadline) {
		struct ::pollfd waiting {};
		waiting.fd = store.watch_fd();
		waiting.events = POLLIN;
		if (::poll(&waiting, 1, 50) > 0 && (waiting.revents & POLLIN) != 0)
			store.drain_watch();
		if (store.reloads() > before)
			return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// The fixture
// ---------------------------------------------------------------------------

class stale_fixture : public ::testing::Test {
protected:
	void SetUp() override { test_hooks::reset_locking_state(); }
	void TearDown() override { test_hooks::reset_locking_state(); }

	lesh::testing::temp_path _temp;
};

class UiHistoryStaleWatch : public stale_fixture {};
class UiHistoryStaleAppend : public stale_fixture {};
class UiHistoryStaleLocking : public stale_fixture {};
class UiHistoryStaleRemote : public stale_fixture {};

} // namespace

// ===========================================================================
// The directory watch - fish #3565
// ===========================================================================

TEST_F(UiHistoryStaleWatch, ASiblingsVacuumIsSeenWithNoWriteOnOurSide) {
	// THE TICKET, IN ONE TEST. A shell that only reads has to see another
	// shell's vacuum, and it has to see it WITHOUT writing anything - because a
	// shell that had to write to notice is fish's `loaded_old` latch again, and
	// the terminal in question is one that has been sitting at a prompt for an
	// hour.
	const std::string dir = _temp.dir();
	{
		const std::string old_cmd = "before the vacuum";
		const record was[] = {record_for(old_cmd, 100)};
		std::filesystem::create_directories(dir);
		blob_writer writer;
		write_file(dir + "/history.data", writer.build(was));
	}

	// The reader: opens, maps, and from here on types nothing.
	history reader;
	const open_report report = open_quietly(reader, dir);
	ASSERT_TRUE(report.tier1_mapped);
	ASSERT_TRUE(report.watching) << "no watch, no test";
	EXPECT_EQ(walk(reader), (std::vector<std::string>{"before the vacuum"}));

	const std::size_t items_before = reader.session_items();

	// THE SECOND INSTANCE - another terminal on the same directory - records two
	// commands and then vacuums them into Tier 1.
	history sibling;
	ASSERT_FALSE(open_quietly(sibling, dir).directory_unusable);
	run_command(sibling, "sibling one");
	run_command(sibling, "sibling two");

	// AFTER THE SIBLING HAS FINISHED WRITING, because the log is SHARED: the
	// bytes this test is about to freeze are the sibling's, and the assertion at
	// the end is that the reader added none of its own.
	const std::vector<std::byte> log_before = read_file(dir + "/history.new.log");

	const std::string a = "sibling two";
	const std::string b = "sibling one";
	const std::string c = "before the vacuum";
	const record after[] = {record_for(a, 300), record_for(b, 200), record_for(c, 100)};
	vacuum_stand_in(dir, after);

	ASSERT_TRUE(drain_until_reloaded(reader)) << "the watch never fired";
	EXPECT_EQ(reader.reloads(), 1u);

	// THE NEW RECORDS, on the next walk, with no write on this side.
	EXPECT_EQ(walk(reader),
	          (std::vector<std::string>{"sibling two", "sibling one", "before the vacuum"}));
	EXPECT_EQ(reader.session_items(), items_before) << "the reader recorded nothing";
	EXPECT_EQ(reader.unwritable_items(), 0u);
	EXPECT_EQ(read_file(dir + "/history.new.log"), log_before)
		<< "the reader wrote to the log to notice, which is the bug";
}

TEST_F(UiHistoryStaleWatch, AChangeToSomethingElseInTheDirectoryIsNotAReload) {
	// The watch is on the directory, so every sibling's temp file wakes every
	// terminal on the machine. The `stat` is what turns that into nothing.
	history store;
	const open_report report = open_quietly(store, _temp.dir());
	ASSERT_TRUE(report.watching);

	write_file(_temp.dir() + "/somebody-elses-file", as_bytes("hello"));

	// Drained repeatedly for a moment: whatever the kernel reports, none of it
	// changes `history.data`, so none of it is a reload.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{300};
	while (std::chrono::steady_clock::now() < deadline) {
		struct ::pollfd waiting {};
		waiting.fd = store.watch_fd();
		waiting.events = POLLIN;
		if (::poll(&waiting, 1, 20) > 0 && (waiting.revents & POLLIN) != 0)
			store.drain_watch();
	}
	EXPECT_EQ(store.reloads(), 0u);
	EXPECT_FALSE(store.reload_needed());
}

TEST_F(UiHistoryStaleWatch, AMemoryOnlyHistoryWatchesNothing) {
	// The state `vared` and every unit test that must not touch the developer's
	// own history runs in: no directory, so no descriptor for the loop to poll,
	// and the topic simply does not exist.
	history store;
	EXPECT_EQ(store.watch_fd(), -1);
	store.drain_watch();
	EXPECT_EQ(store.reloads(), 0u);
}

TEST_F(UiHistoryStaleWatch, TheWatchDescriptorIsNotInheritedByChildren) {
	// This is a shell: it forks and execs on every command line. A notification
	// descriptor leaking into every child is both a leak and a way for a child to
	// hold a reference on the user's data directory.
	history store;
	ASSERT_TRUE(open_quietly(store, _temp.dir()).watching);
	const int flags = ::fcntl(store.watch_fd(), F_GETFD);
	ASSERT_NE(flags, -1);
	EXPECT_NE(flags & FD_CLOEXEC, 0);
}

// ===========================================================================
// The append path - open, lock, verify (ADR-0010)
// ===========================================================================

TEST_F(UiHistoryStaleAppend, AnAppendNoticesASiblingsVacuumWithoutTheWatch) {
	// The other half of staleness, and the one a shell that is being TYPED into
	// takes: `flush` re-`stat`s `history.data` and compares it with the cached
	// id. Nothing here drains the watch, so this is the file-id check alone.
	const std::string dir = _temp.dir();
	const std::string first = "the old one";
	const record was[] = {record_for(first, 100)};
	vacuum_stand_in(dir, was);

	history store;
	ASSERT_TRUE(open_quietly(store, dir).tier1_mapped);
	ASSERT_EQ(walk(store), (std::vector<std::string>{"the old one"}));

	const std::string fresh = "a sibling's command";
	const record now[] = {record_for(fresh, 200), record_for(first, 100)};
	vacuum_stand_in(dir, now);

	// One command typed here, and the check runs on its append.
	run_command(store, "mine");
	EXPECT_EQ(store.reloads(), 1u);
	EXPECT_EQ(walk(store),
	          (std::vector<std::string>{"mine", "a sibling's command", "the old one"}));
}

TEST_F(UiHistoryStaleAppend, AnAppendGoesToTheFileThatIsAtThePathAndNotToAnOrphan) {
	// WHY THE APPEND RE-OPENS AND RE-CHECKS. A vacuum unlinks the log once it has
	// folded it into the blob; a shell writing through a descriptor it has held
	// since start-up would then be writing into an inode with no name, and every
	// command it recorded afterwards would be lost with no error anywhere.
	const std::string dir = _temp.dir();
	const std::string log = dir + "/history.new.log";

	history store;
	ASSERT_TRUE(open_quietly(store, dir).log_writable);
	run_command(store, "before");

	// The sibling's vacuum, as far as the log is concerned: the old inode is
	// gone and a new, empty file has the name.
	ASSERT_EQ(::unlink(log.c_str()), 0);
	write_file(log, {});

	run_command(store, "after");

	const std::vector<std::byte> bytes = read_file(log);
	std::vector<std::string> framed;
	(void)for_each(bytes,
	               [&framed](const record& one) { framed.push_back(as_text(one.cmd)); });
	EXPECT_EQ(framed, (std::vector<std::string>{"after"}))
		<< "the frame went to the orphaned inode";
	EXPECT_EQ(store.unwritable_items(), 0u);
}

TEST_F(UiHistoryStaleAppend, AFlushWithNothingToWriteTouchesNothing) {
	// `save()` and `resolve_pending` both call `flush` unconditionally, and the
	// append path is four syscalls. An `exit` typed at a prompt must not open and
	// lock the log to write nothing.
	history store;
	ASSERT_TRUE(open_quietly(store, _temp.dir()).log_writable);
	run_command(store, "one");

	const std::uint64_t locks_after_one_command = test_hooks::lock_attempts();
	ASSERT_GT(locks_after_one_command, 0u) << "the append is supposed to lock";

	EXPECT_TRUE(store.save());
	EXPECT_TRUE(store.save());
	EXPECT_EQ(test_hooks::lock_attempts(), locks_after_one_command);
}

TEST_F(UiHistoryStaleAppend, TwoFailedStatsAreNotAMatch) {
	// `file_id_equal`'s whole reason for existing next to `operator==`. A TOCTOU
	// loop that treated "I could not look" as "it matched" would accept exactly
	// the descriptor it is there to reject.
	EXPECT_FALSE(file_id_equal(k_invalid_file_id, k_invalid_file_id));
	EXPECT_FALSE(file_id_equal(file_id_of_path(_temp.file("nothing-here")),
	                           file_id_of_path(_temp.file("nothing-here-either"))));

	const std::string path = _temp.file("real");
	write_file(path, as_bytes("bytes"));
	const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
	ASSERT_GE(fd, 0);
	EXPECT_TRUE(file_id_equal(file_id_of_path(path), file_id_of_fd(fd)));
	::close(fd);

	// And the `size` in the triple is load-bearing: an appended-to file is the
	// same inode and not the same contents.
	const file_id_t before = file_id_of_path(path);
	write_file(path, as_bytes("more bytes"));
	EXPECT_NE(before, file_id_of_path(path));
}

// ===========================================================================
// The give-up latch - fish `maybe_lock_file`
// ===========================================================================

TEST_F(UiHistoryStaleLocking, ASlowLockIsAbandonedOnceAndForTheWholeProcess) {
	// ADR-0010: "If any lock takes > 0.25 s, set `abandoned_locking` and never
	// lock again this process." The duration is injected because the alternative
	// is a test that waits a quarter of a second to find out.
	const std::string path = _temp.file("lockable");
	write_file(path, as_bytes("x"));
	const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
	ASSERT_GE(fd, 0);

	EXPECT_FALSE(abandoned_locking());

	test_hooks::set_lock_duration_override(k_lock_give_up_seconds + 0.01);
	EXPECT_TRUE(lock_exclusive(fd));
	unlock(fd);
	EXPECT_TRUE(abandoned_locking());
	EXPECT_EQ(test_hooks::lock_attempts(), 1u);

	// NEVER AGAIN, and "never" is the point: the second call does not reach
	// `flock` at all, which is what the counter proves and a returned `false`
	// would not.
	test_hooks::set_lock_duration_override(-1.0);
	EXPECT_FALSE(lock_exclusive(fd));
	EXPECT_FALSE(lock_shared(fd));
	EXPECT_EQ(test_hooks::lock_attempts(), 1u);
	EXPECT_TRUE(abandoned_locking());

	::close(fd);
}

TEST_F(UiHistoryStaleLocking, AQuickLockLeavesTheLatchAlone) {
	const std::string path = _temp.file("lockable");
	write_file(path, as_bytes("x"));
	const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
	ASSERT_GE(fd, 0);

	EXPECT_TRUE(lock_shared(fd));
	unlock(fd);
	EXPECT_TRUE(lock_exclusive(fd));
	unlock(fd);
	EXPECT_FALSE(abandoned_locking());
	EXPECT_EQ(test_hooks::lock_attempts(), 2u);

	::close(fd);
}

TEST_F(UiHistoryStaleLocking, ARemoteDataDirectoryIsNeverLocked) {
	// ADR-0010: "Never lock ... when the data dir is remote." fish's users have
	// watched `flock` block for minutes on lockless NFS; the refusal is before
	// the syscall and not after it.
	const std::string path = _temp.file("lockable");
	write_file(path, as_bytes("x"));
	const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
	ASSERT_GE(fd, 0);

	set_data_directory_remote(true);
	EXPECT_FALSE(lock_exclusive(fd));
	EXPECT_FALSE(lock_shared(fd));
	EXPECT_EQ(test_hooks::lock_attempts(), 0u);
	EXPECT_FALSE(abandoned_locking()) << "refusing to lock is not giving up on locking";

	::close(fd);
}

TEST_F(UiHistoryStaleLocking, UnlockingSomethingNeverLockedIsHarmless) {
	// The ordinary state once the latch has tripped: every `unlock` in the code
	// is paired with a `lock_*` that answered false.
	unlock(-1);
	const std::string path = _temp.file("lockable");
	write_file(path, as_bytes("x"));
	const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
	ASSERT_GE(fd, 0);
	unlock(fd);
	::close(fd);
}

// ===========================================================================
// Remoteness, and the heap-buffer Tier 1 - fish PR #5097
// ===========================================================================

TEST_F(UiHistoryStaleRemote, TheClassificationIsFishsList) {
#if defined(__linux__)
	EXPECT_EQ(classify_filesystem(nullptr, 0x6969U), remoteness::remote);       // NFS
	EXPECT_EQ(classify_filesystem(nullptr, 0x517BU), remoteness::remote);       // SMB
	EXPECT_EQ(classify_filesystem(nullptr, 0xFE534D42U), remoteness::remote);   // SMB2
	EXPECT_EQ(classify_filesystem(nullptr, 0xFF534D42U), remoteness::remote);   // CIFS
	EXPECT_EQ(classify_filesystem(nullptr, 0xEF53U), remoteness::local);        // ext4
	EXPECT_EQ(classify_filesystem(nullptr, 0x01021994U), remoteness::local);    // tmpfs
#else
	EXPECT_EQ(classify_filesystem("nfs", 0), remoteness::remote);
	EXPECT_EQ(classify_filesystem("smbfs", 0), remoteness::remote);
	EXPECT_EQ(classify_filesystem("afpfs", 0), remoteness::remote);
	EXPECT_EQ(classify_filesystem("webdav", 0), remoteness::remote);
	EXPECT_EQ(classify_filesystem("apfs", 0), remoteness::local);
	EXPECT_EQ(classify_filesystem("hfs", 0), remoteness::local);
	EXPECT_EQ(classify_filesystem(nullptr, 0), remoteness::unknown);
	EXPECT_EQ(classify_filesystem("", 0), remoteness::unknown);
#endif
}

TEST_F(UiHistoryStaleRemote, ARealTemporaryDirectoryIsLocal) {
	// The other half of the classification: the machine the suite runs on. If
	// this ever fails the developer is building on a network share, and the
	// history would quietly stop mapping - worth knowing.
	EXPECT_FALSE(is_remote(_temp.dir()));
}

TEST_F(UiHistoryStaleRemote, AHeapBufferYieldsExactlyTheSameRecords) {
	// THE ASSERTION THAT MATTERS: nothing above `records()` can tell which path
	// ran. Same bytes on disk, one history mapping them and one reading them,
	// and the two walks are equal.
	const std::string a = "git status";
	const std::string b = "cargo build --release";
	const std::string c = "echo done";
	const record written[] = {record_for(a, 300), record_for(b, 200), record_for(c, 100)};

	lesh::testing::temp_path mapped_dir;
	{
		blob_writer writer;
		write_file(mapped_dir.dir() + "/history.data", writer.build(written));
		blob_writer other;
		write_file(_temp.dir() + "/history.data", other.build(written));
	}

	history mapped;
	ASSERT_TRUE(open_quietly(mapped, mapped_dir.dir()).tier1_mapped);
	EXPECT_FALSE(mapped.tier1_copied());
	EXPECT_GT(test_hooks::lock_attempts(), 0u)
		<< "the LOCAL mapping takes the shared lock ADR-0010 asks for";

	// FROM ZERO, so the count below is the remote history's alone.
	test_hooks::reset_locking_state();
	test_hooks::set_remoteness_override(remoteness::remote);
	history copied;
	const open_report report = open_quietly(copied, _temp.dir());
	EXPECT_TRUE(report.directory_remote);
	EXPECT_TRUE(report.tier1_mapped) << "remote or not, Tier 1 is readable";
	EXPECT_TRUE(copied.tier1_copied());

	EXPECT_EQ(walk(copied), walk(mapped));
	EXPECT_EQ(walk(copied), (std::vector<std::string>{a, b, c}));

	// AND NOTHING LOCKED. ADR-0010: "Never lock ... when the data dir is
	// remote" - which includes the `LOCK_SH` the mapping would otherwise take.
	EXPECT_EQ(test_hooks::lock_attempts(), 0u);
}

TEST_F(UiHistoryStaleRemote, ARemoteBlobIsVerifiedLikeAnyOther) {
	// A copy is not a licence to skip the Verifier: the bytes came off a network
	// filesystem, which is the LEAST trustworthy source in the design.
	write_file(_temp.dir() + "/history.data", as_bytes("not a flatbuffer at all"));

	test_hooks::set_remoteness_override(remoteness::remote);
	history store;
	const open_report report = open_quietly(store, _temp.dir());
	EXPECT_FALSE(report.tier1_mapped);
	EXPECT_TRUE(report.tier1_untouchable) << "a file that is not ours is never rewritten";
	EXPECT_FALSE(store.may_rewrite_tier1());
	EXPECT_TRUE(walk(store).empty());
}

TEST_F(UiHistoryStaleRemote, AnEmptyAndAMissingTierOneAreBothFineWhenRemote) {
	// The first run of a shell whose home is on NFS. Neither is damage and
	// neither is an error.
	test_hooks::set_remoteness_override(remoteness::remote);
	history missing;
	EXPECT_FALSE(open_quietly(missing, _temp.dir()).tier1_mapped);
	EXPECT_TRUE(walk(missing).empty());

	lesh::testing::temp_path empty_dir;
	write_file(empty_dir.dir() + "/history.data", {});
	history empty;
	EXPECT_TRUE(open_quietly(empty, empty_dir.dir()).tier1_mapped);
	EXPECT_TRUE(walk(empty).empty());
}

TEST_F(UiHistoryStaleRemote, ARemoteHistoryStillRecordsAndStillReloads) {
	// The whole subsystem over a copied Tier 1: append (unlocked), notice a
	// sibling's vacuum, re-read into a fresh buffer, and walk the result.
	test_hooks::set_remoteness_override(remoteness::remote);
	const std::string dir = _temp.dir();

	history store;
	const open_report report = open_quietly(store, dir);
	ASSERT_TRUE(report.directory_remote);
	ASSERT_TRUE(report.log_writable);

	run_command(store, "typed here");
	EXPECT_EQ(store.unwritable_items(), 0u);

	const std::string theirs = "typed over there";
	const record after[] = {record_for(theirs, 400)};
	vacuum_stand_in(dir, after);

	run_command(store, "typed here again");
	EXPECT_GE(store.reloads(), 1u);
	EXPECT_TRUE(store.tier1_copied());
	EXPECT_EQ(walk(store), (std::vector<std::string>{"typed here again", "typed here",
	                                                 "typed over there"}));
	EXPECT_EQ(test_hooks::lock_attempts(), 0u);
}

// ===========================================================================
// The reload, on its own terms
// ===========================================================================

TEST_F(UiHistoryStaleWatch, IncorporatingWithNothingPendingIsANoOp) {
	history store;
	ASSERT_FALSE(open_quietly(store, _temp.dir()).directory_unusable);
	store.incorporate_external_changes();
	EXPECT_EQ(store.reloads(), 0u);
}

TEST_F(UiHistoryStaleWatch, AReloadReAsksWhetherTierOneMayBeRewritten) {
	// `may_rewrite_tier1()` used to be settled at `open`, which was true while
	// `open` was the only thing that mapped Tier 1. A vacuum in another terminal
	// replaces the bytes, and the policy is a statement about the bytes.
	const std::string dir = _temp.dir();
	write_file(dir + "/history.data", as_bytes("SOMEBODY ELSE'S FILE ENTIRELY"));

	history store;
	ASSERT_TRUE(open_quietly(store, dir).tier1_untouchable);
	ASSERT_FALSE(store.may_rewrite_tier1());

	// Some other lesh renames a real blob over it.
	const std::string cmd = "ours now";
	const record ours[] = {record_for(cmd, 100)};
	vacuum_stand_in(dir, ours);

	ASSERT_TRUE(drain_until_reloaded(store)) << "the watch never fired";
	EXPECT_TRUE(store.may_rewrite_tier1());
	EXPECT_EQ(walk(store), (std::vector<std::string>{"ours now"}));
}

TEST_F(UiHistoryStaleWatch, AVanishedTierOneIsAReloadIntoNothingAndNotACrash) {
	const std::string dir = _temp.dir();
	const std::string cmd = "was here";
	const record was[] = {record_for(cmd, 100)};
	vacuum_stand_in(dir, was);

	history store;
	ASSERT_TRUE(open_quietly(store, dir).tier1_mapped);
	ASSERT_EQ(::unlink((dir + "/history.data").c_str()), 0);

	run_command(store, "mine");
	EXPECT_EQ(store.reloads(), 1u);
	EXPECT_EQ(walk(store), (std::vector<std::string>{"mine"}));
}
