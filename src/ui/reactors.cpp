// The built-in reactors - the highlighter (F-20/F-21/F-22) and the
// autosuggester (F-24 to F-26) - registered through the ABI and reachable by no
// other route (#93, ADR-0008, A-11). Two sections, in that order; the
// autosuggester's begins at the banner near the bottom.
//
// THE HOST'S, SINCE #168 PHASE B. This was `src/leshper/builtin_reactors.cpp`.
// Nothing about how it reaches the editor changed - see below - but what it DOES
// is knowledge: a re-parse of the line, the shell's alias/function/builtin
// tables, a `$PATH` sweep, a walk of the history. The editor's half of both
// features is colouring a region and drawing virtual text, and it keeps that
// half; a file that has to ask what `deploy` IS belongs on the side that knows.
//
// LOOK AT THE INCLUDES, and at what is NOT among them. This file sees
// `leshper/abi.h` and nothing else from leshper - not state.h, not registry.h,
// not text.h. It cannot read the buffer except by copying it out of the request
// token, cannot emit a decoration except through that token, and cannot learn
// that its answer was thrown away. It is a plugin written in C++, held to
// exactly the surface a Lua reactor will get, by the compiler rather than by
// anyone's care - the #110 discipline, applied to the reactor half. That is why
// the move was a file move and a namespace change: the door was already the only
// door.
//
// The syntax layer IS allowed, and deliberately: ADR-0008 records "syntax
// queries on the token" as a door v1 does not open, because "the only clients
// are native and call the syntax layer directly; zle never had parse access,
// which is why zsh highlighting re-implements the grammar, the C-5 bug class".
// So the highlighter calls `parse()` itself. C-1 to C-6 froze as leshper's
// public API when #104 landed, and this is the client that unfreezes nothing -
// and since Phase B it is the client from OUTSIDE the editor, which is why
// `lesh_leshper` no longer links `lesh_syntax` at all.
//
// WHY THIS RUNS ON A WORKER (F-22): classifying a command name touches the
// filesystem - a PATH sweep with a stat per candidate - and that cannot sit on
// the keystroke path. Everything else here is one re-parse, measured at 38.7 us
// per 4 KiB (#95, #104), which is inside N-1's millisecond on its own; the
// filesystem is the part that is not.
//
// THE ARENA. #90 settled "arena per worker, reset per request", and until the
// worker pool lands (#126) the request token hands out no arena, so the
// highlighter carries its own in the registration-time context pointer and
// rewinds it to a mark at the top of every compute. Nothing points into it once
// compute returns - emit copies at the call site - which is the lifetime rule
// #90 made the whole design rest on. When the pool lands, this member moves to
// the worker and nothing else here changes.

#include "ui/reactors.h"

#include "leshper/abi.h"

// The SEARCHER, on the same terms the syntax layer is here on. It is a sibling
// in `lesh::ui` now rather than a leshper header, and it is still held to the
// abi.h-only discipline - read its own opening comment - so it carries no editor
// state, no registry and no arena pointer, and including it opens no door this
// file is meant to be shut out of. What it is, is #125's pure searcher: query in,
// matches out, a `history_source` it does not own. The alternative was
// reimplementing prefix search here, which is the C-5 bug class with a different
// noun.
#include "ui/history_search.h"

#include "substrate/arena.h"
#include "syntax/ast.h"
#include "syntax/lexer.h"
#include "syntax/parser.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace {

using lesh::buffer_pool;
using namespace lesh::syntax;

// Sized for the ~120 KB a 4 KiB parse costs (#95's measurement) with room for a
// line an order of magnitude longer. A paste past it does not fail: the arena
// falls back to malloc per block and arena_array releases those blocks itself,
// so the cost of a huge line is heap traffic, which the allocation gate sees,
// rather than a wrong answer.
constexpr std::size_t kHighlightArenaBytes = 512u * 1024u;

// How far the segment painter follows quoting inside quoting. A double-quoted
// run holds expansions, and an expansion's interior is its own sub-parse handled
// elsewhere, so one or two levels is all this ever needs; the cap exists so a
// pathological line cannot make the walk deep.
constexpr int kMaxSegmentDepth = 8;

// How many tokens the segment sweep paints between cancellation polls. A power
// of two so the test is a mask, and a whole command's worth so the poll is not
// the sweep's cost.
constexpr std::uint32_t kPollEvery = 256;

} // namespace

namespace lesh::ui {

// The highlighter's registration-time context: its arena and its interned style
// ids. Opaque outside this file - registry.h declares the type and the two
// functions that make and unmake one, and that is all any caller can see.
struct highlighter {
	buffer_pool pool{kHighlightArenaBytes};
	// Where the arena stands with nothing in it. Every compute rewinds here.
	char* mark = pool.at();

	// F-21's classes, interned once at registration on the loop thread, because
	// that is where interning is allowed and because a worker wants a plain
	// integer (ADR-0008). A reactor never names a colour: the theme maps these
	// names at render, which is what makes F-21's "independently themeable" a
	// property of the design rather than a promise.
	std::uint32_t command_unknown = LESH_STYLE_NONE;
	std::uint32_t command_path = LESH_STYLE_NONE;
	std::uint32_t command_builtin = LESH_STYLE_NONE;
	std::uint32_t command_function = LESH_STYLE_NONE;
	std::uint32_t command_alias = LESH_STYLE_NONE;
	std::uint32_t keyword = LESH_STYLE_NONE;
	std::uint32_t comment = LESH_STYLE_NONE;
	std::uint32_t string_single = LESH_STYLE_NONE;
	std::uint32_t string_double = LESH_STYLE_NONE;
	std::uint32_t string_ansi_c = LESH_STYLE_NONE;
	std::uint32_t expansion_parameter = LESH_STYLE_NONE;
	std::uint32_t expansion_command = LESH_STYLE_NONE;
	std::uint32_t expansion_arithmetic = LESH_STYLE_NONE;
	std::uint32_t expansion_tilde = LESH_STYLE_NONE;
	std::uint32_t redirect_target = LESH_STYLE_NONE;
	std::uint32_t error_syntax = LESH_STYLE_NONE;
};

} // namespace lesh::ui

namespace {

using lesh::ui::highlighter;

// A run of arena bytes that knows whether it came from the arena's MALLOC
// FALLBACK and is therefore its own to release - arena_array's rule, one layer
// up, and the one LeakSanitizer found the hard way when a pool was small enough
// to overflow. From the arena in the ordinary case, which is what "zero heap on
// the compute path" means.
class arena_block {
public:
	arena_block(buffer_pool& pool, std::size_t bytes) noexcept {
		if (bytes != 0)
			_pooled = pool.allocate(bytes, _data, 1);
	}
	~arena_block() noexcept {
		if (!_pooled && _data != nullptr)
			std::free(_data);
	}

	arena_block(const arena_block&) = delete;
	arena_block& operator=(const arena_block&) = delete;

	[[nodiscard]] char* data() const noexcept { return _data; }

private:
	char* _data = nullptr;
	bool _pooled = true;
};

// --- The painter ------------------------------------------------------------
//
// EMISSION ORDER IS THE CONTRACT, and it is the only thing standing in for a
// renderer that does not exist yet - the theme, which maps a style id to what a
// cell looks like, is a render-side indirection and deliberately not here.
// Spans overlap by nature - a command name sits inside a command substitution
// which sits inside a word - so the batch is ordered OUTERMOST FIRST: a span
// emitted later is contained by, and refines, any earlier span it overlaps. A
// renderer that applies the batch in order and lets the last writer win gets the
// innermost classification, which is the one a reader wants.
//
// Two flat passes deliver that ordering for free, and it is worth saying why
// rather than looking like luck. #104 parses command-substitution interiors
// AFTER the top-level parse, so interior TOKENS and interior NODES are appended
// above every top-level one; index order is therefore already outer-before-inner
// at every depth. So:
//
//   pass 1, over tokens: quoting and expansion - the CONTAINERS, including the
//                        whole of `$(...)` - plus the keyword flag (#105).
//   pass 2, over nodes:  command names and redirect targets - the things
//                        contained, including the ones inside `$(...)`.
//
// The one place the rule reads oddly is a quoted redirect target, `> "$x"`,
// where redirect.target lands over string.double. That is deliberate: for a
// redirect target the role is the more useful answer, and it is the answer
// completion will stand on (#95).
class painter {
public:
	// `is_assignment` is one byte per token, from the arena, or nullptr when
	// there are no tokens. See mark_assignments.
	painter(lesh_request* request, const tree& parsed, const highlighter& styles,
	        char* is_assignment) noexcept
		: _request(request), _tree(&parsed), _source(parsed.source()), _styles(&styles),
		  _is_assignment(is_assignment) {}

	std::int32_t run() noexcept {
		mark_assignments();
		std::int32_t status = paint_tokens();
		if (status != LESH_OK)
			return status;
		status = paint_nodes();
		if (status != LESH_OK)
			return status;
		paint_comments();
		paint_defects();
		return LESH_OK;
	}

private:
	lesh_request* _request;
	const tree* _tree;
	std::string_view _source;
	const highlighter* _styles;
	char* _is_assignment;

	// WHICH WORD TOKENS ARE AN ASSIGNMENT'S, asked of the tree rather than
	// re-derived from the bytes.
	//
	// The segment sweep is over TOKENS, because that is what makes it
	// outer-before-inner across `$(...)` for free, and a token does not know
	// where the grammar put it. `x=~/a` and `echo x=~/a` are the same bytes and
	// the tilde is eligible in exactly one of them - only the parser knows which,
	// and #95's probe is the standing reminder of what re-deriving a role from
	// the bytes costs. So the node array is asked once, here, and the answer is
	// one byte per token in the arena.
	//
	// An assignment is exactly ONE word token (parser.cpp's word_node), so this
	// is a single store per assignment rather than a range.
	void mark_assignments() const noexcept {
		if (_is_assignment == nullptr)
			return;
		const std::size_t tokens = _tree->token_count();
		std::memset(_is_assignment, 0, tokens);
		const std::size_t nodes = _tree->node_count();
		for (std::uint32_t i = 0; i < nodes; ++i) {
			const node& n = (*_tree)[i];
			if (n.kind == node_kind::assignment && n.first_token < tokens)
				_is_assignment[n.first_token] = 1;
		}
	}

	// The cooperative poll (ADR-0008). Not checking would be safe - the loop
	// drops a stale batch either way - it would just waste the worker on a line
	// the user has already typed past.
	[[nodiscard]] bool superseded() const noexcept {
		std::int32_t out = 0;
		return lesh_request_superseded(_request, &out) == LESH_OK && out != 0;
	}

	void emit(std::uint32_t from, std::uint32_t to, std::uint32_t style) const noexcept {
		if (style == LESH_STYLE_NONE || to <= from)
			return;
		lesh_emit_span(_request, from, to, style);
	}

	// True for a token whose bytes are not in the input at all: an alias body
	// lives in a region above source() (#40). The highlight parse passes no alias
	// table, so this cannot happen today - it is a guard against a caller that
	// one day passes one, because a span into a region would name a position the
	// user cannot see.
	[[nodiscard]] bool is_real(const token& t) const noexcept {
		return t.end_offset() <= _source.size();
	}

	[[nodiscard]] std::uint32_t style_of_segment(token_kind kind) const noexcept {
		switch (kind) {
			case token_kind::seg_single_quoted:        return _styles->string_single;
			case token_kind::seg_dollar_single_quoted: return _styles->string_ansi_c;
			case token_kind::seg_double_quoted:        return _styles->string_double;
			case token_kind::seg_parameter:            return _styles->expansion_parameter;
			case token_kind::seg_command_sub:          return _styles->expansion_command;
			case token_kind::seg_arithmetic:           return _styles->expansion_arithmetic;
			case token_kind::seg_tilde:                return _styles->expansion_tilde;
			default:                                   return LESH_STYLE_NONE;
		}
	}

	// One word's interior, re-lexed through C-6. The lexer is independently
	// callable and restartable at any offset precisely so this is possible
	// without a second scanner - which is the C-5 bug class zsh pays 3,093 lines
	// of zle_tricky.c for.
	//
	// OVER THE WORD'S OWN BYTES, from index zero, and rebased on the way out.
	// That is not tidiness: the tilde-prefix rule is "a `~` at the START of the
	// word" (POSIX 2.6.1), which the lexer spells `start == 0`, so a scan begun
	// at the word's offset in the whole line finds a tilde-prefix only in a word
	// that happens to sit at offset 0. This is the same view the EXPANDER takes
	// of the same word (expander.cpp's `lexer lx{text}`), which is what makes the
	// paint and the execution agree - C-5, one layer down.
	//
	// The lexed text is a view into the snapshot, so this borrows and copies
	// nothing.
	void paint_segments(std::string_view text, std::uint32_t base, lex_mode mode,
	                    int depth) const noexcept {
		if (depth >= kMaxSegmentDepth || text.empty())
			return;
		lexer scan(text);
		for (;;) {
			const token seg = scan.next(mode);
			if (seg.kind == token_kind::end)
				break;
			// A zero-length segment would spin the loop; the lexer does not
			// produce one, and the guard costs a comparison to say so.
			if (seg.length == 0)
				break;
			emit(base + seg.offset, base + seg.end_offset(), style_of_segment(seg.kind));
			// Inside double quotes the expansions are still live - `"$HOME/x"` is
			// a parameter in a string - and the lexer needs telling, because a
			// single quote in there is an ordinary byte and not a quote at all.
			if (seg.kind == token_kind::seg_double_quoted) {
				const std::uint32_t open = seg.offset + 1;
				const std::uint32_t close =
					(seg.end_offset() > open && text[seg.end_offset() - 1] == '"')
						? seg.end_offset() - 1
						: seg.end_offset();
				if (close > open)
					paint_segments(text.substr(open, close - open), base + open,
					               lex_mode::double_quote_interior, depth + 1);
			}
			if (seg.end_offset() >= text.size())
				break;
		}
	}

	// Pass 1. Every word's quoting and expansion, and every keyword the parser
	// ACCEPTED as one (#105) - the flag is positional, so `done` is a keyword in
	// a loop and an argument in `echo done`, and only the parser knows which.
	std::int32_t paint_tokens() noexcept {
		const std::size_t count = _tree->token_count();
		for (std::uint32_t i = 0; i < count; ++i) {
			// The node pass polls per command, which is the natural unit there.
			// A token has no such unit, so this counts: often enough that a
			// 100 KiB paste gives up promptly, rare enough that the atomic load
			// is not the sweep's cost.
			if ((i & (kPollEvery - 1)) == 0 && i != 0 && superseded())
				return LESH_ERR_SUPERSEDED;
			const token& t = _tree->token_at(i);
			if (!is_real(t))
				continue;
			if ((t.flags & flag_keyword) != 0) {
				emit(t.offset, t.end_offset(), _styles->keyword);
				continue;
			}
			if (t.kind != token_kind::word)
				continue;
			const std::string_view text = _source.substr(t.offset, t.length);
			// AN ASSIGNMENT'S VALUE IS LEXED THE WAY THE EXECUTOR LEXES IT, and
			// the split is the same one: `text.substr(eq + 1)` through
			// assignment_interior (executor.cpp's expand_value call, and
			// value_context::assignment beside it). The one lexical difference
			// that buys is POSIX 2.6.1's: a `~` after an unquoted `:` is a
			// tilde-prefix too, so `PATH=~/bin:~/sbin` paints BOTH tildes, which
			// is what the shell will expand. Lexed as a plain word interior it
			// painted the first and called the second literal text - the paint
			// and the execution disagreeing about the same bytes, which is the
			// C-5 bug class in miniature.
			//
			// The name half carries no style: `PATH` is a NAME, not a construct.
			const std::size_t eq =
				(_is_assignment != nullptr && _is_assignment[i] != 0)
					? text.find('=')
					: std::string_view::npos;
			if (eq != std::string_view::npos)
				paint_segments(text.substr(eq + 1),
				               t.offset + static_cast<std::uint32_t>(eq) + 1,
				               lex_mode::assignment_interior, 0);
			else
				paint_segments(text, t.offset, lex_mode::word_interior, 0);
		}
		return superseded() ? LESH_ERR_SUPERSEDED : LESH_OK;
	}

	// Pass 2. The two roles #103 recorded, wherever they occur - including inside
	// a command substitution, whose nodes sit above the top-level ones.
	std::int32_t paint_nodes() noexcept {
		const std::size_t count = _tree->node_count();
		for (std::uint32_t i = 0; i < count; ++i) {
			const node& n = (*_tree)[i];
			// Between commands, which is where a cooperative poll belongs: it is
			// often enough that a 100 KiB paste gives up promptly, and rare enough
			// that the atomic load is not the walk's cost.
			if (n.kind == node_kind::simple_command && superseded())
				return LESH_ERR_SUPERSEDED;
			if (n.kind != node_kind::word)
				continue;
			const span at = _tree->span_of(n);
			if (at.end() > _source.size())
				continue;
			switch (static_cast<word_role>(n.aux)) {
				case word_role::command_name: {
					std::uint32_t style = LESH_STYLE_NONE;
					const std::int32_t status = classify_command(n, style);
					if (status != LESH_OK)
						return status;
					emit(at.offset, at.end(), style);
					break;
				}
				case word_role::redirect_target:
					emit(at.offset, at.end(), _styles->redirect_target);
					break;
				default:
					break;
			}
		}
		return LESH_OK;
	}

	// Comments are trivia on a side list (#103), covered by no token in the array
	// and by no node's span, so they are their own pass and can overlap nothing.
	void paint_comments() const noexcept {
		for (std::uint32_t i = 0; i < _tree->comment_count(); ++i) {
			const span& at = _tree->comment_at(i);
			if (at.end() <= _source.size())
				emit(at.offset, at.end(), _styles->comment);
		}
	}

	// C-2's TRISTATE, and the whole of why it is a tristate.
	//
	// `echo "x` is incomplete AND defective. An editor answers incomplete with a
	// continuation prompt, not with red: more input fixes it, and painting it as
	// an error the moment the opening quote is typed is the behaviour that makes
	// live highlighting hated. So incompleteness wins while more input can come
	// (#94's derived view of C-2), and this pass emits nothing at all.
	//
	// Interiors are excluded for #104's reason, read forward: a defect inside
	// `$(...)` is not this command's defect - the expander reports it at
	// expansion time, at status 2 - and has_errors() already declines to see it.
	// Painting it here would put the tree and the paint in disagreement about
	// what is wrong with the line.
	void paint_defects() const noexcept {
		if (_tree->incomplete())
			return;
		const std::size_t count = _tree->node_count();
		for (std::uint32_t i = 0; i < count; ++i) {
			const node& n = (*_tree)[i];
			if (!tree::is_defective(n) || inside_an_interior(i))
				continue;
			const span at = _tree->span_of(n);
			if (at.end() <= _source.size())
				emit(at.offset, at.end(), _styles->error_syntax);
		}
	}

	[[nodiscard]] bool inside_an_interior(std::uint32_t index) const noexcept {
		for (std::uint32_t i = 0; i < _tree->sub_parse_count(); ++i) {
			const sub_parse& one = _tree->sub_parse_at(i);
			if (index >= one.node_begin && index < one.node_end)
				return true;
		}
		return false;
	}

	// F-21's command-name classes - all five of them, since #135 (ADR-0009).
	//
	// A word that is not provably literal is not classified at all. `$cmd`,
	// `'ls'` and `l\s` all name a command only after expansion and quote removal,
	// and guessing at the bytes as typed would paint `$cmd` red for the crime of
	// being a variable. flag_literal is the lexer's own record that neither step
	// has anything to do (#9), so it is exactly the right question, and the
	// word's expansion segments still paint - which is the honest answer.
	//
	// WHAT USED TO BE MISSING. builtin, function and alias need the builtin
	// table, the function registry (#106) and the alias table, and this file
	// sees `abi.h` and nothing else from leshper - reaching around that would be
	// the native side door A-11 forbids. #124 recorded the gap and #130 opened
	// the door: `lesh_request_command_kind`, one additive verb on the token,
	// which also carries the SHELL's `$PATH` instead of the process
	// environment's. `getenv` was the shell's $PATH only until someone assigned
	// to it, and `PATH=/opt/bin` on the line the user is typing is exactly when
	// the two disagree.
	//
	// THE POLL COMES FIRST, and per lookup rather than per command. A lookup can
	// be a stat per `$PATH` directory (F-22, and ADR-0009's cost: a stat storm
	// delays the next highlight, never a keystroke). The node pass already polls
	// at each simple_command; this makes the poll immediately adjacent to the
	// filesystem, which is what the ADR asks for.
	[[nodiscard]] std::int32_t classify_command(const node& n,
	                                            std::uint32_t& style) const noexcept {
		style = LESH_STYLE_NONE;
		const token& first = _tree->token_at(n.first_token);
		if ((first.flags & flag_literal) == 0)
			return LESH_OK;
		const span at = _tree->span_of(n);
		const std::string_view name = _source.substr(at.offset, at.length);
		if (name.empty())
			return LESH_OK;
		if (superseded())
			return LESH_ERR_SUPERSEDED;
		std::uint32_t kind = LESH_COMMAND_UNKNOWN;
		if (lesh_request_command_kind(_request, name.data(), name.size(), &kind) != LESH_OK)
			kind = LESH_COMMAND_UNKNOWN;
		style = style_of_command(kind);
		return LESH_OK;
	}

	// `external` paints `command.path` - the name the vocabulary already had for
	// "the filesystem has a thing exec would run", interned since #124 and used
	// by every test that has ever asserted on it. A second name for one class
	// would be churn in the theme for no new meaning.
	[[nodiscard]] std::uint32_t style_of_command(std::uint32_t kind) const noexcept {
		switch (kind) {
			case LESH_COMMAND_ALIAS:    return _styles->command_alias;
			case LESH_COMMAND_FUNCTION: return _styles->command_function;
			case LESH_COMMAND_BUILTIN:  return _styles->command_builtin;
			case LESH_COMMAND_EXTERNAL: return _styles->command_path;
			// Including a kind this build does not know: the enumerated space is
			// additive (ADR-0008), so an unrecognised number is a name this
			// highlighter cannot classify, which is what command.unknown means.
			default:                    return _styles->command_unknown;
		}
	}
};

// The reactor itself: one function, the shape a Lua trampoline would have.
std::int32_t highlight(lesh_request* request, void* userdata) {
	highlighter* self = static_cast<highlighter*>(userdata);
	if (self == nullptr)
		return LESH_ERR_INVAL;

	// Reset first, so the arena is rewound even if the previous compute returned
	// early. Nothing points into it: emit copied at the call site (#90).
	self->pool.reset(self->mark);

	std::size_t length = 0;
	std::int32_t status = lesh_request_buffer_length(request, &length);
	if (status != LESH_OK)
		return status;

	// The snapshot, copied out of the token into the arena. No accessor lends a
	// pointer - the WASM insurance ADR-0006 bought - so the copy is the contract
	// and not an inefficiency, and the arena is where it belongs.
	//
	// Declared before the tree so that it OUTLIVES it: the tree's spans and its
	// source() view point in here.
	const arena_block snapshot(self->pool, length);
	if (length != 0) {
		if (snapshot.data() == nullptr)
			return LESH_ERR_TOOSMALL;
		std::size_t written = 0;
		status = lesh_request_buffer(request, snapshot.data(), length, &written);
		if (status != LESH_OK)
			return status;
		length = written;
	}

	const std::string_view source{snapshot.data() == nullptr ? "" : snapshot.data(),
	                              length};
	// THE HIGHLIGHT PARSE PASSES NO ALIAS TABLE, which is parse()'s default and
	// #95's whole finding: with a table, `alias e='echo '` puts the substituted
	// tokens in a text region and the `e` the user typed is covered by no token
	// at all. The painter paints what was typed.
	const tree parsed = parse(self->pool, source);
	const arena_block assignments(self->pool, parsed.token_count());
	painter paint{request, parsed, *self, assignments.data()};
	return paint.run();
}

struct style_slot {
	const char* name;
	std::uint32_t highlighter::* field;
};

// THE VOCABULARY, and it is a vocabulary rather than a palette. Each name is
// what the span MEANS; the theme decides what it looks like (F-21). A plugin
// that wants a literal colour interns a fixed-attribute id of its own and gets
// one, without this list growing.
constexpr style_slot kStyles[] = {
	{"command.unknown", &highlighter::command_unknown},
	{"command.path", &highlighter::command_path},
	{"command.builtin", &highlighter::command_builtin},
	{"command.function", &highlighter::command_function},
	{"command.alias", &highlighter::command_alias},
	{"keyword", &highlighter::keyword},
	{"comment", &highlighter::comment},
	{"string.single", &highlighter::string_single},
	{"string.double", &highlighter::string_double},
	{"string.ansi_c", &highlighter::string_ansi_c},
	{"expansion.parameter", &highlighter::expansion_parameter},
	{"expansion.command", &highlighter::expansion_command},
	{"expansion.arithmetic", &highlighter::expansion_arithmetic},
	{"expansion.tilde", &highlighter::expansion_tilde},
	{"redirect.target", &highlighter::redirect_target},
	{"error.syntax", &highlighter::error_syntax},
};

} // namespace

// Out of line rather than in reactors.h, so that this file needs no leshper
// header but abi.h. Declared in reactors.h; see the note at the top.
namespace lesh::ui {

highlighter* highlighter_create() { return new highlighter{}; }

void highlighter_destroy(highlighter* self) noexcept { delete self; }

std::size_t register_reactors(lesh_registry& reg, highlighter& self) {
	// Interning is loop-thread only (ADR-0008), so a reactor interns everything
	// it will ever emit at registration and carries plain integers to the worker.
	for (const style_slot& slot : kStyles) {
		std::uint32_t id = LESH_STYLE_NONE;
		if (lesh_style_intern(&reg, slot.name, &id) == LESH_OK)
			self.*(slot.field) = id;
	}
	// buffer_changed and nothing else: a highlight is a function of the text, so
	// a cursor move cannot change one and asking for the event would be work per
	// arrow key for an identical answer (A-10).
	return lesh_reactor_register(&reg, "highlighter", LESH_EVENT_BUFFER_CHANGED,
	                             highlight, &self) == LESH_OK
	       ? 1u : 0u;
}

} // namespace lesh::ui

// ===========================================================================
// The autosuggester (#133: F-24, F-25, F-26)
//
// Fish's greyed-out completion, and the shape of it is one sentence: the newest
// history entry that starts with what you have typed, drawn as VIRTUAL TEXT at
// the end of the buffer and proposed whole, so that an accepting action has
// something to apply and the buffer has nothing in it the user did not type.
//
// TWO EMISSIONS, ONE ANSWER, and the split is the design rather than an
// accident. The virtual text is the CONTINUATION - the bytes past what is
// typed - drawn at the end of the buffer, after what is typed. The proposal
// is the WHOLE candidate, because that is what accepting means, and computing
// "typed + continuation" at accept time would be an accepting action deriving
// the answer a second time from two halves that a keystroke could have moved
// between (N-4 again, one layer up).
//
// buffer_changed AND NOTHING ELSE, the highlighter's rule and for the
// highlighter's reason (A-10): the suggestion is a function of the typed text
// alone. It is the newest history entry that starts with the buffer, drawn as
// virtual text at the END of the buffer - not at the cursor - so where the
// cursor sits does not change which bytes it is or where they render. #133 had
// this reactor also listen to cursor_moved and retract the suggestion whenever
// the cursor left the end, fish-style; #154 amends that on the owner's call: the
// ghost STAYS VISIBLE off the end (it is anchored to the buffer, not the caret),
// so a cursor move is not an event this reactor has any answer to and asking for
// it would be a prefix walk per arrow key for a batch identical to the last one.
// ACCEPTANCE is unchanged and lives elsewhere: `suggestion_is_acceptable` still
// requires the cursor at the end, so off the end the accept key is a plain
// motion - the suggestion shows but Right just moves, which is exactly the split
// #154 wanted. This compute therefore never reads the cursor at all.
//
// WHAT IS NOT HERE, and it is one thing: F-27's validity filter, which greys a
// `cd` suggestion whose directory no longer exists. It needs the cwd and a path
// check, and both are the SHELL's - #130's door on the request token, which does
// not exist yet. The follow-up is recorded on #130 rather than guessed at here;
// a `stat` against the worker's own cwd would answer a different question than
// the one the user's line asks.
//
// The completion-derived fallback F-24 lists as a SHOULD waits on the completion
// engine, which is fog on the map. Nothing here has to change when it arrives:
// a second reactor proposing under the same kind is what the ABI already is.
// ===========================================================================

namespace lesh::ui {

// The autosuggester's registration-time context. Opaque outside this file, like
// `highlighter` - reactors.h declares the type and the functions that make and
// unmake one, and that is all any caller can see.
struct autosuggester {
	// Sized for a long command line, not for a parse: the only thing that goes
	// in here is the snapshot, so this is an order of magnitude below the
	// highlighter's arena on purpose. A paste past it falls back to malloc,
	// which the allocation gate sees, rather than failing.
	buffer_pool pool{64u * 1024u};
	// Where the arena stands with nothing in it. Every compute rewinds here.
	char* mark = pool.at();

	// BORROWED, never owned, and null until the wiring site hands one in. The
	// adapter over `history_store::for_each_newest_first` lives at that site,
	// which is where lesh-side and leshper-side types are allowed to meet
	// (#125); the tests hand in a `vector_history_source`.
	const history_source* source = nullptr;

	// F-21's one name for this reactor, interned once at registration on the
	// loop thread. The theme decides what `suggestion` looks like - muted, in
	// every implementation anyone has shipped - and a reactor that said "grey"
	// here would be the thing F-21 exists to prevent.
	std::uint32_t suggestion = LESH_STYLE_NONE;
};

} // namespace lesh::ui

namespace {

using lesh::ui::autosuggester;
using lesh::ui::history_search;

// The sink `history_search::run` calls, as a type rather than as a capture.
//
// A LAMBDA CAPTURING ONE POINTER is what `run`'s `std::function` parameter can
// hold inside itself; a lambda capturing four things is a `std::function` that
// reaches the heap on a path that runs on every keystroke. So the state lives
// here, on the compute's own stack, and the lambda captures `&this`.
class candidate {
public:
	candidate(lesh_request* request, std::string_view typed, std::uint32_t style) noexcept
		: _request(request), _typed(typed), _style(style) {}

	// Answers `run`'s question: keep walking? Newest first, so the first entry
	// this accepts is the answer and the walk stops.
	bool offer(std::string_view entry) noexcept {
		// Prefix mode guarantees `entry` starts with `_typed`, so "the match
		// equals the buffer" is exactly "no bytes past what is typed". KEEP
		// WALKING rather than giving up: an entry identical to the line is the
		// commonest thing in a history the user is retyping from, and stopping on
		// it would mean a line you have run before can never be suggested past.
		// Fish walks on for the same reason.
		if (entry.size() <= _typed.size())
			return true;

		const std::string_view rest = entry.substr(_typed.size());
		// The continuation, at the end of the buffer. Styled, because a
		// suggestion that renders like typed text is a suggestion the user will
		// mistake for typed text.
		_status = lesh_emit_virtual_text_styled(_request, _typed.size(), rest.data(),
		                                        rest.size(), _style);
		// And the whole candidate, for the accepting action (F-25). Under
		// LESH_PROPOSAL_AUTOSUGGESTION, which is what that kind was reserved for
		// when the header was written - see the report on #133 for why this is
		// not a fourth kind.
		if (_status == LESH_OK)
			_status = lesh_propose(_request, LESH_PROPOSAL_AUTOSUGGESTION, entry.data(),
			                       entry.size());
		return false;
	}

	[[nodiscard]] std::int32_t status() const noexcept { return _status; }

private:
	lesh_request* _request;
	std::string_view _typed;
	std::uint32_t _style;
	std::int32_t _status = LESH_OK;
};

// The reactor itself: one function, the shape a Lua trampoline would have.
std::int32_t autosuggest(lesh_request* request, void* userdata) {
	autosuggester* self = static_cast<autosuggester*>(userdata);
	if (self == nullptr)
		return LESH_ERR_INVAL;
	// A provider wired up wrong says so, rather than looking like an empty
	// history (#125). F-17's null history is a `vector_history_source` with
	// nothing in it, which walks and matches nothing.
	if (self->source == nullptr)
		return LESH_ERR_INVAL;

	// Reset first, so the arena is rewound even if the previous compute returned
	// early. Nothing points into it: emit copied at the call site (#90).
	self->pool.reset(self->mark);

	std::size_t length = 0;
	std::int32_t status = lesh_request_buffer_length(request, &length);
	if (status != LESH_OK)
		return status;
	// Nothing typed, nothing suggested. An empty query matches every entry in
	// every mode (#125), so without this guard an empty line would suggest the
	// last command - which is what the up-arrow is for, and what F-26's "the
	// suggestion is never in the buffer" would otherwise be constantly denying.
	if (length == 0)
		return LESH_OK;

	// NO CURSOR READ (#154). The continuation is drawn at the end of the buffer
	// and the suggestion is a function of the typed text alone, so the reactor
	// shows the same thing wherever the caret is. Whether the accept key accepts
	// or merely moves is `suggestion_is_acceptable`'s call, made at accept time
	// against the live cursor - not this reactor's.

	// The snapshot, copied out of the token into the arena. No accessor lends a
	// pointer (ADR-0006's WASM insurance), so the copy is the contract; the
	// arena is where it belongs and it is rewound above.
	const arena_block snapshot(self->pool, length);
	if (snapshot.data() == nullptr)
		return LESH_ERR_TOOSMALL;
	std::size_t written = 0;
	status = lesh_request_buffer(request, snapshot.data(), length, &written);
	if (status != LESH_OK)
		return status;

	const std::string_view typed{snapshot.data(), written};
	if (typed.empty())
		return LESH_OK;

	history_search::options options;
	// F-33's mode and F-24's, which are the same one: the typed text IS the
	// constraint.
	options.search = history_search::mode::prefix;
	// No cap. The walk stops at the first entry that is a STRICT extension, and
	// that is not always the first match - see `candidate::offer` - so a cap of
	// one would settle for the entry the user has already typed out in full.
	options.max_matches = 0;
	// No ranges. Nothing here highlights where the match fell, and asking for
	// none is also what keeps the searcher's scratch vector from ever allocating
	// on a path that runs per keystroke.
	options.max_ranges = 0;

	// On the compute's own stack, per #125: two requests in flight must not
	// share scratch. In prefix mode with no ranges it holds nothing.
	history_search searcher{options};
	candidate answer{request, typed, self->suggestion};

	const history_search::outcome walked = searcher.run(
		typed, *self->source,
		[&answer](const history_search::match& one) { return answer.offer(one.entry); },
		// The cooperative poll, between entries (ADR-0008). Not checking would be
		// safe - the loop drops a stale batch either way - it would just walk a
		// history for a line the user has already typed past.
		[request]() {
			std::int32_t superseded = 0;
			lesh_request_superseded(request, &superseded);
			return superseded != 0;
		});

	if (walked.cancelled)
		return LESH_ERR_SUPERSEDED;
	return answer.status();
}

} // namespace

namespace lesh::ui {

autosuggester* autosuggester_create(const history_source* source) {
	autosuggester* self = new autosuggester{};
	self->source = source;
	return self;
}

void autosuggester_destroy(autosuggester* self) noexcept { delete self; }

std::size_t register_autosuggester(lesh_registry& reg, autosuggester& self) {
	// Interning is loop-thread only (ADR-0008), so the one id this reactor will
	// ever emit is interned here and carried to the worker as a plain integer.
	std::uint32_t id = LESH_STYLE_NONE;
	if (lesh_style_intern(&reg, "suggestion", &id) == LESH_OK)
		self.suggestion = id;
	// buffer_changed and nothing else (#154): the suggestion depends on the typed
	// text, not the cursor, so a cursor move has no answer to recompute and the
	// ghost stays visible off the end. See the banner above.
	return lesh_reactor_register(&reg, "autosuggester", LESH_EVENT_BUFFER_CHANGED,
	                             autosuggest, &self) == LESH_OK
	       ? 1u : 0u;
}

} // namespace lesh::ui
