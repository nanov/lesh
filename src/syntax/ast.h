#pragma once

#include "substrate/arena_array.h"
#include "syntax/token.h"

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace lesh::syntax {

// The parse tree. See issue #10.
//
// Nodes live in one contiguous array and refer to each other by index, not by
// pointer. Indices are half the size, survive reallocation, and keep the tree
// relocatable and comparable. Children of a node are contiguous in a second flat
// array, so iterating a command's words is a linear walk rather than a pointer
// chase. This is the shape Zig's AST and rust-analyzer converged on, and it is
// what the data-oriented constraint in the scope spec actually asks for.
//
// Every node carries the token range it spans, and the tree keeps its tokens.
// That is not bookkeeping: zsh needs 3,093 lines of zle_tricky.c specifically
// because `struct eprog` carries no source offsets, so it cannot map its tree
// back to what the user typed - it regenerates canonical source in text.c and
// punts highlighting to userland. fish avoids a second parser entirely because
// its tree carries ranges. Spans are the difference between those two outcomes.

using node_index = uint32_t;
inline constexpr node_index no_node = 0xFFFFFFFFu;

enum class node_kind : uint16_t {
	error,           // a construct the parser could not make sense of
	program,         // the whole input: a sequence of and_or lists
	and_or,          // pipelines joined by && and ||
	pipeline,        // commands joined by |
	simple_command,  // a command name with arguments, assignments, redirections
	word,            // one word of a command; `aux` is its word_role
	assignment,      // NAME=value preceding a command
	redirect,        // an operator with its target

	// --- compound commands (#19) ---------------------------------------------
	// A sequence of and_or lists. The body of every construct below, and what
	// `program` itself is - so the same walker handles all of them.
	compound_list,
	// children: [cond, body, cond, body, ..., else_body?]
	// `aux` holds the number of (cond, body) pairs, so the optional trailing else
	// is whatever remains. That beats a distinct node kind per elif depth.
	if_clause,
	while_loop,      // children: [cond, body]
	until_loop,      // children: [cond, body]
	// children: [word, word, ..., body]. `aux` is the token index of the loop
	// variable's name; the body is always the last child.
	for_loop,
	case_clause,     // children: [subject, case_item, case_item, ...]
	case_item,       // children: [pattern, pattern, ..., body]; `aux` = pattern count
	subshell,        // children: [compound_list] - runs in its own environment
	brace_group,     // children: [compound_list] - runs in this one
	// A here-document. `aux` packs the body's source range, recorded separately
	// from the node's token range because the body is not made of tokens - the
	// lexer never sees it. See here_doc_body below.
	here_doc,
	// `name() compound-command`. children: [body]. `aux` is the name's token.
	function_definition,
	// A list terminated by `&`: runs in the background and is not waited for.
	async_list,
	// `! pipeline`. children: [pipeline]. POSIX makes `!` part of the pipeline
	// grammar rather than an operator, so it is a reserved WORD recognised by
	// position - the same rule `if` and `}` follow. Its own node kind rather than a
	// flag on `pipeline`, because a negated single command is not wrapped in a
	// pipeline node at all.
	negation,
};

// What the GRAMMAR makes of a word, which decides how the expander treats it.
//
// A `case` pattern is a pattern and one value: quoting inside it becomes a
// backslash escape rather than nothing - the matcher's only channel for "this
// asterisk is data" - and it is neither field-split nor pathname-expanded, so a
// pattern that expands to nothing is an EMPTY pattern rather than no pattern at
// all. Nothing about the word itself says so: `*)` and the `*` in `echo *` are
// the same token and the same node, and only the position the parser read them
// in tells them apart.
//
// It therefore travels on the node, the way `for_loop` carries whether an `in`
// clause was written, rather than as an argument every caller of expand_word has
// to remember. Marking it at the one place that can know it beats spreading the
// knowledge over the callers, and there is no way to expand a case pattern as
// though it were a command argument by forgetting a flag.
enum class word_role : uint32_t {
	// A command argument, an assignment's left half, a `for` list word, a `case`
	// SUBJECT: split, globbed, and quote-removed.
	ordinary = 0,
	// One of a `case` item's patterns.
	pattern = 1,
};

enum class parse_error : uint16_t {
	none,
	unexpected_operator,   // an operator where a word was required
	// Something the grammar requires after a keyword or an operator, and the parse
	// found nothing there: `echo >` with no target, `for ; do` with no name,
	// `while; do` with an empty condition. Carried by an ERROR node when the
	// redirection is the whole of what went wrong, and by the COMPOUND NODE itself
	// when a construct is missing its operand - see missing_operand_phrase (#58).
	missing_operand,
	unterminated_word,     // the lexer reported the word incomplete
	// A compound command the grammar requires a keyword or a closing token to
	// finish, and the parse never found one: `{ echo x` with no `}`, `if true`
	// with no `then`. Carried by the COMPOUND node, the way unterminated_word is
	// carried by the word - a construct with a piece missing is still that
	// construct, which is what the line editor wants to highlight (#49).
	missing_terminator,
};

// What a compound command with a piece missing is CALLED in a diagnostic. The
// node's kind is the whole answer.
//
// Which keyword was wanted is deliberately NOT recorded: `aux` is already spoken
// for on three of these kinds, and one parse_error value per keyword would have
// been nine mechanisms where #49 asked for one. dash names the keyword
// (`expecting "fi"`); this names the construct, which is the same information one
// word less precisely, and nothing in either test suite compares the text.
[[nodiscard]] constexpr const char* unterminated_phrase(node_kind kind) noexcept {
	switch (kind) {
		case node_kind::if_clause:   return "unterminated if command";
		case node_kind::while_loop:  return "unterminated while loop";
		case node_kind::until_loop:  return "unterminated until loop";
		case node_kind::for_loop:    return "unterminated for loop";
		case node_kind::case_clause: return "unterminated case command";
		case node_kind::case_item:   return "unterminated case pattern list";
		case node_kind::subshell:    return "unterminated subshell";
		case node_kind::brace_group: return "unterminated brace group";
		default:                     return nullptr;
	}
}

// The same, for a compound command whose OPERAND is missing rather than its
// terminator: `while; do echo x; done` closed the condition with nothing in it,
// `for ; do` named no variable, `{ }` holds no command. POSIX makes a
// `compound_list` at least one `and_or`, so an empty one is a syntax error
// wherever the grammar requires one - which is every position below and NOT a
// `case` item or the program itself, where an empty list is legitimate.
//
// Which POSITION was empty is not recorded, for #49's reason: `aux` is spoken for
// on three of these kinds and one parse_error value per position would have been
// ten mechanisms where one does. The phrase names both places worth looking
// instead, which is one word less precise than dash's `";" unexpected` and, as
// #47 established, compared by nothing.
[[nodiscard]] constexpr const char* missing_operand_phrase(node_kind kind) noexcept {
	switch (kind) {
		case node_kind::if_clause:   return "if command with an empty condition or body";
		case node_kind::while_loop:  return "while loop with an empty condition or body";
		case node_kind::until_loop:  return "until loop with an empty condition or body";
		case node_kind::for_loop:    return "for loop with no variable name or an empty body";
		case node_kind::subshell:    return "empty subshell";
		case node_kind::brace_group: return "empty brace group";
		default:                     return nullptr;
	}
}

// 20 bytes. Children are (start, count) into the tree's child-index array, so a
// node's children are contiguous and iterating them is sequential.
struct node {
	node_kind kind = node_kind::error;
	parse_error error = parse_error::none;
	uint32_t first_token = 0;      // inclusive
	uint32_t last_token = 0;       // inclusive
	uint32_t children_start = 0;   // index into the tree's child array
	uint32_t children_count = 0;
	// Kind-specific slot: for and_or, the index of the joining && or || token;
	// for redirect, the file descriptor. Zig's AST calls the same idea `data`.
	// One slot beats one node kind per variant, and beats deriving the operator
	// by re-scanning the tokens between two children.
	uint32_t aux = 0;

	[[nodiscard]] constexpr bool is_error() const noexcept { return kind == node_kind::error; }
};

static_assert(sizeof(node) == 24, "nodes are the bulk of the tree; the size is a cache property");

// Where a here-document's body lives in the source, and how to treat it.
//
// The lexer emits only the operator and the delimiter; the BODY is collected by
// the parser and recorded as a plain source range. That is what keeps the lexer
// pure: zsh's lexer performs I/O to read here-doc bodies and mksh's runs the
// expander on the delimiter, while Oils keeps bodies as re-parseable lines behind
// a reader. This is the Oils shape, minus the reader, because the parser already
// has the whole buffer.
struct here_doc_body {
	uint32_t offset = 0;
	uint32_t length = 0;
	bool expand = true;   // false when the delimiter was quoted: <<'EOF'
	bool strip_tabs = false;  // <<- strips leading tabs from every line
	// Which fd the body is fed to. `3<<END` is legal and means fd 3, not stdin;
	// the fd lives here rather than in the node's `aux` because `aux` already
	// holds the index of this record.
	uint32_t fd = 0;
};

// Byte range in the source. Derived from the token range rather than stored, so
// there is exactly one representation and it cannot drift.
struct span {
	uint32_t offset = 0;
	uint32_t length = 0;
	[[nodiscard]] constexpr uint32_t end() const noexcept { return offset + length; }
	[[nodiscard]] constexpr bool contains(uint32_t at) const noexcept {
		return at >= offset && at < offset + length;
	}
};

class tree {
public:
	tree(buffer_pool& pool, std::string_view source) noexcept
		: _source(source), _nodes(pool, 32), _children(pool, 64), _tokens(pool, 32),
		  _here_docs(pool, 4), _regions(pool, 4),
		  _region_end(static_cast<uint32_t>(source.size())) {}

	// --- construction ---------------------------------------------------------

	uint32_t add_token(const token& t) noexcept { return _tokens.push(t); }

	// Registers text a token may point into that is NOT the input, and returns the
	// virtual offset of its first byte.
	//
	// An alias's replacement is RE-SCANNED as if it had been typed, so the tokens
	// it yields have no position in the input at all. Left pointing at their offset
	// within the alias body, they were read back as a slice of the input: with
	// `alias e=echo`, the four bytes of `echo` came out as the first four bytes of
	// the script and the shell looked for a command called `alia`. Alias
	// substitution could therefore never actually run a command (#40) - #27 built
	// the read-time machinery, but nothing that executed a substituted word.
	//
	// Regions give those tokens somewhere real to point. The input keeps
	// [0, source.size()), so every span into what the user typed is still its true
	// position - which is what the line editor's highlighting needs (#10) - and each
	// registered region takes the next len bytes above it.
	//
	// The view must outlive the tree. Alias text does: shell_state owns it
	// (ADR-0007), and a parse finishes before any command can redefine an alias.
	uint32_t add_text_region(std::string_view text) noexcept {
		const uint32_t base = _region_end;
		_regions.push(text);
		_region_end += static_cast<uint32_t>(text.size());
		return base;
	}

	// Here-doc bodies are stored separately and referenced by index from a
	// here_doc node's `aux`, because a body is a source range rather than tokens.
	uint32_t add_here_doc(const here_doc_body& b) noexcept { return _here_docs.push(b); }
	[[nodiscard]] const here_doc_body& here_doc_at(uint32_t i) const noexcept {
		return _here_docs[i];
	}
	[[nodiscard]] std::string_view here_doc_text(uint32_t i) const noexcept {
		const here_doc_body& b = _here_docs[i];
		return _source.substr(b.offset, b.length);
	}

	node_index add_node(node n) noexcept { return _nodes.push(n); }

	// Copies a completed run of children in and returns where it landed.
	//
	// Children cannot simply be appended as they are discovered: parsing is
	// recursive, so a child's own descendants would be appended between it and its
	// siblings and the run would not be contiguous. The caller accumulates on a
	// scratch stack and commits the finished run here.
	[[nodiscard]] uint32_t add_children(const node_index* first, uint32_t count) noexcept {
		const uint32_t start = static_cast<uint32_t>(_children.size());
		for (uint32_t i = 0; i < count; ++i)
			_children.push(first[i]);
		return start;
	}

	void set_root(node_index root) noexcept { _root = root; }

	// --- inspection -----------------------------------------------------------

	[[nodiscard]] node_index root() const noexcept { return _root; }
	[[nodiscard]] size_t node_count() const noexcept { return _nodes.size(); }
	[[nodiscard]] size_t token_count() const noexcept { return _tokens.size(); }
	[[nodiscard]] std::string_view source() const noexcept { return _source; }

	[[nodiscard]] const node& operator[](node_index i) const noexcept { return _nodes[i]; }
	[[nodiscard]] const token& token_at(uint32_t i) const noexcept { return _tokens[i]; }

	[[nodiscard]] node_index child_of(const node& parent, uint32_t nth) const noexcept {
		return _children[parent.children_start + nth];
	}


	[[nodiscard]] span span_of(const node& n) const noexcept {
		if (_tokens.empty())
			return {};
		const token& first = _tokens[n.first_token];
		const token& last = _tokens[n.last_token];
		// A node can START in an alias body and END in the input - that is exactly
		// what `alias e='echo '` followed by `e foo` produces, because a definition
		// ending in a blank makes the next word eligible too. Subtracting then would
		// wrap the length round to four billion.
		if (last.end_offset() < first.offset)
			return {first.offset, first.length};
		return {first.offset, last.end_offset() - first.offset};
	}

	[[nodiscard]] std::string_view text_of(const node& n) const noexcept {
		const span s = span_of(n);
		return text_at(s.offset, s.length);
	}

	[[nodiscard]] std::string_view text_of_token(const token& t) const noexcept {
		return text_at(t.offset, t.length);
	}

	// The bytes at a virtual offset: the input, or an alias body registered above
	// it. See add_text_region.
	//
	// The length is CLAMPED to the region the offset falls in rather than trusted,
	// because a node's span can begin in one region and end in another and there is
	// no run of bytes that means. Reading past the region instead would read
	// whatever happens to sit after somebody else's std::string.
	[[nodiscard]] std::string_view text_at(uint32_t offset, uint32_t length) const noexcept {
		if (offset < _source.size())
			return _source.substr(offset, std::min<size_t>(length, _source.size() - offset));
		uint32_t base = static_cast<uint32_t>(_source.size());
		for (const std::string_view& region : _regions) {
			const uint32_t end = base + static_cast<uint32_t>(region.size());
			if (offset < end)
				return region.substr(offset - base, std::min<size_t>(length, end - offset));
			base = end;
		}
		return {};
	}

	// The deepest node whose span contains a byte offset. This is what completion
	// asks: "what am I inside of?". Because the tree carries spans, it is a query
	// rather than a re-derivation - which is exactly what zsh cannot do and what
	// forces it to maintain a second, worse parser for its line editor.
	[[nodiscard]] node_index node_at(uint32_t offset) const noexcept {
		if (_root == no_node || _nodes.empty())
			return no_node;
		node_index best = no_node;
		node_index current = _root;
		for (;;) {
			const node& n = _nodes[current];
			if (!span_of(n).contains(offset))
				break;
			best = current;
			node_index descend = no_node;
			for (uint32_t i = 0; i < n.children_count; ++i) {
				const node_index child = _children[n.children_start + i];
				if (span_of(_nodes[child]).contains(offset)) {
					descend = child;
					break;
				}
			}
			if (descend == no_node)
				break;
			current = descend;
		}
		return best;
	}

	// The lexer reported the input ran out mid-construct. ORTHOGONAL to
	// has_errors(), not a weaker form of it:
	//
	//   incomplete() && has_errors()   `echo "x`   - more input would fix it, but
	//                                              as it stands it is a defect
	//   incomplete() && !has_errors()  `echo a\`   - and an unterminated
	//                                              here-document; dash runs both
	//   !incomplete() && has_errors()  `echo ;;`   - malformed, no continuation helps
	//
	// A caller reading interactively asks THIS FIRST and answers it with a
	// continuation prompt; only when no more input is coming does the defect
	// become a diagnostic. A caller with the whole input already in hand - `-c`, a
	// script, stdin - has nothing to continue, so it asks has_errors() alone.
	// Collapsing the two either loses the continuation prompt or makes a trailing
	// backslash a syntax error, and dash proves it is not one.
	[[nodiscard]] bool incomplete() const noexcept { return _incomplete; }
	void set_incomplete(bool v) noexcept { _incomplete = v; }

	// The first node carrying a defect, or no_node. Nodes are added as they are
	// completed, so among defective ones the first is the earliest in the source -
	// which is the one worth naming in a diagnostic.
	[[nodiscard]] node_index first_error() const noexcept {
		for (uint32_t i = 0; i < _nodes.size(); ++i)
			if (is_defective(_nodes[i]))
				return i;
		return no_node;
	}

	// True when any node is defective. Distinct from "the parse failed", because it
	// never does.
	//
	// An ERROR NODE is not the only defect a tree can hold. A word whose quote was
	// never closed is still a word - that is what the line editor wants to
	// highlight and what completion wants to be inside of (ADR-0002) - so the
	// parser records the defect in the node's `error` field and leaves the kind
	// alone. Testing only the kind made that field invisible to the one check that
	// consults it: `lesh -c "echo it's"` printed `it` and reported success (#47).
	[[nodiscard]] bool has_errors() const noexcept { return first_error() != no_node; }

	// What a defective node left unterminated, as a phrase for a diagnostic, or
	// nullptr when its tokens say nothing more than "syntax error".
	//
	// dash names the construct - `Syntax error: Unterminated quoted string` - and
	// lesh can too for the cost of one lookup, because the lexer already recorded
	// WHICH byte opened it. Tokens are scanned rather than indexed because the
	// defect is not always the node's first one: a redirect's first token is the
	// operator and the unterminated word is its target.
	//
	// The phrases themselves live beside token_error, because the expander names
	// the same defects on a bare segment token of a word it is expanding (#48).
	[[nodiscard]] const char* error_detail(const node& n) const noexcept {
		// A missing terminator is the NODE's own defect and its kind names it, so it
		// is asked FIRST: a compound command spans every token of its body, and any
		// phrase the scan below found there would describe something else.
		if (n.error == parse_error::missing_terminator)
			if (const char* phrase = unterminated_phrase(n.kind))
				return phrase;
		// A missing operand is asked for the same reason, and the phrase lookup is
		// what keeps the two apart: missing_operand also travels on the ERROR node a
		// bad redirection becomes, and there the kind names nothing, so the lookup
		// declines and the token scan below answers exactly as it did before (#58).
		if (n.error == parse_error::missing_operand)
			if (const char* phrase = missing_operand_phrase(n.kind))
				return phrase;
		for (uint32_t i = n.first_token; i <= n.last_token && i < _tokens.size(); ++i)
			if (const char* phrase = error_phrase(_tokens[i].error))
				return phrase;
		return nullptr;
	}

	// A defect is either the node's own kind or the error it carries. One
	// definition, so has_errors() and first_error() cannot come to disagree.
	[[nodiscard]] static constexpr bool is_defective(const node& n) noexcept {
		return n.is_error() || n.error != parse_error::none;
	}

private:
	std::string_view _source;
	arena_array<node> _nodes;
	arena_array<uint32_t> _children;
	arena_array<token> _tokens;
	arena_array<here_doc_body> _here_docs;
	// Text that is not the input: one entry per alias substitution. See
	// add_text_region.
	arena_array<std::string_view> _regions;
	uint32_t _region_end = 0;
	node_index _root = no_node;
	bool _incomplete = false;
};

} // namespace lesh::syntax
