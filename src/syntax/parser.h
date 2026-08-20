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

} // namespace lesh::syntax
