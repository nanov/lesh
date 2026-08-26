#pragma once

// One built-in prompt module (#157, spec §6.10): the class and its one object.
// See `module.h` for the vocabulary and `modules.h` for the table these sit in.

#include "leshper/prompt/module.h"

#include <cstdint>
#include <string_view>

namespace lesh::leshper::prompt {

// --- mode ------------------------------------------------------------------

// The vi-mode indicator (F-40). The TEXT arrives on the facts rather than being
// chosen here: #117 says the indicator is whatever the topmost keymap declares,
// so a module that mapped modes to strings would be a second, disagreeing answer
// to that question.
class module_mode final : public typed_module<no_params> {
public:
	[[nodiscard]] constexpr std::string_view name() const noexcept override { return "mode"; }

protected:
	constexpr bool parse(std::string_view type, no_params&, parse_error& err) const override {
		return refuse_any_type(type, err);
	}

	constexpr int render(const state& facts, const no_params&, sink& out) const override {
		if (facts.mode.empty())
			return code(element_status::omitted);
		out.append(facts.mode);
		return code(element_status::ready);
	}
};

// The one object, for the whole process - see `modules.h`.
inline constexpr module_mode kModuleMode{};

} // namespace lesh::leshper::prompt
