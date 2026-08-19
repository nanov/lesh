#include "syntax/parser.h"

#include "substrate/char_utils.h"

#include <utility>

namespace lesh::syntax {

namespace {

// A word is an assignment when it is NAME=anything and it appears in the command
// prefix. POSIX is specific: the part before '=' must be a NAME, so `a-b=c` is an
// ordinary word and `PATH=/x` is an assignment. Once a command name has been
// seen, later NAME=value words are ordinary arguments.
bool looks_like_assignment(std::string_view text) noexcept {
	if (text.empty() || !lesh::string_utils::is_valid_var_name_first_char(
	                        static_cast<unsigned char>(text[0])))
		return false;
	for (size_t i = 1; i < text.size(); ++i) {
		if (text[i] == '=')
			return i > 0;
		if (!lesh::string_utils::is_valid_var_name_non_first_char(
		        static_cast<unsigned char>(text[i])))
			return false;
	}
	return false;
}

constexpr bool is_redirect_operator(token_kind k) noexcept {
	switch (k) {
		case token_kind::less: case token_kind::great: case token_kind::dless:
		case token_kind::dgreat: case token_kind::dless_dash: case token_kind::less_and:
		case token_kind::great_and: case token_kind::less_great: case token_kind::clobber:
			return true;
		default:
			return false;
	}
}

// Tokens that end a command: a separator, a pipe, or end of input.
constexpr bool ends_command(token_kind k) noexcept {
	switch (k) {
		case token_kind::end: case token_kind::newline: case token_kind::semi:
		case token_kind::amp: case token_kind::pipe: case token_kind::and_if:
		case token_kind::or_if: case token_kind::dsemi: case token_kind::rparen:
			return true;
		default:
			return false;
	}
}

class parser_impl {
public:
	parser_impl(buffer_pool& pool, std::string_view source) noexcept
		: _lexer(source), _tree(pool, source), _scratch(pool, 64) {
		fill();
	}

	tree take() noexcept { return std::move(_tree); }

	void parse_program() noexcept {
		const uint32_t mark = mark_scratch();
		const uint32_t first = _index;

		while (peek().kind != token_kind::end) {
			if (is_separator(peek().kind)) {
				advance();
				continue;
			}

			const uint32_t before = _index;
			_scratch.push(parse_and_or());

			// PROGRESS GUARANTEE. Recovery that consumes nothing is not recovery, it
			// is a hang. `;;` at top level ends a command without being a separator,
			// so parse_and_or returns having consumed no tokens and the loop spins.
			// Any token that cannot begin a command is consumed as an error node
			// instead, which is what makes "the parser never fails" true rather than
			// merely intended.
			if (_index == before)
				_scratch.push(error_node(advance(), parse_error::unexpected_operator));
		}

		node n;
		n.kind = node_kind::program;
		n.first_token = first;
		n.last_token = _index;
		commit_children(n, mark);
		_tree.set_root(_tree.add_node(n));
	}

private:
	static constexpr bool is_separator(token_kind k) noexcept {
		return k == token_kind::newline || k == token_kind::semi || k == token_kind::amp;
	}

	// One token of lookahead, lexed on demand so the parser keeps control of the
	// lexer's mode. Lexing everything up front would surrender that, and the mode
	// channel is the whole reason the lexer takes one.
	void fill() noexcept {
		_index = _tree.add_token(_lexer.next(lex_mode::command));
		if (_lexer.incomplete())
			_tree.set_incomplete(true);
	}
	[[nodiscard]] const token& peek() const noexcept { return _tree.token_at(_index); }
	uint32_t advance() noexcept {
		const uint32_t consumed = _index;
		if (_tree.token_at(_index).kind != token_kind::end)
			fill();
		return consumed;
	}

	node_index parse_and_or() noexcept {
		node_index left = parse_pipeline();
		while (peek().kind == token_kind::and_if || peek().kind == token_kind::or_if) {
			const uint32_t op = advance();
			const node_index right = parse_pipeline();

			const node_index pair[2] = {left, right};
			node n;
			n.kind = node_kind::and_or;
			n.first_token = _tree[left].first_token;
			n.last_token = _tree[right].last_token;
			n.children_start = _tree.add_children(pair, 2);
			n.children_count = 2;
			n.aux = op;  // which operator joined them
			left = _tree.add_node(n);
		}
		return left;
	}

	node_index parse_pipeline() noexcept {
		const uint32_t first = _index;
		const uint32_t mark = mark_scratch();

		_scratch.push(parse_command());
		while (peek().kind == token_kind::pipe) {
			advance();
			_scratch.push(parse_command());
		}

		if (_scratch.size() - mark == 1) {
			// A pipeline of one is just the command. Not wrapping it keeps the tree
			// shallow, which matters when every consumer walks it per keystroke.
			const node_index only = _scratch[mark];
			_scratch.truncate(mark);
			return only;
		}

		node n;
		n.kind = node_kind::pipeline;
		n.first_token = first;
		n.last_token = _index > first ? _index - 1 : first;
		commit_children(n, mark);
		return _tree.add_node(n);
	}

	node_index parse_command() noexcept {
		const uint32_t first = _index;
		const uint32_t mark = mark_scratch();
		bool seen_command_name = false;

		while (!ends_command(peek().kind)) {
			const token& t = peek();

			if (is_redirect_operator(t.kind) || t.kind == token_kind::io_number) {
				_scratch.push(parse_redirect());
				continue;
			}

			if (t.kind == token_kind::word) {
				const uint32_t at = advance();
				node w;
				w.first_token = at;
				w.last_token = at;
				if (!seen_command_name && looks_like_assignment(text_of_token(at))) {
					w.kind = node_kind::assignment;
				} else {
					w.kind = node_kind::word;
					seen_command_name = true;
				}
				if (_tree.token_at(at).is_error())
					w.error = parse_error::unterminated_word;
				_scratch.push(_tree.add_node(w));
				continue;
			}

			// An operator where a word belongs. Emit an error node for it and keep
			// going: the point of recovery is that the rest of the line still parses.
			_scratch.push(error_node(advance(), parse_error::unexpected_operator));
		}

		node n;
		n.kind = node_kind::simple_command;
		n.first_token = first;
		n.last_token = _index > first ? _index - 1 : first;
		commit_children(n, mark);
		return _tree.add_node(n);
	}

	node_index parse_redirect() noexcept {
		const uint32_t first = _index;
		uint32_t fd = 0xFFFFFFFFu;  // unspecified: the operator's default applies

		if (peek().kind == token_kind::io_number) {
			const std::string_view digits = text_of_token(_index);
			fd = 0;
			for (const char c : digits)
				fd = fd * 10 + static_cast<uint32_t>(c - '0');
			advance();
		}

		if (!is_redirect_operator(peek().kind))
			return error_node(advance(), parse_error::unexpected_operator);

		advance();  // the operator

		if (peek().kind != token_kind::word) {
			// `echo >` - an operator with nothing to redirect to.
			node n;
			n.kind = node_kind::error;
			n.error = parse_error::missing_operand;
			n.first_token = first;
			n.last_token = _index > first ? _index - 1 : first;
			n.aux = fd;
			return _tree.add_node(n);
		}

		const uint32_t target = advance();
		node n;
		n.kind = node_kind::redirect;
		n.first_token = first;
		n.last_token = target;
		n.aux = fd;
		return _tree.add_node(n);
	}

	node_index error_node(uint32_t at, parse_error why) noexcept {
		node n;
		n.kind = node_kind::error;
		n.error = why;
		n.first_token = at;
		n.last_token = at;
		return _tree.add_node(n);
	}

	[[nodiscard]] std::string_view text_of_token(uint32_t i) const noexcept {
		const token& t = _tree.token_at(i);
		return _tree.source().substr(t.offset, t.length);
	}

	// Children accumulate here and are committed to the tree as a contiguous run
	// once the node owning them is complete. Recursion makes this necessary: a
	// child's own descendants would otherwise land between it and its siblings.
	[[nodiscard]] uint32_t mark_scratch() const noexcept {
		return static_cast<uint32_t>(_scratch.size());
	}
	void commit_children(node& n, uint32_t mark) noexcept {
		const uint32_t count = static_cast<uint32_t>(_scratch.size()) - mark;
		n.children_start = _tree.add_children(_scratch.data() + mark, count);
		n.children_count = count;
		_scratch.truncate(mark);
	}

	lexer _lexer;
	tree _tree;
	arena_array<node_index> _scratch;
	uint32_t _index = 0;
};

} // namespace

tree parse(buffer_pool& pool, std::string_view source) noexcept {
	parser_impl p{pool, source};
	p.parse_program();
	return p.take();
}

} // namespace lesh::syntax
