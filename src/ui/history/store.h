#pragma once

// THE HISTORY ITSELF (#193, ADR-0010 §In memory, §Recording, §Read path): the
// object the running shell holds, sitting on top of Tier 1 (`blob.h`, #191) and
// Tier 2 (`log.h`, #192) and implementing the read seam the searcher and the
// autosuggester already speak.
//
// WHAT IT IS, IN ONE SENTENCE. `add` before a command runs, `resolve_pending`
// after it finishes, `for_each_newest_first` from anywhere - and the third of
// those runs on a keystroke, on a worker, while the first two are running on
// the loop thread.
//
// THAT IS THE WHOLE DESIGN PROBLEM, and ADR-0009 answers it with SNAPSHOT
// VIEWS. A mutation never edits anything a reader can see: it builds a fresh
// immutable `view` - the resolved items of this session, the items loaded from
// the log at open, and a refcounted handle on the mapping - and swaps a
// `shared_ptr` to it. A walk takes the current view ONCE, at the top, and is
// then reading a graph nothing will ever touch again. A view that a worker is
// still walking keeps its mapping alive by holding the handle, so the mapping
// cannot be pulled out from under it even by a reload; the worst that can
// happen to a walk is that it reports a history one command out of date, which
// is what a snapshot IS.
//
// NO LOCK ANYWHERE IN HERE, and that is not bravado - it is the consequence of
// the above. The only shared mutable state between the two threads is one
// `shared_ptr`, read with `std::atomic_load` and written with
// `std::atomic_store`. `new_items`, the write cursor, the appender and the
// warning latch are LOOP-THREAD-ONLY and are never read by a walk.
//
// THE WALK ALLOCATES NOTHING once it is warm, because it is the autosuggest
// path and `UiAutosuggest`'s zero-heap tests are the gate on it. Two things
// could have allocated and neither does: the view is taken by refcount rather
// than copied, and the per-walk dedup set is a generation-stamped open-address
// table kept in thread-local storage, so the second walk on a thread reuses the
// first walk's buckets and clears them by bumping an integer. `scratch_growths`
// below is the instrument that says so out loud.
//
// STALENESS IS HERE NOW (#195, ADR-0010 §Locking and staleness), and it is
// three facts and one flag. The APPEND takes an exclusive lock and checks, in
// the same breath, whether the `history.data` it has mapped is still the one on
// disk. The DIRECTORY WATCH answers the same question for a session that never
// appends - fish #3565, a tab left open all day that keeps serving a mapping of
// a file three vacuums ago. A REMOTE data directory turns both the lock and the
// `mmap` off and reads Tier 1 into a heap buffer instead (fish PR #5097). All
// three converge on `_reload_needed`, which the next `publish()` consumes by
// re-mapping. NOTHING IS CHECKED PER READ: the walk is what runs on a keystroke,
// and a `stat` on that path is the syscall this whole design removed.
//
// THE VACUUM IS HERE NOW TOO (#194, ADR-0010 §Vacuum): the LRU dedup, the
// 256 Ki cap and the temp-and-`rename` live in `vacuum.h`, and this class owns
// the countdown, the post-rename bookkeeping and `may_rewrite_tier1()`, the
// policy hook the rewrite asks before it writes. `save()` still only flushes -
// an exiting shell does not rewrite the file.
//
// WHAT IS NOT HERE, AND WHOSE IT IS. `remove`, `clear`, `clear_session`, the
// deletion set and a `history` builtin are phase 2 (ADR-0010 §Phase 2).
//
// ADR-0007: the mapping and the log descriptor are the two things that are not
// self-freeing, and the destructor closes both. Everything else is a standard
// container or a `shared_ptr`.

#include "ui/history/blob.h"
#include "ui/history/locking.h"
#include "ui/history/log.h"
#include "ui/history/vacuum.h"
#include "ui/history/watch.h"
#include "ui/history_search.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lesh::ui::history {

// ---------------------------------------------------------------------------
// What an item is
// ---------------------------------------------------------------------------

// Whether an item is written, remembered, or forgotten (ADR-0010 §In memory).
enum class persist_mode : std::uint8_t {
	// The ordinary one: this item reaches the log at `resolve_pending`.
	disk,
	// Remembered for this session and never written. NOTHING SELECTS THIS YET -
	// it is the mode a future `history --memory-only`, or an `HISTCONTROL`-shaped
	// option, sets - and it is here rather than added later because the write
	// path has to be written once against all three or it will be written twice.
	memory,
	// LEADING SPACE (fish's rule, unconditionally): retrievable until the next
	// add, never written. The privacy rule is a rule and not a preference in
	// v1; the option to switch it off is phase 2, and the whole of what that
	// option will do is choose this value or `disk` at `add`.
	ephemeral,
};

// One entry as THIS SESSION holds it: the bytes are OWNED, which is the whole
// difference from `record` (`blob.h`), whose bytes are borrowed from a mapping.
// A session's own items outlive any one view of them, so they cannot borrow.
struct item {
	// The command line, raw - newlines, NULs, invalid UTF-8 and all (F-34).
	std::string cmd;
	// The LOGICAL `$PWD` at add time - the shell's variable, not `getcwd`, so a
	// path reached through a symlink reads back the way the user typed it.
	std::string cwd;
	std::uint64_t when = 0;
	// Filled by `resolve_pending`, which is the only thing that knows it.
	std::int32_t exit_code = 0;
	std::uint64_t session_id = 0;
	persist_mode mode = persist_mode::disk;
	// Added, running, not yet finished: EXCLUDED FROM READS until
	// `resolve_pending`. A session that dies holding one loses it, by design -
	// there is no sentinel write (ADR-0010 §Recording).
	bool pending = false;
};

// How `add` ended.
enum class add_status : std::uint8_t {
	// A new item is at the back of `new_items`.
	added,
	// It was the same command line as the item already at the back, so that
	// item took the newer timestamp instead (fish `history_item_t::merge`) and
	// the write cursor rewound to it so the newer timestamp reaches the log.
	merged,
	// Empty or whitespace-only. Never added, never written, and not an error -
	// no shell records a blank line (fish #6032).
	rejected,
};

// What `open` found. Every field is a degradation this class RUNS THROUGH
// rather than an error it refuses on: a shell whose history file is unreadable
// is a shell with no history, never a shell that will not start.
struct open_report {
	// `history.data` mapped and verified. False on a first run, which is the
	// ordinary state of a shell that has never vacuumed.
	bool tier1_mapped = false;
	// `history.data` exists and MUST NEVER BE WRITTEN: an unknown
	// `file_identifier`, a future lesh's file or somebody else's entirely. The
	// session runs on Tier 2 plus memory and warns once; the vacuum asks
	// `may_rewrite_tier1()` and refuses.
	bool tier1_untouchable = false;
	// `history.data` is OURS and the Verifier rejected it. Not untouchable:
	// #194 decided that such a file is rebuilt, after being renamed aside to
	// `history.data.corrupt-<unix seconds>` so nothing is destroyed
	// (`vacuum.h` carries the argument). Until the next vacuum the session runs
	// on Tier 2 plus memory, exactly as it does for an untouchable one.
	bool tier1_corrupt = false;
	// The append log is open for writing. False costs this session's commands
	// their place on disk and nothing else.
	bool log_writable = false;
	// Frames the log yielded at open, and the bytes it could not account for -
	// `log_scan`'s counters, forwarded so a caller (and a test) can say
	// something precise about a damaged log.
	std::size_t log_frames = 0;
	std::size_t log_discarded_bytes = 0;
	// The data directory could not be created or reached. The session runs on
	// memory alone: reads work, `save()` writes nothing.
	bool directory_unusable = false;
	// The data directory is on a network filesystem (#195). Tier 1 is READ
	// rather than mapped and nothing in this process locks; everything a caller
	// can see is otherwise identical.
	bool directory_remote = false;
	// The directory watch is armed, so a vacuum in another terminal reaches this
	// session with no write on its side. False costs fish #3565's freshness and
	// nothing else - an appending session still notices at its next append.
	bool watching = false;
};

// ---------------------------------------------------------------------------
// The merge walk's yield
// ---------------------------------------------------------------------------

// One entry as the merge walk hands it over: a `record`, whose bytes are
// borrowed under `blob.h`'s rule, plus the one thing the walk knows and the
// record does not.
//
// THE TIER IS NOT DECORATION. "On equal `when`, own `session_id` wins"
// (ADR-0010 §Read path) is decided by the ORDER the three tiers are walked in,
// and a test that cannot see which tier an entry came from cannot tell that
// rule from a coincidence. #194's vacuum wants the same distinction for a
// different reason: this session's unwritten items have to be merged in, and
// the log's have to be dropped once they are in the blob.
struct merged_entry {
	record what;

	enum class origin : std::uint8_t {
		// This session's `new_items`.
		session,
		// A frame the append log gave up at open.
		log,
		// A record in the mapped `history.data`.
		blob,
	};

	origin from = origin::session;
};

// ---------------------------------------------------------------------------
// The history
// ---------------------------------------------------------------------------

class store final : public history_source {
public:
	// A history with no files behind it: reads and records in memory, writes
	// nothing, warns about nothing. This is what `vared` and every unit test
	// that must not touch the developer's own history get, and it is also the
	// state a shell runs in when `open` could not reach a data directory - one
	// object, one code path, no null checks at the call sites.
	store();
	~store() override;

	store(const store&) = delete;
	store& operator=(const store&) = delete;

	// Where the files live (ADR-0010 §Placement): `$XDG_DATA_HOME/lesh`, or
	// `~/.local/share/lesh`. `nullopt` when neither is usable, which is the
	// signal to build no store at all (#101's rule for `~/.lesh_history`, kept).
	//
	// A RELATIVE `$XDG_DATA_HOME` IS IGNORED, per the XDG basedir spec, rather
	// than resolved against the cwd: a history file that moves with `cd` is not
	// a history file.
	[[nodiscard]] static std::optional<std::string> default_data_directory();

	// Creates `directory` (0700, parents 0755), maps `history.data`, reads
	// `history.new.log` into memory and opens it for append.
	//
	// NEVER FAILS in the sense that matters: every way this can go wrong is a
	// field of `open_report` and a session that runs with less. Call it once,
	// before the editor takes the terminal - the one warning it can print goes
	// to stderr, and #98 forbids that over a live edit line.
	open_report open(const std::string& directory);

	// --- Recording (ADR-0010 §Recording), LOOP THREAD ONLY -------------------

	// Records `cmd`, run from `cwd`, as the newest item.
	//
	// `pending` is how `session::execute` says "this is about to run": the item
	// is in `new_items` but invisible to every read and unwritten until
	// `resolve_pending` supplies its exit status. False records a finished item
	// directly, which is what a test wants and what an importer would want.
	//
	// The three rules, all of them fish's and all of them here rather than at
	// the call site, because a second call site would otherwise get one wrong:
	// empty and whitespace-only are `rejected`; a leading space is `ephemeral`;
	// the same command line twice in a row merges into one item with the later
	// timestamp.
	add_status add(std::string_view cmd, std::string_view cwd, bool pending = true);

	// Finishes the pending item: its exit status, its visibility, and - for a
	// `disk` item - its frame in the log. THE APPEND HAPPENS HERE and nowhere
	// else on the ordinary path, which is what makes an exit code something the
	// file can hold at all.
	//
	// Nothing pending is not an error: a cancelled line, or a second call, does
	// nothing.
	void resolve_pending(std::int32_t exit_code);

	// Flushes every unwritten resolved item to the log. Called on interactive
	// exit.
	//
	// `false` MEANS THE SESSION IS NOT WHOLLY ON DISK, not that this call
	// failed: an item dropped three commands ago (no log, or a failed write) is
	// just as absent from the file as one dropped now, and the question a
	// caller asks on the way out is the first one. `unwritable_items()` counts
	// them.
	//
	// NO VACUUM. `save()` never rewrites `history.data` - see the seam below.
	bool save();

	// --- Staleness (#195, ADR-0010 §Locking and staleness) -------------------

	// The descriptor the ui loop's `watch` topic polls, or -1 when this history
	// is watching nothing (memory-only, or a directory that would not give one
	// out). BORROWED: the loop never closes it, and this object outlives the
	// attachment.
	[[nodiscard]] int watch_fd() const noexcept { return _watch.fd(); }

	// WHAT THE LOOP RUNS WHEN THAT DESCRIPTOR IS READABLE. Consumes the
	// notification, re-`stat`s `history.data`, and - if it is not the file this
	// history has mapped - reloads and publishes, so the very next walk sees the
	// other terminal's commands.
	//
	// THE LOOP'S, like every other mutation (ADR-0009). It cannot race `add` or
	// `resolve_pending`: those run inside `shell_side::execute`, which since #201
	// is a call the loop makes - so the loop is in that call for the whole of it
	// and polls nothing while it is (it was blocked in `wait_on_shell` on the
	// `shell` and `signal` topics before, which is the same window).
	//
	// A SPURIOUS WAKE IS THE COMMON CASE and costs one `stat`: the watch is on
	// the DIRECTORY (it has to be - a `rename` over a file never fires on that
	// file), so a sibling shell creating its vacuum temp file wakes every
	// terminal on the machine, and only the `rename` that follows changes the id.
	void drain_watch();

	// Whether something has told this history that its Tier 1 mapping is out of
	// date. Cleared by the reload the next `publish()` performs.
	[[nodiscard]] bool reload_needed() const noexcept { return _reload_needed; }

	// Re-maps, re-verifies and re-caches now, if anything is pending; a no-op
	// otherwise. `drain_watch` calls it, and so may a caller that wants the
	// freshest possible view at a chosen moment. LOOP THREAD.
	void incorporate_external_changes();

	// Times Tier 1 has been re-mapped after the initial `open`. A test
	// instrument, and the one that tells "the reload happened" from "the records
	// were there all along".
	[[nodiscard]] std::size_t reloads() const noexcept { return _reloads; }

	// Whether Tier 1 is a heap buffer rather than a mapping, because the data
	// directory is remote (#195, fish PR #5097). Nothing above this line
	// branches on it; it is here so a test and a log line can say which path ran.
	[[nodiscard]] bool tier1_copied() const noexcept { return _remote; }

	// --- The vacuum (#194, ADR-0010 §Vacuum), LOOP THREAD ONLY ---------------

	// Rewrites `history.data` NOW, whatever the countdown says, and does the
	// bookkeeping a successful rewrite earns: the log is truncated, the write
	// cursor rewinds, the items that are now in the blob leave `new_items`, and
	// the new file is mapped and published in one step.
	//
	// PUBLIC BECAUSE THE TESTS DRIVE IT, and because the countdown is the only
	// thing between this and the ordinary path - twenty-five `resolve_pending`
	// calls do exactly this, and a suite that had to make twenty-five of them
	// per assertion would be a suite nobody reads.
	vacuum_result vacuum_now();

	// Vacuums attempted this session, whatever they answered.
	[[nodiscard]] std::size_t vacuums() const noexcept { return _vacuums; }

	// Turns the periodic vacuum off, leaving `vacuum_now` available (fish's
	// `history_t::disable_automatic_saving`).
	//
	// THE COUNTDOWN STARTS AT A RANDOM VALUE, which is a correctness property
	// - a shell used for twenty commands and closed must still eventually
	// vacuum - and a menace to anyone asserting about the files: one command
	// in twenty-five triggers a rewrite, so a test that says "the log now
	// holds this frame" is right twenty-four times and then is not. Every
	// suite that is about the recording path rather than the rewrite turns
	// this off; #194's own suite drives `vacuum_now` directly.
	void set_automatic_vacuum(bool enabled) noexcept { _automatic_vacuum = enabled; }

	// TEST-ONLY CRASH INJECTION. Called after each of ADR-0010 §Vacuum's steps
	// (and after this class's own post-rename bookkeeping, `published`), so a
	// test can `_exit` a forked child between any two of them and then assert
	// on what a fresh `history` over the same directory can still see. Null in
	// every shipping call; the hook is copied into each `vacuum_request`.
	void set_vacuum_hook(std::function<void(vacuum_step)> hook);

	// TEST-ONLY (#196). The `vacuum_request::cap` this history's rewrites carry,
	// which `vacuum.h` already documents as a knob that exists so a test can
	// reach the edge: honestly filling `k_history_save_max` is a 256 Ki-record,
	// ~25 MB rewrite - a benchmark, not a unit test - and the eviction path is
	// the one part of the merge that only runs there. Defaults to the ADR's
	// constant and no shipping call passes anything.
	void set_vacuum_cap(std::size_t cap) noexcept { _vacuum_cap = cap; }

	// --- The read seam (#125's `history_source`) -----------------------------

	// The merge walk, newest first, deduplicated on `cmd` bytes: this session's
	// resolved items, then the log's, then the mapping's. Safe from any thread,
	// at any time, including while the loop thread is recording.
	void for_each_newest_first(
		const std::function<bool(std::string_view)>& fn) const override;

	// The same walk with the whole entry, for the callers that need more than
	// the text: #194's vacuum, and the tests that have to see WHICH tier won a
	// tie. The override above is this walk with everything but `cmd` dropped.
	void for_each_merged_newest_first(
		const std::function<bool(const merged_entry&)>& fn) const;

	// --- What the session and #194 ask -------------------------------------

	// Low 64 bits of this session's uuidv7 (ADR-0010 §Tier 1). It exists for the
	// dedup tie-break and for nothing else; nobody looks a session up by it.
	[[nodiscard]] std::uint64_t session_id() const noexcept { return _session_id; }

	// THE VACUUM SEAM, and the decision #194 made behind it.
	//
	// False ONLY for an unknown `file_identifier`: those bytes are not ours,
	// ADR-0010 says such a file is never destroyed, and `rename`-ing a rebuilt
	// blob over it would destroy it.
	//
	// TRUE FOR A `corrupt` ONE - ours, and rejected by the Verifier - which is
	// the question #193 left open and #194 closed. Refusing forever would let
	// one flipped byte permanently disable Tier 1: the log would grow without
	// bound and the condition would never clear, because nothing but a vacuum
	// ever writes `history.data`. So it is rebuilt, and the broken file is
	// renamed aside first so the decision stays reversible. `vacuum.h` carries
	// the full argument, and the vacuum re-derives the answer from the file's
	// CURRENT bytes as well - this is the early-out.
	//
	// A RELOAD RE-ASKS IT (#195), and that is the only way it ever changes
	// without a vacuum of our own. The answer used to be "settled at `open`",
	// which was true when `open` was the only thing that mapped Tier 1. It is a
	// statement about the bytes currently mapped, and a vacuum in another
	// terminal replaces those bytes: a foreign file that some other lesh has
	// since renamed ours over is a file this session may rewrite, and a good
	// file replaced by a future format's is not. `map_tier1` re-derives both
	// this flag and `_tier1_corrupt` on every remap, which is what keeps the two
	// in step.
	[[nodiscard]] bool may_rewrite_tier1() const noexcept { return !_tier1_untouchable; }

	// Items this session recorded and could not write - no log, or a failed
	// write. Zero for a healthy session, and the counter a caller reports from.
	[[nodiscard]] std::size_t unwritable_items() const noexcept { return _unwritable; }

	// Items recorded this session, pending and ephemeral ones included.
	[[nodiscard]] std::size_t session_items() const noexcept { return _new_items.size(); }

	// Warnings printed to stderr. At most one, ever - the latch is the point.
	[[nodiscard]] std::size_t warnings() const noexcept { return _warnings; }

	// TIMES THE WALK'S THREAD-LOCAL DEDUP TABLE HAD TO GROW, process-wide.
	//
	// A test instrument, and the honest one for this class: the arena counter
	// `UiAutosuggest`'s zero-heap tests read sees only the arena's malloc
	// fallback, so it would not notice a `std::vector` growing inside the walk.
	// This would. It costs one relaxed increment on a path that runs at most
	// log2(history size) times per thread per process.
	[[nodiscard]] static std::size_t scratch_growths() noexcept;

private:
	// ONE IMMUTABLE SNAPSHOT (ADR-0009). Built on the loop thread by `publish`,
	// read by any number of walks, never modified after the swap.
	struct view {
		// This session's RESOLVED items, newest first. Shared pointers and not
		// copies: an item is immutable once it is in here (a mutation replaces
		// the pointer), so a view costs one pointer per item to build rather
		// than two string copies, and #194's trim of `new_items` will make even
		// that vanish.
		std::vector<std::shared_ptr<const item>> own;
		// The log's frames as they were at `open`, newest first. Shared between
		// views because only a reload changes them, and a reload is #195.
		std::shared_ptr<const std::vector<item>> logged;
		// The mapping, refcounted: a view outliving a reload keeps the pages it
		// points into alive. Null when there is no Tier 1.
		std::shared_ptr<const mapped_blob> blob;
	};

	[[nodiscard]] std::shared_ptr<const view> snapshot() const noexcept;
	// Freezes the current state into a new view and swaps it in. LOOP THREAD.
	//
	// CONSUMES `_reload_needed` FIRST (ADR-0010: "`reload_needed` -> next view
	// build re-maps, re-verifies, re-caches the id"). Here rather than at the
	// three places that set the flag, so that every route to a stale mapping -
	// the watch, the append-path id check, and whatever #196 adds - is served by
	// one implementation and cannot forget.
	void publish();
	// Writes resolved, unwritten `disk` items to the log, advancing the cursor.
	bool flush();
	// Reads `history.new.log` whole into `_logged`, newest first.
	void load_log();
	// The countdown (ADR-0010 §Vacuum), and `vacuum_now` when it reaches zero.
	void maybe_vacuum();
	// One line on stderr, once per process lifetime of this object.
	void warn_once(const char* what, const char* consequence);

	// --- Tier 1's mapping, and re-establishing it ----------------------------

	// Opens `history.data`, maps it (or READS it, when the directory is remote),
	// classifies the outcome, latches the two policy flags, warns at most once
	// and caches its `file_id_t`. `open`, the remap a vacuum earns and the
	// reload all go through here, which is what keeps `_tier1_untouchable`,
	// `_tier1_corrupt` and `_data_id` in step with `_blob` by construction.
	blob_status map_tier1();
	// `map_tier1` plus the bookkeeping a RE-map needs: the flag cleared and the
	// counter bumped. Called by `publish` and by nothing else.
	void reload_tier1();

	// The append path's descriptor (ADR-0010: "On every append: open, lock,
	// verify `file_id_for_path == file_id_for_fd`; retry up to 1024").
	//
	// -1 when it could not be had, which is when `flush` falls back to the
	// session's own `_log_fd`. The returned descriptor is LOCKED and the caller
	// closes it, which is also what releases the lock.
	[[nodiscard]] int open_locked_log();
	// One `stat` of `history.data`, compared with the cached id: the other half
	// of the append-path check, and the thing that makes an appending session
	// notice a sibling's vacuum without a watch.
	void note_tier1_identity();

	// --- Loop-thread state; a walk never touches any of it -------------------

	// Newest at the BACK, which is where `add` merges and where the pending
	// item sits. A deque and not a vector because #194's vacuum drops the
	// written prefix from the front.
	std::deque<std::shared_ptr<const item>> _new_items;
	// The write cursor: everything below it has been offered to the log.
	std::size_t _first_unwritten = 0;
	std::shared_ptr<const std::vector<item>> _logged;
	// `load_log`'s counters, forwarded into the `open_report`.
	std::size_t _log_frames = 0;
	std::size_t _log_discarded_bytes = 0;
	std::shared_ptr<const mapped_blob> _blob;
	log_appender _appender;

	std::string _data_path;
	std::string _log_path;
	// `O_WRONLY|O_APPEND|O_CREAT|O_CLOEXEC`, held for the session. CLOEXEC
	// because this is a shell and every command line forks: a child has no
	// business inheriting a writable descriptor onto the user's history.
	int _log_fd = -1;

	// --- Staleness, all loop-thread ------------------------------------------

	// The identity of the `history.data` whose bytes `_blob` holds, taken BEFORE
	// the open rather than after it. The order is deliberate: a vacuum landing
	// between the `stat` and the `open` then leaves a cached id that is one
	// generation OLD, so the next check says "changed" and re-maps a file that
	// was already current - a wasted remap. The other order would leave the id
	// one generation NEW against an old mapping, and the next check would say
	// "unchanged" forever. One of those failure modes is free and the other is
	// fish #3565 again.
	file_id_t _data_id = k_invalid_file_id;
	// Set by the watch drain, by the append path and by a successful vacuum;
	// consumed by `publish` (the vacuum's own remap clears it in the same call).
	bool _reload_needed = false;
	std::size_t _reloads = 0;
	// The data directory is on a network filesystem: no locks, no `mmap`.
	// Settled at `open` and never re-asked - a directory does not change
	// filesystems under a running shell.
	bool _remote = false;
	directory_watch _watch;

	std::uint64_t _session_id = 0;
	bool _tier1_untouchable = false;
	bool _tier1_corrupt = false;
	bool _warned = false;
	std::size_t _warnings = 0;
	std::size_t _unwritable = 0;

	// --- The vacuum ----------------------------------------------------------

	// Appends left before the next rewrite. NEGATIVE MEANS UNCHOSEN: the first
	// append picks a random value in `[0, k_vacuum_frequency)`, so that a shell
	// closed after twenty commands still eventually vacuums (fish
	// `save_unless_disabled`).
	int _vacuum_countdown = -1;
	std::size_t _vacuums = 0;
	bool _automatic_vacuum = true;
	// Test-only, and empty in every shipping build - see `set_vacuum_hook`.
	std::function<void(vacuum_step)> _vacuum_hook;
	// Test-only, and `k_history_save_max` in every shipping build - see
	// `set_vacuum_cap`.
	std::size_t _vacuum_cap = k_history_save_max;

	// --- The one thing both threads touch ------------------------------------

	// `std::atomic_load`/`std::atomic_store`, not `std::atomic<shared_ptr>`:
	// libc++ does not implement the C++20 specialization (it static_asserts on
	// trivial copyability), and the free functions are ADR-0009's other named
	// option. Never null after construction.
	std::shared_ptr<const view> _view;
};

// ---------------------------------------------------------------------------
// The clock, for tests only (#196)
// ---------------------------------------------------------------------------

// `locking.h` opens this namespace too; both halves of it are the same promise
// - nothing outside `tests/unit/` calls any of it.
namespace test_hooks {

// Makes `add` stamp `when` with `seconds` instead of reading the clock.
//
// THE PROPERTY TESTS NEED CONTROLLED TIME AND NOT FAST TIME. `when` is unix
// SECONDS (ADR-0010 §Tier 1), so a test that let the clock run would stamp
// every item in a run with the same value and could never tell "the tie-break
// is the tier order" from "nothing was ever compared"; one that could only
// advance the clock could never produce a tie at all. A number the test moves
// by nought or one per command gives it both, which is also what a real second
// of resolution gives a real user typing at a real prompt.
//
// PROCESS-WIDE, like every other hook in this namespace, and it stays set until
// `clear_now_override()`: a fixture's `TearDown` has to clear it or the next
// suite records its history in 1970.
void set_now_override(std::uint64_t seconds) noexcept;
void clear_now_override() noexcept;

} // namespace test_hooks

} // namespace lesh::ui::history
