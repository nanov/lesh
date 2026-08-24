#pragma once

#include <cstdint>
#include <string_view>

namespace lesh::runtime {

// Arithmetic expansion: $(( )). See issue #30.
//
// A separate mini-parser, not part of the shell grammar. Arithmetic is a
// genuinely different language - infix with precedence, no words, no quoting,
// variables read WITHOUT `$` - so folding it into the shell grammar would cost
// more than it saves. Every shell surveyed in #14 reaches the same conclusion.

// Reading and writing variables during evaluation. Arithmetic both reads
// (`$((i + 1))`) and ASSIGNS (`$((i = 3))`, `$((i += 1))`), which is why this is
// its own port rather than the expander's read-only parameter_source.
class arithmetic_variables {
public:
	virtual ~arithmetic_variables() = default;
	// An unset or non-numeric variable evaluates to 0 rather than erroring, which
	// is what makes `i=$((i+1))` work without initialising i first.
	[[nodiscard]] virtual int64_t get(std::string_view name) const = 0;
	// False when the write was REFUSED - a readonly variable - having already
	// reported it. `$((x=1))` on a readonly x is a variable assignment error, and
	// POSIX makes that fatal to a non-interactive shell, so the evaluator has to
	// be able to see the refusal rather than compute a value nothing stored.
	[[nodiscard]] virtual bool set(std::string_view name, int64_t value) = 0;
	// Whether the variable exists at all. get() cannot say - it answers 0 for
	// "unset" and for "set to 0" alike - and `set -u` has to tell them apart.
	[[nodiscard]] virtual bool defined(std::string_view name) const = 0;
};

struct arithmetic_result {
	int64_t value = 0;
	bool ok = true;
	const char* error = nullptr;  // set when ok is false
	// The first variable READ that was not set. Reported rather than acted on:
	// POSIX makes it an error under `set -u` and 0 otherwise, and which of those
	// applies is the caller's policy, not the evaluator's.
	std::string_view unset_name;
	// True when the expression tried to ASSIGN to a variable that refused the
	// write. Separate from `error` because the caller acts on it differently: a
	// malformed expression is a bad expansion, while a refused assignment is a
	// variable assignment error POSIX makes fatal to a non-interactive shell.
	bool assignment_refused = false;
};

// POSIX specifies signed long arithmetic. Division by zero is an error rather
// than undefined behaviour, and integer bases follow C: 0x hex, 0 octal.
//
// `&&`, `||` and `?:` SHORT-CIRCUIT, which is observable because arithmetic can
// assign: `$((0 && (x=1)))` leaves x alone. The skipped operand is still parsed,
// so a malformed one still fails, but nothing it describes is done - no write, no
// read for the caller's `set -u` to act on, and no division by zero. See #56.
[[nodiscard]] arithmetic_result evaluate(std::string_view expression,
                                         arithmetic_variables& vars) noexcept;

} // namespace lesh::runtime
