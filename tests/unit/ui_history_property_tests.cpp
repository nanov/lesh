#include "ui/history/history.h"

#include "ui/history/blob.h"
#include "ui/history/locking.h"
#include "ui/history/log.h"
#include "ui/history/vacuum.h"

#include "temp_path.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace lesh::ui::history;

// MILESTONE 6 OF THE TWO-TIER HISTORY (#196, ADR-0010 §Read path, §Vacuum): a
// SEEDED RANDOM PROGRAM against the merge and dedup path.
//
// WHY A PROPERTY TEST AND NOT TWENTY MORE EXAMPLES. Everything below #191..#195
// is tested by example, and the examples are the situations somebody thought
// of. The bugs fish accumulated in this exact code over fifteen years are the
// situations nobody thought of, and they all have the same shape: THREE
// SESSIONS, ONE DIRECTORY, and an interleaving where a pending item, a vacuum
// and a reload land in an order no test enumerates. So this file does not
// enumerate. It runs 2-4 `history` instances over one data directory, has them
// issue a random program of adds, resolutions, saves, vacuums, reloads and
// restarts, and after EVERY STEP demands seven invariants that must hold
// whatever the interleaving was.
//
// THE SEVEN, and what each one is protecting:
//
//   1. NO COMMITTED COMMAND IS EVER LOST. A command line that has reached
//      either file stays reachable - from a fresh instance over the same
//      directory, which is the only reader that has no memory to hide a loss
//      in. The only licence to drop one is the cap, and the cap has to declare
//      itself: a command may leave the disk only in the same step in which a
//      vacuum reported evicting at least that many records.
//   2. NO WALK YIELDS THE SAME COMMAND TWICE. The dedup runs across three tiers
//      that overlap by construction, and a duplicate is what the user sees.
//   3. THE ORDER IS THE ADR'S ORDER - see `check_order` below, which is the one
//      invariant here whose precise statement took an argument.
//   4. NO EPHEMERAL AND NO PENDING ITEM IS EVER ON DISK, or in any other
//      session's walk. Checked against the FILES, frame by frame and record by
//      record, and not against a walk - the walk is the thing that could be
//      hiding it.
//   5. TIER 1 HOLDS AT MOST `cap` RECORDS. Run at a cap of a handful rather
//      than 256 Ki, so that eviction actually happens (#196 lowered it through
//      `history::set_vacuum_cap`; `vacuum.h` says why the knob exists).
//   6. NO TEMP FILE IS LEFT BEHIND. `history.data.XXXXXX` after a clean vacuum
//      is a leak of the user's whole history into a file nothing will remove.
//   7. AND ALL OF (1) AFTER A CRASH. `UiHistoryPropertyCrash` runs the program,
//      then kills a FORKED CHILD in the middle of a vacuum at a random step,
//      then demands every command back from a fresh instance.
//
// THE CLOCK IS CONTROLLED, which is the part that makes (3) worth asserting.
// `when` is unix SECONDS, so a test that let the clock run would stamp a whole
// run with one value and could never tell an ordering rule from a coincidence;
// #196 added `test_hooks::set_now_override` for it, and the program moves the
// clock by NOUGHT OR ONE per command, so ties are ordinary rather than rare.
//
// SEEDS. 32 programs of 32 steps in the gate, plus four more per vacuum step
// for the crash test, which is what keeps this suite in the low seconds under
// the sanitizers. Three environment variables widen it locally:
// `LESH_HISTORY_PROPERTY_SEEDS=n` for the sweep (this file was developed
// against 3000), `LESH_HISTORY_PROPERTY_CRASH_SEEDS=n` for the forking one,
// and `LESH_HISTORY_PROPERTY_SEED=n` to run the ONE seed a failure named -
// every failure prints its seed, its step index and what that step was doing,
// and the run is deterministic from the seed alone.

namespace {

// ---------------------------------------------------------------------------
// Bytes, paths and printing
// ---------------------------------------------------------------------------

[[nodiscard]] std::string as_text(std::span<const std::byte> bytes) {
	return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// A command line as a failure message can print it: the pool below holds
// embedded NULs, newlines and bytes that are not UTF-8, and a raw `<<` of those
// makes a diagnosis unreadable at best and truncated at worst.
[[nodiscard]] std::string printable(std::string_view bytes) {
	std::string out = "\"";
	for (const char raw : bytes) {
		const auto byte = static_cast<unsigned char>(raw);
		if (byte >= 0x20 && byte < 0x7F && byte != '"' && byte != '\\') {
			out.push_back(raw);
			continue;
		}
		char escaped[5] = {};
		std::snprintf(escaped, sizeof(escaped), "\\x%02X", byte);
		out += escaped;
	}
	out.push_back('"');
	return out;
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

// ---------------------------------------------------------------------------
// What a walk yielded
// ---------------------------------------------------------------------------

// One entry, OWNED. The spans a walk hands over borrow the mapping and the
// session's deque (`blob.h`'s rule, all the way up), so anything asserted about
// after the callback returns has to be copied out first.
struct yielded {
	std::string cmd;
	std::uint64_t when = 0;
	std::uint64_t session_id = 0;
	merged_entry::origin from = merged_entry::origin::session;
};

[[nodiscard]] std::vector<yielded> walk(const history& store) {
	std::vector<yielded> out;
	store.for_each_merged_newest_first([&out](const merged_entry& one) {
		out.push_back(yielded{
			.cmd = as_text(one.what.cmd),
			.when = one.what.when,
			.session_id = one.what.session_id,
			.from = one.from,
		});
		return true;
	});
	return out;
}

[[nodiscard]] std::vector<std::string> commands_of(const std::vector<yielded>& entries) {
	std::vector<std::string> out;
	out.reserve(entries.size());
	for (const yielded& one : entries)
		out.push_back(one.cmd);
	return out;
}

[[nodiscard]] const char* name_of(merged_entry::origin from) {
	switch (from) {
	case merged_entry::origin::session:
		return "session";
	case merged_entry::origin::log:
		return "log";
	case merged_entry::origin::blob:
		return "blob";
	}
	return "?";
}

[[nodiscard]] int rank_of(merged_entry::origin from) {
	return static_cast<int>(from);
}

// ---------------------------------------------------------------------------
// What is on disk, read from the files and not from any walk
// ---------------------------------------------------------------------------

// The two files as bytes, which is the only vantage point from which "no
// ephemeral item was ever written" and "nothing was lost" are worth asserting:
// a walk is exactly the code that could be hiding either.
struct on_disk {
	// Every command line in `history.new.log`, oldest first.
	std::vector<std::string> log;
	// Every command line in `history.data`, IN FILE ORDER - newest first, which
	// the format promises and `check_blob_file` verifies.
	std::vector<std::string> blob;
	// The blob's timestamps, in the same order.
	std::vector<std::uint64_t> blob_when;
	// Every command line in either.
	std::set<std::string> everything;
	// Frames the log reader could not account for, and the blob's verdict:
	// nothing in this file damages either file, so anything but a clean read is
	// itself the failure.
	std::size_t log_discarded = 0;
	blob_status blob_verdict = blob_status::io_error;
	bool blob_present = false;
};

[[nodiscard]] on_disk scan(const std::string& dir) {
	on_disk out;

	const std::vector<std::byte> log_bytes = read_file(dir + "/history.new.log");
	const log_scan scanned = for_each(log_bytes, [&out](const record& one) {
		out.log.push_back(as_text(one.cmd));
	});
	out.log_discarded = scanned.discarded_bytes;

	const std::string data_path = dir + "/history.data";
	out.blob_present = std::filesystem::exists(data_path);
	if (out.blob_present) {
		const std::vector<std::byte> blob_bytes = read_file(data_path);
		out.blob_verdict = read_records(blob_bytes, [&out](const record& one) {
			out.blob.push_back(as_text(one.cmd));
			out.blob_when.push_back(one.when);
		});
	}

	out.everything.insert(out.log.begin(), out.log.end());
	out.everything.insert(out.blob.begin(), out.blob.end());
	return out;
}

// Everything in `dir` named `history.data.*`: the `mkstemp` temps and the
// corrupt-aside copies. Nothing in this file corrupts a blob, so on a clean run
// this list must always be empty.
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

// One record per command line in `history.data`, which is what a rewrite that
// ran to the end owes whatever the duplicates going in were.
//
// NOT THE TEMP FILES. This is the check the crash test runs, and a crash
// between steps 1 and 5 leaves a `history.data.XXXXXX` that no code in the
// process that died can remove (`vacuum.h` step 6 says so, and fish leaves the
// same one). `check_no_temp_files` holds the clean runs to the stricter rule.
void check_no_duplicate_records(const std::string& dir) {
	std::vector<std::string> sorted = scan(dir).blob;
	std::sort(sorted.begin(), sorted.end());
	EXPECT_EQ(std::adjacent_find(sorted.begin(), sorted.end()), sorted.end())
		<< "the rewrite left duplicates in history.data";
}

// ---------------------------------------------------------------------------
// The command pool
// ---------------------------------------------------------------------------

// The command lines the program draws from. SMALL ON PURPOSE - duplicates
// across sessions and within one session are the whole point of a dedup test,
// and a pool of eight over four sessions produces them at every step.
//
// Two of them are not text: a command line is RAW BYTES (ADR-0010 §Tier 1 uses
// `[ubyte]` and not `string` for exactly this), and a shell records what the
// user typed whether or not it decodes.
[[nodiscard]] const std::vector<std::string>& command_pool() {
	static const std::vector<std::string> pool = {
		"ls -l",
		"git status",
		"cd ..",
		"echo hello",
		"make -j8\nsecond line",
		std::string("\xFF\xFE not utf-8 at all", 19),
		std::string("with\0a nul", 10),
		"grep -rn needle .",
	};
	return pool;
}

// Blank lines, which are never history (fish #6032) and which the program
// therefore expects to be REJECTED rather than recorded.
[[nodiscard]] const std::vector<std::string>& blank_pool() {
	static const std::vector<std::string> pool = {"", "   ", "\t", " \n\t "};
	return pool;
}

[[nodiscard]] const std::vector<std::string>& cwd_pool() {
	static const std::vector<std::string> pool = {"/tmp", "/home/x", "/", "/var/log"};
	return pool;
}

// ---------------------------------------------------------------------------
// The program
// ---------------------------------------------------------------------------

// One `history` instance and everything the model knows about it.
struct session_state {
	std::unique_ptr<history> store;
	// Which terminal this is, for a failure message. Survives a restart.
	std::size_t slot = 0;

	// The command line of this session's OUTSTANDING PENDING item, if it has
	// one. Reserved bytes that no other draw ever produces, so that "this must
	// never appear anywhere" is a statement the model can make exactly.
	std::string pending;
	// The command line of this session's live EPHEMERAL item - the one a
	// leading space made, retrievable here until the next add and never
	// anywhere else. Also reserved bytes, for the same reason.
	std::string ephemeral;

	// Resolved `disk` items this session has recorded that have not yet been
	// SEEN on disk. `save()` must empty this.
	std::set<std::string> owed;

	// The log's frames as they were when this store last READ the log - at
	// `open`, and again after each of its own successful vacuums. The walk's
	// log tier is a subsequence of this reversed, and nothing else.
	std::vector<std::string> log_at_load;
	// The blob's records as they were when this store last MAPPED `history.data`
	// - at `open`, after its own vacuum, and after any reload.
	std::vector<std::string> blob_at_map;

	std::size_t reloads_seen = 0;
};

class program {
public:
	program(std::uint64_t seed, std::string dir, std::size_t sessions, std::size_t cap)
		: _seed(seed), _dir(std::move(dir)), _cap(cap), _rng(seed) {
		test_hooks::set_now_override(_clock);
		for (std::size_t at = 0; at < sessions; ++at) {
			_sessions.push_back(session_state{});
			_sessions.back().slot = at;
			restart(_sessions.back(), /*saving=*/false);
		}
	}

	// Runs `steps` operations, checking every invariant after each one, and
	// then closes the run out the way the ticket asks: a last vacuum, a restart
	// of every session, and the whole check again.
	void run(std::size_t steps) {
		for (std::size_t at = 0; at < steps; ++at) {
			_step = at;
			one_step();
			check();
			if (::testing::Test::HasFailure())
				return;
		}

		// --- The settling phase --------------------------------------------
		//
		// `save(one)` AND NOT `one.store->save()`, because the step's own scan
		// is what settles what this session still owes the disk: the vacuum
		// below may evict some of it, and a model that had not looked yet would
		// hold the next save to a promise the cap had already taken back.
		_step = steps;
		for (session_state& one : _sessions)
			save(one);
		compact(_sessions.front());
		check();
		if (::testing::Test::HasFailure())
			return;

		_step = steps + 1;
		for (session_state& one : _sessions)
			restart(one, /*saving=*/true);
		check();
		if (::testing::Test::HasFailure())
			return;

		// AND EVERY SESSION SEES EVERYTHING, which a restarted one must: it has
		// no memory left to serve a command out of, so anything it yields came
		// off the disk and anything on the disk it does not yield is lost.
		const on_disk now = scan(_dir);
		for (const session_state& one : _sessions) {
			const std::vector<std::string> seen = commands_of(walk(*one.store));
			for (const std::string& wanted : now.everything) {
				EXPECT_NE(std::find(seen.begin(), seen.end(), wanted), seen.end())
					<< where() << ": session " << one.slot << " lost " << printable(wanted)
					<< " after a restart";
			}
		}
	}

private:
	// --- Diagnostics ---------------------------------------------------------

	[[nodiscard]] std::string where() const {
		return "seed " + std::to_string(_seed) + " step " + std::to_string(_step) + " ("
		       + _what + ")";
	}

	// --- Randomness ----------------------------------------------------------

	[[nodiscard]] std::size_t pick(std::size_t bound) {
		return std::uniform_int_distribution<std::size_t>{0, bound - 1}(_rng);
	}

	[[nodiscard]] bool chance(int percent) {
		return static_cast<int>(pick(100)) < percent;
	}

	// NOUGHT OR ONE SECOND PER COMMAND. Both halves matter: the ties are what
	// make "on equal `when`, own `session_id` wins" a rule the walk can be held
	// to, and the advances are what make "newest first" mean anything at all.
	void tick() {
		if (chance(50))
			++_clock;
		test_hooks::set_now_override(_clock);
	}

	// --- The steps -----------------------------------------------------------

	void one_step() {
		session_state& one = _sessions[pick(_sessions.size())];

		// A session holding a pending item is a session whose command is still
		// RUNNING (ADR-0010 §Recording: `session::execute` adds, waits, then
		// resolves), so the only things it can do are finish, exit, or wait.
		if (!one.pending.empty()) {
			switch (pick(5)) {
			case 0:
			case 1:
				return resolve(one);
			case 2:
				return save(one);
			case 3:
				// A REWRITE WITH A COMMAND STILL RUNNING, which is the ordinary
				// case for a shell that has been up long enough to vacuum: the
				// pending item is excluded from the merge and has to still be
				// there, and still be invisible, when it returns.
				return compact(one);
			case 4:
				return restart(one, /*saving=*/chance(50));
			default:
				return notice(one);
			}
		}

		switch (pick(20)) {
		case 0:
		case 1:
		case 2:
		case 3:
		case 4:
		case 5:
			return run_command(one);
		case 6:
		case 7:
			return add_ephemeral(one);
		case 8:
			return add_blank(one);
		case 9:
		case 10:
			return add_unpending(one);
		case 11:
		case 12:
			return leave_pending(one);
		case 13:
		case 14:
			return save(one);
		case 15:
		case 16:
			return compact(one);
		case 17:
		case 18:
			return notice(one);
		default:
			return restart(one, /*saving=*/chance(50));
		}
	}

	// The ordinary path, and the one every other step exists to disturb: add,
	// run, resolve. The frame reaches the log HERE (ADR-0010 §Recording) and
	// nowhere else, which is why this is the only step that may demand the
	// command be on disk the instant it returns.
	void run_command(session_state& one) {
		const std::string cmd = command_pool()[pick(command_pool().size())];
		_what = "run " + printable(cmd) + " on session " + std::to_string(one.slot);
		tick();

		// WAS IT ALREADY ON DISK? If so, this session is about to hold its own,
		// newer copy of a command line the file also has, which is the exact
		// shape the tie-break rule is about - and the assertion below is the
		// one positive test in this file that the OWN copy is the one that wins.
		const bool was_on_disk = scan(_dir).everything.count(cmd) != 0;

		ASSERT_NE(one.store->add(cmd, cwd_pool()[pick(cwd_pool().size())]),
		          add_status::rejected)
			<< where();
		one.ephemeral.clear();
		one.store->resolve_pending(exit_code());
		EXPECT_EQ(one.store->unwritable_items(), 0u)
			<< where() << ": the resolve could not write its frame";

		const on_disk now = scan(_dir);
		EXPECT_EQ(now.everything.count(cmd), 1u)
			<< where() << ": a resolved command did not reach the disk";
		one.owed.erase(cmd);
		note_disk(now);

		if (!was_on_disk)
			return;
		const std::vector<yielded> seen = walk(*one.store);
		const auto found = std::find_if(seen.begin(), seen.end(),
		                                [&cmd](const yielded& e) { return e.cmd == cmd; });
		ASSERT_NE(found, seen.end()) << where() << ": the command just run is not in the walk";
		EXPECT_EQ(found->from, merged_entry::origin::session)
			<< where() << ": the disk's copy of " << printable(cmd)
			<< " beat this session's own, newer one";
		EXPECT_EQ(found->session_id, one.store->session_id()) << where();
		EXPECT_EQ(found->when, _clock) << where();
	}

	// A LEADING SPACE (ADR-0010 §In memory, fish's rule): retrievable here until
	// the next add, and never written anywhere. The bytes are unique per use, so
	// that "this must never be on disk or in another session's walk" is a claim
	// the model can check exactly rather than approximately.
	void add_ephemeral(session_state& one) {
		const std::string cmd = " secret-" + std::to_string(_reserved++);
		_what = "ephemeral " + printable(cmd) + " on session " + std::to_string(one.slot);
		tick();

		ASSERT_NE(one.store->add(cmd, "/tmp"), add_status::rejected) << where();
		one.ephemeral = cmd;
		_ephemeral_ever.insert(cmd);
		one.store->resolve_pending(exit_code());
	}

	// Empty and whitespace-only, which no shell records (fish #6032). Nothing
	// about the session may change.
	void add_blank(session_state& one) {
		const std::string cmd = blank_pool()[pick(blank_pool().size())];
		_what = "blank " + printable(cmd) + " on session " + std::to_string(one.slot);
		tick();

		const std::size_t items = one.store->session_items();
		EXPECT_EQ(one.store->add(cmd, "/tmp"), add_status::rejected) << where();
		EXPECT_EQ(one.store->session_items(), items)
			<< where() << ": a blank line became an item";
	}

	// `add(pending=false)`: a finished item recorded directly, which is what an
	// importer would do and what the test wants for the state in between. NO
	// FLUSH HAPPENS HERE - the item is resolved and unwritten, and the next
	// `save`, resolve or vacuum on this session is what puts it on disk.
	void add_unpending(session_state& one) {
		const std::string cmd = command_pool()[pick(command_pool().size())];
		_what = "add-unpending " + printable(cmd) + " on session " + std::to_string(one.slot);
		tick();

		ASSERT_NE(one.store->add(cmd, "/tmp", /*pending=*/false), add_status::rejected)
			<< where();
		one.ephemeral.clear();
		if (scan(_dir).everything.count(cmd) == 0)
			one.owed.insert(cmd);
	}

	// A command that is STILL RUNNING. Excluded from every read and from both
	// files until it resolves, and lost outright if this session dies first
	// (ADR-0010 §Recording: no sentinel write).
	void leave_pending(session_state& one) {
		const std::string cmd = "running-" + std::to_string(_reserved++);
		_what = "leave-pending " + printable(cmd) + " on session " + std::to_string(one.slot);
		tick();

		ASSERT_NE(one.store->add(cmd, "/tmp"), add_status::rejected) << where();
		one.ephemeral.clear();
		one.pending = cmd;
		_forbidden.insert(cmd);
	}

	void resolve(session_state& one) {
		_what = "resolve " + printable(one.pending) + " on session " + std::to_string(one.slot);
		one.store->resolve_pending(exit_code());
		const std::string was = one.pending;
		one.pending.clear();
		// It is an ordinary command line from here on: resolved, written, and
		// allowed everywhere the others are.
		_forbidden.erase(was);
		EXPECT_EQ(one.store->unwritable_items(), 0u) << where();
		const on_disk now = scan(_dir);
		EXPECT_EQ(now.everything.count(was), 1u)
			<< where() << ": a resolved command did not reach the disk";
		note_disk(now);
	}

	void save(session_state& one) {
		_what = "save session " + std::to_string(one.slot);
		EXPECT_TRUE(one.store->save()) << where() << ": items this session cannot write";

		const on_disk now = scan(_dir);
		for (const std::string& cmd : one.owed) {
			EXPECT_EQ(now.everything.count(cmd), 1u)
				<< where() << ": " << printable(cmd) << " was still not on disk after a save";
		}
		one.owed.clear();
		note_disk(now);
	}

	// The rewrite, driven directly (`history::vacuum_now`), because the
	// countdown is off in every session here - #194's `set_automatic_vacuum`,
	// and the same reason every other suite turns it off.
	void compact(session_state& one) {
		_what = "vacuum session " + std::to_string(one.slot);
		const vacuum_result done = one.store->vacuum_now();
		// Nothing in this file makes a target unwritable or holds the race
		// open, so the rewrite has no licence to answer anything else.
		ASSERT_EQ(done.status, vacuum_status::renamed)
			<< where() << ": errno " << done.error;
		EXPECT_LE(done.records_written, _cap) << where();
		_evicted_this_step += done.evicted;

		// It just re-read both files, so both snapshots are the files as they
		// are now - nothing else in this single-threaded program writes them.
		const on_disk now = scan(_dir);
		one.log_at_load = now.log;
		one.blob_at_map = now.blob;
		one.reloads_seen = one.store->reloads();
		one.owed.clear();
		note_disk(now);
	}

	// A SIBLING'S VACUUM, NOTICED (#195, fish #3565). The watch is on the
	// directory and the notification is already queued by the time we get here -
	// the `rename` that produced it happened in an earlier step of this very
	// thread - so the poll is a zero-timeout drain and not a wait.
	//
	// `incorporate_external_changes` afterwards, because a session that appended
	// in the meantime already has the flag up from its own file-id check and has
	// no notification to drain.
	void notice(session_state& one) {
		_what = "notice on session " + std::to_string(one.slot);
		if (one.store->watch_fd() >= 0) {
			for (int drains = 0; drains < 4; ++drains) {
				struct ::pollfd waiting {};
				waiting.fd = one.store->watch_fd();
				waiting.events = POLLIN;
				if (::poll(&waiting, 1, 0) <= 0 || (waiting.revents & POLLIN) == 0)
					break;
				one.store->drain_watch();
			}
		}
		one.store->incorporate_external_changes();
	}

	// ANOTHER TERMINAL, CLOSED AND REOPENED on the same directory. The instance
	// is destroyed - which is the only way to test that nothing was being held
	// in it - and a fresh one with a fresh `session_id` opens the same files.
	void restart(session_state& one, bool saving) {
		if (one.store) {
			_what = "restart session " + std::to_string(one.slot)
			        + (saving ? " after a save" : " without saving");
			if (saving) {
				EXPECT_TRUE(one.store->save()) << where();
				const on_disk saved = scan(_dir);
				for (const std::string& cmd : one.owed) {
					EXPECT_EQ(saved.everything.count(cmd), 1u)
						<< where() << ": " << printable(cmd) << " was not saved on the way out";
				}
				note_disk(saved);
			}
			// A SESSION THAT DIES HOLDING A PENDING ITEM LOSES IT, by design and
			// with no sentinel write - so those bytes stay forbidden for the
			// rest of the run rather than becoming an ordinary command line.
			one.store.reset();
		} else {
			_what = "open session " + std::to_string(one.slot);
		}

		one.pending.clear();
		one.ephemeral.clear();
		one.owed.clear();

		one.store = std::make_unique<history>();
		// #194's countdown off, and #196's cap on: the program decides when a
		// rewrite happens, and the cap is low enough that eviction is a thing
		// that actually occurs in a forty-step run.
		one.store->set_automatic_vacuum(false);
		one.store->set_vacuum_cap(_cap);
		const open_report report = one.store->open(_dir);
		ASSERT_FALSE(report.directory_unusable) << where();
		ASSERT_TRUE(report.log_writable) << where();
		EXPECT_FALSE(report.tier1_untouchable) << where();
		EXPECT_FALSE(report.tier1_corrupt) << where();
		EXPECT_EQ(report.log_discarded_bytes, 0u)
			<< where() << ": the log did not read back cleanly";

		const on_disk now = scan(_dir);
		one.log_at_load = now.log;
		one.blob_at_map = now.blob;
		one.reloads_seen = one.store->reloads();
		note_disk(now);
	}

	[[nodiscard]] std::int32_t exit_code() {
		static constexpr std::int32_t codes[] = {0, 0, 1, 2, 127, -1};
		return codes[pick(std::size(codes))];
	}

	// --- The model's one piece of bookkeeping --------------------------------

	// Everything on disk is durable, and stays durable until something with a
	// licence removes it.
	void note_disk(const on_disk& now) {
		_durable.insert(now.everything.begin(), now.everything.end());
	}

	// --- The invariants ------------------------------------------------------

	void check() {
		const on_disk now = scan(_dir);

		// The two files have to read back cleanly, before anything is concluded
		// from what they hold: nothing in this program damages either.
		EXPECT_EQ(now.log_discarded, 0u) << where() << ": the log did not read back cleanly";
		if (now.blob_present)
			EXPECT_EQ(now.blob_verdict, blob_status::ok) << where() << ": history.data";

		check_no_command_is_lost(now);
		check_the_blob_file(now);
		check_secrets_never_reach_the_disk(now);
		check_no_temp_files();

		for (session_state& one : _sessions) {
			// A WALK NEVER RE-MAPS - it is `const` and takes the view as it
			// stands - but the `incorporate`, the append or the vacuum in THIS
			// step may have, and the model's snapshot of what this store has
			// mapped has to follow. Nothing else in this single-threaded program
			// writes `history.data`, so the file as it is now IS what the
			// reload mapped.
			if (one.store->reloads() != one.reloads_seen) {
				one.reloads_seen = one.store->reloads();
				one.blob_at_map = now.blob;
			}
			// Anything this session owed the disk and the disk now has is
			// settled, whichever step put it there.
			for (auto at = one.owed.begin(); at != one.owed.end();)
				at = now.everything.count(*at) != 0 ? one.owed.erase(at) : std::next(at);
			check_one_walk(one);
		}

		_evicted_this_step = 0;
	}

	// (1) NO COMMITTED COMMAND IS EVER LOST, and the only licence to lose one is
	// the cap declaring it.
	void check_no_command_is_lost(const on_disk& now) {
		std::vector<std::string> lost;
		std::string named;
		for (const std::string& cmd : _durable) {
			if (now.everything.count(cmd) != 0)
				continue;
			lost.push_back(cmd);
			named += (named.empty() ? "" : ", ") + printable(cmd);
		}
		EXPECT_LE(lost.size(), _evicted_this_step)
			<< where() << ": " << named
			<< " left the disk, and the vacuums in this step admitted to evicting only "
			<< _evicted_this_step;
		for (const std::string& cmd : lost)
			_durable.erase(cmd);

		// AND A FRESH INSTANCE GETS ALL OF IT BACK. The one reader with no
		// memory: everything it yields came off the files, and everything on the
		// files it does not yield is a command the user cannot reach any more.
		history probe;
		probe.set_automatic_vacuum(false);
		const open_report report = probe.open(_dir);
		ASSERT_FALSE(report.directory_unusable) << where();
		const std::vector<std::string> seen = commands_of(walk(probe));
		for (const std::string& cmd : now.everything) {
			EXPECT_NE(std::find(seen.begin(), seen.end(), cmd), seen.end())
				<< where() << ": " << printable(cmd)
				<< " is on disk and a fresh instance cannot see it";
		}
		// (2), on the reader that has the most tiers in play.
		check_no_duplicates(seen, "a fresh instance");
	}

	// (5), and the format's own promise about the file.
	void check_the_blob_file(const on_disk& now) {
		EXPECT_LE(now.blob.size(), _cap)
			<< where() << ": history.data is over the cap";

		// NEWEST FIRST IS THE FORMAT (ADR-0010 §Tier 1). Asserted on the FILE
		// and not on a walk, because this is the one tier that is sorted rather
		// than concatenated, and it is the vacuum that sorts it.
		for (std::size_t at = 1; at < now.blob_when.size(); ++at) {
			EXPECT_LE(now.blob_when[at], now.blob_when[at - 1])
				<< where() << ": history.data is not newest-first at record " << at;
		}
		// And one record per command line, which is what the LRU is for.
		check_no_duplicates(now.blob, "history.data");
	}

	// (4) NO EPHEMERAL AND NO PENDING ITEM IS EVER ON DISK. Checked frame by
	// frame and record by record, because a walk is exactly the code that could
	// be hiding one.
	void check_secrets_never_reach_the_disk(const on_disk& now) {
		for (const std::string& cmd : now.everything) {
			EXPECT_EQ(_ephemeral_ever.count(cmd), 0u)
				<< where() << ": the ephemeral " << printable(cmd) << " reached the disk";
			EXPECT_EQ(_forbidden.count(cmd), 0u)
				<< where() << ": the unresolved " << printable(cmd) << " reached the disk";
		}
	}

	// (6). A crash leaves one and there is no fixing that from inside the
	// process that died (`vacuum.h` step 6 says so); nothing here crashes.
	void check_no_temp_files() {
		const std::vector<std::string> left = data_siblings(_dir);
		EXPECT_TRUE(left.empty())
			<< where() << ": " << (left.empty() ? std::string{} : left.front())
			<< " was left behind";
	}

	void check_no_duplicates(const std::vector<std::string>& seen, const char* whose) {
		std::set<std::string> once;
		for (const std::string& cmd : seen) {
			EXPECT_TRUE(once.insert(cmd).second)
				<< where() << ": " << whose << " yielded " << printable(cmd) << " twice";
		}
	}

	void check_one_walk(const session_state& one) {
		const std::vector<yielded> seen = walk(*one.store);
		const std::string whose = "session " + std::to_string(one.slot);

		check_no_duplicates(commands_of(seen), whose.c_str());

		for (const yielded& entry : seen) {
			// (4), the other half: a secret belongs to the session that typed
			// it and to no other, and a running command belongs to nobody yet.
			if (_ephemeral_ever.count(entry.cmd) != 0) {
				EXPECT_EQ(entry.cmd, one.ephemeral)
					<< where() << ": " << whose << " can see an ephemeral item that is not "
					<< "its own live one";
				EXPECT_EQ(entry.from, merged_entry::origin::session) << where();
			}
			EXPECT_EQ(_forbidden.count(entry.cmd), 0u)
				<< where() << ": " << whose << " can see the unresolved " << printable(entry.cmd);
		}

		check_order(one, seen);
	}

	// (3) THE ORDER, and the invariant here whose statement took an argument, so
	// it is written out.
	//
	// THE TICKET ASKED FOR "non-increasing `when`, own-before-foreign on ties",
	// AND THAT IS NOT WHAT ADR-0010 SPECIFIES - deliberately. §Read path says
	// the walk "merges `new_items` (resolved, newest first) then the mapped
	// vector", and `history.cpp` says out loud that a CONCATENATION and not a
	// merge sort is the shape: the tier order IS the tie-break, which is how
	// "on equal `when`, own `session_id` wins" is implemented without a
	// comparison anywhere. A concatenation of three descending runs is not
	// itself descending, and both places it can rise are ordinary rather than
	// exotic:
	//
	//   TWO TERMINALS. This one typed `foo` at 10:00 and has been idle since;
	//   the other typed `bar` at 10:05 and vacuumed. `foo` is in this session's
	//   `new_items` and `bar` is in the blob, so the walk yields `foo` then
	//   `bar` - own items first, always, which is fish's behaviour too and is
	//   the price of a per-keystroke walk that does no comparisons.
	//
	//   AND THE LOG IS IN APPEND ORDER, which is resolution order and not
	//   `when` order: a command that started at 10:00 and ran for a minute
	//   appends its frame after one that started at 10:01 and returned at once.
	//
	// So what is checked is the ADR's rule, in four parts, and each part is
	// tighter than the global statement would have been on that tier:
	//
	//   (a) the three tiers appear in order and never interleave;
	//   (b) the session tier is non-increasing in `when` and every entry in it
	//       carries THIS session's id;
	//   (c) the log tier is exactly the frames this store read, in reverse
	//       append order, minus what the dedup removed;
	//   (d) the blob tier is exactly the records it mapped, in file order, minus
	//       the same - and the file itself is sorted (`check_the_blob_file`);
	//   (e) and therefore the walk as a whole is non-increasing in `when` except
	//       at a tier boundary and inside the log tier, which is the ticket's
	//       statement with its two exceptions named rather than waived.
	//
	// Together with (2) they say the whole rule: an own item can never be
	// preceded by a foreign copy of itself, at equal `when` or any other.
	void check_order(const session_state& one, const std::vector<yielded>& seen) {
		const std::string whose = "session " + std::to_string(one.slot);

		// (a)
		for (std::size_t at = 1; at < seen.size(); ++at) {
			EXPECT_LE(rank_of(seen[at - 1].from), rank_of(seen[at].from))
				<< where() << ": " << whose << " yielded a " << name_of(seen[at].from)
				<< " entry before a " << name_of(seen[at - 1].from) << " one";
		}

		// (b)
		const yielded* previous = nullptr;
		for (const yielded& entry : seen) {
			if (entry.from != merged_entry::origin::session)
				continue;
			EXPECT_EQ(entry.session_id, one.store->session_id())
				<< where() << ": " << whose << " has a foreign item in its own tier";
			if (previous != nullptr) {
				EXPECT_LE(entry.when, previous->when)
					<< where() << ": " << whose << " yielded " << printable(entry.cmd)
					<< " after a strictly older own item";
			}
			previous = &entry;
		}

		// (c) and (d), as subsequences of what this store actually read.
		std::vector<std::string> from_log = one.log_at_load;
		std::reverse(from_log.begin(), from_log.end());
		check_subsequence(seen, merged_entry::origin::log, from_log, whose, "the log it read");
		check_subsequence(seen, merged_entry::origin::blob, one.blob_at_map, whose,
		                  "the blob it mapped");

		// (e) THE GLOBAL STATEMENT, which (a)-(d) already imply and which is
		// written out anyway because it is the form the ticket asked for and it
		// is the form a reader will look for: the walk is non-increasing in
		// `when` EXCEPT where the two paragraphs above say it may rise, and
		// nowhere else. Not a weaker check than a bare "non-increasing" would
		// have been - it is that check plus an enumeration of its exceptions.
		for (std::size_t at = 1; at < seen.size(); ++at) {
			if (seen[at].when <= seen[at - 1].when)
				continue;
			const bool crossed = seen[at].from != seen[at - 1].from;
			const bool in_the_log = seen[at].from == merged_entry::origin::log;
			EXPECT_TRUE(crossed || in_the_log)
				<< where() << ": " << whose << " yielded " << printable(seen[at].cmd)
				<< " after a strictly older entry inside the " << name_of(seen[at].from)
				<< " tier, which is sorted and may not rise";
		}
	}

	// Every entry of `tier` appears in `source`, in `source`'s order.
	void check_subsequence(const std::vector<yielded>& seen, merged_entry::origin tier,
	                       const std::vector<std::string>& source, const std::string& whose,
	                       const char* what) {
		std::size_t at = 0;
		for (const yielded& entry : seen) {
			if (entry.from != tier)
				continue;
			while (at < source.size() && source[at] != entry.cmd)
				++at;
			EXPECT_LT(at, source.size())
				<< where() << ": " << whose << " yielded " << printable(entry.cmd) << " from the "
				<< name_of(tier) << " tier, out of order against " << what;
			if (at >= source.size())
				return;
			++at;
		}
	}

	// --- State ---------------------------------------------------------------

	std::uint64_t _seed = 0;
	std::string _dir;
	std::size_t _cap = 0;
	std::mt19937_64 _rng;

	std::vector<session_state> _sessions;
	std::size_t _step = 0;
	std::string _what = "open";

	// The injected clock, in unix seconds (ADR-0010 §Tier 1's unit).
	std::uint64_t _clock = 1'700'000'000;
	// A counter behind every reserved command line, so that the ephemeral and
	// the never-resolved ones are bytes nothing else can produce.
	std::size_t _reserved = 0;

	// Every command line ever seen on disk, minus the ones the cap took.
	std::set<std::string> _durable;
	// Every leading-space command line the program has ever typed.
	std::set<std::string> _ephemeral_ever;
	// Command lines that must not be anywhere: a pending item while it is
	// pending, and forever if the session that held it died first.
	std::set<std::string> _forbidden;
	// Records the vacuum in THIS step admitted to evicting; the only licence
	// under which a durable command may leave the disk.
	std::size_t _evicted_this_step = 0;
};

// ---------------------------------------------------------------------------
// The fixture
// ---------------------------------------------------------------------------

// 32 programs of 32 steps in the gate, which is what keeps this in the low
// seconds under ASan/UBSan. `LESH_HISTORY_PROPERTY_SEEDS` widens it.
constexpr std::uint64_t k_gate_seeds = 32;
constexpr std::uint64_t k_gate_crash_seeds = 4;
constexpr std::size_t k_steps = 32;

[[nodiscard]] std::uint64_t number_from(const char* variable, std::uint64_t fallback) {
	const char* wanted = std::getenv(variable);
	if (wanted == nullptr || wanted[0] == '\0')
		return fallback;
	const long long parsed = std::atoll(wanted);
	return parsed > 0 ? static_cast<std::uint64_t>(parsed) : fallback;
}

// The seeds this run covers. `LESH_HISTORY_PROPERTY_SEEDS=n` widens the sweep;
// `LESH_HISTORY_PROPERTY_SEED=n` runs the ONE seed a failure named, which is
// the whole point of printing it.
[[nodiscard]] std::uint64_t first_seed() {
	return number_from("LESH_HISTORY_PROPERTY_SEED", 1);
}

[[nodiscard]] std::uint64_t last_seed() {
	if (std::getenv("LESH_HISTORY_PROPERTY_SEED") != nullptr)
		return first_seed();
	return number_from("LESH_HISTORY_PROPERTY_SEEDS", k_gate_seeds);
}

// The crash test's own count, and a separate knob because its programs cost a
// FORK EACH at seven steps apiece: four of them is a second, and the number
// that widens the sweep above by a hundred would widen this one into a minute.
[[nodiscard]] std::uint64_t crash_seeds() {
	return number_from("LESH_HISTORY_PROPERTY_CRASH_SEEDS", k_gate_crash_seeds);
}

class property_fixture : public ::testing::Test {
protected:
	void SetUp() override { test_hooks::reset_locking_state(); }
	void TearDown() override {
		test_hooks::clear_now_override();
		test_hooks::reset_locking_state();
	}
};

class UiHistoryProperty : public property_fixture {};
class UiHistoryPropertyCrash : public property_fixture {};

} // namespace

// ===========================================================================
// The program
// ===========================================================================

TEST_F(UiHistoryProperty, ARandomProgramOverOneDirectoryKeepsEveryInvariant) {
	for (std::uint64_t seed = first_seed(); seed <= last_seed(); ++seed) {
		SCOPED_TRACE("reproduce with LESH_HISTORY_PROPERTY_SEED=" + std::to_string(seed));
		const lesh::testing::temp_path scratch;
		std::mt19937_64 shape{seed};
		const std::size_t sessions = 2 + shape() % 3;
		// SMALL ENOUGH THAT THE CAP ACTUALLY EVICTS on some of the seeds and
		// never on others: the pool is eight command lines, so a cap of four to
		// eleven puts the eviction path under the same program as everything
		// else rather than in a test of its own.
		const std::size_t cap = 4 + shape() % 8;

		program running{seed, scratch.dir(), sessions, cap};
		running.run(k_steps);
		// ONE DIAGNOSIS IS ENOUGH. A broken merge fails every seed, and 32
		// copies of the same failure is 32 times as hard to read as one.
		if (HasFailure())
			return;
	}
}

TEST_F(UiHistoryProperty, TheCapIsReachedAndTheEvictionPathRuns) {
	// The seeds above are worth nothing if the cap never bites, and "it does"
	// is not something a random program can promise. This one makes it certain:
	// more distinct command lines than the cap, one vacuum, and the file has to
	// come out at the cap exactly.
	const lesh::testing::temp_path scratch;
	test_hooks::set_now_override(1'700'000'000);

	history store;
	store.set_automatic_vacuum(false);
	store.set_vacuum_cap(4);
	ASSERT_TRUE(store.open(scratch.dir()).log_writable);
	for (int at = 0; at < 10; ++at) {
		test_hooks::set_now_override(1'700'000'000 + static_cast<std::uint64_t>(at));
		ASSERT_NE(store.add("command " + std::to_string(at), "/tmp"), add_status::rejected);
		store.resolve_pending(0);
	}

	const vacuum_result done = store.vacuum_now();
	ASSERT_EQ(done.status, vacuum_status::renamed);
	EXPECT_EQ(done.records_written, 4u);
	EXPECT_EQ(done.evicted, 6u);

	const on_disk now = scan(scratch.dir());
	EXPECT_EQ(now.blob.size(), 4u);
	// The cap evicts the LEAST RECENTLY SEEN, and the sources are fed oldest
	// first, so what survives is the newest four.
	EXPECT_EQ(now.blob, (std::vector<std::string>{"command 9", "command 8", "command 7",
	                                              "command 6"}));
}

// ===========================================================================
// The program, and then a crash in the middle of a vacuum
// ===========================================================================

namespace {

constexpr vacuum_step k_all_steps[] = {
	vacuum_step::target_opened,   vacuum_step::temp_built,
	vacuum_step::target_locked,   vacuum_step::ownership_copied,
	vacuum_step::renamed,         vacuum_step::temp_unlinked,
	vacuum_step::published,
};

// A whole vacuum in a FORKED CHILD that dies at `stop_at`.
//
// A fork and `_exit` and not an exception, for #194's reason: an exception
// unwinds, and a power cut does not. `_exit` also skips `atexit`, so the
// sanitizers do not report the child's live allocations as leaks.
void crash_at(const std::string& dir, vacuum_step stop_at, std::uint64_t clock) {
	const ::pid_t child = ::fork();
	ASSERT_GE(child, 0) << "fork failed: " << std::strerror(errno);
	if (child == 0) {
		test_hooks::set_now_override(clock);
		history store;
		store.set_automatic_vacuum(false);
		(void)store.open(dir);
		(void)store.add("the dying session's command", "/tmp", /*pending=*/false);
		(void)store.save();
		store.set_vacuum_hook([stop_at](vacuum_step step) {
			if (step == stop_at)
				::_exit(70);
		});
		(void)store.vacuum_now();
		::_exit(0);
	}
	int status = 0;
	ASSERT_EQ(::waitpid(child, &status, 0), child);
	ASSERT_TRUE(WIFEXITED(status)) << "the child died of a signal, not of the hook";
	EXPECT_EQ(WEXITSTATUS(status), 70) << "the hook did not fire";
}

} // namespace

TEST_F(UiHistoryPropertyCrash, ARandomProgramSurvivesAKillAtEveryVacuumStep) {
	// Invariant (1) alone, against the wreckage a random program plus a death
	// mid-rewrite leaves. Duplicates are allowed - they are the price of never
	// holding the lock across the expensive part - and a leftover temp is
	// allowed, because a crash between steps 1 and 5 leaves one and no code in
	// the process that died can remove it. An ABSENCE is allowed at no step.
	//
	// THE CAP IS THE REAL ONE HERE, unlike the program above: eviction and loss
	// look identical from the outside, and this test is about the second.
	for (const vacuum_step step : k_all_steps) {
		for (std::uint64_t seed = 1; seed <= crash_seeds(); ++seed) {
			SCOPED_TRACE("crash at step " + std::to_string(static_cast<int>(step)) + ", seed "
			             + std::to_string(seed));
			const lesh::testing::temp_path scratch;
			{
				program running{seed, scratch.dir(), 2, k_history_save_max};
				running.run(12);
			}
			test_hooks::clear_now_override();
			if (HasFailure())
				return;

			const std::set<std::string> before = scan(scratch.dir()).everything;
			crash_at(scratch.dir(), step, 1'800'000'000);
			if (HasFatalFailure())
				return;

			history reopened;
			reopened.set_automatic_vacuum(false);
			const open_report report = reopened.open(scratch.dir());
			EXPECT_FALSE(report.tier1_untouchable) << "step " << static_cast<int>(step);
			EXPECT_FALSE(report.tier1_corrupt) << "step " << static_cast<int>(step);

			const std::vector<std::string> seen = commands_of(walk(reopened));
			for (const std::string& wanted : before) {
				EXPECT_NE(std::find(seen.begin(), seen.end(), wanted), seen.end())
					<< "a crash after step " << static_cast<int>(step) << " lost "
					<< printable(wanted);
			}
			// The child saved before it vacuumed, so its own command was on disk
			// before the step under test and is owed back too.
			EXPECT_NE(std::find(seen.begin(), seen.end(), "the dying session's command"),
			          seen.end())
				<< "a crash after step " << static_cast<int>(step)
				<< " lost the command the dying session had already saved";
			// And one more rewrite settles it: whatever duplicates the crash
			// left, the next vacuum collapses them.
			ASSERT_EQ(reopened.vacuum_now().status, vacuum_status::renamed);
			check_no_duplicate_records(scratch.dir());
			if (HasFailure())
				return;
		}
	}
}
