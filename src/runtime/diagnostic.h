#pragma once

#include "runtime/shell_state.h"
#include "syntax/ast.h"
#include "syntax/source_map.h"

#include <cstdarg>
#include <cstdio>

namespace lesh::runtime {

// EVERY RUNTIME DIAGNOSTIC THE SHELL PRINTS. Decided on #61, built on #76.
//
//     n.sh:3:1: nosuchcmd: not found (via alias a → b)
//
// THE FIELD, MEASURED at b662b5c, for a command not found on line 3 of a script:
//
//     dash  n.sh: 3: nosuchcmd: not found
//     zsh   n.sh:3: command not found: nosuchcmd
//     bash  n.sh: line 3: nosuchcmd: command not found
//     lesh  lesh: nosuchcmd: No such file or directory   <- its own name, no position
//
// dash and zsh already agree on the two things that matter: the INVOCATION LINE,
// and the EXPANDED command name. Through a nested alias (`alias a=b; alias
// b=nosuchcmd`) both still say `nosuchcmd` and both still say line 3. bash is not
// a data point there - it does not expand aliases in scripts at all.
//
// THE COLUMN AND THE CHAIN ARE LESH'S ADDITION, and no shell reports either. It
// can because ast.h:23 says it can: lesh keeps source offsets specifically so it
// can map its tree back to what the user typed, which is the deliberate
// difference from zsh's `struct eprog`. The chain comes from walking #40's region
// stack - see tree::invocation_of.
//
// WHICH FILE. `$0`, which #43 made follow argv[0]. Measured, again rather than
// guessed:
//
//     script    dash/bash/zsh all print the script path AS GIVEN, relative or not
//     -c        dash `/bin/dash`, bash `/bin/bash`, zsh `zsh`  - $0 in dash and bash
//     stdin     dash `/bin/dash`, bash `/bin/bash`, zsh omits the position entirely
//
// So `$0` answers all three for two of the three shells, and it is the only rule
// that needs no special case. #43 deliberately left this prefix alone, saying the
// runtime case was undecided; this is the ticket that decides it.
//
// A FREE FUNCTION OVER A BOUND STATE, rather than a shell_state parameter. Three
// reasons, and none of them is that passing one would be tedious:
//
//   - WHERE THE SHELL IS is a property of the PROCESS, not of an object one
//     caller happens to hold. There is one shell per process and it is at one
//     place at a time, which is the same fact signals.cpp's `g_pending` records
//     about a pending signal.
//   - exec_or_die runs in a FORKED CHILD, after the point where anything can be
//     handed to it, and its diagnostic is the single most important one the shell
//     prints. A binding is inherited across fork; an argument would have to be
//     computed BEFORE it, which means scanning for a line number on every
//     external command that is about to succeed.
//   - the expander holds a `parameter_source`, not a shell_state. Threading state
//     through would widen that interface for the sake of a diagnostic - the exact
//     coupling #11 broke on purpose.
//
// Header-only because CMakeLists.txt lists sources explicitly and this ticket
// does not own it. There is nothing here that wants a translation unit.

// The state a diagnostic positions itself from. shell_state's constructor binds
// it and its destructor unbinds it; nothing else should touch it.
inline const shell_state* g_diagnostic_state = nullptr;

inline void bind_diagnostics(const shell_state* state) noexcept {
	g_diagnostic_state = state;
}

// UNBINDS ONLY ITSELF. Unit tests build several shell_states in nested scopes,
// and one going out of scope must not silently disarm the diagnostics of another
// that is still running.
inline void unbind_diagnostics(const shell_state* state) noexcept {
	if (g_diagnostic_state == state)
		g_diagnostic_state = nullptr;
}

// `file:line:col: `, or `$0: ` when no command is running - startup, or a
// complaint about the shell's own command line, neither of which has a line.
//
// Exposed for a caller that has to write its message some other way; every
// ordinary one goes through report() below.
inline void write_diagnostic_prefix(std::FILE* out) {
	const shell_state* state = g_diagnostic_state;
	if (state == nullptr) {
		// Before a shell exists at all. Only the harness and a test can see this.
		std::fputs("lesh: ", out);
		return;
	}
	const std::string_view name = state->origin_file();
	const syntax::source_position at = state->where();
	if (at.line == 0) {
		std::fprintf(out, "%.*s: ", static_cast<int>(name.size()), name.data());
		return;
	}
	std::fprintf(out, "%.*s:%u:%u: ", static_cast<int>(name.size()), name.data(),
	             at.line, at.column);
}

// The aliases the running command was reached through, as ` (via alias a → b)`,
// or nothing when it was typed as it stands.
//
// AFTER the message rather than inside the prefix: the prefix is a position, and
// `n.sh:3:1:` has to stay the shape an editor can jump to. What the chain adds is
// an answer to "the script says `a`, why does this say `nosuchcmd`?" - which is
// only a question once the message has been read.
inline void write_alias_chain(std::FILE* out) {
	const shell_state* state = g_diagnostic_state;
	if (state == nullptr)
		return;
	syntax::invocation_site site;
	(void)state->where(&site);
	if (site.depth == 0)
		return;
	std::fputs(" (via alias ", out);
	for (uint32_t i = 0; i < site.depth; ++i) {
		if (i != 0)
			std::fputs(" → ", out);
		std::fprintf(out, "%.*s", static_cast<int>(site.chain[i].size()),
		             site.chain[i].data());
	}
	std::fputc(')', out);
}

inline void vreport(const char* fmt, std::va_list args) {
	write_diagnostic_prefix(stderr);
	std::vfprintf(stderr, fmt, args);
	write_alias_chain(stderr);
	std::fputc('\n', stderr);
}

// The message, positioned. `fmt` carries NO TRAILING NEWLINE: the alias chain
// goes after it and the newline after that.
//
// The format attribute is not decoration. Every diagnostic in the shell was moved
// here mechanically from a `std::fprintf(stderr, "lesh: ...")`, and it is what
// makes the compiler re-check all of them rather than the reader.
__attribute__((format(printf, 1, 2)))
inline void report(const char* fmt, ...) {
	std::va_list args;
	va_start(args, fmt);
	vreport(fmt, args);
	va_end(args);
}

} // namespace lesh::runtime
