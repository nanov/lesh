#include "runtime/expander.h"

#include "runtime/arithmetic.h"
#include "runtime/glob.h"
#include "runtime/pattern.h"

#include "syntax/lexer.h"

#include <cstdio>
#include <cstring>
#include <tuple>

namespace lesh::runtime {

using syntax::lex_mode;
using syntax::lexer;
using syntax::token;
using syntax::token_kind;

namespace {

bool is_ifs(char c, std::string_view ifs) noexcept {
	return ifs.find(c) != std::string_view::npos;
}

// The name inside a parameter segment: `$name` or `${name}`.
std::string_view parameter_name(std::string_view segment) noexcept {
	if (segment.size() >= 2 && segment[1] == '{') {
		const size_t close = segment.find('}');
		if (close == std::string_view::npos)
			return segment.substr(2);
		return segment.substr(2, close - 2);
	}
	return segment.substr(1);
}

// The code inside `$(...)` or backticks.
std::string_view substitution_body(std::string_view segment) noexcept {
	if (segment.size() >= 2 && segment[0] == '`')
		return segment.substr(1, segment.size() >= 2 ? segment.size() - 2 : 0);
	if (segment.size() >= 3)
		return segment.substr(2, segment.size() - 3);
	return {};
}

} // namespace

void expander::append(std::string_view bytes) noexcept {
	for (const char c : bytes)
		_current->push(c);
	_field_started = true;
}

// Splits on IFS as it appends. Only the RESULT of an unquoted expansion is split;
// literal text never is, which is why `a b` is one word and `$x` holding "a b" is
// two fields. Getting that distinction wrong is the classic shell bug.
void expander::append_split(std::string_view bytes,
                            arena_array<std::string_view>& out) noexcept {
	const std::string_view ifs = _params.ifs();
	for (const char c : bytes) {
		if (is_ifs(c, ifs)) {
			if (_field_started)
				finish_field(out);
		} else {
			_current->push(c);
			_field_started = true;
		}
	}
}

void expander::finish_field(arena_array<std::string_view>& out) noexcept {
	if (!_field_started)
		return;
	const size_t n = _current->size();
	char* block = nullptr;
	// Exact-size copy out of the accumulator: the accumulator relocates as it
	// grows, so a view into it would dangle the moment the next field is built.
	_pool.allocate(n == 0 ? 1 : n, block);
	if (n > 0)
		std::memcpy(block, _current->data(), n);
	out.push(std::string_view(block, n));
	_current->truncate(0);
	_field_started = false;
}

expansion_status expander::expand_text(std::string_view text, bool quoted,
                                       arena_array<std::string_view>& out) noexcept {
	expansion_status status = expansion_status::ok;
	lexer lx{text};

	for (;;) {
		const token seg = lx.next(lex_mode::word_interior);
		if (seg.kind == token_kind::end)
			break;
		const std::string_view body = text.substr(seg.offset, seg.length);

		switch (seg.kind) {
			case token_kind::seg_literal: {
				// Quoting suppresses pathname expansion: `echo "*.txt"` must print
				// *.txt, not a filename. So a field is only glob-eligible when a
				// metacharacter arrived from unquoted text.
				if (!quoted && has_pattern_characters(body))
					_field_globbable = true;
				// Backslash removal is part of quote removal, and it behaves
				// differently inside double quotes - there it escapes only a few
				// bytes. Outside, it escapes anything.
				for (size_t i = 0; i < body.size(); ++i) {
					if (body[i] == '\\' && i + 1 < body.size()) {
						const char next = body[i + 1];
						if (!quoted || next == '"' || next == '\\' || next == '$' || next == '`') {
							_current->push(next);
							_field_started = true;
							++i;
							continue;
						}
					}
					_current->push(body[i]);
					_field_started = true;
				}
			} break;

			case token_kind::seg_single_quoted: {
				// Nothing inside single quotes is ever special, and the quotes
				// themselves are removed. An empty '' still starts a field.
				if (body.size() >= 2)
					append(body.substr(1, body.size() - 2));
				_field_started = true;
			} break;

			case token_kind::seg_double_quoted: {
				// Expansion happens inside, field splitting does not. Recursing with
				// quoted=true is what encodes that.
				_field_started = true;
				if (body.size() >= 2) {
					const expansion_status inner =
						expand_text(body.substr(1, body.size() - 2), true, out);
					if (inner != expansion_status::ok)
						status = inner;
				}
			} break;

			case token_kind::seg_parameter: {
				std::string_view value;
				if (_params.lookup(parameter_name(body), value)) {
					if (quoted) {
						append(value);
					} else {
						// POSIX: the RESULT of an unquoted expansion is subject to
						// pathname expansion, so `x='*.txt'; echo $x` globs.
						if (has_pattern_characters(value))
							_field_globbable = true;
						append_split(value, out);
					}
				}
				// Unset expands to nothing. Unquoted, that means no field at all.
			} break;

			case token_kind::seg_tilde: {
				// Only a bare ~ or ~/... for now; ~user needs the password database
				// and belongs with shell state.
				if (body == "~")
					append(_params.home_directory());
				else
					append(body);
			} break;

			case token_kind::seg_command_sub: {
				if (_runner == nullptr) {
					// Completion's mode: expand everything else, refuse to execute.
					status = expansion_status::command_substitution_unavailable;
					break;
				}
				arena_array<char> captured{_pool, 256};
				if (_runner->run_and_capture(substitution_body(body), captured)) {
					std::string_view result{captured.data(), captured.size()};
					// POSIX: trailing newlines are removed from the result.
					while (!result.empty() && result.back() == '\n')
						result.remove_suffix(1);
					if (quoted)
						append(result);
					else
						append_split(result, out);
				}
			} break;

			case token_kind::seg_arithmetic: {
				// The lexer spans `$((...))`; strip the delimiters and evaluate.
				std::string_view inner = body;
				if (inner.size() >= 5)
					inner = inner.substr(3, inner.size() - 5);

				// The inner text is expanded first: `$((x + $y))` is legal, and the
				// evaluator sees only arithmetic.
				const std::string_view resolved = expand_assignment_value(inner);

				if (_vars == nullptr) {
					// No mutable state - completion's mode. Evaluating would still be
					// safe for reads, but an assignment would mutate the shell as a
					// side effect of drawing a suggestion.
					status = expansion_status::unsupported_construct;
					break;
				}
				const arithmetic_result r = evaluate(resolved, *_vars);
				if (!r.ok) {
					status = expansion_status::unsupported_construct;
					break;
				}
				char digits[24];
				const int n = std::snprintf(digits, sizeof(digits), "%lld",
				                            static_cast<long long>(r.value));
				append(std::string_view(digits, n > 0 ? static_cast<size_t>(n) : 0));
			} break;

			default:
				append(body);
				break;
		}
	}
	return status;
}

std::string_view expander::expand_assignment_value(std::string_view text) noexcept {
	// RE-ENTRANT. Arithmetic expansion calls this from inside expand_text to
	// resolve its own inner text, so the caller's accumulator must survive.
	// Nulling _current on exit unconditionally clobbered it - UBSan caught the
	// null dereference on the very first `$((1 + 2))`.
	arena_array<char>* const outer_current = _current;
	const bool outer_started = _field_started;
	const bool outer_globbable = _field_globbable;

	arena_array<char> accumulator{_pool, 64};
	arena_array<std::string_view> discard{_pool, 4};
	_current = &accumulator;
	_field_started = false;
	_field_globbable = false;

	// quoted=true suppresses field splitting, which is the assignment rule: the
	// value is one word however many blanks the expansion produced.
	std::ignore = expand_text(text, true, discard);

	const size_t n = accumulator.size();
	char* block = nullptr;
	_pool.allocate(n == 0 ? 1 : n, block, 1);
	if (n > 0)
		std::memcpy(block, accumulator.data(), n);

	_current = outer_current;
	_field_started = outer_started;
	_field_globbable = outer_globbable;
	return {block, n};
}

expansion_status expander::expand_word(const syntax::tree& t, syntax::node_index word,
                                       arena_array<std::string_view>& out) noexcept {
	const syntax::node& n = t[word];
	const std::string_view text = t.text_of(n);
	const token& tok = t.token_at(n.first_token);

	// The fast path the lexer's flag_literal exists for. A word with no quoting,
	// no expansion and no glob characters expands to itself - so the field is a
	// view into the original source and costs no allocation and no copy at all.
	// Most words in most command lines take this path.
	// The fast path flag_literal exists for: no quoting, no expansion, no glob
	// characters, so the field is a view into the source at no cost. The lexer
	// already excludes glob characters from flag_literal, so this cannot skip
	// pathname expansion.
	if ((tok.flags & syntax::flag_literal) != 0) {
		out.push(text);
		return expansion_status::ok;
	}

	arena_array<char> accumulator{_pool, 64};
	_current = &accumulator;
	_field_started = false;

	// Pathname expansion runs AFTER field splitting, on each resulting field, and
	// only on unquoted text - which is why it happens here rather than inside
	// expand_text, where quoting context is still being tracked.
	const size_t before_fields = out.size();
	_field_globbable = false;
	const expansion_status status = expand_text(text, false, out);
	finish_field(out);

	if (_glob_enabled && _field_globbable) {
		arena_array<std::string_view> globbed{_pool, out.size() - before_fields + 4};
		bool any = false;
		for (size_t i = before_fields; i < out.size(); ++i) {
			if (expand_pathnames(_pool, out[i], globbed))
				any = true;
			else
				globbed.push(out[i]);
		}
		if (any) {
			out.truncate(before_fields);
			for (const auto& g : globbed)
				out.push(g);
		}
	}

	_current = nullptr;
	return status;
}

} // namespace lesh::runtime
