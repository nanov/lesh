#pragma once
#include <array>
#include <string_view>

namespace lesh {

// snake_case: a lowercase letter, then lowercase letters, digits, underscores.
//
// Narrow on purpose, and ONE copy of it (the reorg cleanup). The names are what a
// user types into a binding, what an rc file re-sources idempotently, and what a
// prompt template names a module by; a name space that admits hyphens as well
// would make `delete-backward-word` and `delete_backward_word` two actions that
// look like one, which is the failure worth designing out rather than
// documenting around. `leshper/registry.cpp` and `ui/prompt/prompt.cpp` each had
// their own copy of this rule, which is one copy too many for a rule that must
// not drift.
[[nodiscard]] constexpr bool is_snake_case(std::string_view name) noexcept {
	if (name.empty() || name[0] < 'a' || name[0] > 'z')
		return false;
	for (const char one : name) {
		const bool ok = (one >= 'a' && one <= 'z') || (one >= '0' && one <= '9') || one == '_';
		if (!ok)
			return false;
	}
	return true;
}

	class string_utils {
	public:
		static bool is_valid_var_name_first_char(const unsigned char c) {
			constexpr bool VALID_FIRST_CHAR[256] = {
				['A'] = true, ['B'] = true, ['C'] = true, ['D'] = true, ['E'] = true,
				['F'] = true, ['G'] = true, ['H'] = true, ['I'] = true, ['J'] = true,
				['K'] = true, ['L'] = true, ['M'] = true, ['N'] = true, ['O'] = true,
				['P'] = true, ['Q'] = true, ['R'] = true, ['S'] = true, ['T'] = true,
				['U'] = true, ['V'] = true, ['W'] = true, ['X'] = true, ['Y'] = true, ['Z'] = true,

				['a'] = true, ['b'] = true, ['c'] = true, ['d'] = true, ['e'] = true,
				['f'] = true, ['g'] = true, ['h'] = true, ['i'] = true, ['j'] = true,
				['k'] = true, ['l'] = true, ['m'] = true, ['n'] = true, ['o'] = true,
				['p'] = true, ['q'] = true, ['r'] = true, ['s'] = true, ['t'] = true,
				['u'] = true, ['v'] = true, ['w'] = true, ['x'] = true, ['y'] = true, ['z'] = true,

				['_'] = true
			};
			return VALID_FIRST_CHAR[c];
		}

		static bool is_valid_var_name_non_first_char(const unsigned char c) {
			constexpr bool VALID_NON_FIRST_CHAR[256] = {
				// Include all the values from VALID_FIRST_CHAR
				['A'] = true, ['B'] = true, ['C'] = true, ['D'] = true, ['E'] = true,
				['F'] = true, ['G'] = true, ['H'] = true, ['I'] = true, ['J'] = true,
				['K'] = true, ['L'] = true, ['M'] = true, ['N'] = true, ['O'] = true,
				['P'] = true, ['Q'] = true, ['R'] = true, ['S'] = true, ['T'] = true,
				['U'] = true, ['V'] = true, ['W'] = true, ['X'] = true, ['Y'] = true, ['Z'] = true,

				['a'] = true, ['b'] = true, ['c'] = true, ['d'] = true, ['e'] = true,
				['f'] = true, ['g'] = true, ['h'] = true, ['i'] = true, ['j'] = true,
				['k'] = true, ['l'] = true, ['m'] = true, ['n'] = true, ['o'] = true,
				['p'] = true, ['q'] = true, ['r'] = true, ['s'] = true, ['t'] = true,
				['u'] = true, ['v'] = true, ['w'] = true, ['x'] = true, ['y'] = true, ['z'] = true,

				['_'] = true,

				// Plus digits for non-first position
				['0'] = true, ['1'] = true, ['2'] = true, ['3'] = true, ['4'] = true,
				['5'] = true, ['6'] = true, ['7'] = true, ['8'] = true, ['9'] = true
			};
			return VALID_NON_FIRST_CHAR[c];
		}
	};
}