#include "ui/history/vacuum.h"

#include "ui/history/blob.h"
#include "ui/history/store.h"
#include "ui/history/locking.h"
#include "ui/history/log.h"

#include "temp_path.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace lesh::ui::history;

// MILESTONE 4 OF THE TWO-TIER HISTORY (#194, ADR-0010 §Vacuum): the rewrite.
//
// THE SUITE IS ORGANISED AROUND ONE CLAIM, and everything else here is
// arithmetic in support of it: A RESOLVED COMMAND IS NEVER LOST. The vacuum is
// the only code in the subsystem that deletes anything - it truncates the log
// and it renames a new file over `history.data` - so it is the only code that
// can lose a command line, and every way it could is a test below.
//
//   THE MERGE, which has to produce one blob out of three sources that overlap
//   and disagree: the old file, every frame in the log, and the items this
//   session holds. Dedup keeps the newest run of a command line, the cap
//   evicts the least recently seen, and the order on disk is newest-first
//   because that is the format.
//
//   THE PROTOCOL, which is fish's and which exists entirely for the two
//   processes case: build speculatively with no lock, then lock and check
//   whether the target moved, and retry if it did. `ASiblingThatRenames...`
//   drives that race directly through the step hook.
//
//   THE CRASH INJECTION, which is the one that would be worth writing even if
//   nothing else here were. `NoResolvedCommandIsLostAtAnyStep` kills a FORKED
//   CHILD between every pair of steps - a real death, with no unwinding and no
//   destructors, because an exception would test something the power cut does
//   not do - and then opens a fresh `history` over the wreckage and demands
//   every command back. Duplicates are allowed; absences are not.
//
//   AND THE POLICY, which #194 was asked to decide: a `history.data` that is
//   ours and does not verify is rebuilt, after being renamed aside so nothing
//   is destroyed; one whose identifier is not ours is never touched at all.

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

[[nodiscard]] std::vector<std::byte> read_file(const std::string& path) {
	std::ifstream in{path, std::ios::binary};
	const std::string text{std::istreambuf_iterator<char>(in),
	                       std::istreambuf_iterator<char>()};
	std::vector<std::byte> out(text.size());
	if (!text.empty())
		std::memcpy(out.data(), text.data(), text.size());
	return out;
}

void write_file(const std::string& path, std::span<const std::byte> bytes) {
	std::ofstream out{path, std::ios::binary | std::ios::trunc};
	ASSERT_TRUE(out.is_open()) << "could not create " << path;
	out.write(reinterpret_cast<const char*>(bytes.data()),
	          static_cast<std::streamsize>(bytes.size()));
	ASSERT_TRUE(out.good());
}

// A `history.data` at `path` holding `newest_first`, through the real writer.
void write_blob(const std::string& path, std::span<const record> newest_first) {
	blob_writer writer;
	write_file(path, writer.build(newest_first));
}

// One frame appended to `path`, through the real appender.
void append_frame(const std::string& path, const record& one) {
	const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
	ASSERT_GE(fd, 0) << "could not open " << path << ": " << std::strerror(errno);
	log_appender appender;
	EXPECT_EQ(appender.append(fd, one), append_status::ok);
	::close(fd);
}

// One record as a test wants to assert on it: bytes copied out, because the
// spans the readers hand over borrow buffers that do not outlive the call.
struct seen {
	std::string cmd;
	std::string cwd;
	std::uint64_t when = 0;
	std::int32_t exit_code = 0;
	std::uint64_t session_id = 0;
};

// Every record in the `history.data` at `path`, IN FILE ORDER - which the
// format says is newest first, and which is therefore what the order tests
// assert on directly rather than through the merge walk.
[[nodiscard]] std::vector<seen> blob_records(const std::string& path) {
	const std::vector<std::byte> bytes = read_file(path);
	std::vector<seen> out;
	const blob_status status = read_records(bytes, [&out](const record& one) {
		out.push_back(seen{
			.cmd = as_text(one.cmd),
			.cwd = as_text(one.cwd),
			.when = one.when,
			.exit_code = one.exit_code,
			.session_id = one.session_id,
		});
	});
	EXPECT_EQ(status, blob_status::ok) << path;
	return out;
}

[[nodiscard]] std::vector<std::string> blob_commands(const std::string& path) {
	std::vector<std::string> out;
	for (const seen& one : blob_records(path))
		out.push_back(one.cmd);
	return out;
}

// Every frame in the log at `path`, oldest first.
[[nodiscard]] std::vector<std::string> log_commands(const std::string& path) {
	const std::vector<std::byte> bytes = read_file(path);
	std::vector<std::string> out;
	(void)for_each(bytes, [&out](const record& one) { out.push_back(as_text(one.cmd)); });
	return out;
}

// Every command line the merge walk yields, newest first.
[[nodiscard]] std::vector<std::string> walk(const store& storage) {
	std::vector<std::string> out;
	static_cast<const lesh::ui::history_source&>(storage).for_each_newest_first(
		[&out](std::string_view entry) {
			out.emplace_back(entry);
			return true;
		});
	return out;
}

void run_command(store& storage, std::string_view cmd, std::string_view cwd = "/tmp",
                 std::int32_t exit_code = 0) {
	if (storage.add(cmd, cwd) == add_status::rejected)
		return;
	storage.resolve_pending(exit_code);
}

[[nodiscard]] ::mode_t permissions_of(const std::string& path) {
	struct ::stat info {};
	if (::stat(path.c_str(), &info) != 0)
		return 0;
	return info.st_mode & 07777;
}

// Everything in `dir` whose name starts with `history.data.` - the `mkstemp`
// temps and the corrupt-aside copies. Sorted, so an assertion reads the same
// way twice.
[[nodiscard]] std::vector<std::string> data_siblings(const std::string& dir) {
	std::vector<std::string> out;
	for (const auto& entry : std::filesystem::directory_iterator{dir}) {
		const std::string name = entry.path().filename().string();
		if (name.rfind("history.data.", 0) == 0)
			out.push_back(name);
	}
	std::sort(out.begin(), out.end());
	return out;
}

// The temps, on their own: `history.data.XXXXXX` and not
// `history.data.corrupt-<time>`.
[[nodiscard]] std::vector<std::string> temp_files(const std::string& dir) {
	std::vector<std::string> out;
	for (const std::string& name : data_siblings(dir)) {
		if (name.rfind("history.data.corrupt-", 0) != 0)
			out.push_back(name);
	}
	return out;
}

// A blob whose identifier is not ours - a future lesh's file, or nobody's.
[[nodiscard]] std::vector<std::byte> foreign_blob() {
	blob_writer writer;
	const record one[] = {record{.cmd = as_bytes("somebody else's command"), .when = 1}};
	const std::span<const std::byte> built = writer.build(one);
	std::vector<std::byte> bytes(built.begin(), built.end());
	const char foreign[4] = {'S', 'H', 'H', '9'};
	std::memcpy(bytes.data() + 4, foreign, sizeof(foreign));
	return bytes;
}

// Ours, and wrecked past the identifier: what a Verifier rejection looks like.
[[nodiscard]] std::vector<std::byte> corrupt_blob() {
	blob_writer writer;
	const record one[] = {record{.cmd = as_bytes("a command that is gone"), .when = 1}};
	const std::span<const std::byte> built = writer.build(one);
	std::vector<std::byte> bytes(built.begin(), built.end());
	for (std::size_t at = 8; at < bytes.size(); ++at)
		bytes[at] = std::byte{0xEE};
	return bytes;
}

class scoped_umask {
public:
	explicit scoped_umask(::mode_t mask) : _was(::umask(mask)) {}
	~scoped_umask() { ::umask(_was); }

	scoped_umask(const scoped_umask&) = delete;
	scoped_umask& operator=(const scoped_umask&) = delete;

private:
	::mode_t _was;
};

} // namespace

// ===========================================================================
// The merge (ADR-0010 §Vacuum step 2)
// ===========================================================================

TEST(UiHistoryVacuumMerge, AllThreeSourcesReachTheBlobAndTheLogIsEmptiedAfter) {
	const lesh::testing::temp_path scratch;
	const std::string data = scratch.file("history.data");
	const std::string log = scratch.file("history.new.log");

	// The old blob, the log, and this session: the three inputs the ADR names.
	const record older[] = {record{.cmd = as_bytes("from the blob"), .when = 100}};
	write_blob(data, older);
	append_frame(log, record{.cmd = as_bytes("from the log"), .when = 200});

	store storage;
	ASSERT_TRUE(storage.open(scratch.dir()).log_writable);
	run_command(storage, "from this session");

	const vacuum_result done = storage.vacuum_now();
	ASSERT_EQ(done.status, vacuum_status::renamed);
	EXPECT_EQ(done.records_written, 3u);
	EXPECT_EQ(done.retries, 0u);
	EXPECT_EQ(done.evicted, 0u);

	// Newest first, which is the format and not an accident of the merge.
	EXPECT_EQ(blob_commands(data),
	          (std::vector<std::string>{"from this session", "from the log", "from the blob"}));

	// AND THE LOG IS EMPTY, because its frames are in the blob now. This is the
	// only thing in the subsystem that ever shortens that file.
	EXPECT_TRUE(log_commands(log).empty());
	EXPECT_EQ(read_file(log).size(), 0u);
}

TEST(UiHistoryVacuumMerge, TheWalkIsUnchangedAndNewItemsIsEmptyAfterwards) {
	// #193's amendment, from this side: a SUCCESSFUL vacuum clears `new_items`
	// in the same `publish()` that maps the new blob, and the user cannot tell.
	// Both halves matter - the second is the whole point of the amendment, and
	// the first is what makes it safe.
	const lesh::testing::temp_path scratch;
	store storage;
	ASSERT_TRUE(storage.open(scratch.dir()).log_writable);
	run_command(storage, "one");
	run_command(storage, "two");
	run_command(storage, "three");

	const std::vector<std::string> before = walk(storage);
	ASSERT_EQ(before, (std::vector<std::string>{"three", "two", "one"}));

	ASSERT_EQ(storage.vacuum_now().status, vacuum_status::renamed);

	EXPECT_EQ(walk(storage), before);
	// Every one of them is in the file now, so none of them needs to be in the
	// deque - which is what stops `publish()` from being O(session) per command.
	EXPECT_EQ(storage.session_items(), 0u);
}

TEST(UiHistoryVacuumMerge, DedupKeepsTheNewestRunWhole) {
	// fish's `history_lru_cache_t::add_item` keeps the max timestamp. This goes
	// one step further and keeps the whole newer record - see `merge_set::add`:
	// a `when` from one run beside a `cwd` and an exit code from another
	// describes a command that never happened.
	const lesh::testing::temp_path scratch;
	const std::string data = scratch.file("history.data");

	const record older[] = {record{
		.cmd = as_bytes("git status"),
		.when = 100,
		.cwd = as_bytes("/old/place"),
		.exit_code = 1,
		.session_id = 7,
	}};
	write_blob(data, older);

	store storage;
	ASSERT_TRUE(storage.open(scratch.dir()).log_writable);
	// The same command line, run again, from somewhere else, successfully.
	run_command(storage, "git status", "/new/place", 0);

	ASSERT_EQ(storage.vacuum_now().status, vacuum_status::renamed);

	const std::vector<seen> kept = blob_records(data);
	ASSERT_EQ(kept.size(), 1u) << "the same command line twice is one record";
	EXPECT_EQ(kept[0].cmd, "git status");
	EXPECT_EQ(kept[0].cwd, "/new/place");
	EXPECT_EQ(kept[0].exit_code, 0);
	EXPECT_GE(kept[0].when, 100u);
	EXPECT_NE(kept[0].session_id, 7u) << "the newer run is this session's";
}

TEST(UiHistoryVacuumMerge, AnOlderRepeatDoesNotDragTheTimestampBackwards) {
	// The other direction of the same rule, and the one a naive "last writer
	// wins" gets wrong: a log frame with an OLDER timestamp than the blob's
	// copy of the same command must not replace it.
	const lesh::testing::temp_path scratch;
	const std::string data = scratch.file("history.data");
	const std::string log = scratch.file("history.new.log");

	const record newer[] = {record{
		.cmd = as_bytes("make"), .when = 900, .cwd = as_bytes("/recent"), .exit_code = 0}};
	write_blob(data, newer);
	append_frame(log, record{.cmd = as_bytes("make"),
	                         .when = 100,
	                         .cwd = as_bytes("/ancient"),
	                         .exit_code = 2});

	store storage;
	ASSERT_TRUE(storage.open(scratch.dir()).log_writable);
	ASSERT_EQ(storage.vacuum_now().status, vacuum_status::renamed);

	const std::vector<seen> kept = blob_records(data);
	ASSERT_EQ(kept.size(), 1u);
	EXPECT_EQ(kept[0].when, 900u);
	EXPECT_EQ(kept[0].cwd, "/recent");
	EXPECT_EQ(kept[0].exit_code, 0);
}

TEST(UiHistoryVacuumMerge, TheCapEvictsTheLeastRecentlySeen) {
	// `k_history_save_max` is 256 Ki, and reaching it honestly is a ~25 MB
	// benchmark rather than a unit test, so the request carries the cap - see
	// `vacuum_request::cap`. What is being tested is the rule, and the rule is
	// fish's: the cap evicts the LEAST RECENTLY SEEN, not the smallest
	// timestamp. The distinction is visible here because `old` has the largest
	// `when` of the three and is still the one that goes: it was seen first.
	const lesh::testing::temp_path scratch;
	const std::string data = scratch.file("history.data");
	const std::string log = scratch.file("history.new.log");

	const record older[] = {record{.cmd = as_bytes("old"), .when = 900}};
	write_blob(data, older);
	append_frame(log, record{.cmd = as_bytes("middle"), .when = 500});
	append_frame(log, record{.cmd = as_bytes("recent"), .when = 100});

	const vacuum_result done = vacuum(vacuum_request{
		.data_path = data,
		.log_path = log,
		.cap = 2,
	});
	ASSERT_EQ(done.status, vacuum_status::renamed);
	EXPECT_EQ(done.evicted, 1u);
	EXPECT_EQ(done.records_written, 2u);

	// Still newest-FIRST by timestamp among the survivors; the cap decided WHO
	// survives, the sort decided the order.
	EXPECT_EQ(blob_commands(data), (std::vector<std::string>{"middle", "recent"}));
}

TEST(UiHistoryVacuumMerge, ARepeatPromotesAnEntryPastTheCap) {
	// The consequence of "seen again means recently seen": a command line the
	// user keeps typing does not age out just because its first appearance was
	// long ago. fish's LRU behaves the same way and for the same reason.
	//
	// `dropped` is the NEWER of the two in the old blob and is the one that
	// goes, because `kept` turns up a second time in the log.
	const lesh::testing::temp_path scratch;
	const std::string data = scratch.file("history.data");
	const std::string log = scratch.file("history.new.log");

	// Newest first, which is what the file format means.
	const record older[] = {record{.cmd = as_bytes("dropped"), .when = 20},
	                        record{.cmd = as_bytes("kept"), .when = 10}};
	write_blob(data, older);
	append_frame(log, record{.cmd = as_bytes("kept"), .when = 10});
	append_frame(log, record{.cmd = as_bytes("newcomer"), .when = 30});

	const vacuum_result done = vacuum(vacuum_request{
		.data_path = data,
		.log_path = log,
		.cap = 2,
	});
	ASSERT_EQ(done.status, vacuum_status::renamed);
	EXPECT_EQ(done.evicted, 1u);

	EXPECT_EQ(blob_commands(data), (std::vector<std::string>{"newcomer", "kept"}))
		<< "a repeated command line is recently seen, whatever its timestamp";
}

TEST(UiHistoryVacuumMerge, TheOldBlobIsFedToTheCapOldestFirst) {
	// A subtle one, and the reason it is here on its own: Tier 1 is stored
	// NEWEST first and the LRU wants oldest first. Feeding the file in the
	// order it is written makes the blob's most recent records its least
	// recently seen, and the cap then evicts exactly the ones it exists to
	// keep - silently, and only on a history big enough that nobody is
	// watching. With three records and a cap of two the inversion is visible.
	const lesh::testing::temp_path scratch;
	const std::string data = scratch.file("history.data");

	const record older[] = {record{.cmd = as_bytes("newest"), .when = 300},
	                        record{.cmd = as_bytes("middle"), .when = 200},
	                        record{.cmd = as_bytes("oldest"), .when = 100}};
	write_blob(data, older);

	const vacuum_result done = vacuum(vacuum_request{
		.data_path = data,
		.log_path = scratch.file("history.new.log"),
		.cap = 2,
	});
	ASSERT_EQ(done.status, vacuum_status::renamed);
	EXPECT_EQ(done.evicted, 1u);
	EXPECT_EQ(blob_commands(data), (std::vector<std::string>{"newest", "middle"}));
}

TEST(UiHistoryVacuumMerge, AnEphemeralItemNeverReachesTheFile) {
	// The leading-space rule survives the rewrite, which is the only place it
	// could quietly stop applying: `flush` skips those items, so a vacuum that
	// merged the whole deque instead of the writable part of it would write a
	// secret to disk twenty-five commands after the user typed it.
	const lesh::testing::temp_path scratch;
	store storage;
	ASSERT_TRUE(storage.open(scratch.dir()).log_writable);
	run_command(storage, " a secret");
	run_command(storage, "not a secret");

	ASSERT_EQ(storage.vacuum_now().status, vacuum_status::renamed);
	EXPECT_EQ(blob_commands(scratch.file("history.data")),
	          (std::vector<std::string>{"not a secret"}));
}

TEST(UiHistoryVacuumMerge, ADirectoryItCannotWriteGivesUpAndKeepsEverything) {
	// A directory the store can read and not write: no log, and no temp
	// either. The honest outcome is `gave_up` - never a silent drop, and never
	// a claim that anything was saved.
	const lesh::testing::temp_path scratch;
	const std::string dir = scratch.file("ro");
	ASSERT_EQ(::mkdir(dir.c_str(), 0700), 0);
	ASSERT_EQ(::chmod(dir.c_str(), 0500), 0);

	store storage;
	const open_report report = storage.open(dir);
	ASSERT_FALSE(report.directory_unusable);
	ASSERT_FALSE(report.log_writable) << "the fixture needs an unwritable directory";

	run_command(storage, "nowhere to write this");
	EXPECT_EQ(storage.unwritable_items(), 1u);

	const vacuum_result done = storage.vacuum_now();
	EXPECT_EQ(done.status, vacuum_status::gave_up);
	// Still in memory, still walkable, and no `history.data` invented beside
	// it: nothing was lost and nothing was claimed.
	EXPECT_EQ(walk(storage), (std::vector<std::string>{"nowhere to write this"}));
	EXPECT_TRUE(read_file(scratch.file("ro/history.data")).empty());

	::chmod(dir.c_str(), 0700);
}

// ===========================================================================
// Ownership and mode (ADR-0010 §Vacuum step 4, fish #2355)
// ===========================================================================

TEST(UiHistoryVacuumOwnership, TheModeOfTheOriginalSurvivesTheRewrite) {
	// Somebody's first command in a session may have been under `sudo -E`,
	// leaving a `history.data` whose mode is not what this process's umask
	// would produce. A rewrite that reset it would be a permissions change
	// nobody asked for.
	const scoped_umask relaxed{0};
	const lesh::testing::temp_path scratch;
	const std::string data = scratch.file("history.data");

	const record older[] = {record{.cmd = as_bytes("older"), .when = 1}};
	write_blob(data, older);
	ASSERT_EQ(::chmod(data.c_str(), 0640), 0);

	store storage;
	ASSERT_TRUE(storage.open(scratch.dir()).log_writable);
	run_command(storage, "newer");
	ASSERT_EQ(storage.vacuum_now().status, vacuum_status::renamed);

	EXPECT_EQ(permissions_of(data), 0640u)
		<< "the rewritten file is a new inode and must wear the old one's mode";
	EXPECT_EQ(blob_commands(data), (std::vector<std::string>{"newer", "older"}));
}

TEST(UiHistoryVacuumOwnership, AFirstVacuumWritesSixHundred) {
	// No original to copy from: `mkstemp` makes 0600 and step 1's `O_CREAT`
	// makes 0600, so the two agree and a history file is private from the
	// first one - even under a umask that would happily publish it.
	const scoped_umask relaxed{0};
	const lesh::testing::temp_path scratch;

	store storage;
	ASSERT_TRUE(storage.open(scratch.dir()).log_writable);
	run_command(storage, "the very first command");
	ASSERT_EQ(storage.vacuum_now().status, vacuum_status::renamed);

	EXPECT_EQ(permissions_of(scratch.file("history.data")), 0600u);
}

// ===========================================================================
// The temp file (ADR-0010 §Vacuum steps 2 and 6)
// ===========================================================================

TEST(UiHistoryVacuumTemp, NothingIsLeftBehindOnSuccessOnRefusalOrOnGivingUp) {
	// "Unlink the temp on every exit path" is three exit paths, and a leftover
	// on any of them is a copy of the user's whole history sitting in a
	// world-readable-by-accident file that nothing will ever clean up.
	{
		const lesh::testing::temp_path scratch;
		store storage;
		ASSERT_TRUE(storage.open(scratch.dir()).log_writable);
		run_command(storage, "echo hi");
		ASSERT_EQ(storage.vacuum_now().status, vacuum_status::renamed);
		EXPECT_TRUE(temp_files(scratch.dir()).empty());
	}
	{
		const lesh::testing::temp_path scratch;
		write_file(scratch.file("history.data"), foreign_blob());
		store storage;
		ASSERT_TRUE(storage.open(scratch.dir()).log_writable);
		run_command(storage, "echo hi");
		ASSERT_EQ(storage.vacuum_now().status, vacuum_status::refused);
		EXPECT_TRUE(data_siblings(scratch.dir()).empty())
			<< "a refusal creates nothing at all, not even a temp to remove";
	}
	{
		// Every attempt loses the race, because the hook renames a fresh file
		// over the target after every step 1.
		const lesh::testing::temp_path scratch;
		const std::string data = scratch.file("history.data");
		const record one[] = {record{.cmd = as_bytes("older"), .when = 1}};
		write_blob(data, one);

		int opened = 0;
		const vacuum_result done = vacuum(vacuum_request{
			.data_path = data,
			.log_path = scratch.file("history.new.log"),
			.max_tries = 3,
			.on_step = [&](vacuum_step step) {
				if (step != vacuum_step::target_opened)
					return;
				++opened;
				const std::string usurper = scratch.file("usurper");
				write_blob(usurper, one);
				ASSERT_EQ(::rename(usurper.c_str(), data.c_str()), 0);
			},
		});
		EXPECT_EQ(done.status, vacuum_status::gave_up);
		EXPECT_EQ(done.retries, 3u);
		EXPECT_EQ(opened, 3);
		EXPECT_TRUE(temp_files(scratch.dir()).empty());
		// AND THE TARGET IS UNTOUCHED, which is the half of "give up" that
		// matters: losing the race costs the rewrite, never the file.
		EXPECT_EQ(blob_commands(data), (std::vector<std::string>{"older"}));
	}
}

TEST(UiHistoryVacuumTemp, TheTempIsBesideTheTargetAndNotInTmp) {
	// `rename` across filesystems is `EXDEV`, and on macOS `$TMPDIR` is a
	// different volume from `$HOME` as a matter of course. A temp anywhere but
	// the data directory would turn the atomic slide into a copy - which is the
	// in-place modification ADR-0010 forbids, because a sibling shell has the
	// old inode mapped.
	const lesh::testing::temp_path scratch;
	const std::string data = scratch.file("history.data");
	const record one[] = {record{.cmd = as_bytes("older"), .when = 1}};
	write_blob(data, one);

	std::vector<std::string> seen_while_building;
	const vacuum_result done = vacuum(vacuum_request{
		.data_path = data,
		.log_path = scratch.file("history.new.log"),
		.on_step = [&](vacuum_step step) {
			if (step == vacuum_step::temp_built)
				seen_while_building = temp_files(scratch.dir());
		},
	});
	ASSERT_EQ(done.status, vacuum_status::renamed);
	ASSERT_EQ(seen_while_building.size(), 1u)
		<< "exactly one temp, and it is in the data directory";
	EXPECT_EQ(seen_while_building[0].rfind("history.data.", 0), 0u);
}

// ===========================================================================
// Two shells at once (ADR-0010 §Vacuum step 3)
// ===========================================================================

TEST(UiHistoryVacuumRace, ASiblingThatRenamesBetweenStepsOneAndThreeCostsARetry) {
	// The crux of fish's protocol, driven directly. The speculative build runs
	// with no lock; a second writer slides a different file into place while it
	// runs; step 3 takes the lock, re-stats the PATH, sees a different inode
	// and throws the work away. The second attempt sees the sibling's file as
	// the old blob and merges it, which is the entire reason the retry exists -
	// blindly renaming would have deleted the sibling's records.
	const lesh::testing::temp_path scratch;
	const std::string data = scratch.file("history.data");
	const std::string log = scratch.file("history.new.log");

	const record ours[] = {record{.cmd = as_bytes("ours"), .when = 100}};
	write_blob(data, ours);
	append_frame(log, record{.cmd = as_bytes("from the log"), .when = 300});

	bool interfered = false;
	const vacuum_result done = vacuum(vacuum_request{
		.data_path = data,
		.log_path = log,
		.on_step = [&](vacuum_step step) {
			if (step != vacuum_step::target_opened || interfered)
				return;
			interfered = true;
			// A whole different history file, renamed over the target - which
			// is exactly what another shell's vacuum does.
			const record theirs[] = {record{.cmd = as_bytes("theirs"), .when = 200}};
			const std::string sibling = scratch.file("sibling");
			write_blob(sibling, theirs);
			ASSERT_EQ(::rename(sibling.c_str(), data.c_str()), 0);
		},
	});

	ASSERT_EQ(done.status, vacuum_status::renamed);
	EXPECT_EQ(done.retries, 1u) << "one lost race, one redo";

	// NOTHING WAS LOST. The sibling's record is in the file, and so is the log
	// frame that the first attempt had already merged.
	const std::vector<std::string> settled = blob_commands(data);
	EXPECT_EQ(settled, (std::vector<std::string>{"from the log", "theirs"}));
	// `ours` is gone from the FILE, and that is correct rather than a loss:
	// the sibling's rename replaced it before this vacuum ever locked, so it
	// was the sibling that dropped it, and by then it was no longer part of
	// the history this process is allowed to speak for.
	EXPECT_EQ(std::count(settled.begin(), settled.end(), "ours"), 0);
}

TEST(UiHistoryVacuumRace, AnAppendThatLandsDuringTheRewriteIsNotTruncatedAway) {
	// The one hole ADR-0010's step 5 leaves open, and the reason
	// `vacuum_result::log_bytes_merged` exists. A sibling shell appends a frame
	// after the vacuum has read the log; truncating the log afterwards would
	// delete a resolved command that never reached the blob. The length check
	// turns that into the outcome everything else on this path has - the log is
	// left alone, and the duplicate is the next vacuum's problem.
	const lesh::testing::temp_path scratch;
	const std::string data = scratch.file("history.data");
	const std::string log = scratch.file("history.new.log");

	store storage;
	ASSERT_TRUE(storage.open(scratch.dir()).log_writable);
	run_command(storage, "ours");

	bool appended = false;
	storage.set_vacuum_hook([&](vacuum_step step) {
		if (step != vacuum_step::temp_built || appended)
			return;
		appended = true;
		append_frame(log, record{.cmd = as_bytes("a sibling's command"), .when = 999});
	});

	ASSERT_EQ(storage.vacuum_now().status, vacuum_status::renamed);
	storage.set_vacuum_hook(nullptr);

	// The blob does not have it - it landed after the merge, by construction.
	EXPECT_EQ(blob_commands(data), (std::vector<std::string>{"ours"}));
	// AND THE LOG STILL DOES, which is the assertion. A truncation here would
	// have been the one way this design loses a resolved command.
	const std::vector<std::string> logged = log_commands(log);
	EXPECT_NE(std::find(logged.begin(), logged.end(), "a sibling's command"), logged.end())
		<< "the log was truncated over an append the vacuum never merged";

	// And a fresh session sees both, which is what the user actually cares
	// about.
	store reopened;
	(void)reopened.open(scratch.dir());
	const std::vector<std::string> seen_now = walk(reopened);
	EXPECT_NE(std::find(seen_now.begin(), seen_now.end(), "ours"), seen_now.end());
	EXPECT_NE(std::find(seen_now.begin(), seen_now.end(), "a sibling's command"),
	          seen_now.end());
}

// ===========================================================================
// The corrupt-file policy (#194's decision)
// ===========================================================================

TEST(UiHistoryVacuumCorrupt, OursAndBrokenIsMovedAsideAndRebuilt) {
	// The decision, in one test. Refusing forever would let one flipped byte
	// permanently disable Tier 1 - the log would grow without bound and nothing
	// would ever clear the condition, because nothing but a vacuum writes
	// `history.data`. So it is rebuilt; and because "the Verifier said no" is a
	// statement about bytes rather than about their value, the broken file is
	// renamed aside rather than overwritten.
	const lesh::testing::temp_path scratch;
	const std::string data = scratch.file("history.data");
	const std::vector<std::byte> broken = corrupt_blob();
	write_file(data, broken);

	store storage;
	const open_report report = storage.open(scratch.dir());
	ASSERT_FALSE(report.tier1_mapped);
	ASSERT_TRUE(report.tier1_corrupt);
	ASSERT_TRUE(storage.may_rewrite_tier1());
	// THE COUNTDOWN IS RANDOM (`maybe_vacuum`, `store.h`'s own warning on
	// `set_automatic_vacuum`): its first period starts at a random value in
	// `[0, k_vacuum_frequency)`, so one run in twenty-five the SINGLE
	// `run_command` below would itself land on zero and run a vacuum inside
	// `resolve_pending` - moving the broken file aside and rebuilding it
	// before this test ever calls `vacuum_now()` itself. `corrupt_moved_aside`
	// is a one-shot signal on THAT CALL's `vacuum_result`, so a vacuum that
	// already happened leaves it false on the second, redundant call and the
	// assertion below fails for a reason that has nothing to do with the
	// policy under test - exactly the flake #205 chased through three
	// "load-only" sightings before landing here. Off, so the only vacuum in
	// this test is the one the assertions are about.
	storage.set_automatic_vacuum(false);
	run_command(storage, "a command worth keeping");

	const vacuum_result done = storage.vacuum_now();
	ASSERT_EQ(done.status, vacuum_status::renamed);
	ASSERT_TRUE(done.corrupt_moved_aside);

	// The rebuilt file is ours, verifies, and holds what could be recovered.
	EXPECT_EQ(blob_commands(data), (std::vector<std::string>{"a command worth keeping"}));

	// AND NOTHING WAS DESTROYED: the broken bytes are still on disk, byte for
	// byte, under a name that says what they are.
	ASSERT_FALSE(done.corrupt_path.empty());
	EXPECT_EQ(read_file(done.corrupt_path), broken);
	EXPECT_EQ(done.corrupt_path.rfind(data + ".corrupt-", 0), 0u) << done.corrupt_path;
}

TEST(UiHistoryVacuumCorrupt, AnUnknownIdentifierIsNotMovedAsideAndNotRebuilt) {
	// The other half, and the line #194 did NOT cross. Those bytes are not
	// ours: we cannot say what a rename would cost whoever wrote them, we
	// cannot say what would be lost, and ADR-0010's rule is unqualified.
	const lesh::testing::temp_path scratch;
	const std::string data = scratch.file("history.data");
	const std::vector<std::byte> theirs = foreign_blob();
	write_file(data, theirs);

	store storage;
	ASSERT_TRUE(storage.open(scratch.dir()).tier1_untouchable);
	ASSERT_FALSE(storage.may_rewrite_tier1());
	run_command(storage, "echo hi");

	const vacuum_result done = storage.vacuum_now();
	EXPECT_EQ(done.status, vacuum_status::refused);
	EXPECT_FALSE(done.corrupt_moved_aside);
	EXPECT_EQ(read_file(data), theirs) << "byte for byte, or the rule means nothing";
	// The session is not damaged by the refusal: it runs on Tier 2 and memory.
	EXPECT_EQ(walk(storage), (std::vector<std::string>{"echo hi"}));
	EXPECT_EQ(log_commands(scratch.file("history.new.log")),
	          (std::vector<std::string>{"echo hi"}));
}

TEST(UiHistoryVacuumCorrupt, AFileThatBecomesForeignMidRewriteIsStillRefused) {
	// `may_rewrite_tier1()` is settled at `open`, and the file can change after
	// it - which is why the vacuum re-derives the answer from the bytes it just
	// read off the fd rather than trusting the flag.
	const lesh::testing::temp_path scratch;
	const std::string data = scratch.file("history.data");
	const record ours[] = {record{.cmd = as_bytes("ours"), .when = 1}};
	write_blob(data, ours);

	store storage;
	ASSERT_TRUE(storage.open(scratch.dir()).tier1_mapped);
	ASSERT_TRUE(storage.may_rewrite_tier1());

	const std::vector<std::byte> theirs = foreign_blob();
	storage.set_vacuum_hook([&](vacuum_step step) {
		if (step == vacuum_step::target_opened)
			write_file(data, theirs);
	});
	const vacuum_result done = storage.vacuum_now();
	storage.set_vacuum_hook(nullptr);

	EXPECT_EQ(done.status, vacuum_status::refused);
	EXPECT_EQ(read_file(data), theirs);
}

// ===========================================================================
// Crash injection (the invariant)
// ===========================================================================

namespace {

// The steps a crash can land between, `published` included: the vacuum's own
// six plus the bookkeeping `history` does once the rename has succeeded, which
// is where the log truncation and the dropped items live and is therefore
// half of the window.
constexpr vacuum_step k_all_steps[] = {
	vacuum_step::target_opened,   vacuum_step::temp_built,
	vacuum_step::target_locked,   vacuum_step::ownership_copied,
	vacuum_step::renamed,         vacuum_step::temp_unlinked,
	vacuum_step::published,
};

// Lays down a directory holding two commands in the blob and one in the log,
// which is the smallest shape in which all three merge sources are non-empty.
void seed(const lesh::testing::temp_path& scratch) {
	const record older[] = {record{.cmd = as_bytes("blob-newer"), .when = 200},
	                        record{.cmd = as_bytes("blob-older"), .when = 100}};
	write_blob(scratch.file("history.data"), older);
	append_frame(scratch.file("history.new.log"),
	             record{.cmd = as_bytes("logged"), .when = 300});
}

// Runs a whole vacuum in a FORKED CHILD that dies at `stop_at`.
//
// A FORK AND NOT AN EXCEPTION, because the difference is the entire point. An
// exception unwinds: `~temp_file` runs, the log fd closes cleanly, every
// destructor gets its turn - which is precisely what a power cut does not do.
// `_exit` from the hook leaves the process exactly as a `kill -9` would, with
// whatever half-finished state is on disk at that instant, and it skips
// `atexit` so the sanitizers do not report the child's live allocations as
// leaks.
void crash_at(const std::string& dir, vacuum_step stop_at) {
	const ::pid_t child = ::fork();
	ASSERT_GE(child, 0) << "fork failed: " << std::strerror(errno);
	if (child == 0) {
		store storage;
		(void)storage.open(dir);
		// `add(pending=false)` plus `save()` and NOT `resolve_pending`, which
		// would run the countdown - and a countdown that starts at zero one
		// time in twenty-five would have vacuumed before the hook was set.
		// The step under test has to be the first vacuum this child runs.
		(void)storage.add("session", "/tmp", false);
		(void)storage.save();
		storage.set_vacuum_hook([stop_at](vacuum_step step) {
			if (step == stop_at)
				::_exit(70);
		});
		(void)storage.vacuum_now();
		::_exit(0);
	}
	int status = 0;
	ASSERT_EQ(::waitpid(child, &status, 0), child);
	ASSERT_TRUE(WIFEXITED(status)) << "the child died of a signal, not of the hook";
	EXPECT_EQ(WEXITSTATUS(status), 70) << "the hook did not fire";
}

} // namespace

TEST(UiHistoryVacuumCrash, NoResolvedCommandIsLostAtAnyStep) {
	// THE INVARIANT, and the reason the rest of this file is arranged the way
	// it is. Kill the process between every pair of steps; open a fresh
	// `history` over the wreckage; demand all four commands back. Duplicates
	// are allowed - the merge walk hides them and the next vacuum removes them
	// - and are the price of never holding a lock across the expensive part.
	// An absence is not allowed at any step, for any reason.
	for (const vacuum_step step : k_all_steps) {
		const lesh::testing::temp_path scratch;
		seed(scratch);

		crash_at(scratch.dir(), step);
		if (::testing::Test::HasFatalFailure())
			return;

		store reopened;
		const open_report report = reopened.open(scratch.dir());
		EXPECT_FALSE(report.tier1_untouchable)
			<< "step " << static_cast<int>(step) << " left a file we refuse to touch";

		const std::vector<std::string> seen_now = walk(reopened);
		for (const char* wanted : {"blob-older", "blob-newer", "logged", "session"}) {
			EXPECT_NE(std::find(seen_now.begin(), seen_now.end(), wanted), seen_now.end())
				<< "crash after step " << static_cast<int>(step) << " lost " << wanted;
		}

		// AND THE NEXT VACUUM CLEANS UP after any of them: whatever duplicates
		// the crash left, one more rewrite collapses them.
		ASSERT_EQ(reopened.vacuum_now().status, vacuum_status::renamed);
		const std::vector<std::string> settled = blob_commands(scratch.file("history.data"));
		std::vector<std::string> sorted = settled;
		std::sort(sorted.begin(), sorted.end());
		EXPECT_EQ(std::adjacent_find(sorted.begin(), sorted.end()), sorted.end())
			<< "the rewrite after a crash at step " << static_cast<int>(step)
			<< " left duplicates in the file";
		EXPECT_EQ(settled.size(), 4u);
	}
}

// ===========================================================================
// The countdown (ADR-0010 §Vacuum, fish `save_unless_disabled`)
// ===========================================================================

TEST(UiHistoryVacuumCountdown, ItFiresWithinOnePeriodAndThenEveryPeriod) {
	// The countdown starts RANDOM in `[0, k_vacuum_frequency)` so that a shell
	// used for twenty commands and closed still eventually vacuums, and runs at
	// the full period after that. A test cannot assert on the random part, so
	// it asserts on the two things the randomness is bounded by: one has
	// happened by the end of the first period, and exactly one more by the end
	// of the second.
	const lesh::testing::temp_path scratch;
	store storage;
	ASSERT_TRUE(storage.open(scratch.dir()).log_writable);

	for (int at = 0; at < k_vacuum_frequency; ++at)
		run_command(storage, "command " + std::to_string(at));
	const std::size_t after_one_period = storage.vacuums();
	EXPECT_EQ(after_one_period, 1u);

	for (int at = 0; at < k_vacuum_frequency; ++at)
		run_command(storage, "later " + std::to_string(at));
	EXPECT_EQ(storage.vacuums(), 2u);

	// And the vacuums were real ones - the countdown is not just a counter.
	// Not an assertion on the FILE's size, because where in the second period
	// the second rewrite landed is exactly the random part: the commands after
	// it are in the log, and the walk is what unites the two.
	EXPECT_EQ(walk(storage).size(), static_cast<std::size_t>(2 * k_vacuum_frequency));
	EXPECT_FALSE(blob_commands(scratch.file("history.data")).empty());
}

TEST(UiHistoryVacuumCountdown, AnEphemeralCommandIsNotAnAppendAndDoesNotCount) {
	// "Every 25 appends", and a leading-space command is never appended. A
	// cadence that counted them would depend on how many secrets the user
	// typed, which is both wrong and a small side channel.
	const lesh::testing::temp_path scratch;
	store storage;
	ASSERT_TRUE(storage.open(scratch.dir()).log_writable);

	for (int at = 0; at < 4 * k_vacuum_frequency; ++at)
		run_command(storage, " secret " + std::to_string(at));
	EXPECT_EQ(storage.vacuums(), 0u);
}

TEST(UiHistoryVacuumCountdown, AMemoryOnlyHistoryNeverVacuums) {
	// No `open`, no directory, nothing to rewrite - which is what `vared` and
	// most of the suite hold. The guard is in `maybe_vacuum` so that this stays
	// true however many commands go through it.
	store storage;
	for (int at = 0; at < 3 * k_vacuum_frequency; ++at)
		run_command(storage, "command " + std::to_string(at));
	EXPECT_EQ(storage.vacuums(), 0u);
}

// ===========================================================================
// The locking seam (#195 replaces the bodies; the contract is here)
// ===========================================================================

TEST(UiHistoryVacuumLocking, AFileIdFollowsTheInodeAndNotTheName) {
	// The whole reason `file_id` is not a modification time: a rename-over
	// within one second leaves `st_mtime` alone while replacing the inode,
	// which is exactly the case step 3 exists to catch.
	const lesh::testing::temp_path scratch;
	const std::string path = scratch.file("thing");
	write_file(path, as_bytes("one"));
	const file_id_t before = file_id_of_path(path);
	ASSERT_NE(before, k_invalid_file_id);

	const std::string other = scratch.file("other");
	write_file(other, as_bytes("two"));
	ASSERT_EQ(::rename(other.c_str(), path.c_str()), 0);

	const file_id_t after = file_id_of_path(path);
	ASSERT_NE(after, k_invalid_file_id);
	EXPECT_FALSE(file_id_equal(before, after));
	EXPECT_TRUE(file_id_equal(after, file_id_of_path(path)));
}

TEST(UiHistoryVacuumLocking, AnAbsentTargetIsRewritableButIsNotAMatch) {
	// fish's rule is "the file is unchanged, OR the new file doesn't exist",
	// and #195's `file_id_equal` splits those two apart on purpose: two failed
	// `stat`s are NOT a match, because the absence of evidence is not evidence
	// that the same file is at both ends - which is what a TOCTOU loop needs.
	//
	// STEP 3 STILL PROCEEDS ON AN ABSENT TARGET, and it is the second disjunct
	// that carries it. This test pins both halves, because the condition is
	// only correct as a pair.
	const lesh::testing::temp_path scratch;
	const file_id_t nothing = file_id_of_path(scratch.file("absent"));
	EXPECT_EQ(nothing, k_invalid_file_id);
	EXPECT_FALSE(file_id_equal(nothing, file_id_of_path(scratch.file("also absent"))));

	write_file(scratch.file("present"), as_bytes("x"));
	const file_id_t present = file_id_of_path(scratch.file("present"));
	EXPECT_FALSE(file_id_equal(nothing, present));
	EXPECT_TRUE(file_id_equal(present, file_id_of_path(scratch.file("present"))));

	// `vacuum`'s step 3, spelled out: gone is rewritable, unchanged is
	// rewritable, changed is not.
	const auto may_rename = [](const file_id_t& after, const file_id_t& before) {
		return file_id_equal(after, before) || after == k_invalid_file_id;
	};
	EXPECT_TRUE(may_rename(nothing, nothing));
	EXPECT_TRUE(may_rename(nothing, present));
	EXPECT_TRUE(may_rename(present, present));
	EXPECT_FALSE(may_rename(present, file_id_t{.device = 1, .inode = 2, .size = 3}));
}

TEST(UiHistoryVacuumLocking, AnExclusiveLockIsTakenAndReleased) {
	// Not a concurrency test - `flock` is the kernel's and needs no proving
	// here. What this pins is that the seam's two calls agree with each other,
	// so that a `lock_exclusive` followed by an `unlock` leaves the descriptor
	// lockable again rather than wedged.
	//
	// THE LATCH AND THE REMOTE FLAG ARE PROCESS-WIDE (#195), so this resets
	// them first: a sibling suite that provoked either one would otherwise make
	// `lock_exclusive` here answer false for a reason that has nothing to do
	// with what is being tested.
	test_hooks::reset_locking_state();
	const lesh::testing::temp_path scratch;
	const std::string path = scratch.file("locked");
	write_file(path, as_bytes("x"));
	const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
	ASSERT_GE(fd, 0);

	EXPECT_TRUE(lock_exclusive(fd));
	unlock(fd);
	EXPECT_TRUE(lock_exclusive(fd));
	unlock(fd);
	::close(fd);
}
