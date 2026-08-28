#pragma once

// THE DIRECTORY WATCH (#195, ADR-0010 §Locking and staleness): one descriptor
// that becomes readable when something appears in the history's data directory.
//
// THE BUG IT EXISTS FOR IS fish #3565. A shell that only READS history - one
// left open in a tab, autosuggesting - never notices that another shell
// vacuumed, because fish's `loaded_old` latch is cleared by SAVING and a reader
// never saves. So the tab keeps serving a mapping of a file that was replaced an
// hour ago, and the commands typed in every other window are invisible in that
// one until it is restarted. ADR-0010 replaces the latch with this: the vacuum's
// `rename` is an event, and the event arrives on a descriptor the loop already
// polls.
//
// THE DIRECTORY AND NOT THE FILE, and this is the whole reason the class is
// per-platform rather than one `open` and a flag. A vacuum does not write
// `history.data`; it writes a temp file and `rename`s it over the top. The
// inode the old name pointed at is never touched, so a watch ON THAT INODE -
// which is what kqueue's `EVFILT_VNODE` gives you, since a kevent is registered
// against an open descriptor - fires exactly never. The directory's inode, on
// the other hand, is modified by the rename, and that is what both kernels
// agree to report.
//
//   Linux   `inotify`, `IN_MOVED_TO | IN_CREATE` on the directory. Named
//           events: the rename's destination, and a file appearing.
//   macOS   `kqueue`, `EVFILT_VNODE` with `NOTE_WRITE` on a descriptor open on
//           the DIRECTORY. No names - a directory changed, that is all it says,
//           which is why `drain()` answers a bool and the caller re-`stat`s.
//
// EDGE-TRIGGERED, DELIBERATELY. `EV_CLEAR` on macOS and inotify's read-to-empty
// on Linux both mean the descriptor stops being readable once drained. A
// level-triggered watch left undrained would spin the loop's `poll` at 100% CPU,
// which is the failure mode a watch topic has to not have.
//
// WHAT IT DOES NOT DO: it does not `stat` anything, does not know the name
// `history.data`, and does not decide what a change means. It reports "the
// directory changed", once per drain, and `history::drain_watch` is where that
// becomes "the file I mapped is not the file that is there".
//
// NO THREAD (ADR-0010, in as many words). The descriptor is one more topic in
// the loop's `poll`; the drain runs inside a turn like every other drain.
//
// ADR-0007: the destructor closes both descriptors, and there are at most two.

#include <string>

namespace lesh::ui::history {

class directory_watch {
public:
	directory_watch() = default;
	~directory_watch();

	directory_watch(const directory_watch&) = delete;
	directory_watch& operator=(const directory_watch&) = delete;
	directory_watch(directory_watch&& other) noexcept;
	directory_watch& operator=(directory_watch&& other) noexcept;

	// Starts watching `directory`. Any previous watch is closed first.
	//
	// FALSE IS A DEGRADATION AND NOT AN ERROR: no inotify instances left, a
	// filesystem that does not support the notification (every network one),
	// a kernel without either mechanism. The session then behaves exactly as it
	// did before this class existed - it notices a vacuum on its next append,
	// through the file-id check, and a read-only session does not notice at all,
	// which is fish's behaviour today.
	[[nodiscard]] bool open(const std::string& directory);

	// Closes both descriptors. Idempotent.
	void close() noexcept;

	// The descriptor the loop's `watch` topic polls, or -1 when nothing is
	// watched. BORROWED: the loop must not close it, and this object must
	// outlive the attachment.
	[[nodiscard]] int fd() const noexcept { return _fd; }

	[[nodiscard]] bool is_open() const noexcept { return _fd >= 0; }

	// Consumes everything pending and answers whether anything was there.
	//
	// READS TO EMPTY, always, however many events are queued - a vacuum in three
	// sibling shells is three notifications and one answer. Not draining fully
	// would leave the descriptor readable and the loop would turn again
	// immediately for a change it had already seen.
	[[nodiscard]] bool drain() noexcept;

	// The errno from the syscall that made `open` answer false; zero otherwise.
	[[nodiscard]] int error() const noexcept { return _error; }

private:
	// The pollable one: the inotify instance on Linux, the kqueue on macOS.
	int _fd = -1;
	// macOS only: the descriptor the `EVFILT_VNODE` filter is registered
	// against, held open because closing it deletes the registration. -1 on
	// Linux, where the watch is a small integer inside the inotify instance and
	// no second descriptor exists.
	int _target_fd = -1;
	int _error = 0;
};

} // namespace lesh::ui::history
