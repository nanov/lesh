#include "ui/history/vacuum.h"

#include "ui/history/locking.h"
#include "ui/history/log.h"

#include <algorithm>
#include <cerrno>
#include <ctime>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lesh::ui::history {

namespace {

[[nodiscard]] std::string_view as_text(std::span<const std::byte> bytes) noexcept {
	return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// ---------------------------------------------------------------------------
// Bytes in and out of descriptors
// ---------------------------------------------------------------------------

// Reads `fd` whole, from wherever it is.
//
// EIGHT-ALIGNED BY CONSTRUCTION, which `read_records` requires: the vector's
// storage comes from plain `operator new`, aligned for every fundamental type.
[[nodiscard]] std::vector<std::byte> read_all(int fd) {
	std::vector<std::byte> out;
	if (fd < 0)
		return out;

	struct ::stat info {};
	if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode))
		return out;

	out.resize(static_cast<std::size_t>(info.st_size));
	std::size_t filled = 0;
	while (filled < out.size()) {
		const ::ssize_t got = ::read(fd, out.data() + filled, out.size() - filled);
		if (got < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (got == 0)
			break;
		filled += static_cast<std::size_t>(got);
	}
	// A file that shrank under us: keep what arrived. The blob Verifier or the
	// log's torn-tail rule takes it from here, and both already have to.
	out.resize(filled);
	return out;
}

[[nodiscard]] std::vector<std::byte> read_whole_path(const std::string& path) {
	const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return {};
	std::vector<std::byte> out = read_all(fd);
	::close(fd);
	return out;
}

// Writes every byte, or returns the errno that stopped it.
[[nodiscard]] int write_all(int fd, std::span<const std::byte> bytes) {
	std::size_t sent = 0;
	while (sent < bytes.size()) {
		const ::ssize_t wrote = ::write(fd, bytes.data() + sent, bytes.size() - sent);
		if (wrote < 0) {
			if (errno == EINTR)
				continue;
			return errno;
		}
		if (wrote == 0)
			return ENOSPC;
		sent += static_cast<std::size_t>(wrote);
	}
	return 0;
}

// THE ONLY FSYNC IN THE SUBSYSTEM (ADR-0010 §Vacuum step 2), and it is here
// rather than after the `rename` for the reason every temp-and-rename protocol
// puts it here: the rename is atomic with respect to the directory, but it
// makes no promise that the FILE's data reached the platter first. Without this
// a power cut moments after a vacuum can leave `history.data` pointing at an
// inode full of zeroes - a file that exists, is named right, and has no
// history in it. Appends are not fsync'd, deliberately: a lost tail frame is
// one command line and the cost of syncing per keystroke is not one command
// line.
//
// `F_FULLFSYNC` ON macOS BECAUSE `fsync` THERE IS A LIE - it returns once the
// bytes are in the drive's write cache, which a power cut empties. The fcntl is
// the one that flushes the cache, and it is unsupported on some filesystems
// (notably some network mounts), where it answers `ENOTSUP` and the plain
// `fsync` below is the best available.
[[nodiscard]] bool sync_to_disk(int fd) noexcept {
#if defined(F_FULLFSYNC)
	if (::fcntl(fd, F_FULLFSYNC, 0) == 0)
		return true;
#endif
	while (::fsync(fd) != 0) {
		if (errno == EINTR)
			continue;
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// The merge (fish `history_lru_cache_t`)
// ---------------------------------------------------------------------------

// Every record the new blob might hold, keyed on its command bytes.
//
// FISH'S LRU, WITH ITS TWO JOBS SEPARATED. fish's `history_lru_cache_t` does
// dedup and capping in one structure because it evicts as it inserts; this one
// records a sequence number per entry and evicts once, at the end. The result
// is the same whenever nothing evicted is later re-inserted, and STRICTLY
// BETTER when something is: fish would have dropped a command line and then
// re-added it as new, forgetting the older timestamp it had already merged in.
// Deferred eviction costs memory bounded by the inputs, and the inputs are
// bounded - the old blob is itself capped at `k_history_save_max`, the log
// holds ~`k_vacuum_frequency` frames and so does the session.
//
// NOTHING IS COPIED. Every span in here points into the caller's three buffers
// - the bytes read off the target fd, the bytes read off the log, and the
// session's own items - all of which outlive the vacuum. At the cap that is the
// difference between a rewrite that touches ~25 MB and one that also allocates
// it.
class merge_set {
public:
	// Records `one` as MORE RECENTLY SEEN than everything before it, so the
	// caller inserts oldest source first. A repeat of a command line already in
	// here does not add a second entry: it promotes the one that is there and
	// takes the later timestamp.
	void add(const record& one) {
		// fish skips empty items, and so does the schema - `cmd` is
		// `(required)`, so a record without one could not be written back.
		if (one.cmd.empty())
			return;

		const std::string_view key = as_text(one.cmd);
		const auto [at, fresh] = _index.try_emplace(key, _entries.size());
		if (fresh) {
			_entries.push_back(entry{one, ++_seq});
			return;
		}

		entry& stored = _entries[at->second];
		// PROMOTED WHETHER OR NOT IT WINS THE TIMESTAMP. "Most recently seen"
		// is what the cap evicts on, and a command line that turns up again in
		// a newer source is one the user is still using, whatever clock skew
		// says about its `when`.
		stored.seq = ++_seq;
		if (one.when < stored.what.when)
			return;
		// THE WHOLE RECORD AND NOT JUST THE TIMESTAMP, which is where this
		// departs from fish ("What to do about paths here? Let's just ignore
		// them"). Keeping the max `when` beside an older run's cwd and exit
		// code would produce a record that never happened; every field belongs
		// to one run of the command, and the newest run is the one to keep.
		stored.what = one;
	}

	// The survivors, newest first, with the cap applied.
	[[nodiscard]] std::vector<record> finish(std::size_t cap, std::size_t& evicted) {
		evicted = 0;
		if (_entries.size() > cap) {
			// THE CAP EVICTS THE OLDEST, where "oldest" is least recently seen
			// and not smallest timestamp - fish's rule, and the right one: a
			// machine whose clock jumped backwards would otherwise throw away
			// the commands typed since.
			std::nth_element(_entries.begin(), _entries.begin() + static_cast<std::ptrdiff_t>(cap),
			                 _entries.end(),
			                 [](const entry& left, const entry& right) {
				                 return left.seq > right.seq;
			                 });
			evicted = _entries.size() - cap;
			_entries.resize(cap);
		}

		// NEWEST FIRST IS THE FORMAT (ADR-0010 §Tier 1), and the tie-break is
		// insertion order reversed, so that when two command lines share a
		// timestamp - which one second of resolution makes ordinary - the one
		// seen later comes first. That agrees with the merge walk, where the
		// session's items are yielded ahead of the log's and the log's ahead of
		// the mapping's, and where the sources are fed to `add` in exactly the
		// opposite order.
		std::sort(_entries.begin(), _entries.end(),
		          [](const entry& left, const entry& right) {
			          if (left.what.when != right.what.when)
				          return left.what.when > right.what.when;
			          return left.seq > right.seq;
		          });

		std::vector<record> out;
		out.reserve(_entries.size());
		for (const entry& one : _entries)
			out.push_back(one.what);
		return out;
	}

private:
	struct entry {
		record what;
		std::uint64_t seq = 0;
	};

	// Keyed on the FIRST insertion's bytes, which stay alive as long as every
	// other buffer this walks; a later record with equal bytes replaces the
	// value and leaves the key where it is.
	std::unordered_map<std::string_view, std::size_t> _index;
	std::vector<entry> _entries;
	std::uint64_t _seq = 0;
};

// ---------------------------------------------------------------------------
// The temp file
// ---------------------------------------------------------------------------

// `mkstemp` beside the target.
//
// THE SAME DIRECTORY IS NOT A PREFERENCE. `rename` across filesystems is
// `EXDEV`, and `$TMPDIR` is a different volume from `$HOME` on more machines
// than not - on macOS it is a per-user directory under `/var/folders`. A temp
// anywhere else turns the atomic slide into a copy, and a copy is the in-place
// modification ADR-0010 forbids.
class temp_file {
public:
	explicit temp_file(const std::string& beside) : _path(beside + ".XXXXXX") {
		_fd = ::mkstemp(_path.data());
		if (_fd < 0) {
			_error = errno;
			_path.clear();
			return;
		}
		// `mkstemp` does not set it and this is a shell: every command line
		// forks, and a descriptor onto a half-written copy of the user's whole
		// history is not a thing to hand a child.
		(void)::fcntl(_fd, F_SETFD, FD_CLOEXEC);
	}

	// STEP 6. Idempotent, so that the explicit call the function below makes
	// before it fires `temp_unlinked` and the destructor's safety net cannot
	// double-close. `unlink` after a successful `rename` never happens - the
	// rename clears the path - and the destructor is what covers the exits
	// nobody thought of.
	void discard() noexcept {
		if (_fd >= 0) {
			::close(_fd);
			_fd = -1;
		}
		if (!_path.empty()) {
			::unlink(_path.c_str());
			_path.clear();
		}
	}

	~temp_file() { discard(); }

	temp_file(const temp_file&) = delete;
	temp_file& operator=(const temp_file&) = delete;

	[[nodiscard]] bool valid() const noexcept { return _fd >= 0; }
	[[nodiscard]] int fd() const noexcept { return _fd; }
	[[nodiscard]] const std::string& path() const noexcept { return _path; }
	[[nodiscard]] int error() const noexcept { return _error; }

	// Back to empty, for the next speculative build (ADR-0010 step 3).
	[[nodiscard]] bool reset() noexcept {
		return ::ftruncate(_fd, 0) == 0 && ::lseek(_fd, 0, SEEK_SET) == 0;
	}

	// The rename succeeded, so the name is the target's now and must not be
	// unlinked.
	void released() noexcept { _path.clear(); }

private:
	std::string _path;
	int _fd = -1;
	int _error = 0;
};

// A descriptor holding `LOCK_EX`, released when it goes out of scope.
//
// FOUR WAYS OUT OF THE ATTEMPT BELOW - a lost race, a failed aside-rename, a
// failed rename, and success - and each one has to release the lock and the
// descriptor. Written by hand that is four copies of the same two lines and one
// of them is eventually forgotten; a shell that leaked one would hold every
// sibling's vacuum for the rest of the session. `close` alone releases the
// lock, which is `flock`'s own rule; the explicit `unlock` says so out loud.
class locked_target {
public:
	explicit locked_target(const std::string& path)
		: _fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC)) {
		// A REFUSED LOCK IS NOT A REFUSED VACUUM: `locking.h` says why (a
		// lockless filesystem answers `ENOTSUP` and fish proceeds anyway), and
		// the `file_id` check is what actually keeps two vacuums from
		// overwriting each other's work.
		if (_fd >= 0)
			(void)lock_exclusive(_fd);
	}

	~locked_target() {
		if (_fd < 0)
			return;
		unlock(_fd);
		::close(_fd);
	}

	locked_target(const locked_target&) = delete;
	locked_target& operator=(const locked_target&) = delete;

	[[nodiscard]] int fd() const noexcept { return _fd; }

private:
	int _fd = -1;
};

// `<data path>.corrupt-<unix seconds>`.
[[nodiscard]] std::string aside_path(const std::string& data_path) {
	return data_path + ".corrupt-"
	       + std::to_string(static_cast<long long>(std::time(nullptr)));
}

} // namespace

// ---------------------------------------------------------------------------
// The vacuum
// ---------------------------------------------------------------------------

vacuum_result vacuum(const vacuum_request& request) {
	vacuum_result out;

	const auto fire = [&request](vacuum_step step) {
		if (request.on_step)
			request.on_step(step);
	};

	if (request.policy != tier1_policy::rewritable) {
		out.status = vacuum_status::refused;
		fire(vacuum_step::temp_unlinked);
		return out;
	}

	temp_file temp{request.data_path};
	if (!temp.valid()) {
		out.error = temp.error();
		fire(vacuum_step::temp_unlinked);
		return out;
	}

	// THE LOG IS READ ONCE, OUTSIDE THE RETRY LOOP, and that is a correctness
	// choice rather than a saving. `history` truncates the log after a
	// successful rename, and it may only truncate frames this vacuum actually
	// put in the blob; re-reading per attempt would make the set of frames in
	// the blob depend on which attempt won, and a sibling's append landing
	// between two attempts would then be truncated away without ever having
	// been merged. Read once, and the two sides describe the same bytes.
	//
	// A sibling appending AFTER this read is still a live race, and `history`
	// closes it on the other side: it truncates only if the log is still
	// exactly this long, and otherwise leaves it alone and lets the next vacuum
	// collapse the duplicates.
	const std::vector<std::byte> log_bytes = read_whole_path(request.log_path);
	out.log_bytes_merged = log_bytes.size();

	bool done = false;
	for (int attempt = 0; attempt < request.max_tries && !done; ++attempt) {
		if (attempt > 0 && !temp.reset()) {
			out.error = errno;
			break;
		}

		// --- Step 1: open the target and snapshot its identity ---------------
		//
		// `O_CREAT` so that a first vacuum has something to compare against:
		// without it, `before` is invalid on a fresh machine and the check in
		// step 3 could not tell "nobody has written one yet" from "somebody
		// deleted it while I worked".
		const int target_before =
			::open(request.data_path.c_str(), O_RDONLY | O_CREAT | O_CLOEXEC, 0600);
		const file_id before = file_id_of_fd(target_before);
		fire(vacuum_step::target_opened);

		// --- Step 2: build the whole thing into the temp ---------------------
		//
		// RE-READ FROM THE FD AND NOT FROM THE CACHED MAPPING (ADR-0010, and
		// fish says why: "which may have changed out from underneath us, so
		// don't trust our old file contents"). The stronger reason is the
		// protocol: `before` is this descriptor's identity, so the bytes merged
		// below have to be this descriptor's bytes or step 3 guards nothing.
		const std::vector<std::byte> old_bytes = read_all(target_before);
		if (target_before >= 0)
			::close(target_before);

		// THE OLD BLOB IS NEWEST-FIRST AND `merge_set` WANTS OLDEST FIRST, so
		// it is collected and then walked backwards. Feeding it in file order
		// would make the blob's most recent records its least recently seen,
		// and the cap would then evict exactly the ones it exists to keep.
		std::vector<record> old_records;
		merge_set merged;
		const blob_status old_status = read_records(
			old_bytes, [&old_records](const record& one) { old_records.push_back(one); });

		// NOT OURS - and re-derived here rather than trusted from `open`,
		// because the file may have BECOME somebody else's since. ADR-0010's
		// rule is unconditional: the session simply runs without Tier 1.
		if (old_status == blob_status::unknown_identifier) {
			out.status = vacuum_status::refused;
			break;
		}
		// Ours, and unreadable. `vacuum.h` argues the policy: rebuild, but move
		// the broken file aside first, under the lock in step 3. It contributes
		// no records - there is no safe way to read them - so `merged` holds
		// only the log and the session, which are exactly what is not lost.
		const bool corrupt = old_status == blob_status::corrupt;

		// OLDEST SOURCE FIRST AND OLDEST RECORD FIRST WITHIN IT, so that
		// `merge_set`'s promotion and its tie-break both mean what the merge
		// walk means by "newer": the blob, then the log (which is already in
		// append order), then this session.
		for (auto at = old_records.rbegin(); at != old_records.rend(); ++at)
			merged.add(*at);
		(void)for_each(log_bytes, [&merged](const record& one) { merged.add(one); });
		for (const record& one : request.session)
			merged.add(one);

		const std::vector<record> keeping = merged.finish(request.cap, out.evicted);

		blob_writer writer;
		if (const int failed = write_all(temp.fd(), writer.build(keeping)); failed != 0) {
			out.error = failed;
			break;
		}
		if (!sync_to_disk(temp.fd())) {
			out.error = errno;
			break;
		}
		fire(vacuum_step::temp_built);

		// --- Step 3: lock, and see whether the target moved ------------------
		//
		// THE LOCK COMES BEFORE THE CHECK AND IS HELD PAST THE RENAME, which is
		// the only ordering that works: checking first would leave a window in
		// which a sibling renames between our check and our own rename, and
		// dropping the lock before renaming would leave the same window at the
		// other end. And the check is on the PATH, not on the descriptor - the
		// descriptor still names the old inode however many times it has been
		// renamed over.
		const locked_target target_after{request.data_path};
		const file_id after = target_after.fd() >= 0 ? file_id_of_path(request.data_path)
		                                             : file_id{};

		// fish's condition, and the second half of it matters: a target that is
		// GONE is not a target somebody is defending. Nothing there is nothing
		// to lose by renaming ours into place.
		if (!(file_id_equal(after, before) || !after.valid)) {
			++out.retries;
			continue;
		}
		fire(vacuum_step::target_locked);

		if (corrupt) {
			// UNDER THE LOCK, and before the rename that would otherwise
			// destroy it. A failure here is not fatal to the vacuum - the file
			// is unreadable either way - but it does cancel the rebuild, on
			// the principle that we only overwrite what we could preserve.
			out.corrupt_path = aside_path(request.data_path);
			if (::rename(request.data_path.c_str(), out.corrupt_path.c_str()) != 0) {
				out.error = errno;
				out.corrupt_path.clear();
				break;
			}
			out.corrupt_moved_aside = true;
		}

		// --- Step 4: the original's ownership and mode (fish #2355) ----------
		//
		// Somebody's first command in a session may have been `sudo -E`, which
		// leaves a root-owned `history.data` in the user's directory; a rewrite
		// that quietly reset it to the current process's uid and umask would be
		// a permissions change the user never asked for. A failed `stat` is the
		// case fish names too, and the answer is the same: hope the defaults
		// are right, because there is nothing better to copy from.
		struct ::stat original {};
		if (target_after.fd() >= 0 && ::fstat(target_after.fd(), &original) == 0) {
			(void)::fchown(temp.fd(), original.st_uid, original.st_gid);
			// The permission bits only: `fchmod` is specified on those, and
			// handing it `S_IFREG` back is a portability bet with nothing to
			// win.
			(void)::fchmod(temp.fd(), original.st_mode & 07777);
		}
		fire(vacuum_step::ownership_copied);

		// --- Step 5: slide it into place -------------------------------------
		if (::rename(temp.path().c_str(), request.data_path.c_str()) != 0) {
			out.error = errno;
			break;
		}
		temp.released();
		out.status = vacuum_status::renamed;
		out.records_written = keeping.size();
		done = true;
		// STILL UNDER THE LOCK, which `~locked_target` drops at the end of this
		// iteration - the whole point of holding it past the rename.
		fire(vacuum_step::renamed);
	}

	// --- Step 6 --------------------------------------------------------------
	temp.discard();
	fire(vacuum_step::temp_unlinked);
	return out;
}

} // namespace lesh::ui::history
