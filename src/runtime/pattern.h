#pragma once

#include <string_view>

namespace lesh::runtime {

// Shell pattern matching. See issue #23.
//
// ONE matcher, three callers: pathname expansion (globbing), `case` patterns
// (#19), and the ${x#pat} trimming family (#22). Writing it three times is how
// mksh ended up with three decoders for one encoding, so this ticket blocks the
// other two deliberately.
//
// Shell patterns are NOT regular expressions. `*` matches any string including
// empty, `?` exactly one character, `[...]` a bracket expression - and there is
// no alternation, no repetition operator, and no anchoring. Reaching for a regex
// engine here would be both slower and wrong.

// Matches `text` against `pattern` in full - shell patterns are anchored at both
// ends, so `case abc in b) ...` does NOT match.
//
// `period_is_special` implements the rule that a leading `.` in a filename must
// be matched explicitly: `*` does not match `.hidden`. It is true for pathname
// expansion and false for `case` and parameter trimming, which is why it is a
// parameter rather than baked in - the same matcher, two rules.
[[nodiscard]] bool pattern_match(std::string_view pattern, std::string_view text,
                                 bool period_is_special = false) noexcept;

// True when this FRAGMENT of a word contributes an unquoted metacharacter.
//
// A cheap over-approximation, and deliberately so: the expander asks it of one
// SEGMENT at a time, to decide whether the field being assembled is eligible for
// pathname expansion at all. It cannot be the final answer, because a bracket
// expression may open in one segment and close in another - `[a$x` with x=`]` is
// a live bracket expression, and a segment test strict enough to reject `[a` on
// its own would stop globbing it. Whether the assembled word is actually a
// pattern is is_pattern's question, below.
[[nodiscard]] bool has_pattern_characters(std::string_view text) noexcept;

// True when the whole WORD is a pattern - the gate pathname expansion consults
// before it touches the filesystem.
//
// Stricter than has_pattern_characters, and a second function rather than a
// stricter version of it, because the two are asked of different text. POSIX
// 2.13.1: a `[` with no matching `]` LATER IN THE SAME WORD is an ordinary
// character, and a `]` on its own always is. So `[`, `[abc`, `a[b` and `[]` name
// themselves and must not cost a directory scan - which is what `[ $i -lt 1 ]`
// was paying, twenty microseconds an iteration, once per loop turn (#204).
//
// Answering it needs the word entire, which is why it lives here and not at the
// segment gate above. Quoting is unchanged: `"["` was already literal, having
// never set the segment flag in the first place.
[[nodiscard]] bool is_pattern(std::string_view word) noexcept;

// Every byte this matcher reads as SYNTAX rather than as data, and therefore
// every byte that has to be escaped when it arrives QUOTED. Lives here because
// the matcher is what decides it: an expander that kept its own list of them
// would be a second opinion about one grammar, which is the thing #23 exists to
// prevent.
//
// Wider than the three metacharacters, because a bracket expression has a syntax
// of its own: `]` closes it, `-` makes a range, and `!` or `^` negates it. The
// expander cannot consult POSITION to decide which of those are live - whether a
// byte lands inside brackets depends on text that may have come from a different
// segment of the word entirely - so a quoted one is escaped wherever it goes.
// That costs nothing: the matcher reads `\x` as `x` for every x, so an escape
// that turns out to be unnecessary is merely unnecessary. `case '-' in [a\-c]`
// is the two-character set, and dropping the backslash from a quoted `-` put the
// range back.
[[nodiscard]] constexpr bool is_pattern_syntax(char c) noexcept {
	return c == '\\' || c == '*' || c == '?' || c == '[' || c == ']' || c == '-' ||
	       c == '!' || c == '^';
}

// Copies `pattern` with its escapes removed, writing to `out` - which must have
// room for pattern.size() bytes - and returning how many bytes it wrote.
//
// This is POSIX 2.6's quote removal, which runs LAST, after pathname expansion:
// a pattern that took part in a walk still has to lose its backslashes before it
// can name a file. The inverse of the escaping above, and it lives here rather
// than in the expander because the WALK needs it too - a LITERAL component of a
// globbed word, the `a\*b` of `a\*b/*`, is extended by name instead of scanned
// for, and the name is the unescaped one. One decoder for the one encoding,
// which is what the rest of this header is about.
//
// A trailing lone backslash is KEPT, because that is what the matcher does with
// it: with no byte left to escape, it stands for itself.
[[nodiscard]] size_t remove_pattern_escapes(std::string_view pattern, char* out) noexcept;

// Longest or shortest match anchored at one end, for ${x#pat} and friends.
// Returns the number of bytes matched, or npos when nothing matched - zero is a
// legitimate result, since `*` matches the empty string.
[[nodiscard]] size_t match_prefix(std::string_view pattern, std::string_view text,
                                  bool longest) noexcept;
[[nodiscard]] size_t match_suffix(std::string_view pattern, std::string_view text,
                                  bool longest) noexcept;

inline constexpr size_t no_match = static_cast<size_t>(-1);

} // namespace lesh::runtime
