// The built-in reactors - today, the highlighter (F-20/F-21/F-22) - registered
// through the ABI and reachable by no other route (#93, ADR-0008, A-11).
//
// LOOK AT THE INCLUDES, and at what is NOT among them. This file sees
// `leshper/abi.h` and nothing else from leshper - not state.h, not registry.h,
// not text.h. It cannot read the buffer except by copying it out of the request
// token, cannot emit a decoration except through that token, and cannot learn
// that its answer was thrown away. It is a plugin written in C++, held to
// exactly the surface a Lua reactor will get, by the compiler rather than by
// anyone's care - the #110 discipline, applied to the reactor half.
//
// The syntax layer IS allowed, and deliberately: ADR-0008 records "syntax
// queries on the token" as a door v1 does not open, because "the only clients
// are native and call the syntax layer directly; zle never had parse access,
// which is why zsh highlighting re-implements the grammar, the C-5 bug class".
// So the highlighter calls `parse()` itself. C-1 to C-6 froze as leshper's
// public API when #104 landed, and this is the client that unfreezes nothing.
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

#include "leshper/abi.h"

#include "substrate/arena.h"
#include "syntax/ast.h"
#include "syntax/lexer.h"
#include "syntax/parser.h"

#include <sys/stat.h>
#include <unistd.h>

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

// Longer than any PATH_MAX this runs on, and a candidate that would not fit is
// declined rather than truncated - a truncated path names a different file.
constexpr std::size_t kPathBytes = 4096;

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

namespace lesh::leshper {

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

} // namespace lesh::leshper

namespace {

using lesh::leshper::highlighter;

// --- The one filesystem question F-22 exists to keep off the input path ------

bool is_executable_file(const char* path) noexcept {
	struct stat info;
	if (::stat(path, &info) != 0)
		return false;
	// access(X_OK) alone says yes for a DIRECTORY, so `echo /tmp` would paint
	// green as a command. The mode test is what makes the answer mean "this is a
	// thing exec would run".
	if (!S_ISREG(info.st_mode))
		return false;
	return ::access(path, X_OK) == 0;
}

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
	painter(lesh_request* request, const tree& parsed, const highlighter& styles) noexcept
		: _request(request), _tree(&parsed), _source(parsed.source()), _styles(&styles) {}

	std::int32_t run() noexcept {
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
			// word_interior for every word, including an assignment's value. The
			// expander uses assignment_interior there, whose one difference is
			// that a `~` after an unquoted `:` is a tilde-prefix too - so
			// `PATH=~/a:~/b` paints its second tilde as literal text. Recorded
			// rather than fixed: the difference needs the word's NODE, and this
			// sweep is over tokens because that is what makes it outer-before-
			// inner across `$(...)` for free.
			paint_segments(_source.substr(t.offset, t.length), t.offset,
			               lex_mode::word_interior, 0);
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
				case word_role::command_name:
					emit(at.offset, at.end(), classify_command(n));
					break;
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

	// F-21's command-name classes, minus the one this ABI cannot answer.
	//
	// A word that is not provably literal is not classified at all. `$cmd`,
	// `'ls'` and `l\s` all name a command only after expansion and quote removal,
	// and guessing at the bytes as typed would paint `$cmd` red for the crime of
	// being a variable. flag_literal is the lexer's own record that neither step
	// has anything to do (#9), so it is exactly the right question, and the
	// word's expansion segments still paint - which is the honest answer.
	//
	// WHAT IS MISSING, and it is missing by decision rather than by omission:
	// builtin, function and alias. Answering those needs the builtin table, the
	// function registry (#106) and the alias table, none of which the ABI can
	// reach - and reaching around it would be exactly the native side door A-11
	// forbids. So an unresolvable name is command.unknown, and the additive door
	// (a provider query on the token, #94) is recorded rather than invented here.
	// The same door owes this a PATH: `getenv` reads the process environment,
	// which is the shell's $PATH only until someone assigns to it.
	[[nodiscard]] std::uint32_t classify_command(const node& n) const noexcept {
		const token& first = _tree->token_at(n.first_token);
		if ((first.flags & flag_literal) == 0)
			return LESH_STYLE_NONE;
		const span at = _tree->span_of(n);
		const std::string_view name = _source.substr(at.offset, at.length);
		if (name.empty())
			return LESH_STYLE_NONE;
		return resolves(name) ? _styles->command_path : _styles->command_unknown;
	}

	[[nodiscard]] static bool resolves(std::string_view name) noexcept {
		char candidate[kPathBytes];
		// A name with a slash is a path, not a PATH lookup - POSIX 2.9.1.1.
		if (name.find('/') != std::string_view::npos) {
			if (name.size() >= sizeof(candidate))
				return false;
			std::memcpy(candidate, name.data(), name.size());
			candidate[name.size()] = '\0';
			return is_executable_file(candidate);
		}
		const char* path = ::getenv("PATH");
		if (path == nullptr)
			return false;
		std::string_view rest{path};
		for (;;) {
			const std::size_t colon = rest.find(':');
			std::string_view dir =
				colon == std::string_view::npos ? rest : rest.substr(0, colon);
			// POSIX: an empty PATH element means the current directory.
			if (dir.empty())
				dir = std::string_view{"."};
			if (dir.size() + name.size() + 2 <= sizeof(candidate)) {
				std::memcpy(candidate, dir.data(), dir.size());
				candidate[dir.size()] = '/';
				std::memcpy(candidate + dir.size() + 1, name.data(), name.size());
				candidate[dir.size() + 1 + name.size()] = '\0';
				if (is_executable_file(candidate))
					return true;
			}
			if (colon == std::string_view::npos)
				break;
			rest.remove_prefix(colon + 1);
		}
		return false;
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
	char* bytes = nullptr;
	bool pooled = true;
	if (length != 0) {
		pooled = self->pool.allocate(length, bytes, 1);
		if (bytes == nullptr)
			return LESH_ERR_TOOSMALL;
		std::size_t written = 0;
		status = lesh_request_buffer(request, bytes, length, &written);
		if (status != LESH_OK) {
			if (!pooled)
				std::free(bytes);
			return status;
		}
		length = written;
	}

	const std::string_view source{bytes == nullptr ? "" : bytes, length};
	{
		// THE HIGHLIGHT PARSE PASSES NO ALIAS TABLE, which is parse()'s default
		// and #95's whole finding: with a table, `alias e='echo '` puts the
		// substituted tokens in a text region and the `e` the user typed is
		// covered by no token at all. The painter paints what was typed.
		const tree parsed = parse(self->pool, source);
		painter paint{request, parsed, *self};
		status = paint.run();
	}
	if (!pooled)
		std::free(bytes);
	return status;
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

// Out of line rather than in registry.h, so that this file needs no leshper
// header but abi.h. Declared in registry.h; see the note at the top.
namespace lesh::leshper {

highlighter* highlighter_create() { return new highlighter{}; }

void highlighter_destroy(highlighter* self) noexcept { delete self; }

std::size_t register_builtin_reactors(lesh_registry& reg, highlighter& self) {
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

} // namespace lesh::leshper
