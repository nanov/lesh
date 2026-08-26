#pragma once

// One built-in prompt module (#157, spec §6.10): the class and its one object.
// See `module.h` for the vocabulary and `modules.h` for the table these sit in.

#include "ui/prompt/module.h"

#include <cstdint>
#include <string_view>

namespace lesh::ui::prompt {

// --- status ----------------------------------------------------------------

struct status_params {
	enum class form : std::uint8_t { code, symbol };

	form which = form::code;
	fixed_text<16> symbol{};
};

class module_status final : public typed_module<status_params> {
public:
	[[nodiscard]] constexpr std::string_view name() const noexcept override { return "status"; }

protected:
	// AN UNRECOGNIZED TYPE IS NOT AN ERROR HERE, and this is the one module where
	// that is right: the slot's content IS the symbol. `{status:red:✗}` means
	// "show ✗ when the last command failed", and there is no vocabulary of
	// variants for a typo to fall outside of - only `code`, which is the default
	// spelled out.
	constexpr bool parse(std::string_view type, status_params& out,
	                     parse_error& err) const override {
		if (type.empty() || type == "code") {
			out.which = status_params::form::code;
			return true;
		}
		out.which = status_params::form::symbol;
		if (!out.symbol.assign(type)) {
			err.at = 0;
			err.length = 0;
			err.what = ": symbol is too long";
			return false;
		}
		return true;
	}

	// `$?`, and nothing at all when the last command succeeded - the module the
	// whole omission machinery exists for, because the brackets around it are
	// affixes that have to vanish with it.
	constexpr int render(const state& facts, const status_params& params,
	                     sink& out) const override {
		if (facts.status == 0)
			return code(element_status::omitted);

		if (params.which == status_params::form::symbol) {
			out.append(params.symbol.view());
			return code(element_status::ready);
		}

		// A negative status is not a shell exit status, but a `state` filled from
		// somewhere unusual can hold one, and printing `18446744073709551615` for
		// it would be a worse answer than a minus sign.
		std::uint64_t magnitude = 0;
		if (facts.status < 0) {
			out.append("-");
			magnitude = static_cast<std::uint64_t>(-static_cast<std::int64_t>(facts.status));
		} else {
			magnitude = static_cast<std::uint64_t>(facts.status);
		}
		out.append(decimal{magnitude}.view());
		return code(element_status::ready);
	}
};

// The one object, for the whole process - see `modules.h`.
inline constexpr module_status kModuleStatus{};

} // namespace lesh::ui::prompt
