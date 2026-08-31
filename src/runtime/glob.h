#pragma once

#include "substrate/arena.h"
#include "substrate/arena_array.h"

#include <string_view>

namespace lesh::runtime {

// Pathname expansion. See issue #23.
//
// Appends every matching pathname to `out`, sorted. When nothing matches, the
// word is appended UNCHANGED - the POSIX behaviour that surprises everyone, and
// the reason `ls *.nothing` reports "*.nothing: No such file" rather than
// silently doing nothing.
//
// Returns false when the word is not a pattern at all, so the caller can skip
// this entirely; that is the overwhelming majority of words. False is also the
// observable for "no directory was opened": it is decided by is_pattern before
// the walk starts, so a word this rejects has cost no syscall.
//
// TWO forms of the same field, because POSIX 2.6 hands pathname expansion and
// quote removal different text (#210). `pattern` is the field with its quoting
// still marked as backslash escapes - the only channel a matcher has for "this
// asterisk is data" - and is what the walk reads. `quote_removed` is the same
// field with those escapes already gone, and is what the word expands to when
// nothing matched, quote removal having run after the walk either way. The two
// differ only where a metacharacter arrived quoted, so a word with no quoting in
// it is its own pattern - which is what the overload below says.
[[nodiscard]] bool expand_pathnames(buffer_pool& pool, std::string_view pattern,
                                    std::string_view quote_removed,
                                    arena_array<std::string_view>& out);

[[nodiscard]] inline bool expand_pathnames(buffer_pool& pool, std::string_view word,
                                           arena_array<std::string_view>& out) {
	return expand_pathnames(pool, word, word, out);
}

} // namespace lesh::runtime
