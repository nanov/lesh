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

// Reserved words are recognised by POSITION, not lexically. `if` is a keyword in
// command position and an ordinary argument elsewhere - `echo if` prints `if`.
// The lexer deliberately has no reserved-word token kind (#9); this is where the
// decision lives, and it is the caller the mode channel was designed for.
enum class reserved {
	none, kw_if, kw_then, kw_elif, kw_else, kw_fi,
	kw_while, kw_until, kw_do, kw_done,
	kw_for, kw_in, kw_case, kw_esac, kw_lbrace, kw_rbrace,
};

reserved reserved_of(std::string_view text) noexcept {
	if (text == "if") return reserved::kw_if;
	if (text == "then") return reserved::kw_then;
	if (text == "elif") return reserved::kw_elif;
	if (text == "else") return reserved::kw_else;
	if (text == "fi") return reserved::kw_fi;
	if (text == "while") return reserved::kw_while;
	if (text == "until") return reserved::kw_until;
	if (text == "do") return reserved::kw_do;
	if (text == "done") return reserved::kw_done;
	if (text == "for") return reserved::kw_for;
	if (text == "in") return reserved::kw_in;
	if (text == "case") return reserved::kw_case;
	if (text == "esac") return reserved::kw_esac;
	if (text == "{") return reserved::kw_lbrace;
	if (text == "}") return reserved::kw_rbrace;
	return reserved::none;
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

	[[nodiscard]] reserved peek_reserved() const noexcept {
		const token& t = peek();
		if (t.kind != token_kind::word || t.is_error())
			return reserved::none;
		// A quoted or expanded word is never a keyword: `"if"` and `$x` are
		// arguments however they spell out. flag_literal is exactly that test, and
		// the lexer already computed it.
		if ((t.flags & syntax::flag_literal) == 0)
			return reserved::none;
		return reserved_of(text_of_token(_index));
	}

	// True for words that close or continue a construct, so a command list stops
	// rather than swallowing them as arguments.
	[[nodiscard]] bool at_list_terminator() const noexcept {
		switch (peek_reserved()) {
			case reserved::kw_then: case reserved::kw_elif: case reserved::kw_else:
			case reserved::kw_fi:   case reserved::kw_do:   case reserved::kw_done:
			case reserved::kw_esac: case reserved::kw_rbrace:
				return true;
			default:
				return false;
		}
	}

	// Consumes a reserved word if it is next. Recovery depends on this returning
	// false rather than throwing: `if x; then y` with no `fi` must still parse.
	bool accept(reserved want) noexcept {
		if (peek_reserved() != want)
			return false;
		advance();
		return true;
	}

	// A sequence of and_or lists, ending at a terminator or end of input. The body
	// of every compound command, and what `program` is - so one walker handles all.
	node_index parse_compound_list() noexcept {
		const uint32_t first = _index;
		const uint32_t mark = mark_scratch();

		// `)` closes a subshell or a case pattern, and `;;` ends a case item. Both
		// end a compound list without being reserved words, and without them the
		// list loops making no progress and error-nodes the closer.
		while (peek().kind != token_kind::end &&
		       peek().kind != token_kind::rparen &&
		       peek().kind != token_kind::dsemi &&
		       !at_list_terminator()) {
			if (is_separator(peek().kind)) {
				advance();
				continue;
			}
			const uint32_t before = _index;
			_scratch.push(parse_and_or());
			if (_index == before)  // progress guarantee, as in parse_program
				_scratch.push(error_node(advance(), parse_error::unexpected_operator));
		}

		node n;
		n.kind = node_kind::compound_list;
		n.first_token = first;
		n.last_token = _index > first ? _index - 1 : first;
		commit_children(n, mark);
		return _tree.add_node(n);
	}

	node_index parse_if() noexcept {
		const uint32_t first = _index;
		const uint32_t mark = mark_scratch();
		advance();  // `if`
		uint32_t pairs = 0;

		for (;;) {
			_scratch.push(parse_compound_list());   // condition
			if (!accept(reserved::kw_then))
				break;
			_scratch.push(parse_compound_list());   // body
			++pairs;
			if (!accept(reserved::kw_elif))
				break;
		}
		if (accept(reserved::kw_else))
			_scratch.push(parse_compound_list());
		accept(reserved::kw_fi);

		node n;
		n.kind = node_kind::if_clause;
		n.first_token = first;
		n.last_token = _index > first ? _index - 1 : first;
		n.aux = pairs;  // how many (condition, body) pairs; anything after is else
		commit_children(n, mark);
		return _tree.add_node(n);
	}

	node_index parse_while_or_until(bool is_until) noexcept {
		const uint32_t first = _index;
		const uint32_t mark = mark_scratch();
		advance();  // `while` / `until`

		_scratch.push(parse_compound_list());  // condition
		if (accept(reserved::kw_do))
			_scratch.push(parse_compound_list());  // body
		accept(reserved::kw_done);

		node n;
		n.kind = is_until ? node_kind::until_loop : node_kind::while_loop;
		n.first_token = first;
		n.last_token = _index > first ? _index - 1 : first;
		commit_children(n, mark);
		return _tree.add_node(n);
	}

	node_index parse_for() noexcept {
		const uint32_t first = _index;
		const uint32_t mark = mark_scratch();
		advance();  // `for`

		uint32_t name_token = _index;
		if (peek().kind == token_kind::word)
			name_token = advance();

		// `for x; do ...` iterates the positional parameters; `for x in a b; do`
		// iterates the listed words.
		if (accept(reserved::kw_in)) {
			while (peek().kind == token_kind::word && peek_reserved() == reserved::none) {
				const uint32_t at = advance();
				node w;
				w.kind = node_kind::word;
				w.first_token = at;
				w.last_token = at;
				_scratch.push(_tree.add_node(w));
			}
		}
		while (is_separator(peek().kind))
			advance();

		if (accept(reserved::kw_do))
			_scratch.push(parse_compound_list());
		accept(reserved::kw_done);

		node n;
		n.kind = node_kind::for_loop;
		n.first_token = first;
		n.last_token = _index > first ? _index - 1 : first;
		n.aux = name_token;  // the loop variable's name token
		commit_children(n, mark);
		return _tree.add_node(n);
	}

	node_index parse_case() noexcept {
		const uint32_t first = _index;
		const uint32_t mark = mark_scratch();
		advance();  // `case`

		if (peek().kind == token_kind::word) {   // the subject
			const uint32_t at = advance();
			node w;
			w.kind = node_kind::word;
			w.first_token = at;
			w.last_token = at;
			_scratch.push(_tree.add_node(w));
		}
		accept(reserved::kw_in);
		while (is_separator(peek().kind))
			advance();

		while (peek().kind != token_kind::end && peek_reserved() != reserved::kw_esac) {
			const uint32_t item_first = _index;
			const uint32_t item_mark = mark_scratch();
			uint32_t patterns = 0;

			// An optional leading '(' is allowed before the first pattern.
			if (peek().kind == token_kind::lparen)
				advance();

			while (peek().kind == token_kind::word) {
				const uint32_t at = advance();
				node w;
				w.kind = node_kind::word;
				w.first_token = at;
				w.last_token = at;
				_scratch.push(_tree.add_node(w));
				++patterns;
				if (peek().kind != token_kind::pipe)
					break;
				advance();  // `|` separates alternative patterns
			}
			if (peek().kind == token_kind::rparen)
				advance();

			_scratch.push(parse_compound_list());

			node item;
			item.kind = node_kind::case_item;
			item.first_token = item_first;
			item.last_token = _index > item_first ? _index - 1 : item_first;
			item.aux = patterns;
			commit_children(item, item_mark);
			_scratch.push(_tree.add_node(item));

			if (peek().kind == token_kind::dsemi)
				advance();
			while (is_separator(peek().kind))
				advance();
			if (_index == item_first)  // progress guarantee
				break;
		}
		accept(reserved::kw_esac);

		node n;
		n.kind = node_kind::case_clause;
		n.first_token = first;
		n.last_token = _index > first ? _index - 1 : first;
		commit_children(n, mark);
		return _tree.add_node(n);
	}

	node_index parse_brace_group() noexcept {
		const uint32_t first = _index;
		const uint32_t mark = mark_scratch();
		advance();  // `{`
		_scratch.push(parse_compound_list());
		accept(reserved::kw_rbrace);

		node n;
		n.kind = node_kind::brace_group;
		n.first_token = first;
		n.last_token = _index > first ? _index - 1 : first;
		commit_children(n, mark);
		return _tree.add_node(n);
	}

	node_index parse_subshell() noexcept {
		const uint32_t first = _index;
		const uint32_t mark = mark_scratch();
		advance();  // `(`
		_scratch.push(parse_compound_list());
		if (peek().kind == token_kind::rparen)
			advance();

		node n;
		n.kind = node_kind::subshell;
		n.first_token = first;
		n.last_token = _index > first ? _index - 1 : first;
		commit_children(n, mark);
		return _tree.add_node(n);
	}

	// Dispatches on what a command starts with. A reserved word here is a keyword;
	// the same word after a command name is an argument, which is what makes
	// `echo if` print `if`.
	[[nodiscard]] node_index parse_command_or_compound() noexcept {
		if (peek().kind == token_kind::lparen)
			return parse_subshell();
		switch (peek_reserved()) {
			case reserved::kw_if:     return parse_if();
			case reserved::kw_while:  return parse_while_or_until(false);
			case reserved::kw_until:  return parse_while_or_until(true);
			case reserved::kw_for:    return parse_for();
			case reserved::kw_case:   return parse_case();
			case reserved::kw_lbrace: return parse_brace_group();
			default:                  return parse_command();
		}
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

		_scratch.push(parse_command_or_compound());
		while (peek().kind == token_kind::pipe) {
			advance();
			_scratch.push(parse_command_or_compound());
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
			// A reserved word is only a keyword in COMMAND position. `echo done`
			// prints `done`, while `echo yes; done` ends the loop - the `;` is what
			// puts `done` back in command position. Checking every position made
			// `echo done` a syntax error.
			if (_scratch.size() == mark && at_list_terminator())
				break;

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
