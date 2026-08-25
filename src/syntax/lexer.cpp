#include "syntax/lexer.h"

#include "substrate/numeric.h"

#include "substrate/char_utils.h"

namespace lesh::syntax {

namespace {

constexpr bool is_blank(char c) noexcept { return c == ' ' || c == '\t'; }
constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// Bytes whose presence in a word means expansion or quote removal has work to do.
// Their absence is what flag_literal records.
constexpr bool needs_expansion(char c) noexcept {
	switch (c) {
		case '$': case '`': case '\\': case '\'': case '"':
		case '~': case '*': case '?': case '[':
			return true;
		default:
			return false;
	}
}

} // namespace

bool lexer::skip_blanks() noexcept {
	const uint32_t start = _position;
	for (;;) {
		while (!at_end() && is_blank(peek()))
			++_position;
		// A line continuation between tokens is nothing at all, so it is skipped
		// here with the blanks. Left in place it began a WORD - `\<newline>{` lexed
		// as one word rather than as the reserved `{` - which is eleven of
		// quote-p.tst's cases (#42).
		if (const uint32_t after = past_continuations(_position); after != _position) {
			_position = after;
			continue;
		}
		break;
	}
	return _position != start;
}

// True when a byte at `p` sits where a WORD could begin, which is the only place
// POSIX lets a `#` open a comment. `begin` is where the region being scanned
// started, so its first byte counts as word-initial.
bool lexer::starts_a_word(uint32_t p, uint32_t begin) const noexcept {
	if (p <= begin)
		return true;
	switch (_source[p - 1]) {
		case ' ': case '\t': case '\n':
		case ';': case '&': case '|': case '(': case ')':
			return true;
		default:
			return false;
	}
}

// True where a COMMAND could begin at `p`, which is the only place `case` and
// `esac` are RESERVED rather than ordinary words - `echo case` prints a word.
// `begin` is where the scanned region started, so its first byte counts.
//
// The parser's rule, approximated over bytes because the scan keeps no token
// history: the previous non-blank ends a command, or the word before it is one
// that introduces one. Deliberately CONSERVATIVE. A `case` this declines to
// recognise leaves the scan where #68 found it, while one it recognises WRONGLY
// would eat a paren that really did close the substitution - so `then case` is
// worth listing and anything more elaborate is not worth guessing at.
bool lexer::starts_a_command(uint32_t p, uint32_t begin) const noexcept {
	uint32_t q = p;
	while (q > begin && is_blank(_source[q - 1]))
		--q;
	if (q <= begin)
		return true;
	switch (_source[q - 1]) {
		case '\n': case ';': case '&': case '|': case '(': case ')':
			return true;
		default:
			break;
	}
	uint32_t w = q;
	while (w > begin && !is_word_terminator(_source[w - 1]))
		--w;
	const std::string_view before = _source.substr(w, q - w);
	return before == "then" || before == "else" || before == "elif" ||
	       before == "do" || before == "{" || before == "!";
}

bool command_sub_interior(std::string_view source, const token& segment,
                          uint32_t& begin, uint32_t& end) noexcept {
	if (segment.kind != token_kind::seg_command_sub)
		return false;
	// A token drawn from an ALIAS body sits above the input in the tree's virtual
	// text space (see tree::add_text_region), and this function is handed the
	// input. There is no interior to name here, so there is none reported.
	if (segment.offset >= source.size() || segment.length == 0)
		return false;
	uint32_t stop = segment.end_offset();
	if (stop > source.size())
		stop = static_cast<uint32_t>(source.size());
	// The construct CLOSED, so its last byte is the `)` or the backquote. When it
	// never closed - `echo $(ls` - the interior simply runs to the end of what was
	// typed, which is what a line editor wants to paint.
	if (segment.error == token_error::none && stop > segment.offset)
		--stop;

	uint32_t open;
	if (source[segment.offset] == '`') {
		open = segment.offset + 1;
		// BACKQUOTES ARE EXCLUDED THE MOMENT A BACKSLASH APPEARS. POSIX 2.6.3 makes
		// the backquoted text the interior AFTER removing a `\` that precedes `$`,
		// a backquote or another `\` - so `` `echo \`date\`` ``'s interior is not a
		// run of input bytes at all, it is a rewritten string. The expander can
		// rewrite it because it owns a buffer (unescape_backquotes); this cannot,
		// because every span the parser hangs on the result has to be a real input
		// offset or a decoration lands on text the user never typed. A backquoted
		// interior with NO backslash in it needs no rewriting and is descended into
		// like any other; one with a backslash stays opaque, which is the same
		// answer the segment gave before this existed.
		for (uint32_t i = open; i < stop; ++i)
			if (source[i] == '\\')
				return false;
	} else {
		if (source[segment.offset] != '$')
			return false;
		// `$\<newline>(` is a command substitution: POSIX removes the continuation
		// before the input is tokenised, so the opener is three bytes wide here and
		// two there. Looked for rather than assumed, because assuming `offset + 2`
		// would put the interior one byte inside the `(`.
		const uint32_t at = past_continuations(source, segment.offset + 1);
		if (at >= source.size() || source[at] != '(')
			return false;
		open = at + 1;
	}
	if (open > stop)
		return false;
	begin = open;
	end = stop;
	return true;
}

bool here_doc_delimiter_matches(std::string_view raw, std::string_view line) noexcept {
	size_t r = 0, l = 0;
	while (r < raw.size()) {
		const char c = raw[r];
		// A line continuation in the delimiter contributes nothing: `<<E\<newline>ND`
		// is terminated by `END`. Read as an escape it tried to match a newline
		// inside the line and never matched at all.
		if (c == '\\' && r + 1 < raw.size() && raw[r + 1] == '\n') {
			r += 2;
			continue;
		}
		// `$'...'` is one more spelling of a QUOTED delimiter, beside `\END`,
		// `'END'` and `"END"` - so `<<$'E\x4ED'` ends the body at a line reading
		// `END` and the body is not expanded. bash and zsh both do this.
		//
		// Compared as it decodes, one step at a time, because this function exists
		// to compare WITHOUT a buffer: it runs once per line of every here-document
		// and has no business allocating to answer a question about equality.
		if (c == '$' && r + 1 < raw.size() && raw[r + 1] == '\'') {
			size_t b = r + 2;
			for (;;) {
				if (b >= raw.size())
					return false;  // unterminated: it cannot match anything
				if (raw[b] == '\'') {
					r = b + 1;
					break;
				}
				// `raw` itself is the body: the closing quote is found by the loop
				// rather than bounded up front, and an escape that swallows the quote
				// simply carries the scan past it to the `b >= raw.size()` above -
				// which is what `$'a\'` should do, since it never closes.
				const ansi_c_step step = decode_ansi_c_escape(raw, b);
				if (step.truncates)
					// A NUL ends the delimiter's VALUE, so nothing after it can
					// contribute and the line must already be fully matched.
					return l == line.size();
				for (uint8_t i = 0; i < step.count; ++i) {
					if (l >= line.size() || static_cast<uint8_t>(line[l]) != step.bytes[i])
						return false;
					++l;
				}
				b += step.consumed;
			}
			continue;
		}
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

ansi_c_step decode_ansi_c_escape(std::string_view body, size_t at) noexcept {
	ansi_c_step step;
	auto one = [&](uint8_t b, uint8_t used) {
		// A decoded NUL ends the string rather than being emitted. Every escape that
		// can produce zero - `\0`, `\x00`, an octal run that overflows a byte, and
		// `\c@` - arrives here, so they get one answer in one place.
		step.consumed = used;
		if (b == 0) {
			step.truncates = true;
			return step;
		}
		step.bytes[0] = b;
		step.count = 1;
		return step;
	};
	auto two = [&](uint8_t a, uint8_t b) {
		step.bytes[0] = a;
		step.bytes[1] = b;
		step.count = 2;
		step.consumed = 2;
		return step;
	};

	const auto c = static_cast<uint8_t>(body[at]);
	// A trailing backslash escapes nothing: it is the last byte of the value, not
	// the start of a step that reads past the end.
	if (c != '\\' || at + 1 >= body.size())
		return one(c, 1);

	const auto e = static_cast<uint8_t>(body[at + 1]);
	switch (e) {
		case 'a': return one(0x07, 2);
		case 'b': return one(0x08, 2);
		case 'f': return one(0x0c, 2);
		case 'n': return one(0x0a, 2);
		case 'r': return one(0x0d, 2);
		case 't': return one(0x09, 2);
		case 'v': return one(0x0b, 2);
		case '\\': return one('\\', 2);
		case '\'': return one('\'', 2);
		case '"': return one('"', 2);
		// `\e` and `\E` are ESC. They are NOT in POSIX Issue 8's list of escapes,
		// and this was left out on that reading until quote-p.tst:402 - the single
		// assertion #75 exists to move - turned out to REQUIRE it: its expected
		// bytes carry 033 for `\e`, and bash and zsh both produce it. A set the
		// ticket's own measure rejects is the wrong set, so `\e` is in.
		case 'e': case 'E': return one(0x1b, 2);
		default: break;
	}

	// `\xHH`: at most TWO hex digits, which is what keeps it a BYTE escape -
	// `\x414243` is 0x41 followed by the four text bytes `4243` in bash and zsh
	// alike, not a wide character. One digit is enough, and the digits are
	// case-insensitive though the `x` is not: `\XA` is not an escape.
	if (e == 'x') {
		// Through scan_digits, the one numeric reader the project has (#62, #63),
		// rather than a hex table beside it. The DIGIT COUNT is expressed by handing
		// it a bounded substring rather than by counting: scan_digits reads a run and
		// stops at the first non-digit, so a two-byte window is exactly "at most two
		// digits" - which is what leaves `4243` as text in `\x414243`.
		const lesh::digit_run run = lesh::scan_digits(body.substr(at + 2, 2), 16, 0xff);
		// No digit at all means this was never an escape. bash keeps both bytes,
		// zsh emits a NUL; keeping them is the rule an unrecognised escape follows
		// below, so there is ONE rule for "not an escape after all" and not two.
		if (run.consumed == 0)
			return two('\\', 'x');
		return one(static_cast<uint8_t>(run.value), static_cast<uint8_t>(2 + run.consumed));
	}

	// `\NNN`: at most THREE octal digits. POSIX Issue 8 spells the escape `\0nnn`,
	// which reads as a mandatory zero PLUS three digits - and no shell implements
	// that. bash and zsh both take up to three digits with the leading zero merely
	// one of them, so `\101` is 'A' and `\0101` is 0x08 followed by the text `1`.
	// Two shells agreeing against a literal reading of the draft is the stronger
	// evidence, so this is what they do. The value is masked to a byte, which is
	// where `\400` becomes NUL and truncates.
	if (e >= '0' && e <= '7') {
		// A three-byte window, for the same reason: three octal digits at most. The
		// limit is 0777 rather than 0377 because three digits CAN exceed a byte and
		// the excess is masked rather than refused - `\400` is the NUL that
		// truncates, which is bash's answer, and clamping at 0377 would have made it
		// 0xff instead.
		const lesh::digit_run run = lesh::scan_digits(body.substr(at + 1, 3), 8, 0777);
		return one(static_cast<uint8_t>(run.value & 0xff),
		           static_cast<uint8_t>(1 + run.consumed));
	}

	// `\cX`: toupper(X) XOR 0x40, which is why `\cA` and `\ca` are both 001, `\c?`
	// is 0177 and `\c@` is the NUL that truncates. zsh does NOT implement `\cX` at
	// all - it prints a literal `cA` - so this follows bash and POSIX Issue 8
	// against zsh rather than with it. A `\c` at the very end is not an escape.
	if (e == 'c') {
		if (at + 2 >= body.size())
			return two('\\', 'c');
		auto x = static_cast<uint8_t>(body[at + 2]);
		uint8_t used = 3;
		// The character `\c` applies to is itself read as an escape first, so
		// control-backslash is spelled `\c\\` and takes FOUR bytes. This is where
		// lesh and BASH part company, deliberately: bash takes the raw byte after
		// `\c`, so it reads `\c\` as control-backslash and then has `\c?` left over
		// as three stray bytes - and it FAILS quote-p.tst:402 for exactly that,
		// producing `1c 5c 63 3f` where the suite expects `1c 7f`. yash's reading is
		// taken instead, for two reasons: it is what the POSIX conformance suite
		// asserts, and it is the only one under which control-backslash can be
		// written at all - under bash's there is no unambiguous spelling of it.
		// zsh does not implement `\cX` and so casts no vote.
		if (x == '\\' && at + 3 < body.size() && body[at + 3] == '\\') {
			x = '\\';
			used = 4;
		}
		if (x >= 'a' && x <= 'z')
			x = static_cast<uint8_t>(x - ('a' - 'A'));
		return one(static_cast<uint8_t>(x ^ 0x40), used);
	}

	// Anything else is not an escape and KEEPS ITS BACKSLASH, as in bash; zsh
	// drops it. `\e`, `\u`, `\U` and `\E` reach here deliberately: they are bash
	// extensions rather than POSIX Issue 8 escapes, and this implements the
	// standard's set. A backslash-newline reaches here too - it is NOT a line
	// continuation inside `$'...'`, so both bytes survive, which is again bash's
	// answer and the one that falls out of having a single rule.
	return two('\\', e);
}

uint32_t lexer::skip_dollar_single_quote(uint32_t at, bool* terminated) const noexcept {
	if (terminated != nullptr)
		*terminated = false;
	uint32_t p = at + 2;  // past the `$` and the opening quote
	while (p < _source.size() && _source[p] != '\'') {
		// A backslash consumes the next byte for DELIMITING purposes whatever it
		// turns out to mean, which is what makes `\'` not close the construct. This
		// is the one way the extent differs from a plain `'...'`, and it is why
		// three scans call this instead of the single-quote one.
		if (_source[p] == '\\' && p + 1 < _source.size()) {
			p += 2;
			continue;
		}
		++p;
	}
	if (p < _source.size()) {
		if (terminated != nullptr)
			*terminated = true;
		return p + 1;
	}
	return p;
}

uint32_t lexer::skip_quoted_or_expansion(uint32_t at, bool inside_double_quotes,
                                         bool* terminated) const noexcept {
	// What ONE open construct has to remember. A struct rather than the parallel
	// arrays this began as, because the list grew past a closer: whether a single
	// quote is a quote there, whether the level is arithmetic rather than a
	// command list, and how many `case` clauses are open inside it (#68).
	struct level {
		char closer;
		// Whether a single quote is an ordinary byte at this level. `${...}`
		// INHERITS the context it was opened in, while `$(...)` and a backquote
		// start the shell language over and so reset it - which is why this travels
		// beside the closer rather than being read off the bytes.
		bool ordinary_single;
		// `$((` rather than `$(`. The two are told apart because a command list and
		// an arithmetic expression disagree about the very bytes this scan now
		// reads: `<<` is a here-document operator in one and a SHIFT in the other,
		// and reading `$((1<<2))`'s shift as an operator sent the scan looking for
		// a body that does not exist.
		bool arithmetic;
		uint8_t awaiting_in;  // `case` seen, its `in` not yet
		uint8_t open_cases;   // between `in` and `esac`, where `)` ends a pattern list
	};
	level stack[kMaxScanNesting];
	// A here-document operator seen at a `$( )` level, waiting for the newline
	// whose NEXT line begins its body. POSIX puts the body after the newline and
	// not after the operator, so `cat <<A <<B` has two of these outstanding at
	// once and they are consumed in the order the operators were seen.
	struct pending_here_doc {
		uint32_t offset;  // the delimiter word, exactly as it was written
		uint32_t length;
		int at_depth;     // the depth it belongs to: an inner line is consumed first
		bool strip_tabs;  // `<<-`
	};
	// More outstanding bodies than one line of any real script has. Past the cap
	// the operator is simply not recorded, which leaves the scan behaving as it
	// did before #68 rather than mis-attributing a body to the wrong delimiter.
	static constexpr int kMaxPendingHereDocs = 16;
	pending_here_doc pending[kMaxPendingHereDocs];
	int pending_count = 0;
	int depth = 0;
	uint32_t p = at;

	bool too_deep = false;
	auto open = [&](char closer, uint32_t width, bool single_is_ordinary,
	                bool arithmetic = false) {
		if (depth < kMaxScanNesting) {
			stack[depth] = {closer, single_is_ordinary, arithmetic, 0, 0};
			++depth;
		} else {
			too_deep = true;
		}
		p += width;
	};
	auto close = [&] {
		--depth;
		// A here-document operator whose body never arrived dies with its level
		// rather than attaching itself to the next newline outside it - which is
		// what `$((1<<2))` produces when the arithmetic guard is not enough.
		while (pending_count > 0 && pending[pending_count - 1].at_depth > depth)
			--pending_count;
		++p;
	};

	if (terminated != nullptr)
		*terminated = false;

	const char first = char_at(p);
	if (first == '\'' && !inside_double_quotes) {
		// Nothing inside single quotes is ever special, so there is no stack to
		// keep: the run ends at the next quote or at the end of the input.
		++p;
		while (p < _source.size() && _source[p] != '\'')
			++p;
		if (p < _source.size()) {
			if (terminated != nullptr)
				*terminated = true;
			return p + 1;
		}
		return p;
	}
	if (first == '"') {
		open('"', 1, /*single_is_ordinary=*/true);
	} else if (first == '`') {
		open('`', 1, /*single_is_ordinary=*/false);
	} else if (first == '$') {
		// `$'...'` when a single quote is a quote here - the same run the loop below
		// recognises, reached when a caller hands this function the `$` itself.
		if (char_at(p + 1) == '\'' && !inside_double_quotes)
			return skip_dollar_single_quote(p, terminated);
		// The brace or paren may be separated from the `$` by line continuations,
		// which POSIX removed before tokenising. Missing that returned `at`
		// unchanged, and a caller that assigns the result to its own cursor then
		// makes no progress at all - which is a hang, not a wrong answer.
		const uint32_t after_dollar = past_continuations(p + 1);
		if (char_at(after_dollar) == '{') {
			p = after_dollar;
			open('}', 1, inside_double_quotes);
		} else if (char_at(after_dollar) == '(') {
			p = after_dollar;
			open(')', 1, /*single_is_ordinary=*/false,
			     char_at(past_continuations(after_dollar + 1)) == '(');
		} else {
			return at;
		}
	} else {
		return at;
	}

	while (depth > 0 && p < _source.size()) {
		level& cur = stack[depth - 1];
		// Whether the bytes at this level are a COMMAND LIST. Only there do `case`,
		// `esac` and `<<` mean what the shell grammar says they mean; inside quotes,
		// a `${...}` or an arithmetic expression they are ordinary text.
		const bool command_list = cur.closer == ')' && !cur.arithmetic;
		const char c = _source[p];
		// A backslash consumes the next byte for DELIMITING purposes wherever it
		// appears, which is what makes `\`` not close a backquote and `\"` not close
		// a quoted string. What it MEANS is the expander's business.
		if (c == '\\' && p + 1 < _source.size()) {
			p += 2;
			continue;
		}
		if (c == cur.closer) {
			// A `)` between a case clause's `in` and its `esac` ends a PATTERN LIST
			// and is not the substitution's own. Counted as ours, `$(case a in a) echo
			// x;; esac)` ended at the pattern's paren and the rest of the clause was
			// read as ordinary words (#68). Only a clause written without the optional
			// leading `(` shows it - `(a)` balances by accident - which is why the
			// `*)` every case ends with was the case that failed.
			if (command_list && cur.open_cases > 0) {
				++p;
				continue;
			}
			close();
			continue;
		}
		if (c == '\'') {
			// A single quote is a quote everywhere EXCEPT where double quotes are in
			// force - the distinction that made `echo "it's"` print `it` when it was
			// missed (#33), and that keeps `"${x-'}"` from swallowing the rest of the
			// input.
			if (cur.ordinary_single) {
				++p;
				continue;
			}
			p = skip_quoted_or_expansion(p);
			continue;
		}
		if (c == '"') {
			open('"', 1, /*single_is_ordinary=*/true);
			continue;
		}
		if (c == '`') {
			open('`', 1, /*single_is_ordinary=*/false);
			continue;
		}
		if (c == '$') {
			const uint32_t after_dollar = past_continuations(p + 1);
			// `$'...'` is one quoted run, and a `}`, `)` or backquote inside it
			// closes nothing. Without this the `'` below would open a plain
			// single-quoted run, which ends at the quote in `\'` - so `${x-$'}'}`
			// closed at the wrong brace and `$(echo $')')` at the wrong paren.
			if (char_at(p + 1) == '\'' && !cur.ordinary_single) {
				p = skip_dollar_single_quote(p);
				continue;
			}
			if (char_at(after_dollar) == '{') {
				p = after_dollar;
				open('}', 1, cur.ordinary_single);
				continue;
			}
			if (char_at(after_dollar) == '(') {
				p = after_dollar;
				open(')', 1, /*single_is_ordinary=*/false,
				     char_at(past_continuations(after_dollar + 1)) == '(');
				continue;
			}
			++p;
			continue;
		}
		// Inside a command substitution a `#` where a word could begin opens a
		// COMMENT, and everything to the newline - a closing paren included - is
		// text. `$(\n echo a # ) comment \n)` closed at the paren in the comment and
		// ran `echo a #` (cmdsub-p.tst's 'comment in command substitution').
		if (c == '#' && cur.closer == ')' && starts_a_word(p, at)) {
			while (p < _source.size() && _source[p] != '\n')
				++p;
			continue;
		}
		// A here-document BODY is DATA: a `)` in it closes nothing, and neither does
		// a `$(` or a backquote. `$(cat <<\END<newline>foo)<newline>END<newline>)`
		// ended at the paren in the body and printed the raw bytes of the rest (#68).
		// bash gets this one wrong too, byte for byte.
		//
		// This is not the lexer reading ahead for a body, which #21 put in the
		// parser and which stands: nothing is collected and nothing is seeked here.
		// The scan is DELIMITING a construct it was handed, and it has to walk these
		// bytes either way to find the paren. Every body it steps over is collected
		// again, properly, when the parser parses the substitution's text.
		if (command_list && c == '<' && char_at(p + 1) == '<' && char_at(p + 2) != '<') {
			uint32_t d = past_continuations(p + 2);
			bool strip_tabs = false;
			if (char_at(d) == '-') {
				strip_tabs = true;
				d = past_continuations(d + 1);
			}
			while (d < _source.size() && is_blank(_source[d]))
				++d;
			d = past_continuations(d);
			const uint32_t word_start = d;
			// The delimiter word may be quoted - `<<\END`, `<<'END'`, `<<"END"` - and
			// the quotes are part of the word the body is compared against. Scanned
			// inline rather than through this function, because a delimiter needs
			// none of the nesting and a recursive call would carry the whole stack.
			while (d < _source.size()) {
				const char w = _source[d];
				if (w == '\\' && d + 1 < _source.size()) {
					d += 2;
					continue;
				}
				if (w == '\'') {
					++d;
					while (d < _source.size() && _source[d] != '\'')
						++d;
					if (d < _source.size())
						++d;
					continue;
				}
				if (w == '"') {
					++d;
					while (d < _source.size() && _source[d] != '"') {
						if (_source[d] == '\\' && d + 1 < _source.size())
							++d;
						++d;
					}
					if (d < _source.size())
						++d;
					continue;
				}
				if (is_word_terminator(w))
					break;
				++d;
			}
			if (d > word_start && pending_count < kMaxPendingHereDocs)
				pending[pending_count++] = {word_start, d - word_start, depth, strip_tabs};
			p = d;
			continue;
		}
		// The newline the pending bodies were waiting for. Only the ones opened at
		// THIS level are consumed: an operator from an enclosing level is waiting on
		// the enclosing line, not on this one, which is what keeps the outer body of
		// `cat <<OUTER; echo "$(cat <<INNER ...)"` where it belongs.
		if (c == '\n' && pending_count > 0 && pending[pending_count - 1].at_depth == depth) {
			int first = pending_count;
			while (first > 0 && pending[first - 1].at_depth == depth)
				--first;
			++p;
			for (int i = first; i < pending_count; ++i) {
				const std::string_view raw =
					_source.substr(pending[i].offset, pending[i].length);
				while (p < _source.size()) {
					const size_t nl = _source.find('\n', p);
					std::string_view line = _source.substr(
						p, nl == std::string_view::npos ? std::string_view::npos : nl - p);
					// `<<-` strips leading tabs from the delimiter line as well as from
					// the body, so the line is compared with them gone.
					if (pending[i].strip_tabs)
						while (!line.empty() && line.front() == '\t')
							line.remove_prefix(1);
					if (here_doc_delimiter_matches(raw, line)) {
						p = nl == std::string_view::npos
							? static_cast<uint32_t>(_source.size())
							: static_cast<uint32_t>(nl + 1);
						break;
					}
					if (nl == std::string_view::npos) {
						p = static_cast<uint32_t>(_source.size());
						break;
					}
					p = static_cast<uint32_t>(nl + 1);
				}
			}
			pending_count = first;
			continue;
		}
		// `case` and `esac` are reserved WHERE A COMMAND COULD BEGIN and nowhere
		// else, so this reads a whole word and asks the position - `$(echo case)` is
		// one word today and has to stay one. The `in` is required before a `)` is
		// treated as a pattern's: without it `$(echo $(echo a) case)`, where `case`
		// is an ARGUMENT that happens to follow a paren, would lose its own closer.
		if (command_list && (c == 'c' || c == 'i' || c == 'e')) {
			uint32_t w = p;
			while (w < _source.size() && !is_word_terminator(_source[w]))
				++w;
			const std::string_view word = _source.substr(p, w - p);
			if (word == "case" && starts_a_command(p, at)) {
				// Saturating rather than wrapping. A count that wrapped to zero would
				// hand a pattern's paren back to the substitution and mis-scan; stuck at
				// the ceiling the construct is reported UNTERMINATED instead, which is
				// the same answer kMaxScanNesting gives to input this will not follow.
				if (cur.awaiting_in < 255)
					++cur.awaiting_in;
				p = w;
				continue;
			}
			if (word == "in" && cur.awaiting_in > 0) {
				--cur.awaiting_in;
				if (cur.open_cases < 255)
					++cur.open_cases;
				p = w;
				continue;
			}
			if (word == "esac" && starts_a_command(p, at)) {
				if (cur.open_cases > 0)
					--cur.open_cases;
				else if (cur.awaiting_in > 0)
					--cur.awaiting_in;
				p = w;
				continue;
			}
		}
		// A bare paren or brace nests the construct it belongs to, which is how
		// `$(a $(b) c)` and `${x-${y}}` were counted before there was a stack.
		if (c == '(' && cur.closer == ')') {
			// Arithmetic is INHERITED: the inner parens of `$((1+2))` are the same
			// expression, while the `(` of `$( (cmd) )` opens a subshell in a command
			// list. POSIX makes the space the difference and so does this.
			open(')', 1, cur.ordinary_single, cur.arithmetic);
			continue;
		}
		if (c == '{' && cur.closer == '}') {
			open('}', 1, cur.ordinary_single);
			continue;
		}
		++p;
	}
	if (terminated != nullptr)
		*terminated = depth == 0 && !too_deep;
	// Nested deeper than the scan will follow: report it as running to the end of
	// the input, which the callers already treat as unterminated. Continuing with a
	// stack that lost its closers would put `p` somewhere arbitrary and hand the
	// expander a word nobody wrote.
	return too_deep ? static_cast<uint32_t>(_source.size()) : p;
}

token lexer::lex_operator() noexcept {
	const uint32_t start = _position;
	// The characters of an operator may be separated by line continuations, which
	// POSIX removes before the input is tokenised: `>\<newline>>` is `>>` and
	// `<\<newline><\<newline>-` is `<<-`. So each character is looked up past
	// them and `emit` takes the END position rather than a length - the token still
	// SPANS the continuation bytes, because nothing reads an operator's text.
	const uint32_t at1 = past_continuations(start + 1);
	const uint32_t at2 = past_continuations(at1 + 1);
	const uint32_t at3 = past_continuations(at2 + 1);
	const char c = char_at(start);
	const char c1 = char_at(at1);
	const char c2 = char_at(at2);

	auto emit = [&](token_kind kind, uint32_t end) {
		_position = end;
		token t;
		t.kind = kind;
		t.offset = start;
		t.length = end - start;
		return t;
	};

	switch (c) {
		case '|': return c1 == '|' ? emit(token_kind::or_if, at2) : emit(token_kind::pipe, at1);
		case '&': return c1 == '&' ? emit(token_kind::and_if, at2) : emit(token_kind::amp, at1);
		// `;&` is its own token rather than `semi` followed by `amp`: a lexer that
		// owns no memory and mutates nothing cannot glue two tokens back together
		// downstream, so a two-character operator has to be recognised HERE or not
		// at all. Read as two tokens, `case i in i) foo;& bar` parsed as `foo;`
		// then a background `& bar` - silently wrong rather than a syntax error.
		case ';':
			if (c1 == ';') return emit(token_kind::dsemi, at2);
			if (c1 == '&') return emit(token_kind::semi_and, at2);
			return emit(token_kind::semi, at1);
		case '(': return emit(token_kind::lparen, at1);
		case ')': return emit(token_kind::rparen, at1);
		case '<':
			if (c1 == '<' && c2 == '-') return emit(token_kind::dless_dash, at3);
			if (c1 == '<') return emit(token_kind::dless, at2);
			if (c1 == '&') return emit(token_kind::less_and, at2);
			if (c1 == '>') return emit(token_kind::less_great, at2);
			return emit(token_kind::less, at1);
		case '>':
			if (c1 == '>') return emit(token_kind::dgreat, at2);
			if (c1 == '&') return emit(token_kind::great_and, at2);
			if (c1 == '|') return emit(token_kind::clobber, at2);
			return emit(token_kind::great, at1);
		default: {
			// Unreachable for callers that check is_word_terminator first, but a
			// lexer that never fails cannot have an unreachable path that traps.
			token t = emit(token_kind::word, at1);
			t.error = token_error::unexpected_byte;
			t.error_offset = start;
			return t;
		}
	}
}

token lexer::lex_word(lex_mode mode) noexcept {
	const uint32_t start = _position;
	bool literal = true;

	// In a here-document delimiter, quoting still delimits (it decides whether the
	// body is expanded) but nothing else is special.
	const bool operators_terminate = (mode != lex_mode::here_doc_delimiter);

	token t;
	t.kind = token_kind::word;
	t.offset = start;

	auto finish = [&](token_error error = token_error::none, uint32_t error_at = 0) {
		t.length = _position - start;
		t.error = error;
		t.error_offset = (error != token_error::none) ? error_at : 0;
		if (literal)
			t.flags |= flag_literal;
		return t;
	};

	while (!at_end()) {
		const char c = peek();

		if (c == '\'') {
			literal = false;
			const uint32_t quote_at = _position;
			++_position;
			while (!at_end() && peek() != '\'')
				++_position;
			if (at_end()) {
				_incomplete = true;
				return finish(token_error::unterminated_single_quote, quote_at);
			}
			++_position;  // closing quote
			continue;
		}

		if (c == '"') {
			literal = false;
			const uint32_t quote_at = _position;
			++_position;
			while (!at_end() && peek() != '"') {
				// Inside double quotes a backslash escapes only a few bytes, but for
				// delimiting purposes it always consumes the next one.
				if (peek() == '\\' && _position + 1 < _source.size()) {
					_position += 2;
					continue;
				}
				// A substitution inside the quotes may contain quotes of its own, at
				// any depth: `"$(echo "x")"`, `` "`echo "x"`" `` and `"${e=a"b"c}"` are
				// each ONE quoted string, and scanning to the next `"` split all three.
				if (peek() == '`' ||
				    (peek() == '$' && (peek(1) == '(' || peek(1) == '{'))) {
					const uint32_t after =
						skip_quoted_or_expansion(_position, /*inside_double_quotes=*/true);
					if (after > _position) {
						_position = after;
						continue;
					}
				}
				++_position;
			}
			if (at_end()) {
				_incomplete = true;
				return finish(token_error::unterminated_double_quote, quote_at);
			}
			++_position;  // closing quote
			continue;
		}

		if (c == '\\' && peek(1) == '\n') {
			// A line continuation, not an escape: both characters are removed
			// entirely, so `echo one\<newline>two` prints `onetwo`.
			literal = false;
			_position += 2;
			continue;
		}

		if (c == '\\') {
			literal = false;
			if (_position + 1 >= _source.size()) {
				// A trailing backslash is a line continuation waiting for more input,
				// not a malformed token.
				++_position;
				_incomplete = true;
				return finish();
			}
			_position += 2;
			continue;
		}

		// `$(...)` is part of the word, not an operator followed by one. Without
		// this the '(' terminates the word and `echo $(x)` lexes as `$`, `(`, `x`,
		// `)` - which is how the expander first came to receive a bare `$`.
		// Parens are counted so nesting works: $(a $(b) c).
		// `${...}` is part of the word even when it contains blanks, which
		// `${x:?some message}` and `${x:-a default}` both do. Without this the word
		// split at the space and the closing brace leaked into the next word.
		// Past line continuations, for the same reason the segment scan is: `(` is a
		// word TERMINATOR, so `echo $\<newline>(\<newline>(1+2))` ended the word at
		// the paren and parsed as a subshell.
		// `$'...'` is part of the word, and its interior is DATA: a blank in it does
		// not separate words, a newline does not end the command, and an operator
		// byte is not an operator. Reaching the `'` handler above instead would end
		// the run at the quote inside `\'` and split `$'a\'b c'` at the blank.
		if (c == '$' && char_at(_position + 1) == '\'') {
			literal = false;
			const uint32_t opened_at = _position;
			bool closed = false;
			_position = skip_dollar_single_quote(opened_at, &closed);
			if (!closed) {
				_incomplete = true;
				return finish(token_error::unterminated_single_quote, opened_at);
			}
			continue;
		}

		if (c == '$' && char_at(past_continuations(_position + 1)) == '{') {
			literal = false;
			const uint32_t opened_at = _position;
			_position = past_continuations(_position + 1) + 1;
			int depth = 1;
			while (!at_end() && depth > 0) {
				// A quote inside the braces is a quote, so a `}` inside it does NOT
				// close the expansion: `${e=a"b"c}` ends at the last brace and
				// `${a+\}}` at the second. Counted braces alone stopped at the first.
				if (peek() == '\\' && _position + 1 < _source.size()) {
					_position += 2;
					continue;
				}
				if (peek() == '\'' || peek() == '"' || peek() == '`' ||
				    (peek() == '$' && (peek(1) == '(' || peek(1) == '{'))) {
					// Not inside double quotes: the `"` handler above owns those, so a
					// `${` reaching here is at word level.
					const uint32_t after = skip_quoted_or_expansion(_position);
					if (after > _position) {
						_position = after;
						continue;
					}
				}
				if (peek() == '{') ++depth;
				else if (peek() == '}') --depth;
				++_position;
			}
			if (depth > 0) {
				// Incomplete AND defective: `echo ${x` is a word the shell must refuse,
				// not a word it runs without its expansion. Reported as incomplete only,
				// it reached the executor and printed nothing at status zero (#47).
				_incomplete = true;
				return finish(token_error::unterminated_parameter_expansion, opened_at);
			}
			continue;
		}

		if (c == '$' && char_at(past_continuations(_position + 1)) == '(') {
			literal = false;
			const uint32_t opened_at = _position;
			const uint32_t at_paren = past_continuations(_position + 1);
			// `$((` is arithmetic, and counting parens closes it correctly either way -
			// but the two are worth telling apart in a diagnostic, which is the only
			// reason this is looked at here rather than by counting alone.
			const bool arithmetic = char_at(past_continuations(at_paren + 1)) == '(';
			// The whole construct at once, so a paren inside quotes or inside a
			// comment closes nothing: `echo $(echo ')')` reported an unterminated
			// quoted string, because counting parens alone ended the substitution
			// inside the quotes and left the closing one to open a new word.
			bool closed = false;
			_position = skip_quoted_or_expansion(opened_at, false, &closed);
			if (!closed) {
				_incomplete = true;
				return finish(arithmetic ? token_error::unterminated_arithmetic
				                         : token_error::unterminated_command_sub,
				              opened_at);
			}
			continue;
		}

		if (c == '`') {
			literal = false;
			const uint32_t tick_at = _position;
			++_position;
			while (!at_end() && peek() != '`') {
				if (peek() == '\\' && _position + 1 < _source.size())
					++_position;
				++_position;
			}
			if (at_end()) {
				_incomplete = true;
				return finish(token_error::unterminated_backquote, tick_at);
			}
			++_position;
			continue;
		}

		if (operators_terminate && is_word_terminator(c))
			break;

		if (needs_expansion(c))
			literal = false;
		++_position;
	}

	return finish();
}

// Lexes one segment of a word's interior. The caller has already established
// where the word starts and ends; this decomposes it.
//
// Segments are delimited, not interpreted: seg_parameter spans `${x:-y}` without
// deciding what `:-` means, and seg_command_sub spans `$(...)` without parsing
// its contents. Interpretation belongs to the expander, and the contents of a
// command substitution belong to a fresh parse. Keeping the split here means the
// lexer never needs to know what an expansion *does*.
token lexer::lex_word_segment(lex_mode mode) noexcept {
	const uint32_t start = _position;

	token t;
	t.offset = start;

	auto finish = [&](token_kind kind, token_error error = token_error::none,
	                  uint32_t error_at = 0) {
		t.kind = kind;
		t.length = _position - start;
		t.error = error;
		t.error_offset = (error != token_error::none) ? error_at : 0;
		return t;
	};

	const char c = peek();
	// Which quote characters are quotes here. Inside double quotes a single quote
	// is just a byte; in a here-document body BOTH are, because POSIX 2.7.4 gives
	// the body double-quote semantics minus the `"`. Everything else - $, `, \ -
	// is special in all three.
	const bool quotes_are_bytes = mode == lex_mode::double_quote_interior ||
	                              mode == lex_mode::here_doc_body;
	const bool double_quotes_are_bytes = mode == lex_mode::here_doc_body;

	if (c == '\'' && !quotes_are_bytes) {
		++_position;
		while (!at_end() && peek() != '\'')
			++_position;
		if (at_end()) {
			_incomplete = true;
			return finish(token_kind::seg_single_quoted, token_error::unterminated_single_quote, start);
		}
		++_position;
		return finish(token_kind::seg_single_quoted);
	}

	if (c == '"' && !double_quotes_are_bytes) {
		++_position;
		while (!at_end() && peek() != '"') {
			if (peek() == '\\' && _position + 1 < _source.size()) {
				_position += 2;
				continue;
			}
			// A substitution inside double quotes may itself contain quotes:
			// `"outer $(echo "inner") end"` is ONE quoted string. Scanning to the
			// next `"` split it at the inner quote and left a stray `)`.
			if (peek() == '`' ||
			    (peek() == '$' && (peek(1) == '(' || peek(1) == '{'))) {
				const uint32_t after =
					skip_quoted_or_expansion(_position, /*inside_double_quotes=*/true);
				if (after > _position) {
					_position = after;
					continue;
				}
			}
			++_position;
		}
		if (at_end()) {
			_incomplete = true;
			return finish(token_kind::seg_double_quoted, token_error::unterminated_double_quote, start);
		}
		++_position;
		return finish(token_kind::seg_double_quoted);
	}

	// Where a tilde is eligible: the start of a word, and - in an assignment's
	// value only - after an unquoted colon, so `PATH=~/bin:~/sbin` expands both.
	const bool in_assignment = mode == lex_mode::assignment_interior;
	if (c == '~' && (mode == lex_mode::word_interior || in_assignment) &&
	    (start == 0 || (in_assignment && start > 0 && _source[start - 1] == ':'))) {
		// POSIX 2.6.1: the tilde-prefix runs to the first unquoted `/` - or, in an
		// assignment, `:` - or to the end of the word, and if ANY character in it is
		// quoted then NONE of them is a login name. So the prefix is inspected
		// before the segment is claimed, and one that holds quoting is left to the
		// literal scan, which removes those quotes: dash prints `~root` for
		// `~"root"` and for `~roo\t`, quotes gone and tilde intact. `$` and a
		// backquote disqualify it too - `echo ~$USER` prints `~dimitarnanov`.
		uint32_t look = _position + 1;
		bool quoted = false;
		while (look < _source.size()) {
			const char b = _source[look];
			if (b == '/' || is_blank(b) || (in_assignment && b == ':'))
				break;
			if (b == '\'' || b == '"' || b == '\\' || b == '$' || b == '`') {
				quoted = true;
				break;
			}
			++look;
		}
		if (!quoted) {
			_position = look;
			return finish(token_kind::seg_tilde);
		}
		// Fall through: the `~` is ordinary text and the quotes still come off.
	}

	if (c == '`') {
		++_position;
		while (!at_end() && peek() != '`') {
			if (peek() == '\\' && _position + 1 < _source.size())
				++_position;
			++_position;
		}
		if (at_end()) {
			_incomplete = true;
			return finish(token_kind::seg_command_sub, token_error::unterminated_backquote, start);
		}
		++_position;
		return finish(token_kind::seg_command_sub);
	}

	if (c == '$') {
		// Past the line continuations POSIX removes before tokenising, in BOTH
		// lookaheads: `$\<newline>{f}` is a parameter expansion and
		// `$\<newline>(\<newline>(1+2))` an arithmetic one. Read literally, the `$`
		// was "a lone dollar" and the `(` went on to terminate the word, which is
		// three of quote-p.tst's cases.
		const uint32_t at_next = past_continuations(_position + 1);
		const uint32_t at_next2 = past_continuations(at_next + 1);
		const char next = char_at(at_next);
		// `$'...'` - ANSI-C quoting. Recognised only where a single quote is a
		// QUOTE: inside double quotes and in a here-document body it is an ordinary
		// byte, so `"$'a\n'"` is literal text there, exactly as in bash and zsh.
		// Guarded on the `$` and the quote being ADJACENT rather than looked at past
		// continuations, because `$\<newline>'x'` is a lone dollar followed by an
		// ordinary quoted run in bash - the construct is spelled with two characters
		// and not with two tokens.
		if (next == '\'' && !quotes_are_bytes && at_next == _position + 1) {
			bool closed = false;
			_position = skip_dollar_single_quote(start, &closed);
			if (!closed) {
				// Incomplete AND defective, the same pair an unterminated `'...'`
				// carries: more input could finish it, and as it stands the word must
				// not run. It reuses that error rather than adding one, because the
				// diagnostic says "unterminated quoted string" either way and a second
				// spelling of the same phrase is what token.h's error_phrase exists to
				// prevent.
				_incomplete = true;
				return finish(token_kind::seg_dollar_single_quoted,
				              token_error::unterminated_single_quote, start);
			}
			return finish(token_kind::seg_dollar_single_quoted);
		}
		// The three expansions below report the same defect the command-mode scan
		// reports, on the same construct. Saying it in only one of the two scans is
		// what let `echo $(` through: the word carried no error, so the tree the
		// executor refused to run was not the tree it was given (#47).
		if (next == '(' && char_at(at_next2) == '(') {
			_position = at_next2 + 1;
			int depth = 1;
			while (!at_end() && depth > 0) {
				if (peek() == '(') ++depth;
				else if (peek() == ')') --depth;
				++_position;
			}
			// The second `)` of `))` may be separated from the first by a line
			// continuation, so it is looked for past them: `$((1)\<newline>)` closes.
			// Without this the paren count ended at the first `)` and the second was
			// left in the word as literal text, which printed a stray `)`.
			const uint32_t at_close = past_continuations(_position);
			if (char_at(at_close) == ')')
				_position = at_close + 1;
			else if (at_end()) {
				_incomplete = true;
				return finish(token_kind::seg_arithmetic,
				              token_error::unterminated_arithmetic, start);
			}
			return finish(token_kind::seg_arithmetic);
		}
		if (next == '(') {
			// The whole construct at once, for the same reason the command-mode scan
			// does it: a paren inside quotes or inside a comment closes nothing.
			bool closed = false;
			_position = skip_quoted_or_expansion(start, false, &closed);
			if (!closed) {
				_incomplete = true;
				return finish(token_kind::seg_command_sub,
				              token_error::unterminated_command_sub, start);
			}
			return finish(token_kind::seg_command_sub);
		}
		if (next == '{') {
			// Braces are COUNTED: `${x:-${y:-z}}` nests, and stopping at the first
			// `}` left the outer brace as literal text. Quoted runs are skipped
			// whole, because a `}` inside quotes closes nothing.
			_position = at_next + 1;
			int depth = 1;
			while (!at_end() && depth > 0) {
				if (peek() == '\\' && _position + 1 < _source.size()) {
					_position += 2;
					continue;
				}
				if (peek() == '\'' || peek() == '"' || peek() == '`' ||
				    (peek() == '$' && (peek(1) == '(' || peek(1) == '{'))) {
					const uint32_t after =
						skip_quoted_or_expansion(_position, quotes_are_bytes);
					if (after > _position) {
						_position = after;
						continue;
					}
				}
				if (peek() == '{') ++depth;
				else if (peek() == '}') --depth;
				++_position;
			}
			if (depth > 0) {
				_incomplete = true;
				return finish(token_kind::seg_parameter,
				              token_error::unterminated_parameter_expansion, start);
			}
			return finish(token_kind::seg_parameter);
		}
		if (lesh::string_utils::is_valid_var_name_first_char(static_cast<unsigned char>(next))) {
			_position = at_next + 1;
			for (;;) {
				const uint32_t at = past_continuations(_position);
				if (at >= _source.size() ||
				    !lesh::string_utils::is_valid_var_name_non_first_char(
				        static_cast<unsigned char>(_source[at])))
					break;
				_position = at + 1;
			}
			return finish(token_kind::seg_parameter);
		}
		// The special parameters. Each is exactly one character and none is a valid
		// variable name, which is why they need their own case rather than a
		// widened name predicate - `$?x` is `$?` followed by a literal `x`.
		if (next == '?' || next == '#' || next == '$' || next == '!' ||
		    next == '@' || next == '*' || next == '-') {
			_position = at_next + 1;
			return finish(token_kind::seg_parameter);
		}
		// A positional parameter: $0 through $9. Multi-digit needs braces
		// (`${10}`), which POSIX requires and which the ${...} path already
		// handles.
		if (next >= '0' && next <= '9') {
			_position = at_next + 1;
			return finish(token_kind::seg_parameter);
		}
		// A lone '$' is an ordinary character.
		++_position;
		return finish(token_kind::seg_literal);
	}

	// A literal run: everything up to the next byte that starts a segment.
	while (!at_end()) {
		const char ch = peek();
		// A tilde after an unquoted colon starts a new segment in an assignment,
		// so the run has to stop before it: `x=a:~` expands the tilde. Guarded on
		// having consumed something, or a `~` the tilde branch above declined would
		// end a zero-length run forever.
		if (in_assignment && ch == '~' && _position > start && _source[_position - 1] == ':')
			break;
		if (ch == '\\' && peek(1) == '\n') {
			_position += 2;  // line continuation
			continue;
		}
		if (ch == '\\' && _position + 1 < _source.size()) {
			_position += 2;
			continue;
		}
		if ((ch == '\'' && !quotes_are_bytes) || (ch == '"' && !double_quotes_are_bytes) ||
		    ch == '$' || ch == '`')
			break;
		++_position;
	}
	return finish(token_kind::seg_literal);
}

token lexer::next(lex_mode mode) noexcept {
	_incomplete = false;

	if (mode == lex_mode::word_interior || mode == lex_mode::assignment_interior ||
	    mode == lex_mode::double_quote_interior || mode == lex_mode::here_doc_body) {
		if (at_end()) {
			token t;
			t.kind = token_kind::end;
			t.offset = _position;
			return t;
		}
		return lex_word_segment(mode);
	}

	const bool skipped = skip_blanks();

	if (at_end()) {
		token t;
		t.kind = token_kind::end;
		t.offset = _position;
		return t;
	}

	// POSIX: '#' opens a comment only where a word could begin - which is where
	// next() stands once the blanks are skipped: a token boundary. Inside a word
	// `#` is an ordinary character and lex_word consumes it, so this test never
	// fires there. Emitted rather than swallowed (#103); the newline that ends
	// the comment is NOT part of it and comes out as its own token.
	if (peek() == '#') {
		token t;
		t.kind = token_kind::comment;
		t.offset = _position;
		while (!at_end() && peek() != '\n')
			++_position;
		t.length = _position - t.offset;
		if (skipped)
			t.flags |= flag_preceded_by_blank;
		return t;
	}

	const char c = peek();

	if (c == '\n') {
		token t;
		t.kind = token_kind::newline;
		t.offset = _position;
		t.length = 1;
		++_position;
		if (skipped)
			t.flags |= flag_preceded_by_blank;
		return t;
	}

	// IO_NUMBER: a digit run is only a file descriptor when a redirection operator
	// follows immediately, with no blank between. `2>file` redirects; `2 >file`
	// passes 2 as an argument. The lexer can see this without the parser's help,
	// which is why it is one of the few things it decides alone.
	if (mode == lex_mode::command && is_digit(c)) {
		// Across line continuations, both between the digits and before the
		// operator: `3\<newline>>\<newline>>redir` is `3>>redir`, and read as a word
		// the `3` became an ARGUMENT and the redirection landed on stdout - which is
		// how quote-p.tst's operator case came to report `3: not open for output`.
		//
		// AND ONLY WHEN THE DIGITS COULD BE A DESCRIPTOR. `4294967298>file` used to
		// lex as an IO_NUMBER and accumulate into a uint32_t, so it wrapped onto
		// fd 2 and redirected the shell's STDERR - a live descriptor, reached from a
		// number the script wrote as something else entirely (#63). dash, zsh and
		// ksh all read an over-long run as an ordinary word instead, which leaves
		// `>` on its default fd and passes the digits to the command; dash's
		// threshold is a single digit, which is why `99>` is already a word there.
		// lesh's is what a descriptor can hold, so it agrees with bash below the
		// limit and with the other three above it.
		//
		// The limit comes from the same policy row parse_redirect reads, because two
		// readings of one bound is how the lexer and the parser would come to
		// disagree about which words are redirections.
		uint32_t ahead = past_continuations(_position);
		uint64_t fd = 0;
		bool fits = true;
		const uint64_t limit = static_cast<uint64_t>(
			lesh::policy_for(lesh::numeric_site::redirection_word_fd).high);
		while (ahead < _source.size() && is_digit(_source[ahead])) {
			if (!lesh::accumulate_digit(fd, static_cast<uint64_t>(_source[ahead] - '0'),
			                            10, limit))
				fits = false;
			ahead = past_continuations(ahead + 1);
		}
		if (fits && ahead < _source.size() &&
		    (_source[ahead] == '<' || _source[ahead] == '>')) {
			token t;
			t.kind = token_kind::io_number;
			t.offset = _position;
			t.length = ahead - _position;
			t.flags |= flag_literal;
			if (skipped)
				t.flags |= flag_preceded_by_blank;
			_position = ahead;
			return t;
		}
	}

	token t = (mode == lex_mode::command && is_word_terminator(c))
	          ? lex_operator()
	          : lex_word(mode);
	if (skipped)
		t.flags |= flag_preceded_by_blank;
	return t;
}

} // namespace lesh::syntax
