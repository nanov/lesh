#include "leshnici/builtins.h"

#include "leshnici/byte_stream.h"
#include "runtime/diagnostic.h"
#include "substrate/args.h"

#include <cerrno>
#include <cstring>
#include <string_view>
#include <unistd.h>

namespace lesh::leshnici {

namespace {

// `cat` HAS NO OPTIONS HERE, and the empty spec is the whole reason it still has
// a table (#148 phase 2's lesson, recorded in `first_operand`'s note): a
// zero-row spec gives `--` and the lone `-` for free but REFUSES every other
// option word, and that is exactly right for this utility - `cat -n` is not
// something this ships, so saying so is the honest answer rather than opening a
// file called `-n`. It is the opposite of the POSIX builtins that must take
// `exit -1` as an operand.
struct cat_opts {};

constexpr auto kCat = args::spec<cat_opts>();

// One descriptor, copied whole. The buffer is the shared 64 KiB one and no line
// is ever looked for: `cat` does not care where the newlines are, so it does not
// look, which is what makes it the fastest of the three.
[[nodiscard]] bool copy_stream(int fd, const char* name) {
	char* const buffer = io_buffer();
	for (;;) {
		const ssize_t got = read_some(fd, buffer, kIoBufferSize);
		if (got == 0)
			return true;
		if (got < 0) {
			runtime::report("cat: %s: %s", name, std::strerror(errno));
			return false;
		}
		if (!write_all(buffer, static_cast<std::size_t>(got))) {
			runtime::report("cat: %s", std::strerror(errno));
			return false;
		}
	}
}

} // namespace

// `cat [file...]`. No operands, or a lone `-`, is standard input.
//
// EVERY OPERAND IS TRIED. A file that will not open is reported and the status
// becomes 1, but the ones after it are still concatenated - which is what
// coreutils and every historical `cat` do, and the difference from stopping is
// visible the moment a glob picks up a file someone else has just removed.
runtime::builtin_result builtin_cat(runtime::shell_state&, char** argv) {
	const auto parsed = args::parse(kCat, argv);
	if (parsed.err)
		return {runtime::report_option_error("cat", parsed.err)};

	int status = 0;
	char** operand = parsed.rest;
	if (*operand == nullptr) {
		if (!copy_stream(STDIN_FILENO, "-"))
			status = 1;
		return {status};
	}
	for (; *operand != nullptr; ++operand) {
		const int fd = open_operand("cat", *operand);
		if (fd < 0) {
			status = 1;
			continue;
		}
		if (!copy_stream(fd, *operand))
			status = 1;
		close_operand(fd);
	}
	return {status};
}

} // namespace lesh::leshnici
