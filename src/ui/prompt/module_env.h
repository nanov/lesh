#pragma once

// One built-in prompt module (#157, spec §6.10): the class and its one object.
// See `module.h` for the vocabulary and `modules.h` for the table these sit in.

#include "ui/prompt/module.h"

#include <cstdint>
#include <string_view>

namespace lesh::ui::prompt {

// --- env -------------------------------------------------------------------

struct env_params {
	fixed_text<64> name{};
};

class module_env final : public typed_module<env_params> {
public:
	[[nodiscard]] constexpr std::string_view name() const noexcept override { return "env"; }

protected:
	// THE ONE BUILT-IN WHOSE TYPE SLOT IS REQUIRED. `{env}` names no variable and
	// there is no default one to mean; refusing at set time is the difference
	// between a typo and a prompt that silently lost a segment.
	constexpr bool parse(std::string_view type, env_params& out,
	                     parse_error& err) const override {
		if (type.empty()) {
			err.at = 0;
			err.length = 0;
			err.what = " needs a variable name";
			return false;
		}
		if (!out.name.assign(type)) {
			err.at = 0;
			err.length = 0;
			err.what = ": variable name is too long";
			return false;
		}
		return true;
	}

	// AN EMPTY VALUE OMITS. `{env::HOST}@` should vanish on a machine with no
	// `$HOST` rather than render a bare `@`, and "set but empty" is what that
	// machine actually has.
	constexpr int render(const state& facts, const env_params& params,
	                     sink& out) const override {
		if (facts.getvar == nullptr)
			return code(element_status::omitted);

		std::string_view value;
		if (!facts.getvar(facts.getvar_ctx, params.name.view(), value) || value.empty())
			return code(element_status::omitted);
		out.append(value);
		return code(element_status::ready);
	}
};

// The one object, for the whole process - see `modules.h`.
inline constexpr module_env kModuleEnv{};

} // namespace lesh::ui::prompt
