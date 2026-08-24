#pragma once

#include "runtime/arithmetic.h"

#include "substrate/arena.h"
#include "substrate/arena_array.h"
#include "syntax/ast.h"
#include "syntax/lexer.h"

#include <string_view>

namespace lesh::runtime {

// Word expansion. See issue #11.
//
// The expander is a pure function of (word node, shell state) producing zero or
// more FIELDS. It is called by the executor PER COMMAND at execution time, not
// as a stage that runs once: POSIX requires a loop body to re-expand on every
// iteration, and `$?` changes between commands.
//
// It depends on two ports rather than on the executor or on a concrete state
// type. That keeps it testable in isolation, and it breaks a cycle every shell
// surveyed in #14 has: command substitution must run a command, and running a
// command must expand words.

// What the expander needs from shell state, as a port - so it does not depend on
// the state's representation, which #12 has not designed yet, and so tests can
// supply a fake.
class parameter_source {
public:
	virtual ~parameter_source() = default;
	// False when the parameter is unset. An unset parameter expands to nothing;
	// whether that is an error under `set -u` is the caller's policy, not ours.
	[[nodiscard]] virtual bool lookup(std::string_view name, std::string_view& value) const = 0;
	[[nodiscard]] virtual std::string_view home_directory() const = 0;
	// Field separators. POSIX defaults to space, tab, newline when IFS is unset.
	[[nodiscard]] virtual std::string_view ifs() const = 0;

	// Special and positional parameters. Separate from lookup() because they are
	// not variables: they have no names in the variable table, `shift` renumbers
	// them, and a function replaces them for the duration of a call.
	[[nodiscard]] virtual int last_status_value() const = 0;
	[[nodiscard]] virtual int process_id_value() const = 0;
	[[nodiscard]] virtual size_t positional_count() const = 0;
	// 1-based, matching $1. Returns false past the end.
	[[nodiscard]] virtual bool positional_at(size_t index, std::string_view& out) const = 0;
	[[nodiscard]] virtual std::string_view script_name_value() const = 0;
	// `$-`: the letters of the shell options currently on. A parameter, not a
	// query about options, which is why it belongs on this port rather than
	// reaching into shell state - the expander must not know what an option is.
	[[nodiscard]] virtual std::string_view option_flags() const = 0;
};

// The port that breaks the cycle.
//
// Rather than the expander depending on the executor, the executor supplies
// this. Completion supplies NOTHING, and that is a supported mode: expanding
// without the power to execute is exactly what a line editor needs, and it is
// how Oils' CompletionWordEvaluator and fish's FAIL_ON_CMDSUBST both work.
// Running a destructive command substitution merely to offer a completion would
// be catastrophic, so the type system prevents it rather than a convention
// someone has to remember.
class command_runner {
public:
	virtual ~command_runner() = default;
	[[nodiscard]] virtual bool run_and_capture(std::string_view code,
	                                           arena_array<char>& out) = 0;
};

// Assigning to a parameter from ${x=default}. Separate from arithmetic's port
// because it writes strings rather than integers.
class parameter_assigner {
public:
	virtual ~parameter_assigner() = default;
	// False when the assignment was REFUSED - a readonly variable - having already
	// reported it. POSIX makes that a variable assignment error, which is fatal to
	// a non-interactive shell, so `readonly x; : ${x=1}; echo not reached` must
	// print nothing: the expander cannot decide that without a status back.
	[[nodiscard]] virtual bool assign_parameter(std::string_view name,
	                                            std::string_view value) = 0;
};

enum class expansion_status {
	ok,
	command_substitution_unavailable,  // no runner supplied - completion's mode
	unsupported_construct,             // a construct that is not implemented
	parameter_unset,                   // ${x?message} fired
	// An unterminated construct INSIDE an expansion, or expansion nested deeper
	// than the shell will follow. Separate from unsupported_construct because the
	// INPUT is at fault rather than the shell, and POSIX makes it fatal to a
	// non-interactive shell the way a syntax error is (#48).
	malformed_expansion,
};

class expander {
public:
	// `vars` is optional and separate from `params`, because arithmetic ASSIGNS
	// (`$((i += 1))`) while parameter expansion only reads. Completion supplies
	// none, and arithmetic then evaluates against zeroes rather than mutating
	// state as a side effect of drawing a suggestion.
	expander(buffer_pool& pool, const parameter_source& params,
	         command_runner* runner = nullptr, bool glob_enabled = true,
	         arithmetic_variables* vars = nullptr,
	         parameter_assigner* assign = nullptr,
	         bool unset_is_error = false) noexcept
		: _pool(pool), _params(params), _runner(runner), _glob_enabled(glob_enabled),
		  _vars(vars), _assign(assign), _unset_is_error(unset_is_error) {}

	// True when an expansion reported an error POSIX makes FATAL to a
	// non-interactive shell: `${x?message}` fired, or `set -u` met an unset
	// parameter.
	//
	// Sticky, and separate from the returned expansion_status, because
	// expand_assignment_value() returns a VALUE and has no status to give back -
	// so a redirection target or an assignment right-hand side would otherwise
	// swallow the error the caller has to act on.
	[[nodiscard]] bool fatal_error() const noexcept { return _fatal_error; }

	// Expands one word node, appending its fields to `out`.
	//
	// Zero fields is a normal result: an unquoted unset parameter expands to
	// nothing at all, which is why `echo $unset` passes no arguments rather than
	// one empty one.
	expansion_status expand_word(const syntax::tree& t, syntax::node_index word,
	                             arena_array<std::string_view>& out) noexcept;

	// Expands raw text as a single word with NO field splitting and NO pathname
	// expansion, which is what an assignment's value requires: `x=a b` assigns
	// "a" and runs `b`, but `x="a b"` assigns "a b" as one value, and neither is
	// globbed. Used for assignment right-hand sides.
	[[nodiscard]] std::string_view expand_assignment_value(std::string_view text) noexcept;

private:
	// `mode` is how the TEXT is lexed; `quoted` is whether field splitting and
	// pathname expansion apply. They are separate because an assignment value is
	// expanded with quoted=true while its single quotes still mean single quotes.
	expansion_status expand_text(std::string_view text, bool quoted,
	                             arena_array<std::string_view>& out,
	                             syntax::lex_mode mode
	                                 = syntax::lex_mode::word_interior) noexcept;
	void append(std::string_view bytes) noexcept;
	void append_split(std::string_view bytes, arena_array<std::string_view>& out) noexcept;
	void finish_field(arena_array<std::string_view>& out) noexcept;

	buffer_pool& _pool;
	const parameter_source& _params;
	command_runner* _runner;
	// `set -f` disables pathname expansion entirely.
	bool _glob_enabled = true;
	arithmetic_variables* _vars = nullptr;
	parameter_assigner* _assign = nullptr;
	// `set -u`: expanding an unset parameter is an error rather than an empty
	// string. Held as state rather than asked of the parameter_source, because
	// POSIX makes it the CALLER's policy - completion expands the same words
	// without wanting the shell to die over an unset variable.
	bool _unset_is_error = false;
	bool _fatal_error = false;

	// Reports an unset parameter under `set -u` and records the fatal error.
	// Returns true when it fired, so a caller can skip substituting nothing.
	bool report_unset(std::string_view name) noexcept;

	// Reports an unterminated construct found inside an expansion and records the
	// fatal error. Worded exactly as the parser words it, because which layer
	// caught it is lesh's business and not the user's.
	void report_malformed(syntax::token_error error) noexcept;

	// How deep expansion may nest.
	//
	// The expander is re-entrant BY DESIGN - a parameter default, an assignment
	// value and arithmetic's inner text all re-enter expand_text - so nesting in
	// the input is nesting on the C++ stack, and refusing malformed input bounds
	// the recursion only by the LENGTH of the input, which is not a bound. Measured
	// on the perfectly well-formed `${x-${x-...hi...}}`: 1500 levels expanded,
	// 2000 overflowed the stack on the debug build. dash is not a counterexample,
	// only a bigger frame budget - it prints `hi` at 16000 and takes SIGSEGV at
	// 18000, so the reference implementation has this bug too and answers it with
	// a crash.
	//
	// 256 mirrors the executor's kMaxFunctionDepth, for the same reason and with
	// the same shape of answer: a diagnostic and a non-zero status, never a silent
	// empty result. Human-written expansion nests less than ten deep.
	static constexpr int kMaxExpansionDepth = 256;
	int _depth = 0;

	bool lookup_parameter(std::string_view name, std::string_view& out) noexcept;
	std::string_view int_to_scratch(int value) noexcept;

	// Accumulates the field under construction. Completed fields are copied into
	// exact-size arena blocks, because this buffer relocates as it grows and a
	// string_view into it would dangle.
	arena_array<char>* _current = nullptr;
	bool _field_started = false;
	// Set when a glob metacharacter arrived from unquoted text. Quoted ones are
	// literal, so `echo "*.txt"` must not touch the filesystem.
	bool _field_globbable = false;
};

} // namespace lesh::runtime
