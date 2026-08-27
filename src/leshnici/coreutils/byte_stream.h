#pragma once

// THE BYTE PLUMBING `cat`, `head` AND `tail` SHARE (#165).
//
// Three utilities that copy bytes from a descriptor to standard output need the
// same four things: one fixed buffer big enough that the syscall count stops
// mattering, a write that survives a partial one, an open that reports the way
// the shell reports, and the rule that says a missing operand and a bare `-` both
// mean standard input. Written once here rather than three times, because three
// copies of one loop is how one of them comes to drop the tail of a pipe.
//
// THE PERFORMANCE RULE THIS FILE EXISTS TO KEEP: no `std::string` per line, and
// no `std::getline`. `cat`, `head` and `tail` all walk a fixed buffer looking for
// `'\n'` and hand whole spans on; the only bounded exception is `tail`'s ring
// over a pipe, which cannot know where the last N lines start without
// remembering them. 64 KiB is the buffer, `static thread_local` rather than a
// stack array: a builtin runs on the shell's own stack and 64 KiB of it is not
// this code's to take.
//
// WHY `std::fwrite` AND NOT `write(2)`. The shell's other builtins write through
// stdio, the executor flushes around every builtin call, and
// `drop_unwritable_output` reads `ferror(stdout)` afterwards to turn a failed
// write into a non-zero status. A utility here that wrote to fd 1 directly would
// be outside all three: its output could overtake an `echo` in the same command
// list, and a full disk would be a silent success. Whole-span `fwrite` of a
// 64 KiB block is one memcpy more than the syscall and keeps every one of those
// properties.
//
// Header-only for the reason `runtime/option_word.h` is: CMakeLists.txt lists
// sources explicitly and there is nothing here that wants a translation unit.

#include "runtime/diagnostic.h"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string_view>
#include <sys/types.h>
#include <unistd.h>

namespace lesh::leshnici::coreutils {

// 64 KiB, the size at which the read syscall stops being the cost.
inline constexpr std::size_t kIoBufferSize = 64 * 1024;

[[nodiscard]] inline char* io_buffer() noexcept {
	static thread_local char buffer[kIoBufferSize];
	return buffer;
}

// The whole span, or a report to the caller that it did not go. `fwrite` is
// all-or-nothing per element and these are byte elements, so a short return IS
// the error; `ferror(stdout)` carries the reason on to the executor.
[[nodiscard]] inline bool write_all(const char* data, std::size_t size) noexcept {
	return size == 0 || std::fwrite(data, 1, size, stdout) == size;
}

// `read(2)` that retries an interruption. Returns -1 on a real error, 0 at EOF.
//
// The raw descriptor rather than stdio, and deliberately: the shell's own stdin
// is drained and re-offered as a descriptor (#67), the `read` builtin already
// reads it that way, and a second buffered reader over the same fd would steal
// bytes from whatever runs next.
[[nodiscard]] inline ssize_t read_some(int fd, char* into, std::size_t size) noexcept {
	for (;;) {
		const ssize_t got = ::read(fd, into, size);
		if (got < 0 && errno == EINTR)
			continue;
		return got;
	}
}

// WHAT AN OPERAND OPENS. POSIX gives `cat`, `head` and `tail` the same two
// rules - no operand means standard input, and a lone `-` means it too - so the
// three of them read one function rather than three copies of one `if`.
//
// Returns -1 having REPORTED, in the shell's own diagnostic voice: an open that
// fails is the one failure these utilities have that is not a usage error, so
// the wording is `head: nosuchfile: No such file or directory` and the status
// the caller answers is 1 rather than 2.
[[nodiscard]] inline int open_operand(std::string_view utility, const char* name) {
	if (name == nullptr || std::string_view{name} == "-")
		return STDIN_FILENO;
	const int fd = ::open(name, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		runtime::report("%.*s: %s: %s", static_cast<int>(utility.size()), utility.data(),
		                name, std::strerror(errno));
	return fd;
}

// The other half of `open_operand`: standard input is the shell's and must not be
// closed, a file this opened must.
inline void close_operand(int fd) noexcept {
	if (fd >= 0 && fd != STDIN_FILENO)
		::close(fd);
}

// `==> name <==`, POSIX's header for `head` and `tail` with more than one
// operand, with the blank line that separates it from the block before.
inline void print_file_header(const char* name, bool first) {
	std::printf(first ? "==> %s <==\n" : "\n==> %s <==\n", name);
}

} // namespace lesh::leshnici::coreutils
