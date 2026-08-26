// STUB for agent/157-prompt-core - the real reader is agent/157-git-head's; the
// merge keeps that one.
//
// "Not in a work tree" is the honest answer for a reader that has not been
// written yet: every caller already handles it, and it is the one answer that
// cannot make a prompt render something false while the two halves are apart.

#include "leshper/git_head.h"

namespace lesh::leshper::prompt {

git_head read_git_head(std::string_view directory, const git_probe_options& options) {
	(void)directory;
	(void)options;
	return {};
}

} // namespace lesh::leshper::prompt
