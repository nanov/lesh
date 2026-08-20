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
	virtual void set(std::string_view name, int64_t value) = 0;
};

struct arithmetic_result {
	int64_t value = 0;
	bool ok = true;
	const char* error = nullptr;  // set when ok is false
};

// POSIX specifies signed long arithmetic. Division by zero is an error rather
// than undefined behaviour, and integer bases follow C: 0x hex, 0 octal.
[[nodiscard]] arithmetic_result evaluate(std::string_view expression,
                                         arithmetic_variables& vars) noexcept;

} // namespace lesh::runtime
