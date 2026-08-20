#pragma once

#include "substrate/arena_array.h"
#include "syntax/token.h"

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
	word,            // one word of a command
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

enum class parse_error : uint16_t {
	none,
	unexpected_operator,   // an operator where a word was required
	missing_operand,       // an operator with nothing after it
	unterminated_word,     // the lexer reported the word incomplete
};

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
		  _here_docs(pool, 4) {}

	// --- construction ---------------------------------------------------------

	uint32_t add_token(const token& t) noexcept { return _tokens.push(t); }

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
		return {first.offset, last.end_offset() - first.offset};
	}

	[[nodiscard]] std::string_view text_of(const node& n) const noexcept {
		const span s = span_of(n);
		return _source.substr(s.offset, s.length);
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

	// The lexer reported the input ran out mid-construct. Separate from
	// has_errors(): an interactive shell answers this with a continuation prompt.
	[[nodiscard]] bool incomplete() const noexcept { return _incomplete; }
	void set_incomplete(bool v) noexcept { _incomplete = v; }

	// True when any node is an error node. Distinct from "the parse failed",
	// because it never does.
	[[nodiscard]] bool has_errors() const noexcept {
		for (const node& n : _nodes)
			if (n.is_error())
				return true;
		return false;
	}

private:
	std::string_view _source;
	arena_array<node> _nodes;
	arena_array<uint32_t> _children;
	arena_array<token> _tokens;
	arena_array<here_doc_body> _here_docs;
	node_index _root = no_node;
	bool _incomplete = false;
};

} // namespace lesh::syntax
