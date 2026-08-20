#pragma once

#include "runtime/arithmetic.h"

#include "substrate/arena.h"
#include "substrate/arena_array.h"
#include "syntax/ast.h"

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
	virtual void assign_parameter(std::string_view name, std::string_view value) = 0;
};

enum class expansion_status {
	ok,
	command_substitution_unavailable,  // no runner supplied - completion's mode
	unsupported_construct,             // a construct that is not implemented
	parameter_unset,                   // ${x?message} fired
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
	         parameter_assigner* assign = nullptr) noexcept
		: _pool(pool), _params(params), _runner(runner), _glob_enabled(glob_enabled),
		  _vars(vars), _assign(assign) {}

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
	expansion_status expand_text(std::string_view text, bool quoted,
	                             arena_array<std::string_view>& out) noexcept;
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
