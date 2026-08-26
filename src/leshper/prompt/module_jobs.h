#pragma once

// One built-in prompt module (#157, spec §6.10): the class and its one object.
// See `module.h` for the vocabulary and `modules.h` for the table these sit in.

#include "leshper/prompt/module.h"

#include <cstdint>
#include <string_view>

namespace lesh::leshper::prompt {

// --- jobs ------------------------------------------------------------------

class module_jobs final : public typed_module<no_params> {
public:
	[[nodiscard]] constexpr std::string_view name() const noexcept override { return "jobs"; }

protected:
	constexpr bool parse(std::string_view type, no_params&, parse_error& err) const override {
		return refuse_any_type(type, err);
	}

	constexpr int render(const state& facts, const no_params&, sink& out) const override {
		if (facts.jobs == 0)
			return code(element_status::omitted);
		out.append(decimal{static_cast<std::uint64_t>(facts.jobs)}.view());
		return code(element_status::ready);
	}
};

// The one object, for the whole process - see `modules.h`.
inline constexpr module_jobs kModuleJobs{};

} // namespace lesh::leshper::prompt
