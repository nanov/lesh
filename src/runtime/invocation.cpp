#include "runtime/invocation.h"

#include <string_view>

namespace lesh::runtime {

invocation parse_invocation(int argc, char** argv) {
	invocation inv;

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
					if (++i >= argc) {
						inv.error = "-c requires a command string";
						return inv;
					}
					inv.command_string = argv[i];
					// The command string is the NEXT WORD, so the rest of THIS word is
					// still option letters. Ending the group here dropped them silently:
					// `sh -cn 'for i in $(>f); do :; done'` ran the substitution and
					// created the file, which is option-p.tst's 'noexec on: for command
					// is not executed'. dash reads `-cn` the same way.
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

	// Operand handling, and the one part of POSIX's invocation grammar that lesh
	// had wrong: with -c the first operand is $0 (command_name), NOT $1. Without
	// -c it is the script to run, and a script's pathname is its own $0.
	if (inv.command_string != nullptr) {
		if (i < argc)
			inv.command_name = argv[i++];
	} else if (!inv.read_stdin && i < argc) {
		inv.script_path = argv[i];
		inv.command_name = argv[i];
		++i;
	}
	inv.first_argument = i;
	return inv;
}

} // namespace lesh::runtime
