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

// The body of a parameter segment: what sits between `${` and `}`, or after `$`.
std::string_view parameter_body(std::string_view segment) noexcept {
	if (segment.size() >= 2 && segment[1] == '{') {
		const size_t close = segment.rfind('}');
		if (close == std::string_view::npos || close < 2)
			return segment.substr(2);
		return segment.substr(2, close - 2);
	}
	return segment.substr(1);
}

// How a ${...} form modifies the value. POSIX's set, with the colon variants
// distinguishing "unset" from "unset or empty" - a real difference: `${x:-d}`
// substitutes for an empty x while `${x-d}` does not.
enum class param_op {
	none,
	length,        // ${#x}
	use_default,   // ${x-d}  ${x:-d}
	assign_default,// ${x=d}  ${x:=d}
	error_if_unset,// ${x?m}  ${x:?m}
	use_alternate, // ${x+d}  ${x:+d}
	trim_prefix_short,  // ${x#pat}
	trim_prefix_long,   // ${x##pat}
	trim_suffix_short,  // ${x%pat}
	trim_suffix_long,   // ${x%%pat}
};

struct parsed_parameter {
	std::string_view name;
	param_op op = param_op::none;
	std::string_view argument;
	bool colon = false;  // the colon variants also treat empty as unset
};

parsed_parameter parse_parameter(std::string_view body) noexcept {
	parsed_parameter out;
	if (body.empty())
		return out;

	// A body that is exactly one special parameter is a NAME, not an operator.
	// `$?` is the exit status; `${x?msg}` is the error-if-unset form. Without this
	// the operator scan reads `?` as the operator and leaves an empty name, which
	// turned `echo $?` into "parameter null or not set".
	if (body.size() == 1) {
		switch (body[0]) {
			case '?': case '#': case '$': case '!': case '@': case '*': case '-':
				out.name = body;
				return out;
			default:
				break;
		}
		if (body[0] >= '0' && body[0] <= '9') {
			out.name = body;
			return out;
		}
	}

	if (body[0] == '#' && body.size() > 1) {
		// ${#x} is length - but `$#` alone is the positional count, which reaches
		// here as a bare name rather than through this branch.
		out.op = param_op::length;
		out.name = body.substr(1);
		return out;
	}

	// Scan for the operator, which starts after the name.
	size_t at = 0;
	while (at < body.size() && body[at] != ':' && body[at] != '-' && body[at] != '=' &&
	       body[at] != '?' && body[at] != '+' && body[at] != '#' && body[at] != '%')
		++at;

	out.name = body.substr(0, at);
	if (at >= body.size())
		return out;

	if (body[at] == ':' && at + 1 < body.size()) {
		out.colon = true;
		++at;
	}

	const char op = body[at];
	const bool doubled = at + 1 < body.size() && body[at + 1] == op;
	switch (op) {
		case '-': out.op = param_op::use_default; break;
		case '=': out.op = param_op::assign_default; break;
		case '?': out.op = param_op::error_if_unset; break;
		case '+': out.op = param_op::use_alternate; break;
		case '#': out.op = doubled ? param_op::trim_prefix_long : param_op::trim_prefix_short; break;
		case '%': out.op = doubled ? param_op::trim_suffix_long : param_op::trim_suffix_short; break;
		default: return out;
	}
	at += doubled ? 2 : 1;
	out.argument = at <= body.size() ? body.substr(at) : std::string_view{};
	return out;
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

// Resolves a parameter name, including the special ones. `$?`, `$#`, `$$` and
// `$1` are not variables and are not in the variable table.
bool expander::lookup_parameter(std::string_view name, std::string_view& out) noexcept {
	if (name.empty())
		return false;

	if (name == "?") {
		out = int_to_scratch(_params.last_status_value());
		return true;
	}
	if (name == "#") {
		out = int_to_scratch(static_cast<int>(_params.positional_count()));
		return true;
	}
	if (name == "$") {
		out = int_to_scratch(_params.process_id_value());
		return true;
	}
	if (name == "0") {
		out = _params.script_name_value();
		return true;
	}
	if (name[0] >= '1' && name[0] <= '9') {
		size_t index = 0;
		for (const char c : name) {
			if (c < '0' || c > '9')
				return _params.lookup(name, out);
			index = index * 10 + static_cast<size_t>(c - '0');
		}
		return _params.positional_at(index, out);
	}
	return _params.lookup(name, out);
}

// Formats an integer into arena storage so the returned view outlives the call.
std::string_view expander::int_to_scratch(int value) noexcept {
	char digits[24];
	const int n = std::snprintf(digits, sizeof(digits), "%d", value);
	const size_t len = n > 0 ? static_cast<size_t>(n) : 0;
	char* block = nullptr;
	_pool.allocate(len == 0 ? 1 : len, block, 1);
	std::memcpy(block, digits, len);
	return {block, len};
}

expansion_status expander::expand_text(std::string_view text, bool quoted,
                                       arena_array<std::string_view>& out,
                                       lex_mode mode) noexcept {
	expansion_status status = expansion_status::ok;
	lexer lx{text};

	for (;;) {
		const token seg = lx.next(mode);
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
					if (body[i] == '\\' && i + 1 < body.size() && body[i + 1] == '\n') {
						// A line continuation: BOTH characters vanish. Removing only
						// the backslash left the newline in the field, so
						// `echo one\<newline>two` printed two lines.
						++i;
						continue;
					}
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
					// The interior is lexed as the inside of double quotes: a single
					// quote there is an ordinary byte, not the start of a quoted run.
					const expansion_status inner =
						expand_text(body.substr(1, body.size() - 2), true, out,
						            lex_mode::double_quote_interior);
					if (inner != expansion_status::ok)
						status = inner;
				}
			} break;

			case token_kind::seg_parameter: {
				const std::string_view pbody = parameter_body(body);
				const parsed_parameter p = parse_parameter(pbody);

				// $@ and $* are the only expansions whose FIELD COUNT depends on
				// quoting: "$@" produces one field per positional parameter, while
				// "$*" produces exactly one, joined by the first character of IFS.
				// Getting this wrong breaks every argument-forwarding script ever
				// written, which is why it is handled before anything else.
				if (p.op == param_op::none && (p.name == "@" || p.name == "*")) {
					const size_t count = _params.positional_count();
					if (quoted && p.name == "@") {
						for (size_t i = 1; i <= count; ++i) {
							std::string_view arg;
							if (!_params.positional_at(i, arg))
								continue;
							if (i > 1)
								finish_field(out);   // one field each
							append(arg);
						}
					} else if (quoted) {  // "$*"
						const std::string_view sep = _params.ifs();
						const char joiner = sep.empty() ? '\0' : sep[0];
						for (size_t i = 1; i <= count; ++i) {
							std::string_view arg;
							if (!_params.positional_at(i, arg))
								continue;
							if (i > 1 && joiner != '\0')
								append(std::string_view(&joiner, 1));
							append(arg);
						}
						_field_started = true;
					} else {
						// Unquoted, both behave alike: split like any other expansion.
						for (size_t i = 1; i <= count; ++i) {
							std::string_view arg;
							if (!_params.positional_at(i, arg))
								continue;
							if (i > 1)
								finish_field(out);
							append_split(arg, out);
						}
					}
					break;
				}

				std::string_view value;
				bool found = lookup_parameter(p.name, value);

				if (p.op == param_op::length) {
					// ${#x} is the length in bytes; ${#@} and ${#*} the count.
					size_t n = 0;
					if (p.name == "@" || p.name == "*")
						n = _params.positional_count();
					else if (found)
						n = value.size();
					char digits[24];
					const int written = std::snprintf(digits, sizeof(digits), "%zu", n);
					append(std::string_view(digits, written > 0 ? static_cast<size_t>(written) : 0));
					break;
				}

				// The colon variants treat an EMPTY value as unset. Without the
				// colon only a genuinely unset parameter qualifies - a real
				// difference that `${x:-d}` versus `${x-d}` turns on.
				const bool absent = !found || (p.colon && value.empty());

				switch (p.op) {
					case param_op::use_default:
						if (absent) {
							const std::string_view d = expand_assignment_value(p.argument);
							if (quoted) append(d); else append_split(d, out);
							break;
						}
						goto substitute_value;

					case param_op::assign_default:
						if (absent) {
							const std::string_view d = expand_assignment_value(p.argument);
							if (_assign != nullptr)
								_assign->assign_parameter(p.name, d);
							if (quoted) append(d); else append_split(d, out);
							break;
						}
						goto substitute_value;

					case param_op::use_alternate:
						if (!absent) {
							const std::string_view d = expand_assignment_value(p.argument);
							if (quoted) append(d); else append_split(d, out);
						}
						break;

					case param_op::error_if_unset:
						if (absent) {
							const std::string_view message =
								p.argument.empty() ? std::string_view{"parameter null or not set"}
								                   : expand_assignment_value(p.argument);
							std::fprintf(stderr, "lesh: %.*s: %.*s\n",
							             static_cast<int>(p.name.size()), p.name.data(),
							             static_cast<int>(message.size()), message.data());
							status = expansion_status::parameter_unset;
							break;
						}
						goto substitute_value;

					case param_op::trim_prefix_short:
					case param_op::trim_prefix_long:
					case param_op::trim_suffix_short:
					case param_op::trim_suffix_long: {
						if (!found)
							break;
						// The pattern is expanded but NOT globbed - it is a pattern,
						// not a filename. The matcher is #23's, shared with `case`.
						const std::string_view pat = expand_assignment_value(p.argument);
						std::string_view result = value;
						const bool prefix = p.op == param_op::trim_prefix_short ||
						                    p.op == param_op::trim_prefix_long;
						const bool longest = p.op == param_op::trim_prefix_long ||
						                     p.op == param_op::trim_suffix_long;
						const size_t n = prefix ? match_prefix(pat, value, longest)
						                        : match_suffix(pat, value, longest);
						if (n != no_match && n > 0) {
							result = prefix ? value.substr(n)
							                : value.substr(0, value.size() - n);
						}
						if (quoted) append(result); else append_split(result, out);
						break;
					}

					case param_op::none:
					substitute_value:
						if (found) {
							if (quoted) {
								append(value);
							} else {
								if (has_pattern_characters(value))
									_field_globbable = true;
								append_split(value, out);
							}
						}
						break;

					default:
						break;
				}
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
