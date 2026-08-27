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
// WHAT IS NOT HERE, AND WHOSE IT IS. The VACUUM - the LRU dedup, the 256 Ki
// cap, the temp-and-`rename` - is #194; `save()` flushes and returns, and
// `may_rewrite_tier1()` is the policy hook it will ask before it writes.
// STALENESS - `file_id_t`, the directory watch topic, the remote-filesystem
// heap fallback, `flock` - is #195; nothing here locks and nothing here
// re-checks the file it mapped. `remove`, `clear` and a `history` builtin are
// phase 2 (ADR-0010 §Phase 2).
//
// ADR-0007: the mapping and the log descriptor are the two things that are not
// self-freeing, and the destructor closes both. Everything else is a standard
// container or a `shared_ptr`.

#include "ui/history/blob.h"
#include "ui/history/log.h"
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
	// `file_identifier` (a future lesh, or somebody else's file entirely), or
	// ours and rejected by the Verifier. The session runs on Tier 2 plus memory
	// and warns once; #194's vacuum asks `may_rewrite_tier1()` and refuses.
	bool tier1_untouchable = false;
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

class history final : public history_source {
public:
	// A history with no files behind it: reads and records in memory, writes
	// nothing, warns about nothing. This is what `vared` and every unit test
	// that must not touch the developer's own history get, and it is also the
	// state a shell runs in when `open` could not reach a data directory - one
	// object, one code path, no null checks at the call sites.
	history();
	~history() override;

	history(const history&) = delete;
	history& operator=(const history&) = delete;

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

	// THE VACUUM SEAM (#194), and the one decision this milestone owes it.
	// False when `history.data` is not ours or did not verify: ADR-0010 says
	// such a file is never destroyed, and `rename`-ing a rebuilt blob over it
	// would destroy it. #194 asks this before it writes; it is here, and not
	// there, because the answer is settled at `open` and by nothing after it.
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
	void publish();
	// Writes resolved, unwritten `disk` items to the log, advancing the cursor.
	bool flush();
	// One line on stderr, once per process lifetime of this object.
	void warn_once(const char* what);

	// --- Loop-thread state; a walk never touches any of it -------------------

	// Newest at the BACK, which is where `add` merges and where the pending
	// item sits. A deque and not a vector because #194's vacuum drops the
	// written prefix from the front.
	std::deque<std::shared_ptr<const item>> _new_items;
	// The write cursor: everything below it has been offered to the log.
	std::size_t _first_unwritten = 0;
	std::shared_ptr<const std::vector<item>> _logged;
	std::shared_ptr<const mapped_blob> _blob;
	log_appender _appender;

	std::string _data_path;
	std::string _log_path;
	// `O_WRONLY|O_APPEND|O_CREAT|O_CLOEXEC`, held for the session. CLOEXEC
	// because this is a shell and every command line forks: a child has no
	// business inheriting a writable descriptor onto the user's history.
	int _log_fd = -1;

	std::uint64_t _session_id = 0;
	bool _tier1_untouchable = false;
	bool _warned = false;
	std::size_t _warnings = 0;
	std::size_t _unwritable = 0;

	// --- The one thing both threads touch ------------------------------------

	// `std::atomic_load`/`std::atomic_store`, not `std::atomic<shared_ptr>`:
	// libc++ does not implement the C++20 specialization (it static_asserts on
	// trivial copyability), and the free functions are ADR-0009's other named
	// option. Never null after construction.
	std::shared_ptr<const view> _view;
};

} // namespace lesh::ui::history
