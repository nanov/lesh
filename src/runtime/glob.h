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
[[nodiscard]] bool expand_pathnames(buffer_pool& pool, std::string_view word,
                                    arena_array<std::string_view>& out);

} // namespace lesh::runtime
