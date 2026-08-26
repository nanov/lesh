#pragma once

// What `.git/HEAD` says, for the prompt's one budgeted module (#157, spec
// §6.10).
//
// DELIBERATELY MINIMAL, AND NOT THIS BRANCH'S TO FILL IN. The reader itself -
// symbolic and detached HEAD, loose and packed refs, gitfile indirection, and
// "unrecognized layout -> spawn `git`" as the correctness fallback - is being
// written beside this one. This header exists so the composer can name the seam
// and so `module_git` compiles; when the two land together the merge keeps the
// other file, which is why nothing here says more than the call needs.

#include <cstdint>
#include <string>
#include <string_view>

namespace lesh::leshper::prompt {

// The branch, or the abbreviated object name when HEAD is detached.
//
// `found` false is not an error: a directory outside a work tree is the common
// case, and it is what makes the `git` seg vanish rather than render an excuse.
struct git_head {
	bool found = false;
	bool detached = false;
	std::string branch;
	std::string short_sha;
};

// How hard to try. The budget is §6.10's floor rule made an argument: the
// filesystem half of the prompt blocks for a small, bounded stretch and never
// for as long as an NFS mount would like to, and `allow_spawn` is the door for
// the layout the hand-rolled reader does not recognize.
struct git_probe_options {
	std::uint32_t budget_ms = 50;
	bool allow_spawn = true;
	const char* git_command = "git";
};

[[nodiscard]] git_head read_git_head(std::string_view directory,
                                     const git_probe_options& options = {});

} // namespace lesh::leshper::prompt
