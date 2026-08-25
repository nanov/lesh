#pragma once
#include <array>

namespace lesh {
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