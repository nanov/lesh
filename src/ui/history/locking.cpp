#include "ui/history/locking.h"

#include <cerrno>

#include <sys/file.h>
#include <sys/stat.h>

namespace lesh::ui::history {

namespace {

[[nodiscard]] file_id from_stat(const struct ::stat& info) noexcept {
	return file_id{
		.device = static_cast<std::uint64_t>(info.st_dev),
		.inode = static_cast<std::uint64_t>(info.st_ino),
		.size = static_cast<std::uint64_t>(info.st_size),
		.valid = true,
	};
}

} // namespace

bool file_id_equal(const file_id& left, const file_id& right) noexcept {
	// TWO INVALID IDS ARE EQUAL. fish: "the file is unchanged, or the new file
	// doesn't exist or we can't read it" - both license the replacement, and
	// folding them together here is what keeps that rule out of the caller.
	if (!left.valid || !right.valid)
		return left.valid == right.valid;
	return left.device == right.device && left.inode == right.inode
	       && left.size == right.size;
}

file_id file_id_of_fd(int fd) noexcept {
	struct ::stat info {};
	if (fd < 0 || ::fstat(fd, &info) != 0)
		return {};
	return from_stat(info);
}

file_id file_id_of_path(const std::string& path) noexcept {
	struct ::stat info {};
	if (::stat(path.c_str(), &info) != 0)
		return {};
	return from_stat(info);
}

bool lock_exclusive(int fd) noexcept {
	if (fd < 0)
		return false;
	// EINTR AND NOTHING ELSE IS RETRIED. A signal arriving while a shell blocks
	// on a lock is ordinary - SIGCHLD alone makes it likely - and treating it
	// as a failed lock would turn a common event into a skipped safety check.
	while (::flock(fd, LOCK_EX) != 0) {
		if (errno == EINTR)
			continue;
		return false;
	}
	return true;
}

void unlock(int fd) noexcept {
	if (fd < 0)
		return;
	while (::flock(fd, LOCK_UN) != 0 && errno == EINTR) {
	}
}

} // namespace lesh::ui::history
