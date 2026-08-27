#include "leshnici/coreutils/builtins.h"

#include "leshnici/coreutils/byte_stream.h"
#include "runtime/diagnostic.h"
#include "substrate/args.h"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <unistd.h>

namespace lesh::leshnici::coreutils {

namespace {

// `-n count`, and POSIX's default of ten. `store_integral` range-checks the
// argument against the field for free, so `head -n abc` and
// `head -n 99999999999999999999` are both `invalid argument to option -n`
// through the one wording every utility in the tree uses.
struct head_opts {
	long count = 10;
};

constexpr auto kHead = args::spec<head_opts>(
	args::option{'n', args::field<&head_opts::count>, args::value("COUNT")}
		.help("write the first COUNT lines instead of the first ten"));

// The first `count` lines of one descriptor.
//
// NO LINE IS EVER MATERIALISED. The buffer is scanned with `memchr` for the
// newline that ENDS the count, and everything before it is handed to `fwrite` in
// one span - so a 400 MB file whose first ten lines are wanted costs one read, a
// memchr walk over at most those ten lines, and one write.
//
// A FINAL LINE WITH NO NEWLINE STILL COUNTS. `printf 'a'` is one line to every
// `head` that exists, so EOF ends the last line as surely as `'\n'` does; the
// loop simply stops writing when the descriptor does.
[[nodiscard]] bool head_stream(int fd, const char* name, long count) {
	if (count <= 0)
		return true;
	char* const buffer = io_buffer();
	long remaining = count;
	for (;;) {
		const ssize_t got = read_some(fd, buffer, kIoBufferSize);
		if (got == 0)
			return true;
		if (got < 0) {
			runtime::report("head: %s: %s", name, std::strerror(errno));
			return false;
		}
		const char* const end = buffer + got;
		const char* cursor = buffer;
		while (remaining != 0 && cursor != end) {
			const char* const newline =
				static_cast<const char*>(std::memchr(cursor, '\n', static_cast<std::size_t>(end - cursor)));
			if (newline == nullptr) {
				cursor = end;  // the rest of this block is a line still in progress
				break;
			}
			cursor = newline + 1;
			--remaining;
		}
		if (!write_all(buffer, static_cast<std::size_t>(cursor - buffer))) {
			runtime::report("head: %s", std::strerror(errno));
			return false;
		}
		if (remaining == 0)
			return true;
	}
}

} // namespace

// `head [-n count] [file...]`.
//
// THE HEADER RULE IS POSIX'S AND IS ABOUT THE OPERAND COUNT, not about whether
// the operand is a file: `head a b` writes `==> a <==` before each block and
// `head a` writes none, so a script that reads one file gets bytes and nothing
// else. `tail` beside this file keeps the identical rule from the identical
// helper, which is the point of the helper.
//
// A SHORT READ ON ONE OPERAND DOES NOT STOP THE REST, exactly as in `cat`: the
// status becomes 1 and the next operand is still written.
runtime::builtin_result builtin_head(runtime::shell_state&, char** argv) {
	const auto parsed = args::parse(kHead, argv);
	if (parsed.err)
		return {runtime::report_option_error("head", parsed.err)};
	if (parsed.opts.count < 0) {
		// coreutils reads a negative count as "all but the last N", which is a
		// second algorithm and a different utility's job. Refusing it is the honest
		// answer to an option this does not implement; guessing would be worse.
		runtime::report("head: %ld: invalid number of lines", parsed.opts.count);
		return {2};
	}

	char** const operands = parsed.rest;
	int count = 0;
	for (char** a = operands; *a != nullptr; ++a)
		++count;

	int status = 0;
	if (count == 0) {
		if (!head_stream(STDIN_FILENO, "-", parsed.opts.count))
			status = 1;
		return {status};
	}
	// The blank line before a header separates it from the block ABOVE, so it is
	// owed to whether anything was written rather than to the operand's index: an
	// operand that would not open printed nothing, and a leading blank line after
	// its diagnostic would be a separator with nothing on the other side.
	bool wrote_a_block = false;
	for (int i = 0; i < count; ++i) {
		const int fd = open_operand("head", operands[i]);
		if (fd < 0) {
			status = 1;
			continue;
		}
		if (count > 1) {
			print_file_header(operands[i], !wrote_a_block);
			wrote_a_block = true;
		}
		if (!head_stream(fd, operands[i], parsed.opts.count))
			status = 1;
		close_operand(fd);
	}
	return {status};
}

} // namespace lesh::leshnici::coreutils
