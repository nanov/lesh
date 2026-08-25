#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace lesh::syntax {

// A byte offset in the source, as a place a person can point at. See issue #76.
//
// BOTH FIELDS ARE 1-BASED. Every tool whose diagnostics read `file:line:col`
// numbers both from one, and an editor told to jump to `n.sh:3:1` expects the
// first character of the third line.
struct source_position {
	uint32_t line = 1;
	uint32_t column = 1;
};

// Byte offset -> (line, column) over one source buffer.
//
// ONE MAPPER, THREE CONSUMERS, and building it once is the point: `$LINENO`, the
// LINE in a runtime diagnostic and the COLUMN in it are one question, and this
// project has paid repeatedly for one question answered N ways.
//
// WHY A MAPPER RATHER THAN A COUNTER. A running count of newlines as the lexer
// consumes them cannot answer `LINENO after expansions`: a command substitution
// spanning three physical lines has to advance the number by three, and by the
// time the command RUNS the counter has moved on past it. The line has to come
// from the OFFSET of the command being run, which is a lookup, not a tally. lesh
// can ask it at all only because every node carries a span - see ast.h's opening
// comment, which records that this is the deliberate difference from zsh's
// `struct eprog`.
//
// NO LINE-START TABLE, and that is the design rather than an omission.
//
// A table would have to be built, which means allocating, and the command path
// must not allocate (tests/unit/allocation_tests.cpp). Moving the table's
// construction earlier - once per input rather than once per diagnostic - would
// satisfy the letter of that, but the table is not needed at all: this is
// consulted only when a script READS `$LINENO` or when the shell PRINTS a
// diagnostic. Neither is per-command, so nothing hot is paying for a scan.
//
// What makes the scan cheap anyway is a three-word MEMO of the last answer, and
// it works in BOTH directions. Execution moves locally: down a script, and back
// to the top of a loop body. A forward-only memo would rescan from byte zero on
// every iteration of a loop near the end of a long script; stepping backwards
// line by line instead costs the size of the loop body. The memo is `mutable`
// because it changes nothing an observer can see - two calls with the same offset
// give the same answer - so the queries stay const.
//
// A LINE-START TABLE IS THE ALTERNATIVE, and it is left as an optimisation
// rather than rejected. Measured, so that whoever picks it up starts from a
// number: on a 20,000-line script whose execution ALTERNATES between line 2 and
// line 20,005 - a function at the top called from a loop at the bottom, reading
// `$LINENO` at both ends, which is the shape the memo is worst at - the mapper
// costs 0.12s over 20,000 iterations against a 2.63s baseline. Under 5%, on a
// script written specifically to defeat it, and below the noise floor at 2,000
// iterations. A binary search over a table would make that O(log n), at the cost
// of O(lines) of memory that has nowhere good to live: the arena is 32 KB
// (BUFFER_POOL_SIZE) and overflows to malloc, which is the ONE counter
// arena.h:26 says to watch, and a table for this script would be 40 KB on its
// own. It would also have to be keyed on the SOURCE rather than owned by the
// tree, because run_input parses one tree per COMMAND over a single shared
// source - so a per-tree table would be built once per command, not once per
// script.
class source_map {
public:
	explicit source_map(std::string_view source) noexcept : _source(source) {}

	[[nodiscard]] std::string_view source() const noexcept { return _source; }

	[[nodiscard]] source_position at(uint32_t offset) const noexcept {
		// CLAMPED rather than trusted. An offset above the input is an alias body's
		// (tree::add_text_region), and while a caller should resolve that to its
		// invocation site first, a mapper that walked off the buffer looking for a
		// newline would turn a diagnostic into a crash. Naming the last line is the
		// worse answer only in the case that should never arrive.
		offset = static_cast<uint32_t>(std::min<size_t>(offset, _source.size()));

		// Backwards first: step whole lines back until the memo's line contains the
		// offset. `_line_start` is zero only on line 1, and no offset is below zero,
		// so the loop cannot underflow.
		while (offset < _line_start) {
			uint32_t start = _line_start - 1;  // the newline that ended the line above
			while (start > 0 && _source[start - 1] != '\n')
				--start;
			_line_start = start;
			--_line;
		}
		// Then forwards, from the START of the memo's line rather than from the memo
		// itself: an offset earlier on the same line is already answered, and
		// starting from the line start makes that the same code path as any other.
		uint32_t line = _line;
		uint32_t start = _line_start;
		for (uint32_t i = start; i < offset; ++i)
			if (_source[i] == '\n') {
				++line;
				start = i + 1;
			}
		_line = line;
		_line_start = start;

		return {line, column_at(start, offset)};
	}

	[[nodiscard]] uint32_t line_at(uint32_t offset) const noexcept {
		return at(offset).line;
	}

private:
	// COLUMNS COUNT CHARACTERS, NOT BYTES, and the case that distinguishes them is a
	// UTF-8 line with a multi-byte character ahead of the fault.
	//
	// The format `file:line:col` is a convention with readers, and every one of them
	// counts characters: GCC's default is `-fdiagnostics-column-unit=display`, and
	// Vim's error format, Emacs' compile mode and every editor's problem matcher
	// place the caret by character. A byte column puts the caret in the wrong place
	// on every line holding a non-ASCII character, which on a shell script is a
	// comment or a message in any language but English.
	//
	// CHARACTERS, not display columns: a wide CJK character counts one, not two.
	// Display width needs a width table, the shell has none, and inventing one for
	// a diagnostic would be a second answer to a question nothing else here asks.
	//
	// The whole cost is this test. A UTF-8 continuation byte is `10xxxxxx` and
	// nothing else is, so a byte that is not one begins a character. Invalid UTF-8
	// degrades to counting bytes, which is the right failure: the shell is
	// byte-oriented everywhere else and must not reject input it can run.
	[[nodiscard]] uint32_t column_at(uint32_t line_start, uint32_t offset) const noexcept {
		uint32_t column = 1;
		for (uint32_t i = line_start; i < offset; ++i)
			if ((static_cast<unsigned char>(_source[i]) & 0xC0) != 0x80)
				++column;
		return column;
	}

	std::string_view _source;
	// The last answer, as the line it fell on and where that line begins. Line 1
	// begins at byte 0, which is what makes the initial state a valid memo rather
	// than a case the queries have to test for.
	mutable uint32_t _line = 1;
	mutable uint32_t _line_start = 0;
};

} // namespace lesh::syntax
