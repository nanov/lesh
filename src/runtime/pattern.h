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

// True when the pattern contains an unquoted metacharacter. A word without one
// expands to itself, so this is the cheap test that avoids a directory scan for
// the overwhelming majority of words.
[[nodiscard]] bool has_pattern_characters(std::string_view text) noexcept;

// Longest or shortest match anchored at one end, for ${x#pat} and friends.
// Returns the number of bytes matched, or npos when nothing matched - zero is a
// legitimate result, since `*` matches the empty string.
[[nodiscard]] size_t match_prefix(std::string_view pattern, std::string_view text,
                                  bool longest) noexcept;
[[nodiscard]] size_t match_suffix(std::string_view pattern, std::string_view text,
                                  bool longest) noexcept;

inline constexpr size_t no_match = static_cast<size_t>(-1);

} // namespace lesh::runtime
