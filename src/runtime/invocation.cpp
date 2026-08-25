#include "runtime/invocation.h"

#include <string_view>

namespace lesh::runtime {

invocation parse_invocation(int argc, char** argv) {
	invocation inv;

	// `-c` does NOT take the next word. POSIX writes the form as
	// `sh -c [options] command_string [command_name [argument...]]`: the option list
	// CONTINUES after -c, and the command string is the FIRST OPERAND. Measured,
	// dash and bash agree on every corner of that - `-c -e code` sets errexit and
	// runs `code`, and `-c - code` and `-c -- code` both run `code`, because POSIX
	// says a single hyphen "shall be treated as the first operand and then ignored"
	// (startup-p.tst:158 and :169).
	//
	// Reading the next word instead was one bug wearing two faces (issue #44). It
	// walked ON past the command string, so `-c code -- -x` read `--` as the option
	// terminator and then took `-x` for $0, losing a positional parameter that dash
	// and bash both keep; and it ran the hyphen in `-c - code`. Both go away here,
	// because the option list ends at the first operand by construction and the
	// command string IS that operand.
	bool want_command_string = false;

	int i = 1;
	for (; i < argc; ++i) {
		const std::string_view arg{argv[i]};

		// `--` ends the options. A lone `-` or `+` does too, and POSIX's obsolete
		// `set -` spelling means nothing on the command line, so both are simply
		// consumed rather than treated as an operand.
		if (arg == "--" || arg == "-" || arg == "+") {
			++i;
			break;
		}
		if (arg.size() < 2 || (arg[0] != '-' && arg[0] != '+'))
			break;  // an operand; the options are over

		const bool enable = arg[0] == '-';
		bool group_ended = false;
		for (size_t c = 1; c < arg.size() && !group_ended; ++c) {
			switch (arg[c]) {
				case 'c':
					// POSIX spells this `-c`; `+c` is not an option at all.
					if (!enable) {
						inv.error = "invalid option";
						inv.error_operand = argv[i];
						return inv;
					}
					// The rest of THIS word is still option letters, and so is the rest
					// of the command line up to the first operand. Ending the group here
					// dropped the letters silently: `sh -cn 'for i in $(>f); do :; done'`
					// ran the substitution and created the file, which is option-p.tst's
					// 'noexec on: for command is not executed'.
					want_command_string = true;
					break;
				case 'i':
					inv.interactive = enable;
					break;
				case 's':
					// Read commands from standard input. Already the default when there
					// is no operand; explicit -s means it holds even when there is one.
					inv.read_stdin = true;
					break;
				case 'o': {
					// `-o` takes the option's long name. POSIX writes it as a separate
					// word; a bare `-o` with nothing after it lists the options, which
					// belongs to `set` (issue #31) and is not an invocation form.
					if (++i >= argc) {
						inv.error = "-o requires an option name";
						return inv;
					}
					if (!shell_state::apply_option_name(inv.options, argv[i], enable)) {
						inv.error = "invalid option name";
						inv.error_operand = argv[i];
						return inv;
					}
					group_ended = true;
					break;
				}
				default:
					if (!shell_state::apply_option_letter(inv.options, arg[c], enable)) {
						inv.error = "invalid option";
						inv.error_operand = argv[i];
						return inv;
					}
					break;
			}
		}
	}

	// Operand handling, and the part of POSIX's invocation grammar lesh had wrong:
	// with -c the first operand is the command string and the SECOND is $0
	// (command_name), NOT $1. Without -c the first operand is the script to run,
	// and a script's pathname is its own $0.
	if (want_command_string) {
		if (i >= argc) {
			inv.error = "-c requires a command string";
			return inv;
		}
		inv.command_string = argv[i++];
		if (i < argc)
			inv.command_name = argv[i++];
	} else if (!inv.read_stdin && i < argc) {
		inv.script_path = argv[i];
		inv.command_name = argv[i];
		++i;
	}
	// With no operand to name it, $0 is argv[0] exactly as it was spelled - symlink
	// included - which is what dash, bash and zsh all report, and what
	// startup-p.tst's '$0 with -s' asserts. The fallback belongs HERE rather than in
	// main(): main() cannot be unit-tested, and leaving the answer to a literal
	// default in shell_state is how $0 came to be the string "lesh" for every
	// invocation that named no command_file (issue #43).
	//
	// This is $0, and #61 has since split what used to be one question about it.
	// A COMMAND-LINE diagnostic keeps the hardcoded `lesh: ` prefix, because dash
	// prints its own name there too rather than $0 - `dash: 0: -c requires an
	// argument`, whatever it was invoked as - and because there is no $0 yet when
	// the command line is what failed. A RUNTIME diagnostic now uses $0 and a
	// position, which is what dash and bash both do; see runtime/diagnostic.h.
	if (inv.command_name == nullptr && argc > 0)
		inv.command_name = argv[0];
	inv.first_argument = i;
	return inv;
}

} // namespace lesh::runtime
