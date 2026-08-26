#include "runtime/invocation.h"

#include "runtime/option_word.h"

#include <string_view>

namespace lesh::runtime {

namespace {

// The shell's own command line, as a table (#148).
//
// It reads the SAME option letters `set` does, from the same
// shell_state::option_table(), because POSIX gives them the same set and two
// tables would drift - `sh -m` and `set -m` disagreeing about which letters exist
// is the failure this arrangement exists to prevent. What is here and not in
// `set` is the three letters that belong to an INVOCATION rather than to a
// running shell: `-c`, `-i` and `-s`.
struct invocation_scan : shell_state::options {
	std::string_view name{};      // -o NAME
	bool command_string = false;  // -c
	bool interactive = false;     // -i / +i
	bool read_stdin = false;      // -s / +s
};

// `shell_option<&shell_state::options::trace>` - the same trick `set`'s table
// uses, and for the same reason: a row must bind the struct the spec names, and
// `&shell_state::options::trace` has type `bool options::*` however it is spelled.
template <bool shell_state::options::*Member>
inline constexpr auto shell_option =
	args::field<static_cast<bool invocation_scan::*>(Member)>;

constexpr auto kInvocation = args::spec<invocation_scan>(
	args::option{'a', shell_option<&shell_state::options::all_export>, args::toggle}
		.help("export every name that is assigned"),
	args::option{'b', shell_option<&shell_state::options::notify>, args::toggle}
		.help("report terminated background jobs at once (recorded, inert)"),
	args::option{'C', shell_option<&shell_state::options::no_clobber>, args::toggle}
		.help("refuse to truncate an existing file with >"),
	args::option{'e', shell_option<&shell_state::options::exit_on_error>, args::toggle}
		.help("exit when a command fails"),
	args::option{'f', shell_option<&shell_state::options::no_glob>, args::toggle}
		.help("disable pathname expansion"),
	args::option{'h', shell_option<&shell_state::options::hash_all>, args::toggle}
		.help("hash commands as functions are defined (recorded, inert)"),
	args::option{'m', shell_option<&shell_state::options::monitor>, args::toggle}
		.help("job control (recorded, inert)"),
	args::option{'n', shell_option<&shell_state::options::no_exec>, args::toggle}
		.help("read commands without executing them"),
	args::option{'u', shell_option<&shell_state::options::error_on_unset>, args::toggle}
		.help("treat an unset parameter as an error"),
	args::option{'v', shell_option<&shell_state::options::verbose>, args::toggle}
		.help("echo input lines as they are read"),
	args::option{'x', shell_option<&shell_state::options::trace>, args::toggle}
		.help("trace commands as they are executed"),
	// POSIX spells the command string `-c`; `+c` is not an option at all, so the
	// row declares no `+` sigil and the parser refuses one.
	args::option{'c', args::field<&invocation_scan::command_string>}
		.help("read the commands from the first operand"),
	args::option{'i', args::field<&invocation_scan::interactive>, args::toggle}
		.help("run interactively"),
	args::option{'s', args::field<&invocation_scan::read_stdin>, args::toggle}
		.help("read the commands from standard input"),
	args::option{'o', args::field<&invocation_scan::name>, args::value("NAME")}
		.plus_sigil()
		.help("set the option NAME names"));

} // namespace

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
	// and bash both keep; and it ran the hyphen in `-c - code`. Both stay gone here,
	// because the option list ends at the first operand by construction and the
	// command string IS that operand.

	// TWO SEEDS, ONE TABLE. `-i` and `+i` need a THIRD answer - "neither was given"
	// - because POSIX then decides interactiveness from isatty rather than from the
	// command line, and this parser must not pretend to know. A `toggle` binds a
	// bool and a bool holds two answers, so the same table is stepped over the same
	// words into two structs seeded oppositely: a field that comes back the SAME
	// either way was written, and one that comes back as it was seeded was not.
	//
	// This is not #150's bug wearing a new hat. That was two DIFFERENT rules reading
	// one command line and disagreeing; this is one rule applied twice, which cannot.
	// It costs a few dozen byte compares, once, at startup.
	invocation_scan scan;
	invocation_scan probe;
	probe.interactive = true;

	// THE ARRAY IS NULL-TERMINATED, and the parser reads it through that terminator
	// rather than through `argc`. ISO C 5.1.2.2.1 and POSIX XBD 8 both require
	// argv[argc] to be a null pointer, so this is the same array every other option
	// scan in the tree walks; `argc` is still what the operand indices are reported
	// against.
	char** cur = argv;
	for (;;) {
		const option_word step = next_option_word(kInvocation, cur, scan);
		(void)next_option_word(kInvocation, cur, probe);
		if (step.err) {
			// `-o` is the only row that takes an argument, so a missing one is its.
			if (step.err.kind == args::error_kind::missing_argument) {
				inv.error = "-o requires an option name";
				return inv;
			}
			// The whole WORD, which is what this parser has always named - `-xZ` and
			// `--nosuch` read better than the letter alone on a command line nobody
			// has run yet.
			inv.error = "invalid option";
			inv.error_operand = cur[1];
			return inv;
		}
		if (scan.name.data() != nullptr) {
			// One option table, shared with the `set` builtin, so `sh -o monitor` and
			// `set -o monitor` cannot disagree about which names exist. The sigil is
			// read off the word the stored view points into - `+o` clears.
			const bool enable = cur[1][0] == '-';
			// #165: whether the command line NAMED this one, which main() needs to
			// know before it writes the interactive default over it. Recorded here
			// rather than inferred from the value, because "off" and "not named"
			// are two different answers on an interactive shell.
			if (scan.name == "leshnici")
				inv.leshnici = enable;
			if (!shell_state::apply_option_name(scan, scan.name, enable)) {
				inv.error = "invalid option name";
				inv.error_operand = scan.name.data();
				return inv;
			}
			// The probe carries the same options; only `interactive` differs.
			static_cast<shell_state::options&>(probe) =
				static_cast<const shell_state::options&>(scan);
			scan.name = {};
			probe.name = {};
		}
		cur += step.consumed;
		if (step.done) {
			// A lone `-` or `+` ends the options too, and POSIX's obsolete `set -`
			// spelling means nothing on a command line, so it is consumed rather than
			// left to be read as the script's pathname. The parser stops at it - a
			// one-character word is an operand - and this is the utility saying what
			// its own first operand means, the way `trap` says a lone `-` is a reset.
			if (!step.separator && cur[1] != nullptr) {
				const std::string_view first{cur[1]};
				if (first == "-" || first == "+")
					cur += 1;
			}
			break;
		}
	}

	inv.options = scan;
	inv.read_stdin = scan.read_stdin;
	// Written by either sigil, or by neither - see the two seeds above.
	if (scan.interactive == probe.interactive)
		inv.interactive = scan.interactive;

	int i = static_cast<int>(cur + 1 - argv);

	// Operand handling, and the part of POSIX's invocation grammar lesh had wrong:
	// with -c the first operand is the command string and the SECOND is $0
	// (command_name), NOT $1. Without -c the first operand is the script to run,
	// and a script's pathname is its own $0.
	if (scan.command_string) {
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
