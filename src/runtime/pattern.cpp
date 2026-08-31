#include "runtime/pattern.h"

#include <cstddef>
#include <tuple>

namespace lesh::runtime {

namespace {

// One of POSIX's twelve character classes.
//
// Spelled out rather than delegated to <ctype.h>, because those functions read
// the process locale and this matcher must not: it is a pure function of two
// strings, and the same pattern has to mean the same thing in `case`, in a glob
// and in `${x#pat}` whichever locale the caller happens to be running under. The
// C locale is the only one lesh has, so the two agree today - and this way they
// still agree the day lesh grows a locale.
//
// An unknown name matches NOTHING rather than being an error. POSIX leaves it
// undefined; dash matches nothing, including the bytes the construct is spelled
// with, so `[[:nosuch:]]` matches neither `a` nor `[` nor `:`.
bool in_class(std::string_view name, unsigned char c) {
	const bool lower = c >= 'a' && c <= 'z';
	const bool upper = c >= 'A' && c <= 'Z';
	const bool digit = c >= '0' && c <= '9';
	if (name == "lower")  return lower;
	if (name == "upper")  return upper;
	if (name == "alpha")  return lower || upper;
	if (name == "digit")  return digit;
	if (name == "alnum")  return lower || upper || digit;
	if (name == "xdigit") return digit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
	if (name == "blank")  return c == ' ' || c == '\t';
	if (name == "space")
		return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
	if (name == "cntrl")  return c < 0x20 || c == 0x7f;
	if (name == "print")  return c >= 0x20 && c < 0x7f;
	if (name == "graph")  return c > 0x20 && c < 0x7f;
	if (name == "punct")  return c > 0x20 && c < 0x7f && !(lower || upper || digit);
	return false;
}

// One element of a bracket expression, which is not always one character.
//
// POSIX admits three bracketed forms inside the brackets - `[:class:]`,
// `[.symbol.]` and `[=equivalent=]` - and they do not behave alike: a class is a
// SET and so can never bound a range, while the other two name a single
// character and can. Reading an element is therefore its own step, and the range
// lookahead runs on what it produced rather than on raw bytes: `[[.0.]-[.2.]]`
// is the range `0` to `2` written the long way, and stepping over the endpoints
// a character at a time cannot see that.
struct bracket_element {
	bool well_formed = true;
	bool is_class = false;  // a set, so not a range endpoint
	bool matched = false;   // is_class only: the class holds the subject byte
	char value = 0;         // otherwise: the one character it denotes
};

// Reads the element at `at` and advances past it.
bracket_element read_element(std::string_view pattern, size_t& at, unsigned char c) {
	bracket_element e;

	if (pattern[at] == '[' && at + 1 < pattern.size() &&
	    (pattern[at + 1] == ':' || pattern[at + 1] == '.' || pattern[at + 1] == '=')) {
		const char delimiter = pattern[at + 1];
		const size_t body = at + 2;
		const char closing[3] = {delimiter, ']', '\0'};
		const size_t close = pattern.find(closing, body);
		if (close == std::string_view::npos) {
			e.well_formed = false;  // `[[:` that never closes: the '[' opens nothing
			return e;
		}
		const std::string_view name = pattern.substr(body, close - body);
		if (delimiter == ':') {
			e.is_class = true;
			e.matched = in_class(name, c);
		} else if (name.size() == 1) {
			// A collating symbol and an equivalence class both name exactly one
			// character in the C locale, and in the C locale a character is
			// equivalent to itself and nothing else.
			e.value = name[0];
		} else {
			e.well_formed = false;  // no multi-character collating element in C
			return e;
		}
		at = close + 2;
		return e;
	}

	// A backslash escapes the next byte, including a `]` that would otherwise
	// close the expression. POSIX leaves a backslash inside brackets to the
	// locale, but the SHELL has already put this one there deliberately: quoting
	// a metacharacter is translated into `\` on its way to the matcher, so
	// `["*"]` has to be the set holding an asterisk rather than the set holding a
	// backslash as well.
	if (pattern[at] == '\\' && at + 1 < pattern.size())
		++at;
	e.value = pattern[at];
	++at;
	return e;
}

// Matches a bracket expression starting at pattern[0] == '['.
//
// Returns whether it matched, and advances `pattern_at` past the closing ']'.
// An unterminated '[' is a literal '[' - POSIX's rule, and the reason this
// returns a separate "well formed" flag rather than just failing.
bool match_bracket(std::string_view pattern, size_t& pattern_at, char c, bool& well_formed) {
	const unsigned char subject = static_cast<unsigned char>(c);
	size_t at = pattern_at + 1;
	bool negated = false;
	if (at < pattern.size() && (pattern[at] == '!' || pattern[at] == '^')) {
		negated = true;
		++at;
	}

	bool matched = false;
	bool first = true;
	for (;;) {
		if (at >= pattern.size()) {
			well_formed = false;  // no closing ']': the '[' was literal after all
			return false;
		}
		// A ']' immediately after '[' or '[!' is a literal ']', not the terminator.
		if (pattern[at] == ']' && !first)
			break;
		first = false;

		const bracket_element low = read_element(pattern, at, subject);
		if (!low.well_formed) {
			well_formed = false;
			return false;
		}
		if (low.is_class) {
			// A class is a set, so it never bounds a range: a `-` after one is an
			// ordinary character, which is the reading POSIX's "undefined" leaves
			// open and the only one that does not need a second kind of endpoint.
			matched = matched || low.matched;
			continue;
		}

		// `a-z`. A `-` immediately before the closing `]` is an ordinary character
		// instead - `[a-]` is the two-character set - which is what the second half
		// of this lookahead is for.
		if (at + 1 < pattern.size() && pattern[at] == '-' && pattern[at + 1] != ']') {
			size_t after = at + 1;
			const bracket_element high = read_element(pattern, after, subject);
			if (!high.well_formed || high.is_class) {
				well_formed = false;
				return false;
			}
			at = after;
			if (c >= low.value && c <= high.value)
				matched = true;
			continue;
		}
		if (c == low.value)
			matched = true;
	}

	well_formed = true;
	pattern_at = at + 1;  // past the ']'
	return negated ? !matched : matched;
}

// Iterative backtracking match. Recursion on `*` would be O(2^n) on pathological
// patterns and can blow the stack on input a user can type; this remembers the
// last `*` and resumes there instead, which is linear in practice.
bool match_here(std::string_view pattern, std::string_view text, bool period_is_special) {
	size_t p = 0, t = 0;
	size_t star_p = std::string_view::npos, star_t = 0;

	// A leading period must be matched explicitly, never by a wildcard.
	if (period_is_special && !text.empty() && text[0] == '.') {
		if (pattern.empty() || (pattern[0] != '.' && pattern[0] != '\\'))
			return false;
	}

	while (t < text.size()) {
		if (p < pattern.size()) {
			const char pc = pattern[p];

			if (pc == '\\' && p + 1 < pattern.size()) {
				if (pattern[p + 1] == text[t]) {
					p += 2;
					++t;
					continue;
				}
			} else if (pc == '?') {
				++p;
				++t;
				continue;
			} else if (pc == '[') {
				size_t next = p;
				bool well_formed = true;
				const bool ok = match_bracket(pattern, next, text[t], well_formed);
				if (!well_formed) {
					if (text[t] == '[') {  // literal '['
						++p;
						++t;
						continue;
					}
				} else if (ok) {
					p = next;
					++t;
					continue;
				}
			} else if (pc == '*') {
				star_p = p++;
				star_t = t;
				continue;
			} else if (pc == text[t]) {
				++p;
				++t;
				continue;
			}
		}

		// No match at this position: back up to the last '*' and let it swallow one
		// more character.
		if (star_p != std::string_view::npos) {
			p = star_p + 1;
			t = ++star_t;
			continue;
		}
		return false;
	}

	// Trailing '*'s can match the empty string.
	while (p < pattern.size() && pattern[p] == '*')
		++p;
	return p == pattern.size();
}

} // namespace

bool pattern_match(std::string_view pattern, std::string_view text,
                   bool period_is_special) noexcept {
	return match_here(pattern, text, period_is_special);
}

bool has_pattern_characters(std::string_view text) noexcept {
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] == '\\') {
			++i;  // escaped: not a metacharacter
			continue;
		}
		if (text[i] == '*' || text[i] == '?' || text[i] == '[')
			return true;
	}
	return false;
}

bool is_pattern(std::string_view word) noexcept {
	size_t last_close = std::string_view::npos;
	bool looked_for_close = false;

	for (size_t i = 0; i < word.size(); ++i) {
		if (word[i] == '\\') {
			++i;  // escaped: not a metacharacter
			continue;
		}
		if (word[i] == '*' || word[i] == '?')
			return true;
		// A `]` is never a metacharacter on its own, so it is not tested for here
		// at all - only reached as the terminator of a `[` found below.
		if (word[i] != '[')
			continue;

		// Located once, on the first `[` and never for a word without one, so the
		// overwhelming majority of words still cost exactly the loop above. It is
		// also what keeps `[[[[[[...` linear: with no `]` anywhere to the right,
		// no bracket can close, and none of them is scanned to the end of the word
		// to find that out one at a time.
		if (!looked_for_close) {
			for (size_t j = 0; j < word.size(); ++j) {
				if (word[j] == '\\')
					++j;
				else if (word[j] == ']')
					last_close = j;
			}
			looked_for_close = true;
		}
		if (last_close == std::string_view::npos || i >= last_close)
			continue;

		// Asked of the MATCHER rather than decided here. "Does this bracket close"
		// is a question about bracket grammar - `[]a]` holds a literal `]`, and the
		// `]` inside `[[:alpha:]]` terminates the class rather than the expression -
		// and a second opinion about that grammar is exactly what #23 exists to
		// prevent. The subject byte cannot change the answer: match_bracket reads it
		// only to decide whether the expression MATCHED.
		//
		// Every `[` is tried, not just the first, because the matcher re-reads from
		// the byte after one that opened nothing: `[[:alpha:]` is a literal `[`
		// followed by a character class, and so is a pattern after all.
		size_t past = i;
		bool well_formed = true;
		std::ignore = match_bracket(word, past, '\0', well_formed);
		if (well_formed)
			return true;
	}
	return false;
}

size_t remove_pattern_escapes(std::string_view pattern, char* out) noexcept {
	size_t written = 0;
	for (size_t i = 0; i < pattern.size(); ++i) {
		// The escaped byte is kept and the backslash is not - and a backslash with
		// nothing after it is kept, exactly as match_here reads it: that branch
		// tests `p + 1 < pattern.size()` too, so a trailing one compares as data.
		if (pattern[i] == '\\' && i + 1 < pattern.size())
			++i;
		out[written++] = pattern[i];
	}
	return written;
}

size_t match_prefix(std::string_view pattern, std::string_view text, bool longest) noexcept {
	// Try every split point. Shortest wants the first hit, longest the last, and
	// both must consider zero length because `*` matches empty.
	size_t found = no_match;
	for (size_t n = 0; n <= text.size(); ++n) {
		const size_t len = longest ? text.size() - n : n;
		if (pattern_match(pattern, text.substr(0, len))) {
			found = len;
			break;
		}
	}
	return found;
}

size_t match_suffix(std::string_view pattern, std::string_view text, bool longest) noexcept {
	size_t found = no_match;
	for (size_t n = 0; n <= text.size(); ++n) {
		const size_t len = longest ? text.size() - n : n;
		if (pattern_match(pattern, text.substr(text.size() - len))) {
			found = len;
			break;
		}
	}
	return found;
}

} // namespace lesh::runtime
