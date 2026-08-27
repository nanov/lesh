#pragma once

// LOCKING AND FILE IDENTITY for the two-tier history (#195, ADR-0010
// §Locking and staleness).
//
// Three questions, one file, because all three are answers to the same one:
// TWO SHELLS SHARE THESE FILES AND NEITHER KNOWS THE OTHER EXISTS.
//
//   Is anybody else writing right now?   `lock_exclusive` / `lock_shared`.
//   Is this still the file I opened?     `file_id_t` and its two constructors.
//   Is locking even legal here?          `is_remote`, and the give-up latch.
//
// EVERY ONE OF THEM MAY ANSWER "NO" AND THE SHELL CARRIES ON. That is the whole
// posture, and it is fish's: a lock that cannot be taken is a lock we do without
// (the risk is interleaved items, which is better than no history, and the
// single-`writev` frame in `log.h` was chosen so that the risk is bounded to
// ordering rather than to corruption); a filesystem that will not say whether it
// is remote is treated as local; an `flock` that took a quarter of a second once
// is an `flock` this process never takes again. None of these is an error path.
//
// THE GIVE-UP LATCH IS PROCESS-WIDE AND IT IS THE ONE PIECE OF SHARED MUTABLE
// STATE IN THE SUBSYSTEM (ADR-0010 says so in as many words: "no thread, no
// atomic beyond `abandoned_locking`"). fish learned this from users on lockless
// NFS and on filesystems where `flock` blocks for minutes: the first slow lock
// is a warning, and every lock after it is skipped, for every history object in
// the process. One relaxed atomic bool, set once, never cleared outside a test.
//
// WHY A `size` IN THE FILE ID. `dev` and `ino` alone say "the same inode"; the
// history's question is "the same CONTENTS I mapped", and an appended-to file
// has the same inode and different contents. ADR-0010 spells the triple out for
// that reason. It is not a hash and it is not a guarantee - a rewrite to the
// same length through the same inode would fool it - but nothing in this design
// writes that way: a vacuum renames a fresh inode over the old one, and a log
// only grows.
//
// NO HISTORY TYPE APPEARS HERE. This file knows descriptors, paths and
// `struct statfs`, and it is deliberately free of `history.data`, of the log and
// of the vacuum, so that #194's rewrite and #195's append path share one
// implementation of the identity check rather than each carrying a copy.
//
// THREADS: `file_id_*`, `lock_*` and `unlock` are called from the loop thread
// (ADR-0009) and hold no state between calls; the latch is atomic. Nothing here
// allocates.

#include <cstdint>
#include <string>

namespace lesh::ui::history {

// ---------------------------------------------------------------------------
// File identity
// ---------------------------------------------------------------------------

// `{dev, inode, size}` - ADR-0010's triple, and fish's `file_id_t` minus the
// timestamps it carries for `path_get_config`'s benefit and history never reads.
struct file_id_t {
	std::uint64_t device = 0;
	std::uint64_t inode = 0;
	std::uint64_t size = 0;

	friend bool operator==(const file_id_t&, const file_id_t&) noexcept = default;
};

// What a failed `stat` answers.
//
// ALL-ONES AND NOT ALL-ZEROS, because zero is a plausible `st_size` and a
// plausible `st_dev`: an id that compared equal to a real empty file's would
// make "the file went away" and "the file is empty" the same answer, and the
// append path distinguishes them (a missing `history.data` is a first run, a
// vanished one is a vacuum that just happened).
inline constexpr file_id_t k_invalid_file_id{
	.device = UINT64_MAX,
	.inode = UINT64_MAX,
	.size = UINT64_MAX,
};

// `fstat`, as an id. `k_invalid_file_id` when the descriptor is closed or the
// call failed.
[[nodiscard]] file_id_t file_id_of_fd(int fd) noexcept;

// `stat`, as an id. `k_invalid_file_id` when the path does not exist, which is
// how the TOCTOU check below reads "somebody unlinked it out from under us".
[[nodiscard]] file_id_t file_id_of_path(const std::string& path) noexcept;

// The comparison, spelled out.
//
// A FREE FUNCTION AND NOT JUST `operator==`, because it has a rule attached that
// the operator cannot carry: TWO INVALID IDS ARE NOT EQUAL. `stat` failing twice
// is not evidence that the same file is at both ends - it is the absence of
// evidence - and a TOCTOU loop that accepted it would treat "I could not look"
// as "it matched".
[[nodiscard]] bool file_id_equal(const file_id_t& left, const file_id_t& right) noexcept;

// ---------------------------------------------------------------------------
// Remoteness
// ---------------------------------------------------------------------------

// What `statfs` said about the filesystem under a path.
enum class remoteness : std::uint8_t {
	// Not on a network filesystem we recognise. Locking and `mmap` are on.
	local,
	// NFS, SMB/CIFS, or one of their friends. NEVER lock (fish's users have
	// watched `flock` block for minutes on lockless NFS) and NEVER `mmap` (fish
	// PR #5097: a mapping over NFS can fault on pages the server has since
	// dropped, and a shell that SIGBUSes because somebody vacuumed on another
	// host is a shell with a bug). Tier 1 is read into a heap buffer instead.
	remote,
	// `statfs` failed, or the platform has no way to ask. TREATED AS LOCAL,
	// which is fish's choice too: the cost of being wrong here is a slow lock,
	// and the give-up latch below already bounds that.
	unknown,
};

// The CLASSIFICATION, split from the syscall so a test can exercise the whole
// table without a network mount (the issue's "injectable for tests").
//
// `type_name` is macOS's `f_fstypename` and `type_magic` is Linux's `f_type`;
// each platform passes the one it has and a placeholder for the other. ADR-0010
// names both, and this is where the two lists live side by side.
//
// LINUX GETS A MAGIC LIST AND MACOS GETS A NAME LIST because that is what each
// kernel offers. macOS also has `MNT_LOCAL` in `f_flags`, which is strictly more
// general than any name list; ADR-0010 chose the name, and the reason to keep
// its choice is that a name is what a test can inject and what a log line can
// print, where a flag is neither. The cost of a name this list has not heard of
// is that lesh locks and maps on it, which is exactly what every shell before
// fish PR #5097 did.
[[nodiscard]] remoteness classify_filesystem(const char* type_name,
                                             std::uint64_t type_magic) noexcept;

// `statfs(path)`, classified. `unknown` when the call failed.
[[nodiscard]] remoteness remoteness_of(const std::string& path) noexcept;

// `remoteness_of(path) == remoteness::remote`. The one-line question the data
// directory is asked at `open`.
[[nodiscard]] bool is_remote(const std::string& path) noexcept;

// ---------------------------------------------------------------------------
// The locks
// ---------------------------------------------------------------------------

// How long an `flock` may take before this process stops taking them
// (fish `maybe_lock_file`, and ADR-0010 quotes the number).
inline constexpr double k_lock_give_up_seconds = 0.25;

// `flock(fd, LOCK_EX)` / `flock(fd, LOCK_SH)`, with the three refusals.
//
// FALSE MEANS "NOT LOCKED", not "failed": the caller proceeds either way, which
// is what makes this `maybe_lock_file` and not `lock_file`. It is false when the
// process has given up, when the data directory is remote, and when `flock`
// itself refused - and the caller cannot tell those apart because there is
// nothing it would do differently.
//
// LOCK_EX for an append or a vacuum; LOCK_SH for the moment a reader spends
// reading a consistent size out of `history.data` before it maps it
// (ADR-0010: "mapping Tier 1 takes `LOCK_SH` only long enough to get a
// consistent size").
[[nodiscard]] bool lock_exclusive(int fd) noexcept;
[[nodiscard]] bool lock_shared(int fd) noexcept;

// `flock(fd, LOCK_UN)`. Safe on a descriptor that was never locked - which is
// the ordinary case once the latch has tripped, and the reason this takes no
// "was it locked" flag from the caller.
void unlock(int fd) noexcept;

// Whether this process has given up on locking. Latched: once true, forever.
[[nodiscard]] bool abandoned_locking() noexcept;

// Records that the history's data directory is on a remote filesystem, for
// every subsequent `lock_*` in this process (fish asks
// `path_get_data_remoteness()` from inside `maybe_lock_file`, and the reason
// the answer is cached rather than re-derived is that it is one `statfs` per
// lock otherwise, on the per-command append path).
//
// PROCESS-WIDE, LIKE THE LATCH, and for the same reason: one shell has one data
// directory, and a second `history` object in the process (a test's) is over the
// same one or over a temp dir the test also controls.
void set_data_directory_remote(bool remote) noexcept;
[[nodiscard]] bool data_directory_remote() noexcept;

// ---------------------------------------------------------------------------
// The test seam
// ---------------------------------------------------------------------------

// NOTHING IN THE SHIPPED BINARY CALLS ANY OF THIS. It exists because the two
// behaviours this file is here for - "a slow lock is abandoned" and "a remote
// directory is never locked or mapped" - cannot be provoked by a unit test
// otherwise: one needs a quarter-second stall inside `flock` and the other needs
// an NFS mount.
namespace test_hooks {

// Makes every subsequent `lock_*` report `seconds` as the time its `flock` took,
// instead of reading the clock. Negative turns the override off.
void set_lock_duration_override(double seconds) noexcept;

// Makes `remoteness_of` answer this without calling `statfs`. Passing
// `remoteness::unknown` after a `local`/`remote` override does NOT turn the
// override off - `clear_remoteness_override` does, because `unknown` is a real
// answer a test wants to be able to inject.
void set_remoteness_override(remoteness answer) noexcept;
void clear_remoteness_override() noexcept;

// `flock` calls actually issued, process-wide. The instrument that says "never
// lock again this process" is a fact and not a comment.
[[nodiscard]] std::uint64_t lock_attempts() noexcept;

// Puts the latch, the remote flag, the overrides and the counter back. A test
// fixture's `SetUp`, because every one of them is process-wide by design.
void reset_locking_state() noexcept;

} // namespace test_hooks

} // namespace lesh::ui::history
