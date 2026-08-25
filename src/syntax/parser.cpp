#include "syntax/parser.h"

#include "substrate/numeric.h"

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
		// A line continuation inside the NAME is removed before the input is
		// tokenised, so `fo\<newline>o=bar` is an assignment to `foo`. Read as a
		// backslash it was not an assignment at all and `foo=bar` ran as a command
		// (quote-p.tst's 'line continuation in assignment').
		if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '\n') {
			++i;
			continue;
		}
		if (text[i] == '=')
			return i > 0;
		if (!lesh::string_utils::is_valid_var_name_non_first_char(
		        static_cast<unsigned char>(text[i])))
			return false;
	}
	return false;
}

// True when a here-document delimiter carries quoting that suppresses expansion
// in the body. A line continuation is not quoting, which flag_literal cannot say.
bool delimiter_is_quoted(std::string_view raw) noexcept {
	for (size_t i = 0; i < raw.size(); ++i) {
		if (raw[i] == '\\') {
			if (i + 1 < raw.size() && raw[i + 1] == '\n') {
				++i;
				continue;  // a continuation, not an escape
			}
			return true;
		}
		if (raw[i] == '\'' || raw[i] == '"')
			return true;
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
	kw_for, kw_in, kw_case, kw_esac, kw_lbrace, kw_rbrace, kw_bang,
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
	if (text == "!") return reserved::kw_bang;
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
		// `;&` ends a case item's compound_list exactly as `;;` does; the only
		// difference is what happens AFTER the item, which is run_case's concern.
		case token_kind::semi_and:
			return true;
		default:
			return false;
	}
}

class parser_impl {
public:
	// `from` is where in `source` to start. The whole source is handed over even
	// when only a tail of it is to be parsed, so every span in the tree is an
	// offset into the input the user actually wrote - which is what keeps $LINENO
	// and a line editor's highlighting honest when input is read one command at a
	// time.
	parser_impl(buffer_pool& pool, std::string_view source,
	            const alias_source* aliases, uint32_t from = 0) noexcept
		: _lexer(source, from), _tree(pool, source), _scratch(pool, 64),
		  _aliases(aliases), _input_cursor(from) {
		fill();
	}

	tree take() noexcept { return std::move(_tree); }

	void parse_program() noexcept { parse_body(false); }

	// One complete command, so the caller can run it before the next is read.
	void parse_complete_command() noexcept { parse_body(true); }

	// Where reading stopped, as a byte offset into the source. Only meaningful
	// after parse_complete_command: it is what the next call must start from.
	[[nodiscard]] uint32_t input_cursor() const noexcept { return _input_cursor; }

private:
	void parse_body(bool one_command) noexcept {
		const uint32_t mark = mark_scratch();
		const uint32_t first = _index;

		while (peek().kind != token_kind::end) {
			if (is_separator(peek().kind)) {
				// A NEWLINE of the input ends a complete command: POSIX substitutes an
				// alias when the command is READ, so `alias e=echo` on one line has to
				// take effect before the next line is read. A newline drawn from an
				// ALIAS BODY ends nothing - `alias b='cat | c - ; a '` yields two
				// commands from one word, and stopping inside the body would throw the
				// rest of it away.
				const bool ends_command = one_command && _alias_stack.empty() &&
				                          peek().kind == token_kind::newline;
				advance();
				if (ends_command)
					break;
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

		// Nothing but blanks, comments or end of input is left, and the cursor
		// recorded by fill() still points before them. Saying so ends the caller's
		// read loop; leaving it where it was would offer the same bytes again.
		if (peek().kind == token_kind::end)
			_input_cursor = static_cast<uint32_t>(_tree.source().size());

		node n;
		n.kind = node_kind::program;
		n.first_token = first;
		n.last_token = _index;
		commit_children(n, mark);
		_tree.set_root(_tree.add_node(n));
	}

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

	// The reserved word a token spells once its LINE CONTINUATIONS are removed.
	//
	// False unless the joined text could BE one: only lower-case letters, `{`, `}`
	// and `!` are accepted, so a quote, a `$`, or a backslash before anything but a
	// newline refuses rather than being guessed at. False too when nothing was
	// joined, which means the word is quoted some other way - `\case` is not a
	// keyword and must not become one.
	[[nodiscard]] static bool reserved_across_line_continuations(
		std::string_view text, char* buffer, size_t capacity,
		std::string_view& out) noexcept {
		size_t at = 0;
		bool joined_any = false;
		for (size_t i = 0; i < text.size(); ++i) {
			if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '\n') {
				++i;
				joined_any = true;
				continue;
			}
			const char c = text[i];
			const bool allowed = (c >= 'a' && c <= 'z') || c == '{' || c == '}' || c == '!';
			if (!allowed || at == capacity)
				return false;
			buffer[at++] = c;
		}
		if (!joined_any)
			return false;
		out = std::string_view{buffer, at};
		return true;
	}

	[[nodiscard]] reserved peek_reserved() const noexcept {
		const token& t = peek();
		if (t.kind != token_kind::word || t.is_error())
			return reserved::none;
		// A quoted or expanded word is never a keyword: `"if"` and `$x` are
		// arguments however they spell out. flag_literal is exactly that test, and
		// the lexer already computed it.
		if ((t.flags & syntax::flag_literal) != 0)
			return reserved_of(text_of_token(_index));
		// With ONE exception: a line continuation quotes nothing, so
		// `c\<newline>ase` is the reserved word `case`. flag_literal is cleared by
		// any backslash, which made every one of these an ordinary word - so
		// `\<newline>{\<newline> echo 1` ran a command called `{`. The join accepts
		// only the bytes a reserved word is spelled with, so anything else quoted
		// still refuses.
		char joined[8];
		std::string_view text;
		if (!reserved_across_line_continuations(text_of_token(_index), joined,
		                                        sizeof joined, text))
			return reserved::none;
		return reserved_of(text);
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

	// The `linebreak` of the grammar, where nothing but newlines may stand: between
	// a `for` name or a `case` subject and the `in` that may follow it. No alias is
	// substituted here, unlike skip_linebreak - the next word is `in`, a reserved
	// word, and offering it to the alias table would be offering the word that
	// follows a keyword rather than one in command position.
	void skip_newlines() noexcept {
		while (peek().kind == token_kind::newline)
			advance();
	}

	// POSIX allows a newline after `&&`, `||` and `|` - the grammar spells it
	// `linebreak` - and the continuation line is part of the SAME command. Without
	// this, `echo foo |` followed by `cat` on the next line lost the right-hand side
	// of the pipeline entirely and printed nothing, and `false &&` followed by a
	// command ran that command unconditionally, because reading one command at a
	// time ended the unit at the newline.
	void skip_linebreak() noexcept {
		for (;;) {
			while (peek().kind == token_kind::newline)
				advance();
			// An alias that substituted to NOTHING leaves the newline it stood in front
			// of, and the linebreak rule then applies again: with `alias a=`,
			// `echo foo | a` followed by `cat` is one pipeline of two commands in dash.
			// Substituting here rather than only in parse_command_or_compound is what
			// makes that true: the empty replacement has to be read before the newline
			// after it can be recognised as a linebreak.
			const uint32_t before = _index;
			substitute_word();
			if (_index == before)
				return;
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

	// Records that a construct is missing a piece the grammar requires: a
	// terminating keyword, or an operand.
	//
	// THE ONE MECHANISM for all nine terminator positions - `then`, `fi`, `do`,
	// `done`, `in`, `esac`, `}` and the two `)` - because accept() returning false
	// rather than throwing is exactly right for the line editor and five of the six
	// compound commands then DISCARDED that false. `lesh -c '{ echo x'` printed x
	// and reported success where dash reports a syntax error (#49); fixing the sites
	// one at a time is how five of the six end up subtly different. It is now also
	// the mechanism for #58's nine OPERAND positions - eight required
	// `compound_list`s and the `for` name - which are the same defect one production
	// earlier: `{ echo x` has no `}`, `{ }` has no command.
	//
	// The defect travels on the COMPOUND NODE's own error field, which is #47's
	// shape unchanged: a word whose quote was never closed is still a word, and an
	// `if` whose `fi` was never typed is still an if - the kind is what the line
	// editor highlights and what node_at() puts completion inside of, so the defect
	// goes beside it rather than replacing it. tree::has_errors() already consults
	// the field, so nothing downstream changes.
	//
	// INCOMPLETE stays ORTHOGONAL, and that is the whole reason this looks at what
	// is next. `if true` ran out of INPUT, so more of it would help and an
	// interactive reader answers with a continuation prompt; `if true; fi` has a
	// `fi` where `then` belongs and no continuation helps. Both are defects, only
	// the first is incomplete - the same split the lexer keeps between an
	// unterminated quote and a trailing backslash.
	bool record_missing(parse_error& defect, parse_error why) noexcept {
		if (peek().kind == token_kind::end)
			_tree.set_incomplete(true);
		// The FIRST thing missing is the one worth naming, and a construct has one
		// error field: `if true` misses `then` and `fi` both, and `while; do` misses
		// its condition before it misses anything else.
		if (defect == parse_error::none)
			defect = why;
		return false;
	}

	// Consumes a reserved word the grammar requires, or records its absence.
	bool require(reserved want, parse_error& defect) noexcept {
		return accept(want) || record_missing(defect, parse_error::missing_terminator);
	}

	// The same, for a construct closed by an operator rather than by a keyword: a
	// subshell's `)` and the `)` after a case item's patterns.
	bool require(token_kind want, parse_error& defect) noexcept {
		if (peek().kind != want)
			return record_missing(defect, parse_error::missing_terminator);
		advance();
		return true;
	}

	// A `compound_list` position where POSIX requires at least one command, passed
	// through so the caller can wrap the call that produced it.
	//
	// `compound_list` reduces to `term`, and `term` to at least one `and_or`, so an
	// EMPTY list is a syntax error everywhere the grammar spells one - every body
	// and every condition. dash refuses every such shape and lesh accepted all of
	// them at status 0, two of them by looping: `while; do echo x; done` and
	// `while true; do done` both spin, because an empty list runs to status 0 and 0
	// is what keeps a `while` going (#58).
	//
	// Deliberately NOT applied to a `case` item, whose list POSIX makes optional
	// (`case a in b) ;; esac` is valid), nor to `program`, which may be empty
	// (`lesh -c ''` is status 0). Those two are why the CALLER decides rather than
	// parse_compound_list itself.
	node_index require_list(node_index list, parse_error& defect) noexcept {
		if (_tree[list].children_count == 0)
			record_missing(defect, parse_error::missing_operand);
		return list;
	}

	// A sequence of and_or lists, ending at a terminator or end of input. The body
	// of every compound command, and what `program` is - so one walker handles all.
	node_index parse_compound_list() noexcept {
		const uint32_t first = _index;
		const uint32_t mark = mark_scratch();

		// `)` closes a subshell or a case pattern, and `;;` or `;&` ends a case
		// item. All three end a compound list without being reserved words, and
		// without them the list loops making no progress and error-nodes the
		// closer.
		for (;;) {
			if (is_separator(peek().kind)) {
				advance();
				continue;
			}
			// The word here is in COMMAND POSITION, so an alias has to be substituted
			// before the word is tested for CLOSING the list: `alias d='do echo'` and
			// `alias dn='done | cat -'` are how alias-p.tst spells a loop, and the
			// terminator test on the un-substituted word saw neither.
			substitute_word();
			// A replacement may BEGIN with a separator - `alias r='<newline>)<newline>'`
			// is how alias-p.tst closes a subshell - and the separator has to be
			// consumed here rather than handed to a command parser that would consume
			// nothing and trip the progress guarantee into an error node.
			if (is_separator(peek().kind))
				continue;
			if (peek().kind == token_kind::end ||
			    peek().kind == token_kind::rparen ||
			    peek().kind == token_kind::dsemi ||
			    peek().kind == token_kind::semi_and ||
			    at_list_terminator())
				break;
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
		parse_error defect = parse_error::none;

		for (;;) {
			// `if; then echo x; fi` printed x and reported success. Every one of these
			// three lists is a required `compound_list` and an empty one is a syntax
			// error - the condition of the `if` and of each `elif`, each body, and the
			// `else` part below (#58).
			_scratch.push(require_list(parse_compound_list(), defect));   // condition
			if (!require(reserved::kw_then, defect))
				break;
			_scratch.push(require_list(parse_compound_list(), defect));   // body
			++pairs;
			if (!accept(reserved::kw_elif))
				break;
		}
		if (accept(reserved::kw_else))
			_scratch.push(require_list(parse_compound_list(), defect));
		require(reserved::kw_fi, defect);

		node n;
		n.kind = node_kind::if_clause;
		n.error = defect;
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
		parse_error defect = parse_error::none;

		// THE HANG. An empty condition runs to status 0, and 0 is exactly what keeps a
		// `while` going, so `while; do echo x; done` spun - a typo a user can make by
		// hand wedging the shell. `while true; do done` is the same defect at the other
		// list. #19's 10-million-iteration ceiling in run_loop is a guard against
		// taking the MACHINE down, not against this: measured here, the empty
		// condition reaches the ceiling in 40 seconds with a no-op body and about 100
		// with `echo x`, and then returns 0 - so the ceiling turns an unbounded hang
		// into a long one that reports SUCCESS for input dash refuses (#58).
		_scratch.push(require_list(parse_compound_list(), defect));  // condition
		if (require(reserved::kw_do, defect))
			_scratch.push(require_list(parse_compound_list(), defect));  // body
		require(reserved::kw_done, defect);

		node n;
		n.kind = is_until ? node_kind::until_loop : node_kind::while_loop;
		n.error = defect;
		n.first_token = first;
		n.last_token = _index > first ? _index - 1 : first;
		commit_children(n, mark);
		return _tree.add_node(n);
	}

	node_index parse_for() noexcept {
		const uint32_t first = _index;
		const uint32_t mark = mark_scratch();
		// The word this advance DRAWS is the loop's name, and no reserved word is
		// accepted there - so an alias named `in` or `do` applies to it. Cleared
		// immediately: the word after the name is read by the next advance, and there
		// `in` and `do` really are keywords (alias-p.tst:351).
		_plain_word_position = true;
		advance();  // `for`
		_plain_word_position = false;
		parse_error defect = parse_error::none;

		// `for name` - the one compound command whose operand is a WORD rather than a
		// list, and the grammar makes it mandatory in all three productions. `for ; do
		// echo x; done` ran the body zero times at status 0 where dash reports 2 (#58).
		//
		// The test is "is this a word", not "is this a NAME": the lexer has no
		// reserved-word kind and the parser decides by position, so `for in in a b; do`
		// names a variable called `in` and dash runs it. A word that is not a valid
		// name - `for 1 in a` - is a different refusal dash spells `Bad for loop
		// variable`, and is not this one.
		uint32_t name_token = _index;
		if (peek().kind == token_kind::word)
			name_token = advance();
		else
			record_missing(defect, parse_error::missing_operand);

		// POSIX puts a `linebreak` between the name and `in`, so `for i<newline>in a b`
		// is one loop. Without this the `in` was read as the first word of the BODY,
		// and `do` was then missing - which cost nothing while a missing `do` was
		// discarded, and would have turned a loop dash runs into a syntax error the
		// moment it stopped being.
		skip_newlines();

		// `for x do ...` and `for x; do ...` iterate the POSITIONAL PARAMETERS;
		// `for x in a b; do` iterates the listed words. The distinction matters and
		// is not the same as an empty list: `for x in; do` iterates nothing.
		// Recorded in the node so the executor can tell them apart.
		bool has_in = false;
		if (accept(reserved::kw_in)) {
			has_in = true;
			// The word list is a list of WORDS (#65) - `do` and `in` are reserved
			// only by POSITION (#19), and this is not a reserved-word position at
			// all: `for i in in; do` names the word `in`, and `for i in do\ndo`
			// names the word `do` before the do_group's OWN `do` starts on the next
			// line. peek_reserved() must not be consulted here, unlike
			// at_list_terminator() a compound_list checks - the only thing that
			// closes a for loop's word list is running out of WORD tokens; the
			// keyword that follows is read by require(kw_do) below, after the
			// separator loop, exactly as the grammar's `sequential_sep do_group`
			// spells it.
			while (peek().kind == token_kind::word)
				_scratch.push(word_node(advance(), node_kind::word));
		}
		while (is_separator(peek().kind))
			advance();

		// The `do_group`'s list is required even though the WORDLIST is not: `for i in;
		// do echo x; done` iterates nothing and is valid, `for i in a; do done` is not.
		if (require(reserved::kw_do, defect))
			_scratch.push(require_list(parse_compound_list(), defect));
		require(reserved::kw_done, defect);

		node n;
		n.kind = node_kind::for_loop;
		n.error = defect;
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
		parse_error defect = parse_error::none;

		if (peek().kind == token_kind::word)    // the subject
			_scratch.push(word_node(advance(), node_kind::word, word_role::case_subject));
		// A `linebreak` is allowed on either side of `in`: `case a<newline>in` is one
		// clause in dash. Without skipping it the `in` was read as the first PATTERN
		// of the first item, which happened to produce dash's output for the cases
		// that reach it and would have become a syntax error here.
		skip_newlines();
		require(reserved::kw_in, defect);
		while (is_separator(peek().kind))
			advance();

		while (peek().kind != token_kind::end && peek_reserved() != reserved::kw_esac) {
			const uint32_t item_first = _index;
			const uint32_t item_mark = mark_scratch();
			uint32_t patterns = 0;
			parse_error item_defect = parse_error::none;

			// An optional leading '(' is allowed before the first pattern.
			if (peek().kind == token_kind::lparen)
				advance();

			while (peek().kind == token_kind::word) {
				_scratch.push(word_node(advance(), node_kind::word, word_role::pattern));
				++patterns;
				if (peek().kind != token_kind::pipe)
					break;
				advance();  // `|` separates alternative patterns
			}
			// The `)` closing the pattern list is what dash names for `case a in`
			// itself - `expecting ")"`. Recorded on the ITEM rather than on the clause
			// so `case a in a) x;; b x;; esac` points at the item that is wrong.
			require(token_kind::rparen, item_defect);

			// NOT require_list: a case item's `compound_list` is the one POSIX makes
			// optional, so `case a in b) ;; esac` and `case a in esac` are both valid
			// and dash runs them. The other exception is `program` itself (#58).
			_scratch.push(parse_compound_list());

			// Read before the terminator is consumed, so the node records which one
			// closed IT rather than whatever follows.
			const bool falls_through = peek().kind == token_kind::semi_and;

			node item;
			item.kind = node_kind::case_item;
			item.error = item_defect;
			item.first_token = item_first;
			item.last_token = _index > item_first ? _index - 1 : item_first;
			// The high bit says `;&` (POSIX.1-2024) closed this item rather than
			// `;;` or `esac` directly - the same packing for_loop uses for its name
			// token and `in` flag. run_case reads it to decide whether to keep
			// running into the NEXT item's body without testing its pattern.
			item.aux = patterns | (falls_through ? 0x80000000u : 0u);
			commit_children(item, item_mark);
			_scratch.push(_tree.add_node(item));

			if (peek().kind == token_kind::dsemi || peek().kind == token_kind::semi_and)
				advance();
			while (is_separator(peek().kind))
				advance();
			if (_index == item_first)  // progress guarantee
				break;
		}
		require(reserved::kw_esac, defect);

		node n;
		n.kind = node_kind::case_clause;
		n.error = defect;
		n.first_token = first;
		n.last_token = _index > first ? _index - 1 : first;
		commit_children(n, mark);
		return _tree.add_node(n);
	}

	node_index parse_brace_group() noexcept {
		const uint32_t first = _index;
		const uint32_t mark = mark_scratch();
		advance();  // `{`
		parse_error defect = parse_error::none;
		// `{ }` and `f() { }` both ran to status 0; dash refuses both, because the
		// grammar is `{ compound_list }` and a compound_list holds at least one
		// and_or. A group holding nothing but a REDIRECTION is a different thing and
		// stays valid - `{ >/dev/null; }` is a simple command with no words (#58).
		_scratch.push(require_list(parse_compound_list(), defect));
		require(reserved::kw_rbrace, defect);

		node n;
		n.kind = node_kind::brace_group;
		n.error = defect;
		n.first_token = first;
		n.last_token = _index > first ? _index - 1 : first;
		commit_children(n, mark);
		return _tree.add_node(n);
	}

	node_index parse_subshell() noexcept {
		const uint32_t first = _index;
		const uint32_t mark = mark_scratch();
		advance();  // `(`
		parse_error defect = parse_error::none;
		_scratch.push(require_list(parse_compound_list(), defect));  // `( )` - see above
		require(token_kind::rparen, defect);

		node n;
		n.kind = node_kind::subshell;
		n.error = defect;
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
		token after[2];
		probe_ahead(after, 2);
		return after[0].kind == token_kind::lparen &&
		       after[1].kind == token_kind::rparen;
	}

	// Lexes the next tokens WITHOUT consuming them, from wherever the next token
	// would actually come from: the innermost alias body first, the ones outside it
	// next, the input last.
	//
	// Probing the input alone was wrong the moment an alias could supply the
	// lookahead: `alias def='f()'` puts both parentheses in the alias body, and a
	// probe over the source saw whatever happened to sit at a virtual offset past
	// the end of it. Copies, because a probe must consume nothing - a lexer owns no
	// input and copying one is three words.
	// The alias a PROBED word names, if it names one. Separate from
	// try_substitute_alias because a probed token is not in the tree: its offsets are
	// relative to the body it was lexed from, so the text comes in as an argument
	// rather than through text_of_token.
	[[nodiscard]] bool probe_alias_value(const token& t, std::string_view from,
	                                     std::string_view& value) const noexcept {
		if (_aliases == nullptr || t.kind != token_kind::word || t.is_error())
			return false;
		if (static_cast<size_t>(t.offset) + t.length > from.size())
			return false;
		const std::string_view raw = from.substr(t.offset, t.length);
		char joined[64];
		std::string_view name;
		if ((t.flags & syntax::flag_literal) != 0)
			name = raw;
		else if (!name_across_line_continuations(raw, joined, sizeof joined, name))
			return false;
		// The same rule the real substitution applies: a reserved word is not an
		// alias. There is no plain-word position to except here, because a function
		// definition's name is never a keyword.
		if (reserved_of(name) != reserved::none)
			return false;
		return _aliases->lookup_alias(name, value);
	}

	void probe_ahead(token* out, size_t count) noexcept {
		// Text and position rather than lexer copies, so nothing here needs a lexer
		// that can be default-constructed. A lexer owns neither, which is what makes
		// resuming one at a recorded position the same operation as starting it.
		//
		// trailing_blank travels with the frame for the same reason it does on
		// alias_frame: the eligible word is the one drawn once the body RUNS OUT, and
		// a probe that ignored it could not see a lookahead token that does not exist
		// until an alias is substituted. `alias f='f ' p='()'` is exactly that - the
		// name ends one body and both parentheses are a second alias the trailing
		// blank makes eligible (alias-p.tst:439).
		struct probe_frame { std::string_view text; uint32_t at; bool trailing_blank; };
		probe_frame bodies[kMaxAliasDepth];
		size_t depth = 0;
		for (const auto& frame : _alias_stack) {
			if (depth == static_cast<size_t>(kMaxAliasDepth))
				break;
			bodies[depth++] = {frame.lex.source(), frame.lex.position(),
			                   frame.trailing_blank};
		}
		probe_frame input{_tree.source(), _lexer.position(), false};
		for (size_t i = 0; i < count; ++i) {
			bool eligible = false;
			// Bounded rather than `for (;;)`: each round either draws the token or
			// pushes a body, and the depth ceiling is what stops `alias a=b; alias b=a`
			// from spinning here as well as in try_substitute_alias.
			for (int guard = 0; guard <= kMaxAliasDepth; ++guard) {
				probe_frame& from = depth == 0 ? input : bodies[depth - 1];
				lexer probe{from.text, from.at};
				out[i] = probe.next(lex_mode::command);
				// The probe wants the next GRAMMAR token; trivia is not one (#103).
				while (out[i].kind == token_kind::comment)
					out[i] = probe.next(lex_mode::command);
				if (out[i].kind == token_kind::end && depth != 0) {
					eligible = eligible || from.trailing_blank;
					--depth;
					continue;
				}
				from.at = probe.position();
				if (!eligible)
					break;
				std::string_view value;
				if (!probe_alias_value(out[i], from.text, value) ||
				    depth == static_cast<size_t>(kMaxAliasDepth))
					break;
				// A body already being probed is not re-entered, which is the cycle
				// guard try_substitute_alias states over the real stack.
				bool cycle = false;
				for (size_t d = 0; d < depth; ++d)
					cycle = cycle || bodies[d].text == value;
				for (const auto& frame : _alias_stack)
					cycle = cycle || frame.text == value;
				if (cycle)
					break;
				bodies[depth++] = {value, 0,
				                   !value.empty() &&
				                       (value.back() == ' ' || value.back() == '\t')};
				// The head of the replacement is itself subject to substitution, as in
				// try_substitute_alias, so eligibility carries into the new body.
			}
		}
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

	// Offers the word the parser is looking at to alias substitution.
	//
	// Called from two kinds of place. In COMMAND POSITION, because that is where
	// POSIX substitutes an alias - `ls foo` substitutes ls, `echo ls` does not - and
	// BEFORE the word is classified, because the replacement is RE-SCANNED and may
	// itself be a keyword (`alias i='if echo'`), a function definition
	// (`alias def='f()'`) or a `!`. Substituting inside parse_command, after the
	// keyword and function-definition tests had already run on the un-substituted
	// word, is why a third of alias-p.tst failed while substitution itself worked.
	//
	// And from fill(), for the word a TRAILING BLANK in a definition made eligible,
	// wherever that word sits. See the call there.
	void substitute_word() noexcept {
		if (_aliases != nullptr)
			try_substitute_alias();
	}

	[[nodiscard]] node_index parse_command_or_compound() noexcept {
		substitute_word();
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
		// Where this body lives in the tree's virtual text space, so the tokens it
		// yields can be read back. See tree::add_text_region.
		uint32_t base;
		// The definition ended in a blank, which makes the word AFTER this body
		// eligible for substitution as well. Recorded on the frame rather than
		// checked at substitution time because the eligible word is the one drawn
		// once this body RUNS OUT - `alias e='echo '` followed by `e c c` substitutes
		// the first `c`, not the `echo` inside the body.
		bool trailing_blank;
	};

	void fill() noexcept {
		// Draw from an alias body while one is active, popping when it runs out.
		bool eligible = false;
		while (!_alias_stack.empty()) {
			token t = _alias_stack.back().lex.next(lex_mode::command);
			// Trivia from an alias body: recorded at its virtual offset - the same
			// shift every real token from this frame gets - and drawn past (#103).
			if (t.kind == token_kind::comment) {
				_tree.add_comment({t.offset + _alias_stack.back().base, t.length});
				continue;
			}
			if (t.kind != token_kind::end) {
				// The lexer reports offsets within the body it was handed; the tree
				// addresses that body above the input. Without the shift a token from an
				// alias reads back as a slice of the script.
				alias_frame& frame = _alias_stack.back();
				t.offset += frame.base;
				if (t.is_error())
					t.error_offset += frame.base;
				_index = _tree.add_token(t);
				// A here-document opened on this line takes its body from the lines that
				// follow the newline ENDING that line - and when the newline came from an
				// alias body, those lines are in the alias body too. Collected before the
				// eligible word is substituted, so the body is never lexed as commands.
				if (!_pending_here_docs.empty() && t.kind == token_kind::newline) {
					const uint32_t resume = collect_here_doc_bodies(
						frame.text, frame.base, t.end_offset());
					_alias_stack.back().lex.seek(resume - frame.base);
				}
				if (eligible)
					substitute_word();
				return;
			}
			eligible = eligible || _alias_stack.back().trailing_blank;
			_alias_stack.pop_back();
			_alias_depth = _alias_stack.size();
		}

		// Where the input stands BEFORE this token is drawn from it. A caller reading
		// one command at a time resumes here, so the lookahead token this fill is
		// about to produce is read again rather than lost.
		_input_cursor = _lexer.position();
		token drawn = _lexer.next(lex_mode::command);
		// A comment is trivia: recorded for the painter, excluded from the token
		// array so no node's span moves (#103). The cursor advances past it, so a
		// caller reading one command at a time does not record it twice.
		while (drawn.kind == token_kind::comment) {
			_tree.add_comment({drawn.offset, drawn.length});
			_input_cursor = _lexer.position();
			drawn = _lexer.next(lex_mode::command);
		}
		_index = _tree.add_token(drawn);
		if (_lexer.incomplete())
			_tree.set_incomplete(true);

		// A here-doc body begins after the newline that ends the command line.
		// Collect the pending ones and resume past them, so the body is never
		// lexed as commands.
		if (!_pending_here_docs.empty() &&
		    _tree.token_at(_index).kind == token_kind::newline) {
			const uint32_t after_newline = _tree.token_at(_index).end_offset();
			// A body is read from the INPUT, on the lines after the one that opened it.
			// A newline drawn from an ALIAS body has no such line, and seeking the
			// input lexer to an alias offset would silently skip the rest of the
			// script - so the here-doc is left unterminated, which is what it is.
			if (after_newline <= _tree.source().size())
				_lexer.seek(collect_here_doc_bodies(_tree.source(), 0, after_newline));
			else {
				_pending_here_docs.clear();
				_tree.set_incomplete(true);
			}
		}

		// The trailing-blank rule reaches out of the alias body and into the input:
		// `alias e='echo '` makes the `c` of `e c` eligible too. Applied as the word
		// is READ rather than at each grammatical position that might ask, which is
		// what gets the argument, case-pattern and `for`-word positions right without
		// a call in each of them - `alias c='case a in ' p='(a)'` closes a case
		// clause with `c p`.
		if (eligible)
			substitute_word();
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
			skip_linebreak();
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

		// Before the `!` test, because an alias may expand to one: `alias e='! echo'`.
		substitute_word();

		// `! pipeline` inverts the pipeline's status: zero becomes one and anything
		// non-zero becomes zero. POSIX puts Bang in the pipeline production, so it
		// binds to the WHOLE pipeline - `! a | b` negates the pipeline, not `a`.
		if (accept(reserved::kw_bang)) {
			const uint32_t child_mark = mark_scratch();
			_scratch.push(parse_pipeline());
			node neg;
			neg.kind = node_kind::negation;
			neg.first_token = first;
			neg.last_token = _index > first ? _index - 1 : first;
			commit_children(neg, child_mark);
			return _tree.add_node(neg);
		}

		const uint32_t mark = mark_scratch();
		_scratch.push(parse_command_or_compound());
		while (peek().kind == token_kind::pipe) {
			advance();
			skip_linebreak();
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
				// POSIX substitutes an alias for the COMMAND WORD, which is the first
				// word that is not an assignment: `alias s=sh; a=A s -c ...` runs sh, and
				// `alias e=echo; >/dev/null e x` runs echo. The word that opened the
				// command was already offered before it was classified (see
				// substitute_word); this is the one that follows a prefix.
				//
				// Round again only when something was actually replaced - the
				// replacement may carry its own prefix, as ` >&- c >/dev/null ` does, and
				// its command word deserves the same turn. A word that is not an alias
				// falls through and is consumed, so the loop cannot spin.
				if (!seen_command_name && _scratch.size() > mark &&
				    !looks_like_assignment(text_of_token(_index))) {
					const uint32_t before = _index;
					substitute_word();
					if (_index != before)
						continue;
				}
				const uint32_t at = advance();
				node_kind kind = node_kind::word;
				word_role role = word_role::ordinary;
				if (!seen_command_name && looks_like_assignment(text_of_token(at)))
					kind = node_kind::assignment;
				else if (!seen_command_name) {
					seen_command_name = true;
					// The flip IS the classification: this word names the command
					// (#103). The expander treats it exactly as ordinary; the role is
					// for the readers that resolve it - highlighter, completer.
					role = word_role::command_name;
				}
				_scratch.push(word_node(at, kind, role));
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

	// Collects every pending here-doc body out of `src`, whose bytes begin at `base`
	// in the tree's virtual text space, starting at the virtual offset `from`.
	// Returns where the last body ended, again virtual, so lexing can resume past it.
	//
	// TEXT AND BASE RATHER THAN THE INPUT, because a here-document can be written
	// entirely inside an alias body: `alias c='cat <<\END' d='c<newline>here-doc
	// <newline>END'` puts the operator in one definition and the body in the other,
	// and dash runs it (alias-p.tst:223). The body then lives in the alias text, and
	// a collector that could only read _tree.source() had nowhere to find it. Both
	// callers hand over the buffer the NEWLINE came from, which is the buffer whose
	// following lines the body occupies.
	//
	// The parser does this, not the lexer. The lexer never reads and never seeks
	// on its own - it is handed a buffer and a position, which is exactly what
	// makes it safe to run on every keystroke over an editor's buffer.
	uint32_t collect_here_doc_bodies(std::string_view src, uint32_t base,
	                                 uint32_t from) noexcept {
		uint32_t at = from - base;

		for (const auto& pending : _pending_here_docs) {
			const std::string_view raw = text_of_token(pending.delimiter_token);

			// A quoted delimiter suppresses expansion in the body: <<'EOF' is
			// literal, <<EOF is expanded. flag_literal is nearly that test, but it is
			// cleared by a LINE CONTINUATION too, which quotes nothing - so
			// `<<E\<newline>ND` would have suppressed expansion in a body dash
			// expands. Re-scanned for the real thing.
			const bool quoted = (_tree.token_at(pending.delimiter_token).flags &
			                     syntax::flag_literal) == 0 &&
			                    delimiter_is_quoted(text_of_token(pending.delimiter_token));
			// The delimiter is compared with quote removal applied on the fly - see
			// here_doc_delimiter_matches. `raw` is the word exactly as it was typed.

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

				if (here_doc_delimiter_matches(raw, compared)) {
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
			// Back into the tree's virtual space, where the executor reads it from.
			body.offset = base + body_start;
			body.length = body_end > body_start ? body_end - body_start : 0;
			body.expand = !quoted;
			body.strip_tabs = n.aux != 0xFFFFFFFFu && _tree.here_doc_at(n.aux).strip_tabs;
			body.fd = n.aux != 0xFFFFFFFFu ? _tree.here_doc_at(n.aux).fd : 0;
			// Overwrite the placeholder recorded when the operator was seen.
			const_cast<syntax::here_doc_body&>(_tree.here_doc_at(n.aux)) = body;
		}

		_pending_here_docs.clear();
		return base + at;
	}

	// The alias name a word spells once its LINE CONTINUATIONS are removed, for a
	// word the lexer did not call plainly literal.
	//
	// `ee\<newline>e\<newline>e` is the name `eeee`: a line continuation quotes
	// nothing, and dash substitutes an alias for it. Every other backslash and quote
	// does quote, and blocks substitution outright - `ech\o` is never an alias - so
	// this accepts only the bytes POSIX allows in an alias name and refuses the rest
	// rather than guessing. False also for a name longer than the buffer, which is
	// not a name any script writes.
	[[nodiscard]] static bool name_across_line_continuations(
		std::string_view text, char* buffer, size_t capacity,
		std::string_view& name) noexcept {
		size_t at = 0;
		bool joined_any = false;
		for (size_t i = 0; i < text.size(); ++i) {
			if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '\n') {
				++i;
				joined_any = true;
				continue;
			}
			const unsigned char c = static_cast<unsigned char>(text[i]);
			const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			                     (c >= '0' && c <= '9') || c == '_' || c == '!' ||
			                     c == '%' || c == ',' || c == '@';
			if (!allowed || at == capacity)
				return false;
			buffer[at++] = text[i];
		}
		if (!joined_any)
			return false;  // nothing was joined, so the word is quoted some other way
		name = std::string_view{buffer, at};
		return true;
	}

	// Replaces a word with its alias definition, if it has one.
	//
	// A RESERVED WORD is never substituted, whatever its position. POSIX checks the
	// word against the reserved words first and only then against the alias table,
	// so `alias if=:` cannot shadow `if` - while an alias that EXPANDS to `if` still
	// produces the keyword, because the replacement is re-scanned. The rule holds
	// for a word a trailing blank made eligible too: `alias forx='for x ' do=';'`
	// leaves `forx do echo $x; done` looping over the positional parameters, which
	// is the case alias-p.tst calls an inapplicable substitution.
	void try_substitute_alias() noexcept {
		for (int guard = 0; guard < kMaxAliasDepth; ++guard) {
			if (peek().kind != token_kind::word || peek().is_error())
				return;
			// A reserved word blocks substitution only where the grammar could accept
			// one. See _plain_word_position: `in` after `for` is a NAME, not a keyword.
			if (!_plain_word_position && peek_reserved() != reserved::none)
				return;
			// A quoted word is never an alias: `\ls` and `'ls'` are how you bypass
			// one, and flag_literal is exactly that test - with one exception, a LINE
			// CONTINUATION, which quotes nothing.
			char joined[64];
			std::string_view name;
			if ((peek().flags & syntax::flag_literal) != 0)
				name = text_of_token(_index);
			else if (!name_across_line_continuations(text_of_token(_index), joined,
			                                        sizeof joined, name))
				return;

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
			//
			// The word's own offset goes to the region as its invocation site (#76).
			// It is read BEFORE the substitution because that is the last moment it
			// exists: from here on the parser is looking at the replacement. For a
			// NESTED alias this offset is itself inside the outer body, which is
			// exactly what lets invocation_of walk the whole chain out.
			const uint32_t invoked_at = peek().offset;
			// The word AS SPELLED, not `name`: across a line continuation `name`
			// views `joined`, a buffer that dies with this scope, and a region
			// outlives the parse. The spelling is a view into the tree's own text and
			// is what the user typed, which is what a diagnostic should quote back.
			const std::string_view spelled = text_of_token(_index);
			const bool trailing_blank =
				!value.empty() && (value.back() == ' ' || value.back() == '\t');
			_alias_stack.push_back(
				{lexer{value}, value,
				 _tree.add_text_region(value, spelled, invoked_at), trailing_blank});
			_alias_depth = _alias_stack.size();
			fill();

			// Round again, because POSIX RE-SCANS the replacement: the first word of
			// the definition is itself subject to substitution, which is what makes
			// `alias e='echo echo'` with an aliased `echo` expand twice. The loop used
			// to be entered only when the definition ended in a blank, which confused
			// re-scanning with the trailing-blank rule and got both wrong.
		}
	}

	node_index parse_redirect() noexcept {
		const uint32_t first = _index;
		uint32_t fd = 0xFFFFFFFFu;  // unspecified: the operator's default applies

		if (peek().kind == token_kind::io_number) {
			const std::string_view digits = text_of_token(_index);
			// Accumulated through the shared checked form, though the LEXER has
			// already guaranteed it fits: a run of digits too large to be a descriptor
			// is not lexed as an IO_NUMBER at all. Both take the limit from the same
			// policy row, which is what stops the guarantee from quietly ceasing to
			// hold (#63).
			uint64_t value = 0;
			const uint64_t limit = static_cast<uint64_t>(
				lesh::policy_for(lesh::numeric_site::redirection_word_fd).high);
			for (size_t i = 0; i < digits.size(); ++i) {
				// The token spans any line continuations between the digits, because
				// the lexer records the extent it consumed rather than the text it
				// means. `1\<newline>2>file` is fd 12.
				if (digits[i] == '\\' && i + 1 < digits.size() && digits[i + 1] == '\n') {
					++i;
					continue;
				}
				(void)lesh::accumulate_digit(
					value, static_cast<uint64_t>(digits[i] - '0'), 10, limit);
			}
			fd = static_cast<uint32_t>(value);
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
			placeholder.fd = fd == 0xFFFFFFFFu ? 0 : fd;
			node n;
			n.kind = node_kind::here_doc;
			n.first_token = first;
			n.last_token = delimiter;
			// `cat <<"EOF` - the DELIMITER is a word, and an unterminated one makes
			// the redirection as unrunnable as an unterminated argument does.
			if (_tree.token_at(delimiter).is_error())
				n.error = parse_error::unterminated_word;
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
		// The target becomes a WORD NODE like every other word, so it has a span
		// for a painter and a place for completion to stand (#103). The executor
		// still reads the token at last_token and expands its text - the child is
		// additive, and its role records what POSIX 2.7 makes the target:
		// expanded, never field-split.
		const uint32_t children = mark_scratch();
		_scratch.push(word_node(target, node_kind::word, word_role::redirect_target));
		node n;
		n.kind = node_kind::redirect;
		n.first_token = first;
		n.last_token = target;
		// `cat > "x` opened a file called `x` with the quote still open, and
		// reported "No such file or directory" for a name the shell should never
		// have expanded. dash calls it a syntax error, and so does this.
		if (_tree.token_at(target).is_error())
			n.error = parse_error::unterminated_word;
		n.aux = fd;
		commit_children(n, children);
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

	// A node for one word token, carrying that token's own defect.
	//
	// The defect has to travel with the word or nothing downstream can see it:
	// tree::has_errors() is what stops a tree with an unterminated quote in it from
	// being executed, and a word built without this step is a word the shell runs
	// anyway. Recorded here, in the one place a word node is made, rather than at
	// each of the four grammatical positions that make one - the command's own
	// words did it and the `for` list, the `case` subject and its patterns did not,
	// so `for i in "a; do echo $i; done` was accepted in silence (#47).
	//
	// The kind is a parameter because an assignment prefix is the same token with
	// the same defect: `x="abc` is as unterminated as `echo "abc`.
	[[nodiscard]] node_index word_node(uint32_t at, node_kind kind,
	                                   word_role role = word_role::ordinary) noexcept {
		node w;
		w.kind = kind;
		w.first_token = at;
		w.last_token = at;
		w.aux = static_cast<uint32_t>(role);
		if (_tree.token_at(at).is_error())
			w.error = parse_error::unterminated_word;
		return _tree.add_node(w);
	}

	[[nodiscard]] std::string_view text_of_token(uint32_t i) const noexcept {
		return _tree.text_of_token(_tree.token_at(i));
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
	// The word about to be drawn stands where the grammar accepts NO reserved word,
	// so the alias table applies to it even if it spells one. POSIX 2.4 recognises a
	// reserved word only where one can appear, and the `for` NAME is the position
	// where that distinction is observable: `alias f=' for ' w=' in ' in=' x '` makes
	// `f w in 1` a loop over `x`, because the `in` from `w`'s body is a name there
	// rather than the keyword (alias-p.tst:298, which dash also passes).
	//
	// A flag on the parser rather than an argument to try_substitute_alias, because
	// substitution happens when the token is READ - inside the advance() that
	// consumes `for` - and the reading is several frames below the parse function
	// that knows which position it is filling.
	bool _plain_word_position = false;
	size_t _alias_depth = 0;
	static constexpr int kMaxAliasDepth = 16;
	std::vector<uint32_t> _strip_tabs_for;
	uint32_t _index = 0;
	uint32_t _input_cursor = 0;
};

} // namespace

tree parse(buffer_pool& pool, std::string_view source,
           const alias_source* aliases) noexcept {
	parser_impl p{pool, source, aliases};
	p.parse_program();
	return p.take();
}

tree parse_next_command(buffer_pool& pool, std::string_view source, size_t& position,
                        const alias_source* aliases) noexcept {
	parser_impl p{pool, source, aliases, static_cast<uint32_t>(position)};
	p.parse_complete_command();
	position = p.input_cursor();
	return p.take();
}

bool is_reserved_word(std::string_view text) noexcept {
	return reserved_of(text) != reserved::none;
}

} // namespace lesh::syntax
