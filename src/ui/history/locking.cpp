#include "ui/history/locking.h"

#include "substrate/log.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>

#include <sys/file.h>
#include <sys/stat.h>

#if defined(__linux__)
#include <sys/vfs.h>
#else
#include <sys/mount.h>
#include <sys/param.h>
#endif

namespace lesh::ui::history {

namespace {

// ---------------------------------------------------------------------------
// The process-wide state: three flags and a counter, and that is the whole of it
// ---------------------------------------------------------------------------

// ADR-0010's one atomic. `relaxed` throughout, exactly as fish's
// `relaxed_atomic_bool_t` is: nothing is published through this flag - it
// guards no data - so all it has to be is not a data race.
std::atomic<bool> g_abandoned{false};
std::atomic<bool> g_data_dir_remote{false};

// The test seam. Atomics rather than plain bools because the loop thread reads
// them, and a test that sets one from the main thread would otherwise be a race
// TSan is right about even though nothing real depends on the ordering.
std::atomic<double> g_lock_duration_override{-1.0};
std::atomic<int> g_remoteness_override{-1};
std::atomic<std::uint64_t> g_lock_attempts{0};

[[nodiscard]] double monotonic_seconds() noexcept {
	struct ::timespec now {};
	// CLOCK_MONOTONIC and not fish's `timef` (which is `gettimeofday`): this
	// measures a duration, and a duration measured on the wall clock is a
	// duration an NTP step can make negative.
	::clock_gettime(CLOCK_MONOTONIC, &now);
	return static_cast<double>(now.tv_sec)
	       + static_cast<double>(now.tv_nsec) / 1000000000.0;
}

[[nodiscard]] file_id_t id_from(const struct ::stat& info) noexcept {
	return file_id_t{
		.device = static_cast<std::uint64_t>(info.st_dev),
		.inode = static_cast<std::uint64_t>(info.st_ino),
		.size = static_cast<std::uint64_t>(info.st_size),
	};
}

// The shared body of the two lock verbs. `LOCK_EX` or `LOCK_SH`, never
// `LOCK_UN` - unlocking has its own entry point and does none of this.
[[nodiscard]] bool maybe_lock(int fd, int how) noexcept {
	if (fd < 0)
		return false;
	// THE THREE REFUSALS, in fish's order. The latch first because it is the
	// cheapest and the most likely; remote second because it is a cached flag
	// rather than a `statfs` per lock.
	if (g_abandoned.load(std::memory_order_relaxed))
		return false;
	if (g_data_dir_remote.load(std::memory_order_relaxed))
		return false;

	g_lock_attempts.fetch_add(1, std::memory_order_relaxed);

	const double began = monotonic_seconds();
	const int answer = ::flock(fd, how);
	const double override_seconds = g_lock_duration_override.load(std::memory_order_relaxed);
	const double took = override_seconds >= 0.0 ? override_seconds
	                                            : monotonic_seconds() - began;

	if (took > k_lock_give_up_seconds) {
		// SET ONCE AND NEVER CLEARED. `exchange` rather than `store` so the log
		// line is printed by the one call that tripped it, however many threads
		// or history objects the process has (it has one of each today, and the
		// line is here so that stays true by accident rather than by luck).
		if (!g_abandoned.exchange(true, std::memory_order_relaxed)) {
			LESH_LOG(log::level::warn, log::category::history,
			         "locking the history file took %.3f s; locking is abandoned for "
			         "this process",
			         took);
		}
	}
	return answer != -1;
}

} // namespace

// ---------------------------------------------------------------------------
// File identity
// ---------------------------------------------------------------------------

file_id_t file_id_of_fd(int fd) noexcept {
	if (fd < 0)
		return k_invalid_file_id;
	struct ::stat info {};
	if (::fstat(fd, &info) != 0)
		return k_invalid_file_id;
	return id_from(info);
}

file_id_t file_id_of_path(const std::string& path) noexcept {
	struct ::stat info {};
	if (::stat(path.c_str(), &info) != 0)
		return k_invalid_file_id;
	return id_from(info);
}

bool file_id_equal(const file_id_t& left, const file_id_t& right) noexcept {
	// See the header: two failures are not a match.
	if (left == k_invalid_file_id || right == k_invalid_file_id)
		return false;
	return left == right;
}

// ---------------------------------------------------------------------------
// Remoteness
// ---------------------------------------------------------------------------

remoteness classify_filesystem(const char* type_name, std::uint64_t type_magic) noexcept {
#if defined(__linux__)
	(void)type_name;
	// fish `path_remoteness`, verbatim, including its comment: the kernel has
	// constants for these (NFS_SUPER_MAGIC, SMB_SUPER_MAGIC, CIFS_MAGIC_NUMBER)
	// but they live in headers that come and go, so they are hard-coded. The
	// cast to 32 bits is fish's too - `f_type` is signed and long on some
	// 32-bit ABIs, and CIFS_MAGIC_NUMBER has its top bit set.
	switch (static_cast<unsigned int>(type_magic)) {
		case 0x6969U:      // NFS_SUPER_MAGIC
		case 0x517BU:      // SMB_SUPER_MAGIC
		case 0xFE534D42U:  // SMB2_MAGIC_NUMBER - not in the manpage
		case 0xFF534D42U:  // CIFS_MAGIC_NUMBER
			return remoteness::remote;
		default:
			// Every other filesystem is assumed local, which is fish's default
			// and the safe one: being wrong costs a lock the latch will give up
			// on, not correctness.
			return remoteness::local;
	}
#else
	(void)type_magic;
	if (type_name == nullptr || type_name[0] == '\0')
		return remoteness::unknown;
	// macOS `f_fstypename`, per ADR-0010. The five names a `mount` on a Mac can
	// produce for a network volume: `nfs`, `smbfs` (SMB/CIFS, both dialects),
	// `afpfs` (AppleShare), `webdav` (iCloud Drive's transport, and every
	// mounted WebDAV share), `ftp`. `cifs` is here because Linux's name for the
	// same thing shows up on some third-party macOS clients.
	static constexpr const char* k_remote_names[] = {
		"nfs", "smbfs", "cifs", "afpfs", "webdav", "ftp",
	};
	for (const char* one : k_remote_names) {
		if (std::strcmp(type_name, one) == 0)
			return remoteness::remote;
	}
	return remoteness::local;
#endif
}

remoteness remoteness_of(const std::string& path) noexcept {
	if (const int forced = g_remoteness_override.load(std::memory_order_relaxed);
	    forced >= 0)
		return static_cast<remoteness>(forced);

	struct ::statfs info {};
	if (::statfs(path.c_str(), &info) != 0)
		return remoteness::unknown;
#if defined(__linux__)
	return classify_filesystem(nullptr, static_cast<std::uint64_t>(info.f_type));
#else
	return classify_filesystem(info.f_fstypename, 0);
#endif
}

bool is_remote(const std::string& path) noexcept {
	return remoteness_of(path) == remoteness::remote;
}

// ---------------------------------------------------------------------------
// The locks
// ---------------------------------------------------------------------------

bool lock_exclusive(int fd) noexcept { return maybe_lock(fd, LOCK_EX); }

bool lock_shared(int fd) noexcept { return maybe_lock(fd, LOCK_SH); }

void unlock(int fd) noexcept {
	if (fd >= 0)
		(void)::flock(fd, LOCK_UN);
}

bool abandoned_locking() noexcept { return g_abandoned.load(std::memory_order_relaxed); }

void set_data_directory_remote(bool remote) noexcept {
	g_data_dir_remote.store(remote, std::memory_order_relaxed);
}

bool data_directory_remote() noexcept {
	return g_data_dir_remote.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// The test seam
// ---------------------------------------------------------------------------

namespace test_hooks {

void set_lock_duration_override(double seconds) noexcept {
	g_lock_duration_override.store(seconds, std::memory_order_relaxed);
}

void set_remoteness_override(remoteness answer) noexcept {
	g_remoteness_override.store(static_cast<int>(answer), std::memory_order_relaxed);
}

void clear_remoteness_override() noexcept {
	g_remoteness_override.store(-1, std::memory_order_relaxed);
}

std::uint64_t lock_attempts() noexcept {
	return g_lock_attempts.load(std::memory_order_relaxed);
}

void reset_locking_state() noexcept {
	g_abandoned.store(false, std::memory_order_relaxed);
	g_data_dir_remote.store(false, std::memory_order_relaxed);
	g_lock_duration_override.store(-1.0, std::memory_order_relaxed);
	g_remoteness_override.store(-1, std::memory_order_relaxed);
	g_lock_attempts.store(0, std::memory_order_relaxed);
}

} // namespace test_hooks

} // namespace lesh::ui::history
