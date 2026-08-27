#include "ui/history/store.h"

#include "ui/history/blob.h"
#include "ui/history/log.h"

#include "temp_path.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace lesh::ui::history;

// MILESTONE 3 OF THE TWO-TIER HISTORY (#193, ADR-0010 §In memory, §Recording,
// §Read path): the object the running shell holds.
//
// Three things are actually being tested here and the rest is arithmetic.
//
//   THE RECORDING RULES, which are fish's and which every shell gets subtly
//   wrong at least once: a blank line is not history, a leading space is a
//   secret, the same command twice in a row is one entry with the later
//   timestamp, and a command that is still running is not yet an entry at all.
//
//   THE MERGE WALK, which has to produce ONE newest-first sequence out of three
//   sources that overlap - this session, the log, the mapping - and has to get
//   the tie-break right in a way that is a property of the code and not of the
//   clock. It is: the ordering IS the tie-break, and the tests below assert on
//   which TIER an entry came from rather than on how it compared.
//
//   THE SNAPSHOT DISCIPLINE (ADR-0009), which is the only part that can fail in
//   a way a single-threaded test would never see. `AWalkSeesTheViewItStartedWith`
//   is the one that matters: it stops a walk in the middle, mutates the history
//   underneath it from another thread, and then lets it finish.
//
// And one policy, which is not a rule about shells but a rule about DATA: a
// `history.data` this build does not recognize is never touched. The test for
// it compares the file byte for byte, because "we did not overwrite it" is the
// only useful form of that assertion.

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

// Every command line the walk yields, newest first.
[[nodiscard]] std::vector<std::string> walk(const store& storage) {
	std::vector<std::string> out;
	static_cast<const lesh::ui::history_source&>(storage).for_each_newest_first(
		[&out](std::string_view entry) {
			out.emplace_back(entry);
			return true;
		});
	return out;
}

// The same walk with the whole entry, OWNED. The spans a walk yields are
// borrowed for the duration of the callback (`blob.h`'s rule, all the way up),
// so a test that wants to assert on them afterwards has to copy them out - and
// saying so with a type is clearer than saying it in a comment at each site.
struct seen_entry {
	std::string cmd;
	std::string cwd;
	std::uint64_t when = 0;
	std::int32_t exit_code = 0;
	std::uint64_t session_id = 0;
	merged_entry::origin from = merged_entry::origin::session;
};

[[nodiscard]] std::vector<seen_entry> walk_merged(const store& storage) {
	std::vector<seen_entry> out;
	storage.for_each_merged_newest_first([&out](const merged_entry& one) {
		out.push_back(seen_entry{
			.cmd = as_text(one.what.cmd),
			.cwd = as_text(one.what.cwd),
			.when = one.what.when,
			.exit_code = one.what.exit_code,
			.session_id = one.what.session_id,
			.from = one.from,
		});
		return true;
	});
	return out;
}

// `open`, with #194's PERIODIC VACUUM TURNED OFF.
//
// Every test in this file is about the recording rules, the merge walk or the
// snapshot discipline, and none of them is about the rewrite. The countdown
// starts at a random value in `[0, 25)` - which is a correctness property, so
// that a shell used for twenty commands still eventually vacuums - and one
// command in twenty-five therefore moves the frames these tests assert about
// out of the log and into `history.data`. Left on, this file would be right
// most of the time. `UiHistoryVacuum*` is where the rewrite is tested, and it
// drives it directly.
[[nodiscard]] open_report open_quietly(store& storage, const std::string& directory) {
	storage.set_automatic_vacuum(false);
	return storage.open(directory);
}

// Records `cmd` and finishes it, which is what a command that ran looks like.
void run_command(store& storage, std::string_view cmd, std::string_view cwd = "/tmp",
                 std::int32_t exit_code = 0) {
	if (storage.add(cmd, cwd) == add_status::rejected)
		return;
	storage.resolve_pending(exit_code);
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

// A `history.data` at `path` holding `newest_first`, built through the real
// Tier 1 writer - the test never hand-rolls the format it is asserting about.
void write_blob(const std::string& path, std::span<const record> newest_first) {
	blob_writer writer;
	write_file(path, writer.build(newest_first));
}

// One frame appended to `path`, through the real Tier 2 appender.
void append_frame(const std::string& path, const record& one) {
	const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
	ASSERT_GE(fd, 0) << "could not open " << path << ": " << std::strerror(errno);
	log_appender appender;
	EXPECT_EQ(appender.append(fd, one), append_status::ok);
	::close(fd);
}

// Every frame in the log at `path`, oldest first, as command lines.
[[nodiscard]] std::vector<std::string> log_commands(const std::string& path) {
	const std::vector<std::byte> bytes = read_file(path);
	std::vector<std::string> out;
	(void)for_each(bytes, [&out](const record& one) { out.push_back(as_text(one.cmd)); });
	return out;
}

[[nodiscard]] ::mode_t permissions_of(const std::string& path) {
	struct ::stat info {};
	if (::stat(path.c_str(), &info) != 0)
		return 0;
	return info.st_mode & 07777;
}

// `umask` around one test, put back on the way out. The permissions this
// class asks the kernel for are only observable when the process is not
// subtracting from them, and the suite's umask is whoever ran it.
class scoped_umask {
public:
	explicit scoped_umask(::mode_t mask) : _was(::umask(mask)) {}
	~scoped_umask() { ::umask(_was); }

	scoped_umask(const scoped_umask&) = delete;
	scoped_umask& operator=(const scoped_umask&) = delete;

private:
	::mode_t _was;
};

// `setenv`/`unsetenv` around one test, put back on the way out. The suite runs
// in one process, so a test that left `$HOME` changed would be a test that
// broke the next one.
class scoped_env {
public:
	scoped_env(const char* name, const char* value) : _name(name) {
		if (const char* was = std::getenv(name); was != nullptr) {
			_had = true;
			_was = was;
		}
		if (value == nullptr)
			::unsetenv(name);
		else
			::setenv(name, value, 1);
	}

	~scoped_env() {
		if (_had)
			::setenv(_name.c_str(), _was.c_str(), 1);
		else
			::unsetenv(_name.c_str());
	}

	scoped_env(const scoped_env&) = delete;
	scoped_env& operator=(const scoped_env&) = delete;

private:
	std::string _name;
	std::string _was;
	bool _had = false;
};

} // namespace

// ===========================================================================
// Where the files live (ADR-0010 §Placement)
// ===========================================================================

TEST(UiHistoryPlacement, XdgWinsAndHomeIsTheFallback) {
	{
		const scoped_env xdg{"XDG_DATA_HOME", "/somewhere/data"};
		const scoped_env home{"HOME", "/home/somebody"};
		EXPECT_EQ(store::default_data_directory(), "/somewhere/data/lesh");
	}
	{
		const scoped_env xdg{"XDG_DATA_HOME", nullptr};
		const scoped_env home{"HOME", "/home/somebody"};
		EXPECT_EQ(store::default_data_directory(), "/home/somebody/.local/share/lesh");
	}
}

TEST(UiHistoryPlacement, ARelativeXdgIsIgnoredRatherThanResolved) {
	// The basedir spec says a relative `$XDG_DATA_HOME` must be ignored, and the
	// shell reason is sharper than the spec's: a history file resolved against
	// the cwd would follow the user around with `cd`.
	const scoped_env xdg{"XDG_DATA_HOME", "relative/data"};
	const scoped_env home{"HOME", "/home/somebody"};
	EXPECT_EQ(store::default_data_directory(), "/home/somebody/.local/share/lesh");
}

TEST(UiHistoryPlacement, NoHomeAndNoXdgIsNoStoreAtAll) {
	// #101's rule for `~/.lesh_history`, kept: `nullopt` here is what makes
	// `main` build no store, which reads back as an empty history rather than
	// as an error.
	const scoped_env xdg{"XDG_DATA_HOME", nullptr};
	const scoped_env home{"HOME", nullptr};
	EXPECT_FALSE(store::default_data_directory().has_value());
}

TEST(UiHistoryPlacement, TheDirectoryIsSevenHundredAndTheFilesAreSixHundred) {
	const scoped_umask relaxed{0};
	const lesh::testing::temp_path scratch;
	const std::string dir = scratch.file("a/b/lesh");

	store storage;
	const open_report report = open_quietly(storage, dir);
	ASSERT_FALSE(report.directory_unusable);
	EXPECT_TRUE(report.log_writable);

	// The leaf holds every command the user has typed. The parents this call
	// had to invent along the way are ordinary directories and are not ours to
	// make private on the user's behalf.
	EXPECT_EQ(permissions_of(dir), 0700u);
	EXPECT_EQ(permissions_of(scratch.file("a/b")), 0755u);

	run_command(storage, "echo hi");
	EXPECT_EQ(permissions_of(dir + "/history.new.log"), 0600u);
}

TEST(UiHistoryPlacement, ADirectoryThatCannotBeMadeCostsTheDiskAndNotTheSession) {
	const lesh::testing::temp_path scratch;
	// A regular file where the data directory belongs.
	write_file(scratch.file("lesh"), as_bytes("not a directory"));

	store storage;
	const open_report report = open_quietly(storage, scratch.file("lesh"));
	EXPECT_TRUE(report.directory_unusable);
	EXPECT_FALSE(report.log_writable);

	// And the session still remembers its own commands, which is the point.
	run_command(storage, "echo hi");
	EXPECT_EQ(walk(storage), (std::vector<std::string>{"echo hi"}));
	EXPECT_FALSE(storage.save());
	EXPECT_EQ(storage.unwritable_items(), 1u);
}

// ===========================================================================
// The recording rules (ADR-0010 §In memory, §Recording)
// ===========================================================================

TEST(UiHistoryRecording, APendingItemIsNotHistoryUntilItHasAnExitStatus) {
	// ADR-0010 §Recording: `add` before the run, `resolve_pending` after the
	// wait. Between the two the command is in `new_items` and invisible - a
	// history entry with no exit code is an entry the format cannot hold.
	store storage;
	ASSERT_EQ(storage.add("sleep 1", "/tmp"), add_status::added);
	EXPECT_TRUE(walk(storage).empty());
	EXPECT_EQ(storage.session_items(), 1u);

	storage.resolve_pending(0);
	EXPECT_EQ(walk(storage), (std::vector<std::string>{"sleep 1"}));
}

TEST(UiHistoryRecording, ASecondResolveDoesNothingAndIsNotAnError) {
	store storage;
	run_command(storage, "echo hi");
	storage.resolve_pending(1);
	EXPECT_EQ(walk(storage), (std::vector<std::string>{"echo hi"}));
	EXPECT_EQ(walk_merged(storage)[0].exit_code, 0);
}

TEST(UiHistoryRecording, BlankLinesAreNeverHistory) {
	// fish #6032. The rule is the store's and not the call site's, so that the
	// second call site cannot get it wrong.
	store storage;
	EXPECT_EQ(storage.add("", "/tmp"), add_status::rejected);
	EXPECT_EQ(storage.add("   ", "/tmp"), add_status::rejected);
	EXPECT_EQ(storage.add("\t\n \r", "/tmp"), add_status::rejected);
	EXPECT_EQ(storage.session_items(), 0u);
	EXPECT_TRUE(walk(storage).empty());
}

TEST(UiHistoryRecording, TheSameCommandTwiceInARowIsOneItem) {
	// fish `history_item_t::merge`: same text, same persist mode, so the item
	// already at the back takes the newer timestamp.
	store storage;
	run_command(storage, "git status");
	run_command(storage, "git status");
	EXPECT_EQ(storage.session_items(), 1u);
	EXPECT_EQ(walk(storage), (std::vector<std::string>{"git status"}));

	// NOT adjacent is not a merge: the rule is about repetition, not about
	// duplicates, and the dedup in the walk is what keeps the second one from
	// being SHOWN twice.
	run_command(storage, "ls");
	run_command(storage, "git status");
	EXPECT_EQ(storage.session_items(), 3u);
	EXPECT_EQ(walk(storage), (std::vector<std::string>{"git status", "ls"}));
}

TEST(UiHistoryRecording, AMergedItemTakesTheNewerRunsDirectoryAndExitCode) {
	store storage;
	ASSERT_EQ(storage.add("make", "/one"), add_status::added);
	storage.resolve_pending(0);
	ASSERT_EQ(storage.add("make", "/two"), add_status::merged);
	storage.resolve_pending(2);

	const std::vector<seen_entry> seen = walk_merged(storage);
	ASSERT_EQ(seen.size(), 1u);
	// The merged item is REWRITTEN with the newer timestamp, so its cwd and its
	// exit code have to be the ones that timestamp belongs to - a record whose
	// fields came from two different runs is a record that never happened.
	EXPECT_EQ(seen[0].cwd, "/two");
	EXPECT_EQ(seen[0].exit_code, 2);
}

TEST(UiHistoryRecording, ALeadingSpaceIsRetrievableUntilTheNextCommand) {
	// fish's privacy rule, unconditionally (ADR-0010 §In memory). The point of
	// "until the next add" is that the up-arrow right after a secret command
	// still finds it - the user can fix a typo in the thing they did not want
	// remembered - and that the command after it takes it away.
	store storage;
	run_command(storage, " secret --token=hunter2");
	EXPECT_EQ(walk(storage), (std::vector<std::string>{" secret --token=hunter2"}));

	run_command(storage, "echo ok");
	EXPECT_EQ(walk(storage), (std::vector<std::string>{"echo ok"}));
	EXPECT_EQ(storage.session_items(), 1u);
}

TEST(UiHistoryRecording, ALeadingSpaceNeverReachesTheLog) {
	const lesh::testing::temp_path scratch;
	store storage;
	ASSERT_FALSE(open_quietly(storage, scratch.dir()).directory_unusable);

	run_command(storage, "echo before");
	run_command(storage, " secret --token=hunter2");
	run_command(storage, "echo after");
	EXPECT_TRUE(storage.save());

	EXPECT_EQ(log_commands(scratch.file("history.new.log")),
	          (std::vector<std::string>{"echo before", "echo after"}));
	// And nothing failed to write: an ephemeral item is not an unwritten one.
	EXPECT_EQ(storage.unwritable_items(), 0u);
}

TEST(UiHistoryRecording, ATabIndentedCommandIsOrdinaryHistory) {
	// A LITERAL SPACE and not any whitespace. The space is the gesture users
	// have been taught; a tab is what a paste from a script looks like.
	const lesh::testing::temp_path scratch;
	store storage;
	ASSERT_FALSE(open_quietly(storage, scratch.dir()).directory_unusable);
	run_command(storage, "\techo indented");
	EXPECT_TRUE(storage.save());
	EXPECT_EQ(log_commands(scratch.file("history.new.log")),
	          (std::vector<std::string>{"\techo indented"}));
}

// ===========================================================================
// The log, the frame, and coming back (ADR-0010 §Recording, §Tier 2)
// ===========================================================================

TEST(UiHistoryLogging, ResolvingIsWhatAppendsTheFrame) {
	const lesh::testing::temp_path scratch;
	const std::string log = scratch.file("history.new.log");
	store storage;
	ASSERT_TRUE(open_quietly(storage, scratch.dir()).log_writable);

	ASSERT_EQ(storage.add("echo hi", "/tmp"), add_status::added);
	// Nothing yet: before the wait there is no exit code to write.
	EXPECT_TRUE(log_commands(log).empty());

	storage.resolve_pending(3);
	EXPECT_EQ(log_commands(log), (std::vector<std::string>{"echo hi"}));
}

TEST(UiHistoryLogging, ARestartSeesTheWholeRecord) {
	const lesh::testing::temp_path scratch;
	{
		store first;
		ASSERT_TRUE(open_quietly(first, scratch.dir()).log_writable);
		run_command(first, "echo one", "/one", 0);
		run_command(first, "echo two", "/two", 7);
		EXPECT_TRUE(first.save());
	}

	store second;
	const open_report report = open_quietly(second, scratch.dir());
	EXPECT_EQ(report.log_frames, 2u);
	EXPECT_EQ(report.log_discarded_bytes, 0u);
	// A first run has no `history.data` and that is not damage.
	EXPECT_FALSE(report.tier1_mapped);
	EXPECT_FALSE(report.tier1_untouchable);

	const std::vector<seen_entry> seen = walk_merged(second);
	ASSERT_EQ(seen.size(), 2u);
	EXPECT_EQ(seen[0].cmd, "echo two");
	EXPECT_EQ(seen[0].cwd, "/two");
	EXPECT_EQ(seen[0].exit_code, 7);
	EXPECT_EQ(seen[0].from, merged_entry::origin::log);
	EXPECT_EQ(seen[1].cmd, "echo one");
}

TEST(UiHistoryLogging, AMergeRewindsTheCursorSoTheNewerRunReachesDisk) {
	// The half of `history_item_t::merge` that is easy to leave out: without
	// fish's cursor rewind the merged timestamp lives in memory and the file
	// still says what the first run said. The frame is written TWICE, which is
	// fine - the walk dedups on the way back in and #194's vacuum collapses it.
	const lesh::testing::temp_path scratch;
	{
		store first;
		ASSERT_TRUE(open_quietly(first, scratch.dir()).log_writable);
		run_command(first, "make", "/one", 0);
		run_command(first, "make", "/two", 2);
		EXPECT_TRUE(first.save());
	}
	EXPECT_EQ(log_commands(scratch.file("history.new.log")),
	          (std::vector<std::string>{"make", "make"}));

	store second;
	ASSERT_EQ(open_quietly(second, scratch.dir()).log_frames, 2u);
	const std::vector<seen_entry> seen = walk_merged(second);
	ASSERT_EQ(seen.size(), 1u);
	EXPECT_EQ(seen[0].exit_code, 2);
	EXPECT_EQ(seen[0].cwd, "/two");
}

TEST(UiHistoryLogging, SaveFlushesAndDoesNotVacuum) {
	// ADR-0010: `save()` on interactive exit flushes and does not vacuum. The
	// observable half of "does not vacuum" is that `history.data` is not
	// created and the log is not truncated - both of which are #194's.
	const lesh::testing::temp_path scratch;
	store storage;
	ASSERT_TRUE(open_quietly(storage, scratch.dir()).log_writable);
	run_command(storage, "echo hi");
	EXPECT_TRUE(storage.save());

	EXPECT_EQ(log_commands(scratch.file("history.new.log")).size(), 1u);
	struct ::stat info {};
	EXPECT_NE(::stat(scratch.file("history.data").c_str(), &info), 0);
}

// ===========================================================================
// The merge walk (ADR-0010 §Read path)
// ===========================================================================

TEST(UiHistoryWalk, TheThreeTiersComeBackAsOneNewestFirstSequence) {
	const lesh::testing::temp_path scratch;
	const record older[] = {
		record{.cmd = as_bytes("blob newer"), .when = 200},
		record{.cmd = as_bytes("blob older"), .when = 100},
	};
	write_blob(scratch.file("history.data"), older);
	append_frame(scratch.file("history.new.log"),
	             record{.cmd = as_bytes("log one"), .when = 300});
	append_frame(scratch.file("history.new.log"),
	             record{.cmd = as_bytes("log two"), .when = 400});

	store storage;
	const open_report report = open_quietly(storage, scratch.dir());
	ASSERT_TRUE(report.tier1_mapped);
	ASSERT_EQ(report.log_frames, 2u);
	run_command(storage, "typed just now");

	EXPECT_EQ(walk(storage), (std::vector<std::string>{"typed just now", "log two", "log one",
	                                                 "blob newer", "blob older"}));
}

TEST(UiHistoryWalk, ACommandInEveryTierIsYieldedOnce) {
	const lesh::testing::temp_path scratch;
	const record blob_records[] = {record{.cmd = as_bytes("git status"), .when = 100}};
	write_blob(scratch.file("history.data"), blob_records);
	append_frame(scratch.file("history.new.log"),
	             record{.cmd = as_bytes("git status"), .when = 200});

	store storage;
	ASSERT_TRUE(open_quietly(storage, scratch.dir()).tier1_mapped);
	run_command(storage, "git status");

	EXPECT_EQ(walk(storage), (std::vector<std::string>{"git status"}));
}

TEST(UiHistoryWalk, TheOrderingIsTheTieBreakAndOwnWins) {
	// ADR-0010 §Read path: "the newest wins; on equal `when`, own `session_id`
	// wins". Neither half is a comparison anywhere in the walk - both are the
	// order the three tiers are visited in, which is what makes them hold
	// without a clock the test would have to control. So the assertion is about
	// WHICH TIER survived the dedup, and the equal-`when` case is the same
	// mechanism seen from a different angle.
	const lesh::testing::temp_path scratch;
	const record blob_records[] = {
		record{.cmd = as_bytes("shared"), .when = 100, .session_id = 11},
		record{.cmd = as_bytes("only in the blob"), .when = 100, .session_id = 11},
	};
	write_blob(scratch.file("history.data"), blob_records);
	append_frame(scratch.file("history.new.log"),
	             record{.cmd = as_bytes("shared"), .when = 100, .session_id = 22});
	append_frame(scratch.file("history.new.log"),
	             record{.cmd = as_bytes("only in the blob"), .when = 100, .session_id = 22});

	store storage;
	ASSERT_TRUE(open_quietly(storage, scratch.dir()).tier1_mapped);
	run_command(storage, "shared");

	const std::vector<seen_entry> seen = walk_merged(storage);
	ASSERT_EQ(seen.size(), 2u);
	// This session's copy of `shared` won, and carries this session's id.
	EXPECT_EQ(seen[0].cmd, "shared");
	EXPECT_EQ(seen[0].from, merged_entry::origin::session);
	EXPECT_EQ(seen[0].session_id, storage.session_id());
	// And where this session has no opinion, the LOG beats the mapping for the
	// same reason: it is the tier that was written later.
	EXPECT_EQ(seen[1].cmd, "only in the blob");
	EXPECT_EQ(seen[1].from, merged_entry::origin::log);
	EXPECT_EQ(seen[1].session_id, 22u);
}

TEST(UiHistoryWalk, TheSessionIdIsNotZeroAndNotShared) {
	// It exists for the tie-break and for nothing else, but a store that handed
	// every session the same one would make the tie-break meaningless.
	const store one;
	const store two;
	EXPECT_NE(one.session_id(), 0u);
	EXPECT_NE(one.session_id(), two.session_id());
	// RFC 9562's variant bits are the two high ones of the low half.
	EXPECT_EQ(one.session_id() >> 62, 0x2u);
}

TEST(UiHistoryWalk, AWalkThatStopsSeesNothingAfterTheStop) {
	// #125's requirement on `history_source`: the callback can stop, because the
	// supersede poll would otherwise be a formality.
	store storage;
	run_command(storage, "one");
	run_command(storage, "two");
	run_command(storage, "three");

	std::vector<std::string> seen;
	static_cast<const lesh::ui::history_source&>(storage).for_each_newest_first(
		[&seen](std::string_view entry) {
			seen.emplace_back(entry);
			return false;
		});
	EXPECT_EQ(seen, (std::vector<std::string>{"three"}));
}

TEST(UiHistoryWalk, CommandLinesAreBytesAndNotStrings) {
	// F-34 and `blob.h`'s reason for `[ubyte]`: a shell that cannot recall a
	// command because it was not valid UTF-8 is a shell with a bug. NULs and
	// newlines included, and they survive the round trip through the log.
	const lesh::testing::temp_path scratch;
	const std::string awkward = std::string("echo 'a\0b'\n\xff\xfe", 13);
	{
		store first;
		ASSERT_TRUE(open_quietly(first, scratch.dir()).log_writable);
		run_command(first, awkward);
		EXPECT_TRUE(first.save());
	}
	store second;
	ASSERT_EQ(open_quietly(second, scratch.dir()).log_frames, 1u);
	EXPECT_EQ(walk(second), (std::vector<std::string>{awkward}));
}

// ===========================================================================
// A file that is not ours (ADR-0010: never destroy it)
// ===========================================================================

namespace {

// A well-formed blob with its four identifier bytes replaced, which is exactly
// what a `history.data` written by a future lesh looks like from here.
[[nodiscard]] std::vector<std::byte> blob_with_a_foreign_identifier() {
	blob_writer writer;
	const record one[] = {record{.cmd = as_bytes("somebody else's command"), .when = 1}};
	const std::span<const std::byte> built = writer.build(one);
	std::vector<std::byte> bytes(built.begin(), built.end());
	const char foreign[4] = {'S', 'H', 'H', '9'};
	std::memcpy(bytes.data() + 4, foreign, sizeof(foreign));
	return bytes;
}

} // namespace

TEST(UiHistoryForeignFile, AnUnknownIdentifierIsLeftByteForByteAlone) {
	const lesh::testing::temp_path scratch;
	const std::string data = scratch.file("history.data");
	const std::vector<std::byte> original = blob_with_a_foreign_identifier();
	write_file(data, original);

	store storage;
	const open_report report = open_quietly(storage, scratch.dir());
	EXPECT_FALSE(report.tier1_mapped);
	EXPECT_TRUE(report.tier1_untouchable);
	// The one question this milestone answers on #194's behalf.
	EXPECT_FALSE(storage.may_rewrite_tier1());

	// The session runs on Tier 2 plus memory, which is the whole of what
	// ADR-0010 asks for - and then the file is still there, unchanged.
	run_command(storage, "echo hi");
	EXPECT_TRUE(storage.save());
	EXPECT_EQ(walk(storage), (std::vector<std::string>{"echo hi"}));
	EXPECT_EQ(read_file(data), original);
	EXPECT_EQ(log_commands(scratch.file("history.new.log")),
	          (std::vector<std::string>{"echo hi"}));
}

TEST(UiHistoryForeignFile, ItWarnsOnceAndNotOnceAgain) {
	const lesh::testing::temp_path scratch;
	write_file(scratch.file("history.data"), blob_with_a_foreign_identifier());

	::testing::internal::CaptureStderr();
	store storage;
	(void)open_quietly(storage, scratch.dir());
	// A second open of the same directory - the reload #195 will make routine -
	// must not say it again. A shell that warned every prompt would be a shell
	// nobody could use.
	(void)open_quietly(storage, scratch.dir());
	const std::string said = ::testing::internal::GetCapturedStderr();

	EXPECT_EQ(storage.warnings(), 1u);
	EXPECT_NE(said.find("history.data"), std::string::npos) << said;
	EXPECT_NE(said.find("not a lesh history file"), std::string::npos) << said;
}

TEST(UiHistoryForeignFile, ABlobOursAndBrokenIsRebuildableButNotYetRebuilt) {
	// #194's decision, from this side of the seam: a `corrupt` Tier 1 - ours,
	// and rejected by the Verifier - is NOT untouchable. It is rebuilt at the
	// next vacuum, after being renamed aside; until then the session runs on
	// Tier 2 plus memory exactly as it would for a foreign file, and `open`
	// itself still does not touch a byte of it.
	// `UiHistoryVacuumCorrupt` is where the rebuild is asserted.
	const lesh::testing::temp_path scratch;
	blob_writer writer;
	const record one[] = {record{.cmd = as_bytes("a command"), .when = 1}};
	const std::span<const std::byte> built = writer.build(one);
	std::vector<std::byte> bytes(built.begin(), built.end());
	// Keep the identifier, wreck the rest.
	for (std::size_t at = 8; at < bytes.size(); ++at)
		bytes[at] = std::byte{0xEE};
	write_file(scratch.file("history.data"), bytes);

	store storage;
	const open_report report = open_quietly(storage, scratch.dir());
	EXPECT_FALSE(report.tier1_mapped);
	EXPECT_FALSE(report.tier1_untouchable);
	EXPECT_TRUE(report.tier1_corrupt);
	EXPECT_TRUE(storage.may_rewrite_tier1());
	EXPECT_EQ(storage.warnings(), 1u);
	EXPECT_EQ(read_file(scratch.file("history.data")), bytes);
}

// ===========================================================================
// Snapshot views (ADR-0009) - the half a single-threaded test cannot see
// ===========================================================================

TEST(UiHistoryView, AWalkSeesTheViewItStartedWithAndNothingAfterIt) {
	// The whole reason the view exists. A worker walks; the loop thread records
	// three commands underneath it; the walk finishes and has seen the history
	// as it was when it started - not a mixture, not a torn container, and not
	// a crash.
	store storage;
	run_command(storage, "one");
	run_command(storage, "two");
	run_command(storage, "three");

	std::promise<void> walk_began;
	std::promise<void> mutations_done;
	std::future<void> began = walk_began.get_future();
	std::future<void> mutated = mutations_done.get_future();

	std::vector<std::string> seen;
	std::thread walker([&] {
		bool first = true;
		static_cast<const lesh::ui::history_source&>(storage).for_each_newest_first(
			[&](std::string_view entry) {
				seen.emplace_back(entry);
				if (first) {
					first = false;
					walk_began.set_value();
					mutated.wait();
				}
				return true;
			});
	});

	began.wait();
	run_command(storage, "four");
	run_command(storage, "five");
	run_command(storage, " six");
	mutations_done.set_value();
	walker.join();

	EXPECT_EQ(seen, (std::vector<std::string>{"three", "two", "one"}));
	// And the next walk sees all of it, which is what makes the first one a
	// snapshot rather than a bug.
	EXPECT_EQ(walk(storage),
	          (std::vector<std::string>{" six", "five", "four", "three", "two", "one"}));
}

TEST(UiHistoryView, AWalkOnAFreshStoreIsEmptyRatherThanUndefined) {
	// The view is published by the constructor, because a worker can be handed
	// the source before `open` is ever called - and `vared` never calls it.
	const store storage;
	EXPECT_TRUE(walk(storage).empty());
}

TEST(UiHistoryView, TwoThreadsWalkingAtOnceDoNotShareScratch) {
	// The dedup table is thread-local, which is the only reason two workers can
	// walk the same history at the same time. If they shared one, this would
	// come back short.
	store storage;
	for (int i = 0; i < 200; ++i)
		run_command(storage, "command " + std::to_string(i));

	std::vector<std::vector<std::string>> results(4);
	std::vector<std::thread> walkers;
	walkers.reserve(results.size());
	for (std::vector<std::string>& into : results)
		walkers.emplace_back([&storage, &into] { into = walk(storage); });
	for (std::thread& one : walkers)
		one.join();

	for (const std::vector<std::string>& one : results) {
		ASSERT_EQ(one.size(), 200u);
		EXPECT_EQ(one.front(), "command 199");
		EXPECT_EQ(one.back(), "command 0");
	}
}

TEST(UiHistoryView, AWarmWalkGrowsNoScratch) {
	// #90's rule, on the instrument that can actually see this class's scratch:
	// the arena counter `UiAutosuggest` reads would not notice a `std::vector`
	// growing inside the walk, and `scratch_growths` would.
	store storage;
	for (int i = 0; i < 300; ++i)
		run_command(storage, "command " + std::to_string(i));

	// Warm: the first walks on this thread are where the table grows to fit.
	(void)walk(storage);
	(void)walk(storage);

	const std::size_t before = store::scratch_growths();
	for (int i = 0; i < 20; ++i)
		(void)walk(storage);
	EXPECT_EQ(store::scratch_growths(), before);
}
