#include "runtime/pattern.h"

#include <cstddef>

namespace lesh::runtime {

namespace {

// Matches a bracket expression starting at pattern[0] == '['.
//
// Returns whether it matched, and advances `pattern_at` past the closing ']'.
// An unterminated '[' is a literal '[' - POSIX's rule, and the reason this
// returns a separate "well formed" flag rather than just failing.
bool match_bracket(std::string_view pattern, size_t& pattern_at, char c, bool& well_formed) {
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

		const char low = pattern[at];
		if (at + 2 < pattern.size() && pattern[at + 1] == '-' && pattern[at + 2] != ']') {
			const char high = pattern[at + 2];
			if (c >= low && c <= high)
				matched = true;
			at += 3;
		} else {
			if (c == low)
				matched = true;
			++at;
		}
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
