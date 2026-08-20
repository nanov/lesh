#include "syntax/parser.h"

#include "substrate/char_utils.h"

#include <utility>
#include <vector>

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

// Does `line` equal the here-document delimiter spelled `raw`, once quote removal
// is applied to `raw`?
//
// POSIX applies quote removal to the delimiter word, so `<<\\END`, `<<'END'`,
// `<<"END"` and `<<E'ND'` all end the body at a line reading `END`. Only the fully
// quoted forms were handled before, which meant `<<\\END` - the spelling the yash
// conformance suite uses in every one of its ~1,700 here-documents - never matched
// its terminator, so the rest of the file silently became the body. Twenty signal
// files scored zero for that reason alone.
//
// This compares rather than unquoting into a buffer, because the parser has no
// business allocating for a comparison it makes once per line.
bool delimiter_matches(std::string_view raw, std::string_view line) noexcept {
	size_t r = 0, l = 0;
	while (r < raw.size()) {
		const char c = raw[r];
		if (c == '\'') {
			// Single quotes: everything up to the next one is literal, backslash
			// included. An unterminated quote cannot match anything.
			++r;
			while (r < raw.size() && raw[r] != '\'') {
				if (l >= line.size() || line[l] != raw[r])
					return false;
				++r;
				++l;
			}
			if (r >= raw.size())
				return false;
			++r;  // closing quote
			continue;
		}
		if (c == '"') {
			++r;
			while (r < raw.size() && raw[r] != '"') {
				// Inside double quotes a backslash escapes only these four bytes;
				// anywhere else it stands for itself.
				if (raw[r] == '\\' && r + 1 < raw.size() &&
				    (raw[r + 1] == '$' || raw[r + 1] == '`' || raw[r + 1] == '"' ||
				     raw[r + 1] == '\\'))
					++r;
				if (l >= line.size() || line[l] != raw[r])
					return false;
				++r;
				++l;
			}
			if (r >= raw.size())
				return false;
			++r;  // closing quote
			continue;
		}
		if (c == '\\') {
			// A trailing backslash quotes nothing; treat it as itself rather than
			// reading past the end of the word.
			if (r + 1 >= raw.size())
				return false;
			++r;
			if (l >= line.size() || line[l] != raw[r])
				return false;
			++r;
			++l;
			continue;
		}
		if (l >= line.size() || line[l] != c)
			return false;
		++r;
		++l;
	}
	return l == line.size();
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
	parser_impl(buffer_pool& pool, std::string_view source,
	            const alias_source* aliases) noexcept
		: _lexer(source), _tree(pool, source), _scratch(pool, 64), _aliases(aliases) {
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
			_scratch.push(parse_list_item());

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

	// One and-or list plus the `&` that may follow it, which makes it ASYNCHRONOUS.
	//
	// Shared by parse_program and parse_compound_list because it was fixed in only
	// one of them once already. `&` is in is_separator, so a list that is not
	// wrapped here has its `&` silently consumed as a `;` and runs in the
	// foreground - and that deadlocks the moment the background command waits on
	// something the foreground has yet to do, which is exactly what
	// `( echo x >fifo & ); cat fifo` does. It ran in the program body and hung in
	// every compound command.
	node_index parse_list_item() noexcept {
		node_index item = parse_and_or();
		if (peek().kind != token_kind::amp)
			return item;
		advance();
		node async;
		async.kind = node_kind::async_list;
		async.first_token = _tree[item].first_token;
		async.last_token = _index > 0 ? _index - 1 : 0;
		const uint32_t child_mark = mark_scratch();
		_scratch.push(item);
		commit_children(async, child_mark);
		return _tree.add_node(async);
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
			_scratch.push(parse_list_item());
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

		// `for x do ...` and `for x; do ...` iterate the POSITIONAL PARAMETERS;
		// `for x in a b; do` iterates the listed words. The distinction matters and
		// is not the same as an empty list: `for x in; do` iterates nothing.
		// Recorded in the node so the executor can tell them apart.
		bool has_in = false;
		if (accept(reserved::kw_in)) {
			has_in = true;
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
		// aux packs the name token and the has_in flag: the top bit says whether
		// an `in` clause was written.
		n.aux = name_token | (has_in ? 0x80000000u : 0u);
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
	// `name ( ) compound-command`. There is no keyword, so this is recognised by
	// SHAPE: a word in command position followed by `(` then `)`. It must be
	// checked before parse_command consumes the name as a command.
	[[nodiscard]] bool at_function_definition() noexcept {
		if (peek().kind != token_kind::word || peek_reserved() != reserved::none)
			return false;
		char* ignored = nullptr;
		(void)ignored;
		// Look ahead without consuming: lex from the current position on a copy.
		syntax::lexer probe{_tree.source(), _tree.token_at(_index).end_offset()};
		const token after_name = probe.next(lex_mode::command);
		if (after_name.kind != token_kind::lparen)
			return false;
		const token after_paren = probe.next(lex_mode::command);
		return after_paren.kind == token_kind::rparen;
	}

	node_index parse_function_definition() noexcept {
		const uint32_t first = _index;
		const uint32_t name_token = advance();  // the name
		advance();  // (
		advance();  // )
		while (is_separator(peek().kind))
			advance();

		const uint32_t mark = mark_scratch();
		// The body is a compound command - in practice a brace group, but POSIX
		// permits any of them.
		_scratch.push(parse_command_or_compound());

		node n;
		n.kind = node_kind::function_definition;
		n.first_token = first;
		n.last_token = _index > first ? _index - 1 : first;
		n.aux = name_token;
		commit_children(n, mark);
		return _tree.add_node(n);
	}

	// POSIX permits redirections AFTER a compound command: `if ...; fi > file`
	// redirects the whole construct. They are attached to the compound node so the
	// executor applies them around it.
	node_index attach_trailing_redirects(node_index compound) noexcept {
		if (!is_redirect_operator(peek().kind) && peek().kind != token_kind::io_number)
			return compound;

		const uint32_t mark = mark_scratch();
		_scratch.push(compound);
		while (is_redirect_operator(peek().kind) || peek().kind == token_kind::io_number)
			_scratch.push(parse_redirect());

		// A brace group wrapping the compound plus its redirections: the executor
		// already applies a command's redirect children, so reusing that shape
		// avoids a second mechanism.
		node n;
		n.kind = node_kind::brace_group;
		n.first_token = _tree[compound].first_token;
		n.last_token = _index > n.first_token ? _index - 1 : n.first_token;
		commit_children(n, mark);
		return _tree.add_node(n);
	}

	[[nodiscard]] node_index parse_command_or_compound() noexcept {
		if (peek().kind == token_kind::lparen)
			return attach_trailing_redirects(parse_subshell());
		if (at_function_definition())
			return parse_function_definition();
		switch (peek_reserved()) {
			case reserved::kw_if:     return attach_trailing_redirects(parse_if());
			case reserved::kw_while:  return attach_trailing_redirects(parse_while_or_until(false));
			case reserved::kw_until:  return attach_trailing_redirects(parse_while_or_until(true));
			case reserved::kw_for:    return attach_trailing_redirects(parse_for());
			case reserved::kw_case:   return attach_trailing_redirects(parse_case());
			case reserved::kw_lbrace: return attach_trailing_redirects(parse_brace_group());
			default:                  return parse_command();
		}
	}

	// One token of lookahead, lexed on demand so the parser keeps control of the
	// lexer's mode. Lexing everything up front would surrender that, and the mode
	// channel is the whole reason the lexer takes one.
	// Alias substitution replaces a command word with its definition, which is
	// then re-scanned. A STACK of lexers rather than textual pre-substitution:
	// pre-substituting would lose the association between a token and the text the
	// user typed, and the line editor needs that association to survive (#10).
	//
	// The cost, recorded rather than hidden: tokens produced from an alias body
	// carry spans into the ALIAS TEXT, not into the typed line. Highlighting an
	// aliased command therefore has nothing to underline in the user's buffer.
	// Solving that needs a span that can name its origin, which is line-editor
	// work (#14 found zsh has the same problem and solves it no better).
	struct alias_frame {
		lexer lex;
		std::string_view text;
	};

	void fill() noexcept {
		// Draw from an alias body while one is active, popping when it runs out.
		while (!_alias_stack.empty()) {
			const token t = _alias_stack.back().lex.next(lex_mode::command);
			if (t.kind != token_kind::end) {
				_index = _tree.add_token(t);
				return;
			}
			_alias_stack.pop_back();
			_alias_depth = _alias_stack.size();
		}

		_index = _tree.add_token(_lexer.next(lex_mode::command));
		if (_lexer.incomplete())
			_tree.set_incomplete(true);

		// A here-doc body begins after the newline that ends the command line.
		// Collect the pending ones and resume past them, so the body is never
		// lexed as commands.
		if (!_pending_here_docs.empty() &&
		    _tree.token_at(_index).kind == token_kind::newline) {
			const uint32_t resume = collect_here_doc_bodies(
				_tree.token_at(_index).end_offset());
			_lexer.seek(resume);
		}
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

		// Alias substitution, at command position only. POSIX substitutes an alias
		// when the word is a command NAME - `ls foo` substitutes ls, `echo ls` does
		// not - which is the same rule reserved words follow.
		if (_aliases != nullptr && _scratch.size() == mark)
			try_substitute_alias();

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

	// A here-document whose delimiter has been seen but whose body has not been
	// collected yet. POSIX puts the body after the NEXT newline, not after the
	// operator, so `cat <<A <<B` collects both bodies in order once the line ends.
	struct pending_here_doc {
		node_index node;
		uint32_t delimiter_token;
	};

	// Collects every pending here-doc body, starting at `from`. Returns where the
	// last body ended so lexing can resume past it.
	//
	// The parser does this, not the lexer. The lexer never reads and never seeks
	// on its own - it is handed a buffer and a position, which is exactly what
	// makes it safe to run on every keystroke over an editor's buffer.
	uint32_t collect_here_doc_bodies(uint32_t from) noexcept {
		const std::string_view src = _tree.source();
		uint32_t at = from;

		for (const auto& pending : _pending_here_docs) {
			const std::string_view raw = text_of_token(pending.delimiter_token);

			// A quoted delimiter suppresses expansion in the body: <<'EOF' is
			// literal, <<EOF is expanded. The lexer already recorded whether the
			// word was literal, so this needs no re-scanning.
			const bool quoted = (_tree.token_at(pending.delimiter_token).flags &
			                     syntax::flag_literal) == 0;
			// The delimiter is compared with quote removal applied on the fly - see
			// delimiter_matches. `raw` is the word exactly as it was typed.

			node& n = const_cast<node&>(_tree[pending.node]);
			const bool strip = n.error == parse_error::none && _strip_tabs_for.size() > 0
			                   ? false : false;
			(void)strip;

			const uint32_t body_start = at;
			uint32_t body_end = at;
			bool terminated = false;

			while (at <= src.size()) {
				const size_t nl = src.find('\n', at);
				const std::string_view line = src.substr(
					at, nl == std::string_view::npos ? std::string_view::npos : nl - at);

				// <<- strips leading tabs from the body AND from the delimiter line.
				std::string_view compared = line;
				if (n.aux != 0xFFFFFFFFu && _tree.here_doc_at(n.aux).strip_tabs)
					while (!compared.empty() && compared.front() == '\t')
						compared.remove_prefix(1);

				if (delimiter_matches(raw, compared)) {
					terminated = true;
					at = nl == std::string_view::npos ? static_cast<uint32_t>(src.size())
					                                  : static_cast<uint32_t>(nl + 1);
					break;
				}
				if (nl == std::string_view::npos) {
					body_end = static_cast<uint32_t>(src.size());
					at = body_end;
					break;
				}
				body_end = static_cast<uint32_t>(nl + 1);
				at = body_end;
			}

			if (!terminated) {
				// An unterminated here-doc is INCOMPLETE, not malformed: an
				// interactive shell answers it with a continuation prompt.
				_tree.set_incomplete(true);
			}

			syntax::here_doc_body body{};
			body.offset = body_start;
			body.length = body_end > body_start ? body_end - body_start : 0;
			body.expand = !quoted;
			body.strip_tabs = n.aux != 0xFFFFFFFFu && _tree.here_doc_at(n.aux).strip_tabs;
			// Overwrite the placeholder recorded when the operator was seen.
			const_cast<syntax::here_doc_body&>(_tree.here_doc_at(n.aux)) = body;
		}

		_pending_here_docs.clear();
		return at;
	}

	// Replaces a command word with its alias definition, if it has one.
	void try_substitute_alias() noexcept {
		for (int guard = 0; guard < kMaxAliasDepth; ++guard) {
			if (peek().kind != token_kind::word || peek().is_error())
				return;
			// A quoted word is never an alias: `\ls` and `'ls'` are how you bypass
			// one, and flag_literal is exactly that test.
			if ((peek().flags & syntax::flag_literal) == 0)
				return;

			const std::string_view name = text_of_token(_index);
			std::string_view value;
			if (!_aliases->lookup_alias(name, value))
				return;

			// Cycle guard: `alias a=b; alias b=a` must terminate. POSIX says an
			// alias is not re-substituted while its own expansion is being read,
			// and a depth limit is the simplest form of that which also stops
			// mutual recursion.
			for (const auto& frame : _alias_stack)
				if (frame.text == value)
					return;

			// Replace the word: drop it and lex the definition in its place.
			_alias_stack.push_back({lexer{value}, value});
			_alias_depth = _alias_stack.size();
			fill();

			// POSIX: a definition ending in a blank makes the NEXT word eligible
			// too. That is what makes `alias sudo='sudo '` work with an aliased
			// command after it.
			if (value.empty() || (value.back() != ' ' && value.back() != '\t'))
				return;
		}
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

		const token_kind op = peek().kind;
		advance();  // the operator

		if (op == token_kind::dless || op == token_kind::dless_dash) {
			if (peek().kind != token_kind::word) {
				node n;
				n.kind = node_kind::error;
				n.error = parse_error::missing_operand;
				n.first_token = first;
				n.last_token = _index > first ? _index - 1 : first;
				return _tree.add_node(n);
			}
			// The pending record MUST be pushed before advancing past the
			// delimiter. advance() lexes the next token, and if that token is the
			// newline, fill() collects bodies right then - so pushing afterwards
			// means fill() sees an empty list and the body gets lexed as commands.
			const uint32_t delimiter = _index;

			syntax::here_doc_body placeholder{};
			placeholder.strip_tabs = op == token_kind::dless_dash;
			node n;
			n.kind = node_kind::here_doc;
			n.first_token = first;
			n.last_token = delimiter;
			n.aux = _tree.add_here_doc(placeholder);
			const node_index node = _tree.add_node(n);
			// The body starts after the next newline, so it cannot be collected
			// here - the rest of this line may hold more redirections.
			_pending_here_docs.push_back({node, delimiter});
			advance();
			return node;
		}

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
	std::vector<pending_here_doc> _pending_here_docs;
	std::vector<alias_frame> _alias_stack;
	const alias_source* _aliases = nullptr;
	size_t _alias_depth = 0;
	static constexpr int kMaxAliasDepth = 16;
	std::vector<uint32_t> _strip_tabs_for;
	uint32_t _index = 0;
};

} // namespace

tree parse(buffer_pool& pool, std::string_view source,
           const alias_source* aliases) noexcept {
	parser_impl p{pool, source, aliases};
	p.parse_program();
	return p.take();
}

} // namespace lesh::syntax
