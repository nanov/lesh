#pragma once

// Stepping the shared option parser ONE OPTION WORD AT A TIME (#148).
//
// `args::parse` reads every option word of an argv in one call, which is what a
// utility wants when its options are independent switches - `cd`, `pwd`, `trap`,
// `read`, `export`, `bind` and `unalias` all use it and nothing here. Three
// callers cannot:
//
//   - `set` and the shell's own command line apply `-o NAME` AS IT ARRIVES. A
//     string_view field holds one name, and `set -o allexport -o noglob` gives it
//     two - dash applies both, and so must this. The sigil is per occurrence too:
//     `set -o errexit +o xtrace` turns one on and the other off, and a stored view
//     remembers a name rather than a polarity.
//   - `kill` re-reads a word the table REFUSES as a signal name, because `-TERM`
//     and `-9` are signals standing in option position. It needs to know WHICH
//     word was refused, and a whole-argv parse reports the letter and stops.
//
// THE GRAMMAR IS STILL THE PARSER'S, and that is the whole point of showing it a
// window rather than writing a second loop: clustering, `-dX` and `-d X`, `--`,
// the lone `-`, the `+` sigil and every diagnostic come from the same table and
// the same code as `cd`'s. This decides only HOW MUCH of argv the parser is shown
// at a time, never what any of it means - which is the distinction #150 turned on,
// where two readings of one command line were free to disagree because they were
// two different rules rather than one rule applied twice.
//
// Header-only for the reason runtime/diagnostic.h is: CMakeLists.txt lists sources
// explicitly and there is nothing here that wants a translation unit.

#include "substrate/args.h"

#include <cstddef>
#include <string_view>

namespace lesh::runtime {

// What one step consumed, and whether the options are over.
struct option_word {
	args::error err{};
	// Words the group took: 0 when `cur[1]` is an operand, 1 for a plain option
	// word or `--`, 2 when the option-argument was the word after it.
	std::size_t consumed = 0;
	// The options end here. Either `cur[1]` was an operand (`consumed` 0) or it was
	// `--` (`consumed` 1, `separator` set).
	bool done = false;
	bool separator = false;
};

// `cur[1]` is the next unread word and `cur[2]` is offered only as an
// option-ARGUMENT. Advance the caller's cursor by `consumed`; once `done`, the
// operands begin at `cur[1]`.
//
// THE WINDOW IS ONE WORD, WIDENED TO TWO only when the first word ends in an
// option that needs an argument - which the parser itself says, by answering
// `missing_argument`. Offering two words up front would be wrong: in
// `set -x -o errexit` the parser would take `-o` for the second word's own
// business and find nothing after it, and `kill -l -s TERM` reads the same way.
// Widening only on demand means the second word can ONLY ever be an
// option-argument, never a second option word, so no word is read as the wrong
// thing. It also means a `missing_argument` that DOES come back is real: argv
// held no next word, which is what `set` reads as a bare `-o`.
//
// Re-parsing the first word inside the widened window applies its rows a second
// time. Every setter these tables use is idempotent - `set_bool`, `toggle`,
// `set_enum_value` and `store_view` all write a value rather than accumulate - and
// a `count` row would not be; no table in the tree has one.
template <class T, std::size_t N>
[[nodiscard]] inline option_word next_option_word(const args::spec_table<T, N>& spec,
                                                  char** cur, T& into) {
	char* const word = cur[1];
	if (word == nullptr)
		return option_word{{}, 0, true, false};
	// `--` IS THE UTILITY'S BUSINESS as well as the parser's: `set --` clears the
	// positional parameters where a bare `set` leaves them alone, so the caller has
	// to know the separator was THERE and not merely that the options ended. The
	// parser consumes it and reports the same `done` either way.
	if (std::string_view{word} == "--")
		return option_word{{}, 1, true, true};

	char* one[3] = {cur[0], word, nullptr};
	const args::scan_result first = args::parse_into(spec, one, into);
	if (!first.err) {
		const bool operand = first.rest == one + 1;
		return option_word{{}, operand ? std::size_t{0} : std::size_t{1}, operand, false};
	}
	// The parser asked for an argument the window did not hold. If argv really has
	// a next word, that word is the argument; if it does not, the error is real.
	if (first.err.kind != args::error_kind::missing_argument || cur[2] == nullptr)
		return option_word{first.err, 0, false, false};

	char* two[4] = {cur[0], word, cur[2], nullptr};
	const args::scan_result second = args::parse_into(spec, two, into);
	if (second.err)
		return option_word{second.err, 0, false, false};
	return option_word{{}, 2, false, false};
}

} // namespace lesh::runtime
