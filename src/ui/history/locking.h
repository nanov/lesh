#pragma once

// THE FILE-IDENTITY AND LOCKING SEAM (ADR-0010 §Locking and staleness).
//
// FOUR FUNCTIONS, AND THEY ARE A SEAM RATHER THAN A LAYER. #194's vacuum has to
// take an exclusive lock before it renames, and it has to be able to ask
// whether the file at a path is still the file it read - and neither of those
// is the vacuum's subject. #195 owns the subject: the 0.25 s give-up timer and
// the process-wide `abandoned_locking` latch (fish `maybe_lock_file`), the
// remote-filesystem detection that turns locking off entirely (fish PR #5097),
// and the directory watch that makes a sibling's rename visible here. THE
// BODIES BELOW ARE THE STRAIGHTFORWARD ONES - `flock`, `stat`, `fstat` - and
// #195 replaces them without any caller changing.
//
// WHY A `file_id` AND NOT A MODIFICATION TIME. The question the vacuum asks is
// "is the file at this path the same INODE I read a moment ago", and a
// timestamp answers a different one: a rename-over within the same second
// leaves `st_mtime` unchanged while replacing the inode, which is exactly the
// case this exists to catch. `{dev, ino, size}` is fish's `file_id_t` minus the
// fields it carries for its own change detection; size is in because a file
// that grew is a file somebody appended to, and the vacuum's snapshot of what
// it read is no longer the whole of what is there.
//
// AN INVALID ID IS NOT AN ERROR, and both callers rely on that. `stat` of a
// path that does not exist is the ordinary state of a `history.data` that has
// never been vacuumed, and fish's rule - "the file is unchanged, OR the new
// file doesn't exist or we can't read it" - is the reason `file_id_equal`
// treats two invalid ids as equal: nothing there before and nothing there now
// is not a change somebody made under us.
//
// ADR-0007: nothing here owns a descriptor or allocates. `lock_exclusive` takes
// a descriptor the caller owns and the lock is released by the caller's
// `unlock` or by its `close`, which is `flock`'s own rule and not a convention
// invented here.

#include <cstddef>
#include <cstdint>
#include <string>

#include <sys/types.h>

namespace lesh::ui::history {

// `{dev, inode, size}`, or nothing at all.
struct file_id {
	std::uint64_t device = 0;
	std::uint64_t inode = 0;
	std::uint64_t size = 0;
	// False when the `stat` failed - the file is missing, or unreachable.
	bool valid = false;
};

// True when both describe the same file, INCLUDING when both describe no file.
// See the header comment: "nothing there, still nothing there" is not a change.
[[nodiscard]] bool file_id_equal(const file_id& left, const file_id& right) noexcept;

[[nodiscard]] file_id file_id_of_fd(int fd) noexcept;
[[nodiscard]] file_id file_id_of_path(const std::string& path) noexcept;

// `flock(fd, LOCK_EX)`, retried through `EINTR`.
//
// FALSE IS NOT FATAL TO ANY CALLER. A lockless filesystem answers `ENOTSUP` or
// `EOPNOTSUPP` and the vacuum proceeds anyway - fish makes the same call, on
// the grounds that interleaved history is better than no history, and the
// `file_id` re-check after the lock is what actually keeps two vacuums from
// overwriting each other's work. THERE IS NO GIVE-UP TIMER HERE: a lock that
// blocks forever on a wedged NFS mount is #195's problem, along with the
// `abandoned_locking` latch that is its answer.
[[nodiscard]] bool lock_exclusive(int fd) noexcept;

// `flock(fd, LOCK_UN)`. Closing `fd` does this too; the explicit call exists so
// the vacuum can drop a lock without dropping the descriptor it `fstat`s.
void unlock(int fd) noexcept;

} // namespace lesh::ui::history
