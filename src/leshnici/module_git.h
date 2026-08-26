#pragma once

// THE `git` PROMPT MODULE (#157, spec §6.10), and leshnici's first resident.
//
// IT IS NOT A BUILT-IN. Everything else `{…}` can name is a pure function of
// the facts struct; this one reads a filesystem, which is why it needs
// `git_head.{h,cpp}` beside it and why it is not in `prompt/modules.h`. The
// engine learns it when the wiring site installs the shipped extension set -
// see `prompt_modules.h` - and by no other route.

#include "leshnici/git_head.h"
#include "ui/prompt/module.h"

#include <string_view>

namespace lesh::leshnici {

// The vocabulary is leshper's; the module is ours. Named rather than pulled in
// wholesale so the class body below is the same text it was in `prompt.h`.
using ui::prompt::code;
using ui::prompt::element_status;
using ui::prompt::no_params;
using ui::prompt::parse_error;
using ui::prompt::refuse_any_type;
using ui::prompt::sink;
using ui::prompt::state;
using ui::prompt::typed_module;

// --- git -------------------------------------------------------------------

class module_git final : public typed_module<no_params> {
public:
	[[nodiscard]] constexpr std::string_view name() const noexcept override { return "git"; }

protected:
	constexpr bool parse(std::string_view type, no_params&, parse_error& err) const override {
		return refuse_any_type(type, err);
	}

	// The branch, or the short object name on a detached HEAD - v1's one budgeted
	// module.
	//
	// THE GUARD IS FIRST AND IT IS TWO THINGS AT ONCE. At runtime it is §6.10's
	// floor rule: a caller that has not allowed filesystem work gets no syscall,
	// not a fast one. During constant evaluation it is what makes this function
	// legal at all - C++23 permits a `constexpr` function to CONTAIN a call it
	// never evaluates, and `fs_allowed` false is what keeps the evaluation off the
	// `read_git_head` line. One module, two worlds, no paper copy of it for the
	// compiled default to render instead.
	constexpr int render(const state& facts, const no_params&, sink& out) const override {
		if (!facts.fs_allowed || facts.pwd.empty())
			return code(element_status::omitted);

		const git_head head = read_git_head(facts.pwd, git_probe_options{
			.budget_ms = facts.fs_budget_ms,
			.allow_spawn = true,
			.git_command = "git",
		});
		if (!head.found)
			return code(element_status::omitted);

		out.append(head.detached ? head.short_sha : head.branch);
		return code(element_status::ready);
	}
};

// One object for the whole process, exactly as the built-ins are.
inline constexpr module_git kModuleGit{};

} // namespace lesh::leshnici
