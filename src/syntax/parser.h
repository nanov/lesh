#pragma once

#include "substrate/arena.h"
#include "syntax/ast.h"
#include "syntax/lexer.h"

#include <string_view>

namespace lesh::syntax {

// Parses shell input into a tree. See issue #10.
//
// THE PARSER NEVER FAILS. Invalid input produces error nodes and the parse
// continues; incomplete input sets tree::incomplete(). There is no error return
// and nothing throws. That is not politeness - it is what makes the same parser
// usable by a line editor, which must parse input that is by definition
// half-typed, on every keystroke.
//
// Error recovery is built in rather than retrofitted. Retrofitting it is
// notoriously worse: Oils, whose front end is otherwise the most deliberate of
// any shell surveyed, runs its real parser for completion and catches the
// exception, then reads a side-channel to find out where it got to.
// Supplies alias definitions to the parser. POSIX alias substitution happens at
// READ time and is lexical, so it belongs to the parser rather than to any later
// stage - which is why this is a port rather than a lookup on shell state.
//
// Completion may pass nullptr: expanding aliases while drawing a suggestion is
// safe, but not doing it keeps the tree's spans pointing at what the user typed.
class alias_source {
public:
	virtual ~alias_source() = default;
	[[nodiscard]] virtual bool lookup_alias(std::string_view name,
	                                        std::string_view& value) const = 0;
};

tree parse(buffer_pool& pool, std::string_view source,
           const alias_source* aliases = nullptr) noexcept;

// Parses ONE complete command out of `source`, starting at `position`, and moves
// `position` past what was read.
//
// A shell does not read its input all at once: POSIX substitutes an alias when
// the command containing it is READ, so `alias e=echo` on one line is in effect
// for the next line and not for the rest of its own. Parsing a whole script first
// made every such script wrong, which is the larger of the two reasons alias-p.tst
// scored 13/67 (#40; the other was that a token from an alias body could not be
// read back at all - see tree::add_text_region). dash reads the same unit,
// and so does `-c`: `dash -c 'alias e=echo<newline>e hi'` prints hi, while
// `dash -c 'alias e=echo; e hi'` reports `e: not found`.
//
// The unit is a complete command terminated by a NEWLINE of the input, not by
// `;`, and a newline inside a compound command or inside an alias body does not
// end one.
//
// `position` reaches `source.size()` when nothing executable is left, which is
// what ends the caller's loop. The whole source is passed every time so that
// spans stay offsets into the real input.
tree parse_next_command(buffer_pool& pool, std::string_view source, size_t& position,
                        const alias_source* aliases = nullptr) noexcept;

// True for one of the shell's reserved words.
//
// Exported because `command -v if` must print `if`, and the RUNTIME has no other
// way to ask: a reserved word is recognised by POSITION here rather than by the
// lexer, so the table lives inside the parser. Asking through this rather than
// keeping a second list in the runtime is the whole point - two lists of the
// sixteen keywords would drift, which is exactly how `test` and `readonly` came
// to be classified with no implementation (#35).
[[nodiscard]] bool is_reserved_word(std::string_view text) noexcept;

} // namespace lesh::syntax
