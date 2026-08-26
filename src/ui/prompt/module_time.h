#pragma once

// One built-in prompt module (#157, spec §6.10): the class and its one object.
// See `module.h` for the vocabulary and `modules.h` for the table these sit in.

#include "ui/prompt/module.h"

#include <cstdint>
#include <string_view>

namespace lesh::ui::prompt {

// --- time ------------------------------------------------------------------

struct time_params {
	enum class form : std::uint8_t {
		h24,    // HH:MM, the default
		h24s,   // HH:MM:SS
		h12,    // HH:MM on a twelve-hour clock
		h12s,   // HH:MM:SS on a twelve-hour clock
	};

	form which = form::h24;
};

constexpr void append_two_digits(sink& out, unsigned value) {
	out.append_byte(static_cast<char>('0' + (value / 10u) % 10u));
	out.append_byte(static_cast<char>('0' + value % 10u));
}

class module_time final : public typed_module<time_params> {
public:
	[[nodiscard]] constexpr std::string_view name() const noexcept override { return "time"; }

protected:
	constexpr bool parse(std::string_view type, time_params& out,
	                     parse_error& err) const override {
		using form = time_params::form;
		if (type.empty() || type == "24h") { out.which = form::h24; return true; }
		if (type == "24hs")                { out.which = form::h24s; return true; }
		if (type == "12h")                 { out.which = form::h12; return true; }
		if (type == "12hs")                { out.which = form::h12s; return true; }

		err.at = 0;
		err.length = type.size();
		err.what = ": unknown variant";
		return false;
	}

	// The one v1 module that asks to be woken.
	//
	// THE REQUEST IS DERIVED FROM THE TICK, NEVER STORED: the next second on the
	// 10 ms grid is `100 - tick % 100` ticks away, and the next MINUTE is that
	// plus the whole seconds left in this one - both functions of the facts and
	// of nothing this module remembers. That is §6.10's "the tick is the state"
	// at its smallest: a clock parked through a long command re-arms from the
	// fire rather than catching up on the minutes it slept through.
	//
	// AND THE CADENCE FOLLOWS THE FORM. A prompt showing HH:MM has no business
	// waking sixty times a minute to redraw bytes that did not move; §6.10's
	// "unchanged output produces no write" would have caught the write, but not
	// the wakeup, and the wakeup is what costs a laptop its battery.
	constexpr int render(const state& facts, const time_params& params,
	                     sink& out) const override {
		const bool twelve = params.which == time_params::form::h12
		                    || params.which == time_params::form::h12s;
		const bool seconds = params.which == time_params::form::h24s
		                     || params.which == time_params::form::h12s;

		unsigned hours = facts.hours;
		if (twelve) {
			// NO am/pm SUFFIX. prmt has none, the hour is unambiguous to somebody
			// looking at their own terminal, and two more columns of a line's width
			// is a real price for a fact the user already knows.
			hours %= 12u;
			if (hours == 0)
				hours = 12u;
		}

		append_two_digits(out, hours);
		out.append_byte(':');
		append_two_digits(out, facts.minutes);
		if (seconds) {
			out.append_byte(':');
			append_two_digits(out, facts.seconds);
		}

		const std::uint64_t to_next_second = 100 - facts.tick % 100;
		out.wake_in(seconds ? to_next_second
		                    : (59u - facts.seconds) * 100u + to_next_second);
		return code(element_status::ready);
	}
};

// The one object, for the whole process - see `modules.h`.
inline constexpr module_time kModuleTime{};

} // namespace lesh::ui::prompt
