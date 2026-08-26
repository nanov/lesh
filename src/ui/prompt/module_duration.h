#pragma once

// One built-in prompt module (#157, spec §6.10): the class and its one object.
// See `module.h` for the vocabulary and `modules.h` for the table these sit in.

#include "ui/prompt/module.h"

#include <cstdint>
#include <string_view>

namespace lesh::ui::prompt {

// --- duration --------------------------------------------------------------

// Under this, the last command was fast enough that saying so is noise. Two
// seconds is starship's default and prmt's, and agreeing with them costs
// nothing.
inline constexpr std::uint64_t kDurationFloorMs = 2000;

class module_duration final : public typed_module<no_params> {
public:
	[[nodiscard]] constexpr std::string_view name() const noexcept override { return "duration"; }

protected:
	constexpr bool parse(std::string_view type, no_params&, parse_error& err) const override {
		return refuse_any_type(type, err);
	}

	// How long the last command took, humanized. Integer seconds throughout: a
	// prompt that reported `2.317s` would be inviting the eye to read a digit that
	// changes every run and means nothing.
	constexpr int render(const state& facts, const no_params&, sink& out) const override {
		if (facts.duration_ms < kDurationFloorMs)
			return code(element_status::omitted);

		const std::uint64_t total = facts.duration_ms / 1000;
		if (total < 60) {
			out.append(decimal{total}.view());
			out.append("s");
		} else if (total < 3600) {
			out.append(decimal{total / 60}.view());
			out.append("m");
			out.append(decimal{total % 60}.view());
			out.append("s");
		} else {
			out.append(decimal{total / 3600}.view());
			out.append("h");
			out.append(decimal{(total / 60) % 60}.view());
			out.append("m");
			out.append(decimal{total % 60}.view());
			out.append("s");
		}
		return code(element_status::ready);
	}
};

// The one object, for the whole process - see `modules.h`.
inline constexpr module_duration kModuleDuration{};

} // namespace lesh::ui::prompt
