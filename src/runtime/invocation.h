#pragma once

#include "runtime/shell_state.h"

#include <optional>

namespace lesh::runtime {

// How lesh was invoked. See issue #33.
//
// This exists as its own component because it was a silent gate on more than
// three thousand conformance assertions. POSIX XCU gives `sh` three invocation
// forms, and every one of them admits the whole `set` option list on the command
// line, in BOTH polarities: `-x` turns an option on, `+x` turns it off. lesh
// knew only `-i`, `-c` and `-s`, so the yash signal suite - which runs the shell
// under test as `sh +i +m` - got "cannot open +i" and exit 127 before reading a
// byte of any test. Twenty files, 180 assertions each, failing for a reason that
// had nothing to do with signals.
//
// Being a seam rather than a loop inside main() is the point: main() is not
// testable, and this was worth a unit test per invocation form.
struct invocation {
	// -c: the string to run instead of reading input.
	const char* command_string = nullptr;
	// The command_file operand, when there is one and -c was not given.
	const char* script_path = nullptr;
	// $0. With -c this is the operand AFTER the command string, which is what
	// POSIX calls command_name; otherwise it is the script's pathname. When no
	// operand names it, argv[0] - exactly as spelled, symlink included, which is
	// what every reference shell reports. Never null for a real argv.
	const char* command_name = nullptr;
	// argv index of $1. Equal to argc when there are no positional parameters.
	int first_argument = 0;
	// -s: read from standard input even though operands were given.
	bool read_stdin = false;
	// -i / +i. Unset when neither appeared, because then POSIX decides
	// interactiveness from isatty rather than from the command line, and this
	// parser must not pretend to know.
	std::optional<bool> interactive;
	// `-o leshnici` / `+o leshnici`, and unset when neither appeared (#165).
	//
	// The SAME tri-state `interactive` above needs, for the same reason: the
	// option's default is "on iff this shell is interactive", which is a question
	// this parser cannot answer and main() can. A plain bool would make the two
	// indistinguishable, so `lesh -o leshnici script.sh` would be silently undone
	// by the default main() writes afterwards.
	//
	// Not derived from the two-seed probe that answers `interactive`: that probe
	// copies the whole option struct across on every `-o` occurrence, so any OTHER
	// `-o` word would make this look like it had been named. The `-o` branch
	// records it directly instead.
	std::optional<bool> leshnici;
	shell_state::options options{};
	// Non-null when the invocation cannot be honoured. The caller reports it and
	// exits; a shell that cannot parse its own command line must not guess.
	const char* error = nullptr;
	// The offending word, when `error` names one.
	const char* error_operand = nullptr;
};

[[nodiscard]] invocation parse_invocation(int argc, char** argv);

} // namespace lesh::runtime
