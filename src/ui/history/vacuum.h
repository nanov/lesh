#pragma once

// THE VACUUM (#194, ADR-0010 §Vacuum): the thing that turns a growing append
// log back into one compact `history.data`.
//
// WHAT IT IS FOR. Tier 2 makes recording a command cheap - one `writev` and no
// rewrite - and the bill for that arrives as a log that only ever grows and a
// blob that only ever ages. The vacuum pays it: every `kVacuumFrequency`
// appends, it merges the old blob, every frame in the log and this session's
// items into one deduplicated, capped, newest-first blob, and slides it into
// place with a `rename`. It is fish's `save_internal_via_rewrite` and
// `rewrite_to_temporary_file`, with fish's `history_lru_cache_t` for the dedup.
//
// THE ONE INVARIANT, AND EVERY DECISION BELOW SERVES IT: A RESOLVED COMMAND IS
// NEVER LOST. Not by a crash, not by a concurrent vacuum in another terminal,
// not by a lock that could not be taken, not by a `history.data` this build
// cannot read. The cost of holding that line is duplicates - a crash between
// the `rename` and the log truncation leaves the same commands in both tiers -
// and duplicates are free: the merge walk deduplicates on the way out and the
// next vacuum removes them from the file. Losing a command is not recoverable;
// showing it twice for one more command is not a bug anybody can see.
//
// WHY A TEMP FILE AND A `rename` AND NOT A REWRITE IN PLACE. Two reasons, and
// only the second is about crashes. A sibling shell has `history.data` MMAP'd;
// truncating it under that mapping is a SIGBUS in the other process the moment
// it touches a page past the new end, whereas unlinking the inode out from
// under it leaves the mapping valid forever - stale, never invalid (ADR-0010:
// "writers never truncate or modify `history.data` in place"). And `rename` is
// atomic, so there is no instant at which the file on disk is half a history.
//
// WHY IT IS SPECULATIVE - build first, lock second. The rewrite is the
// expensive part (at the cap, ~25 MB), and holding an exclusive lock across it
// would stall every sibling shell's append for the duration. So the temp is
// built with no lock at all, and only then is the target locked and its
// identity re-checked: if somebody else vacuumed while we were writing, the
// work is thrown away and redone. fish's structure exactly, and its comment for
// it - "we want to rewrite the file, while holding the lock for as briefly as
// possible" - is the whole justification.
//
// WHAT THIS FILE DOES NOT DO. It does not append (Tier 2's `log_appender`
// does), it does not decide WHEN to run (the countdown lives in `history`), it
// does not touch `new_items` or the write cursor or the mapping (`history`'s
// bookkeeping, and it happens only after this returns `ok`), and it does not
// own the give-up-timer or remote-filesystem policy that `locking.h` fronts
// (#195). It is handed the three sources, and it either renames a finished blob
// into place or reports precisely why it did not.

#include "ui/history/blob.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace lesh::ui::history {

// ---------------------------------------------------------------------------
// The three constants
// ---------------------------------------------------------------------------

// Appends between vacuums (fish's `kVacuumFrequency`), and the countdown starts
// at a random value in `[0, kVacuumFrequency)` so that a shell used for twenty
// commands and closed still eventually vacuums instead of never doing it.
//
// THE COST ARITHMETIC, because 25 looks arbitrary and is not. A vacuum rewrites
// the whole blob: at the `k_history_save_max` cap of 256 Ki records and a
// command line plus cwd averaging ~100 bytes, that is ~25 MB read, merged and
// written, every 25 commands - about 1 MB of I/O per command line amortised,
// against an append of a few hundred bytes. That is the accepted price
// (ADR-0010 says so in as many words), and it is paid only by a history that
// has actually reached the cap; a realistic 10^4-record history rewrites ~1 MB.
// Raising the frequency lowers the amortised cost and lengthens the log, which
// is the startup scan; lowering it does the reverse. Anything here wants a
// profile, not an opinion.
inline constexpr int k_vacuum_frequency = 25;

// Records a rewritten `history.data` keeps (fish's `HISTORY_SAVE_MAX`). The
// oldest beyond it are evicted - see `vacuum_plan` on what "oldest" means.
inline constexpr std::size_t k_history_save_max = 256 * 1024;

// Times the speculative build is retried when a sibling vacuum wins the race
// (fish's `max_save_tries`). A bound rather than a loop because the failure
// mode it guards against - a machine where somebody rewrites `history.data`
// faster than we can - must end in "give up safely", never in a shell that
// stopped responding.
inline constexpr int k_max_save_tries = 1024;

// ---------------------------------------------------------------------------
// The crash-injection seam
// ---------------------------------------------------------------------------

// Where a `vacuum_request::on_step` hook is called, so a test can kill the
// process between any two of ADR-0010 §Vacuum's steps and then assert that a
// fresh `history` over the same directory has lost nothing.
//
// NUMBERED AFTER THE ADR'S STEPS and fired AFTER the step completes, so
// `on_step(renamed)` runs at the instant the new file is visible and the old
// one is not. `published` is the odd one out: it is not this file's step at all
// but `history`'s bookkeeping - the log truncation, the cursor, the dropped
// items, the new mapping - which is the other half of the window a crash can
// land in and therefore has to be injectable from the same hook.
enum class vacuum_step : int {
	// Step 1: the target is open `O_RDONLY|O_CREAT` and its `file_id` is
	// snapshotted.
	target_opened = 1,
	// Step 2: the temp holds the whole merged blob and has been fsync'd.
	temp_built = 2,
	// Step 3: the target is reopened, `LOCK_EX` is held, and the path's
	// `file_id` still matches the snapshot.
	target_locked = 3,
	// Step 4: the temp carries the original's uid, gid and mode.
	ownership_copied = 4,
	// Step 5: the rename is done. The new blob IS the history file now.
	renamed = 5,
	// Step 6: the temp is closed and unlinked. Fired on EVERY path out of
	// `vacuum`, success or refusal or give-up, and after the unlink rather
	// than before it - so a test that kills the process here is testing a
	// directory with no temp in it.
	//
	// A CRASH AT STEPS 1-5 DOES LEAVE ONE, and there is no fixing that from
	// inside the process that died: the temp is a real file with a real name
	// and nothing else knows to remove it. fish has the same leftover for the
	// same reason. It costs a few hundred bytes of a blob nobody reads, it is
	// never mapped (the name is `history.data.XXXXXX`, not `history.data`), and
	// no data is lost either way.
	temp_unlinked = 6,
	// `history`'s step, not this file's: the log is truncated, the write cursor
	// is advanced, the written items are dropped and Tier 1 is re-mapped.
	published = 7,
};

// ---------------------------------------------------------------------------
// What goes in
// ---------------------------------------------------------------------------

// What a `history.data` may have done to it, decided at `open` and re-decided
// from the fd's CURRENT bytes in step 2 (see `blob.h`'s `read_records`).
enum class tier1_policy : std::uint8_t {
	// Ours, or absent, or empty. Rebuild it.
	rewritable,
	// An unknown `file_identifier`: a future lesh, or somebody else's file
	// entirely. NEVER TOUCHED, not even renamed aside - we do not know what it
	// is, so we do not know that moving it is safe for whoever owns it.
	untouchable,
};

// One command line the vacuum must not lose, as `history` holds it. The spans
// are borrowed and must outlive the call, like everything else on this path.
using session_records = std::span<const record>;

struct vacuum_request {
	// `<data dir>/history.data`. The temp is created beside it - same
	// directory, because `rename` across filesystems is `EXDEV` and a
	// `$TMPDIR` on another volume is the ordinary case on macOS.
	std::string data_path;
	// `<data dir>/history.new.log`. Read whole, at step 2. NOT truncated here:
	// that is `history`'s job and it happens only after a successful rename.
	std::string log_path;
	// This session's resolved, writable items - oldest first.
	//
	// ALL OF THEM AND NOT JUST THE UNWRITTEN ONES, which is a deliberate
	// widening of ADR-0010's "unwritten `new_items`". fish's write cursor never
	// advances past an item the file refused; #193's does (`history::flush`
	// counts it `unwritable` and steps over it, so that a broken log does not
	// make every later command retry a write that cannot succeed). Under that
	// rule "unwritten" would silently exclude exactly the items that never
	// reached the disk - the ones with the most to lose. The superset costs
	// nothing: the dedup below collapses the overlap with the log, and
	// `new_items` is bounded by this very cadence at ~25 entries.
	session_records session;
	// False when `history.data` must never be written. The vacuum re-derives
	// this from the file's current bytes as well; this is the early-out, and
	// the two agree except when the file changed since startup.
	tier1_policy policy = tier1_policy::rewritable;

	// --- The two knobs, which exist so the tests can reach the edges ---------
	//
	// Both default to the ADR's constants and no shipping call passes either.
	// They are parameters rather than `#define`s because the behaviour at the
	// cap and the behaviour after the last retry are the two things this file
	// most needs tested, and reaching them at the real values costs a 256 Ki
	// blob and 1024 `F_FULLFSYNC`s - which is not a unit test, it is a
	// benchmark that occasionally fails.
	std::size_t cap = k_history_save_max;
	int max_tries = k_max_save_tries;

	// Test-only. Null in every shipping call.
	std::function<void(vacuum_step)> on_step;
};

// ---------------------------------------------------------------------------
// What comes out
// ---------------------------------------------------------------------------

enum class vacuum_status : std::uint8_t {
	// A fresh blob was renamed into place. THE CALLER MUST NOW truncate the
	// log, advance the cursor, drop the written items and re-map Tier 1 - and
	// only on this status.
	renamed,
	// `history.data` may not be written (`tier1_policy::untouchable`, or the
	// bytes on disk turned out not to be ours). Nothing was touched, nothing
	// was lost: the session's commands are in the log where `flush` put them.
	refused,
	// `k_max_save_tries` speculative builds all lost the race, or the temp
	// could not be created or written. Nothing was touched. THE CALLER'S
	// FALLBACK IS A PLAIN APPEND - ADR-0010 step 3's "on give-up, do not drop
	// data" - which in a two-tier design is simply the Tier 2 append `flush`
	// already does, so the caller re-flushes and tries again in another
	// `k_vacuum_frequency` commands.
	gave_up,
};

struct vacuum_result {
	vacuum_status status = vacuum_status::gave_up;
	// Records in the blob that was renamed into place. Zero unless `renamed`.
	std::size_t records_written = 0;
	// Speculative builds thrown away because a sibling won the race. Zero on
	// an uncontended vacuum, which is every vacuum on a machine with one shell.
	std::size_t retries = 0;
	// Records the cap evicted. Non-zero only on a history at 256 Ki.
	std::size_t evicted = 0;
	// BYTES OF THE LOG THAT WENT INTO THE BLOB, and the caller may truncate the
	// log only if it is still exactly this long.
	//
	// This is the one hole ADR-0010's step 5 leaves open, closed here. A
	// sibling shell appending a frame between this read and the truncation
	// would have that frame deleted without it ever having been merged - the
	// one way this design could lose a resolved command. Comparing the length
	// turns that into the outcome everything else on this path has: the log is
	// left alone, its frames are duplicated in the blob, the merge walk hides
	// the duplicates and the next vacuum removes them.
	std::size_t log_bytes_merged = 0;
	// The errno from the syscall that ended a `gave_up`; zero otherwise.
	int error = 0;
	// Set when a `corrupt` Tier 1 - ours, and rejected by the Verifier - was
	// moved aside before being rebuilt. `corrupt_path` is where it went.
	bool corrupt_moved_aside = false;
	std::string corrupt_path;
};

// ---------------------------------------------------------------------------
// The vacuum
// ---------------------------------------------------------------------------

// Runs ADR-0010 §Vacuum steps 1-6. Blocking, loop-thread only, and the only
// `fsync` in the subsystem.
//
// THE CORRUPT-FILE POLICY, which #194 was asked to decide and which is decided
// HERE rather than at the call site because it is a rule about the file and not
// about the session. A `history.data` whose identifier is ours and which the
// Verifier rejects IS REBUILT - but the broken file is `rename`d aside to
// `history.data.corrupt-<unix seconds>` first, so nothing is destroyed and a
// user who wants to go digging still can.
//
// The argument for rebuilding: the alternative is that one flipped byte
// permanently disables Tier 1. The log then grows without bound, startup goes
// back to scanning it whole, and the condition never clears by itself, because
// nothing but a vacuum ever writes `history.data`. That is a much larger loss
// than the file, and it is inflicted on a user who did nothing wrong.
//
// The argument for moving it aside rather than overwriting: "corrupt" here
// means "the Verifier said no", which is a statement about the bytes and not
// about their value. A truncated blob still holds most of its records in
// readable form; a future recovery tool could get them back. `rename` costs one
// syscall and makes the decision reversible, which an overwrite does not.
//
// AND `unknown_identifier` IS NOT COVERED BY ANY OF THAT. Those bytes are not
// ours, so no argument above applies: we cannot say what would be lost, we
// cannot say a rename is safe for whoever wrote it, and ADR-0010's rule stands
// unqualified. The vacuum refuses and the session runs on Tier 2 plus memory.
[[nodiscard]] vacuum_result vacuum(const vacuum_request& request);

} // namespace lesh::ui::history
