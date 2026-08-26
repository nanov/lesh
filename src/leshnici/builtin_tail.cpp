#include "leshnici/builtins.h"

#include "leshnici/byte_stream.h"
#include "runtime/diagnostic.h"
#include "substrate/args.h"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace lesh::leshnici {

namespace {

struct tail_opts {
	long count = 10;
};

constexpr auto kTail = args::spec<tail_opts>(
	args::option{'n', args::field<&tail_opts::count>, args::value("COUNT")}
		.help("write the last COUNT lines instead of the last ten"));

// THE SEEKABLE CASE: FIND THE START, THEN COPY FORWARD.
//
// A file's last N lines are a suffix, and a suffix has an offset. Reading the
// whole file to find it - which is what a naive `tail` and every line-based one
// does - is O(file); scanning BACKWARD in 64 KiB blocks counting newlines is
// O(the answer), so `tail -n 10` of a 4 GB log touches one block instead of
// 65,536 of them. That difference is the reason this branch exists at all;
// without it there would be one implementation, the pipe one, and it would be
// correct and unusable.
//
// THE TRAILING NEWLINE IS A TERMINATOR, NOT A LINE. A file ending `...z\n` has
// its last line `z`, so the search wants N newlines BEFORE that final one, which
// is why the scan starts one byte earlier when the last byte is `'\n'`. Getting
// this wrong is the classic off-by-one that makes `tail -n 1` print an empty
// line, and it is why the two cases are spelled out rather than folded.
//
// Returns false having reported; `handled` says whether the descriptor turned
// out to be seekable at all, so the caller can fall through to the ring.
[[nodiscard]] bool tail_seekable(int fd, const char* name, long count, bool& handled) {
	handled = false;
	const off_t size = ::lseek(fd, 0, SEEK_END);
	if (size < 0)
		return true;  // a pipe or a terminal: not this function's case
	handled = true;
	if (size == 0 || count <= 0)
		return true;

	char* const buffer = io_buffer();
	// Where the search begins: past the final newline, which terminates the last
	// line rather than starting a new one.
	off_t scan_end = size;
	{
		char last = 0;
		if (::pread(fd, &last, 1, size - 1) == 1 && last == '\n')
			--scan_end;
	}

	long wanted = count;      // newlines still to find
	off_t start = 0;          // where the answer begins; 0 if the file is shorter
	off_t position = scan_end;
	while (position > 0 && wanted > 0) {
		const std::size_t block = position >= static_cast<off_t>(kIoBufferSize)
			? kIoBufferSize
			: static_cast<std::size_t>(position);
		position -= static_cast<off_t>(block);
		ssize_t got = 0;
		while (got < static_cast<ssize_t>(block)) {
			const ssize_t chunk = ::pread(fd, buffer + got, block - static_cast<std::size_t>(got),
			                              position + got);
			if (chunk < 0 && errno == EINTR)
				continue;
			if (chunk <= 0) {
				runtime::report("tail: %s: %s", name, std::strerror(errno));
				return false;
			}
			got += chunk;
		}
		// Backwards through the block. `memrchr` is a GNU extension, so the walk is
		// explicit; the block is at most 64 KiB and the loop stops the moment the
		// count is reached, which is the property that matters.
		for (std::size_t i = block; i-- > 0;) {
			if (buffer[i] != '\n')
				continue;
			if (--wanted == 0) {
				start = position + static_cast<off_t>(i) + 1;
				break;
			}
		}
	}

	// Copy from `start` to the real end of the file - `scan_end` was only where
	// the SEARCH began, and the trailing newline is part of the output.
	if (::lseek(fd, start, SEEK_SET) < 0) {
		runtime::report("tail: %s: %s", name, std::strerror(errno));
		return false;
	}
	for (;;) {
		const ssize_t got = read_some(fd, buffer, kIoBufferSize);
		if (got == 0)
			return true;
		if (got < 0) {
			runtime::report("tail: %s: %s", name, std::strerror(errno));
			return false;
		}
		if (!write_all(buffer, static_cast<std::size_t>(got))) {
			runtime::report("tail: %s", std::strerror(errno));
			return false;
		}
	}
}

// THE PIPE CASE: A RING OF THE LAST N LINES.
//
// A descriptor that cannot seek can only be read once and forward, so the last N
// lines are not findable - they have to be REMEMBERED, and the memory is
// therefore bounded by N and by how long those lines are rather than by the size
// of the stream. `yes | tail -n 3` holds three short strings however long it
// runs.
//
// THE STRINGS ARE REUSED, WHICH IS THE WHOLE POINT. `assign` on a slot that
// already has capacity copies bytes and allocates nothing, so after the first N
// lines the steady state is allocation-free - the property `head` and `cat` get
// by never building a line at all, bought here at the one place the algorithm
// forbids that.
[[nodiscard]] bool tail_ring(int fd, const char* name, long count) {
	if (count <= 0)
		return true;
	const std::size_t n = static_cast<std::size_t>(count);
	std::vector<std::string> ring(n);
	std::size_t next = 0;    // where the NEXT completed line goes
	std::size_t held = 0;    // how many slots are live, capped at n
	std::string partial;     // the line still being read across block boundaries

	char* const buffer = io_buffer();
	for (;;) {
		const ssize_t got = read_some(fd, buffer, kIoBufferSize);
		if (got < 0) {
			runtime::report("tail: %s: %s", name, std::strerror(errno));
			return false;
		}
		if (got == 0)
			break;
		const char* cursor = buffer;
		const char* const end = buffer + got;
		while (cursor != end) {
			const char* const newline = static_cast<const char*>(
				std::memchr(cursor, '\n', static_cast<std::size_t>(end - cursor)));
			if (newline == nullptr) {
				partial.append(cursor, static_cast<std::size_t>(end - cursor));
				break;
			}
			const std::size_t span = static_cast<std::size_t>(newline - cursor) + 1;
			if (partial.empty()) {
				ring[next].assign(cursor, span);
			} else {
				partial.append(cursor, span);
				ring[next].assign(partial);
				partial.clear();
			}
			next = (next + 1) % n;
			if (held < n)
				++held;
			cursor = newline + 1;
		}
	}
	// A last line with no newline is still a line, and it is the NEWEST one.
	if (!partial.empty()) {
		ring[next].assign(partial);
		next = (next + 1) % n;
		if (held < n)
			++held;
	}

	const std::size_t first = (next + n - held) % n;
	for (std::size_t i = 0; i < held; ++i) {
		const std::string& line = ring[(first + i) % n];
		if (!write_all(line.data(), line.size())) {
			runtime::report("tail: %s", std::strerror(errno));
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool tail_stream(int fd, const char* name, long count) {
	bool handled = false;
	if (!tail_seekable(fd, name, count, handled))
		return false;
	return handled || tail_ring(fd, name, count);
}

} // namespace

// `tail [-n count] [file...]`. The header rule and the keep-going-after-a-bad-
// operand rule are `head`'s, from the same helper, for the same reason.
runtime::builtin_result builtin_tail(runtime::shell_state&, char** argv) {
	const auto parsed = args::parse(kTail, argv);
	if (parsed.err)
		return {runtime::report_option_error("tail", parsed.err)};
	if (parsed.opts.count < 0) {
		// coreutils reads `tail -n -5` as `-n 5` and `tail -n +5` as "from line 5
		// on". The second is a different algorithm and neither is what this ships,
		// so a negative count is refused rather than quietly reinterpreted.
		runtime::report("tail: %ld: invalid number of lines", parsed.opts.count);
		return {2};
	}

	char** const operands = parsed.rest;
	int count = 0;
	for (char** a = operands; *a != nullptr; ++a)
		++count;

	int status = 0;
	if (count == 0) {
		if (!tail_stream(STDIN_FILENO, "-", parsed.opts.count))
			status = 1;
		return {status};
	}
	bool wrote_a_block = false;
	for (int i = 0; i < count; ++i) {
		const int fd = open_operand("tail", operands[i]);
		if (fd < 0) {
			status = 1;
			continue;
		}
		if (count > 1) {
			print_file_header(operands[i], !wrote_a_block);
			wrote_a_block = true;
		}
		if (!tail_stream(fd, operands[i], parsed.opts.count))
			status = 1;
		close_operand(fd);
	}
	return {status};
}

} // namespace lesh::leshnici
