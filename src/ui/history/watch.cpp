#include "ui/history/watch.h"

#include <cerrno>
#include <iterator>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/inotify.h>
#else
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#endif

namespace lesh::ui::history {

directory_watch::~directory_watch() { close(); }

directory_watch::directory_watch(directory_watch&& other) noexcept
	: _fd(std::exchange(other._fd, -1)),
	  _target_fd(std::exchange(other._target_fd, -1)),
	  _error(std::exchange(other._error, 0)) {}

directory_watch& directory_watch::operator=(directory_watch&& other) noexcept {
	if (this != &other) {
		close();
		_fd = std::exchange(other._fd, -1);
		_target_fd = std::exchange(other._target_fd, -1);
		_error = std::exchange(other._error, 0);
	}
	return *this;
}

void directory_watch::close() noexcept {
	if (_fd >= 0) {
		::close(_fd);
		_fd = -1;
	}
	if (_target_fd >= 0) {
		::close(_target_fd);
		_target_fd = -1;
	}
}

#if defined(__linux__)

bool directory_watch::open(const std::string& directory) {
	close();
	_error = 0;

	// NONBLOCK because `drain` reads to empty and needs EAGAIN to know when it
	// is; CLOEXEC because this is a shell and every command line forks.
	_fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (_fd < 0) {
		_error = errno;
		return false;
	}

	// ADR-0010's two masks and no others. `IN_MOVED_TO` is the vacuum's
	// `rename` landing; `IN_CREATE` is a file appearing, which is what a
	// `mkstemp` in the same directory does and what a first-ever `history.data`
	// does. Deliberately NOT `IN_MODIFY`: a sibling shell appending to
	// `history.new.log` would fire it once per command in every open terminal,
	// and the log is not what a reload re-reads.
	if (::inotify_add_watch(_fd, directory.c_str(), IN_MOVED_TO | IN_CREATE) < 0) {
		_error = errno;
		close();
		return false;
	}
	return true;
}

bool directory_watch::drain() noexcept {
	if (_fd < 0)
		return false;

	// One `inotify_event` plus the longest name the kernel will hand back, which
	// is what the manual page's own example sizes its buffer to. On the stack:
	// this runs on the loop thread and must not allocate.
	alignas(struct inotify_event) char buffer[4096];
	bool anything = false;
	for (;;) {
		const ::ssize_t got = ::read(_fd, buffer, sizeof(buffer));
		if (got > 0) {
			anything = true;
			continue;
		}
		if (got < 0 && errno == EINTR)
			continue;
		// EAGAIN - drained - or a read error, which is not something a history
		// can act on. Either way there is nothing more to consume.
		break;
	}
	return anything;
}

#else

bool directory_watch::open(const std::string& directory) {
	close();
	_error = 0;

	// O_EVTONLY (Darwin) says "this descriptor is for notifications": it does not
	// count as a reference that would stop the volume being unmounted, which
	// matters for a directory a shell holds open for its whole life.
	_target_fd = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC | O_EVTONLY);
	if (_target_fd < 0) {
		_error = errno;
		return false;
	}

	_fd = ::kqueue();
	if (_fd < 0) {
		_error = errno;
		close();
		return false;
	}
	// A kqueue descriptor is not inherited across `fork` on Darwin, so it needs
	// no CLOEXEC of its own - but it is inherited across `fork` WITHOUT exec on
	// some BSDs, and this shell forks to run shell code. One `fcntl` is cheaper
	// than the platform footnote.
	(void)::fcntl(_fd, F_SETFD, FD_CLOEXEC);

	struct ::kevent change {};
	// EV_CLEAR is what makes this edge-triggered: without it the kqueue stays
	// readable after a change and the loop's `poll` spins. NOTE_WRITE is the
	// directory's contents changing, which is ADR-0010's filter; NOTE_LINK and
	// NOTE_RENAME are the same event seen from two other angles (a subdirectory
	// appearing, the directory itself being moved) and cost nothing to include
	// because the drain answers one bool either way.
	EV_SET(&change, static_cast<::uintptr_t>(_target_fd), EVFILT_VNODE,
	       EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_EXTEND | NOTE_LINK | NOTE_RENAME, 0,
	       nullptr);
	if (::kevent(_fd, &change, 1, nullptr, 0, nullptr) < 0) {
		_error = errno;
		close();
		return false;
	}
	return true;
}

bool directory_watch::drain() noexcept {
	if (_fd < 0)
		return false;

	struct ::kevent events[8];
	const struct ::timespec immediately { .tv_sec = 0, .tv_nsec = 0 };
	bool anything = false;
	for (;;) {
		const int got = ::kevent(_fd, nullptr, 0, events,
		                         static_cast<int>(std::size(events)), &immediately);
		if (got > 0) {
			anything = true;
			// A full batch may mean there are more waiting; anything short of
			// one is the queue emptied.
			if (got == static_cast<int>(std::size(events)))
				continue;
			break;
		}
		if (got < 0 && errno == EINTR)
			continue;
		break;
	}
	return anything;
}

#endif

} // namespace lesh::ui::history
