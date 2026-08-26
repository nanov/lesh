#pragma once
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>

namespace lesh {

// ONE READING OF A NUMERIC OPERAND, for every site in the shell that has one.
//
// Surveyed at `1319724` there were FIFTEEN sites and SIX idioms for the same
// question - saturate, clamp, break-before-overflow, unsigned-magnitude, refuse,
// and wrap - plus four calls to `std::atoi`, which cannot report failure at all
// and so answered 0 for `notanumber`. Three of those four were wrong answers a
// script could branch on (`exit notanumber` reported SUCCESS) and the fourth
// reached `waitpid` as pid 0, which means ANY CHILD IN THE PROCESS GROUP - the
// wrong-syscall half of #45 verbatim, in the one path that ticket did not fix.
//
// #59 and #62 each fixed a subset independently, which is precisely how N sites
// become N behaviours. This header is the consolidation (#63).
//
// THE MECHANISM IS SHARED; THE POLICY STAYS WITH THE CALLER. #62 established that
// the policies genuinely differ and must not be flattened:
//
//   - `shell_state::get` answers 0 for a non-number DELIBERATELY - that is what
//     lets `i=$((i+1))` work without initialising i.
//   - `OPTIND` and `break`/`continue` levels CLAMP on purpose.
//   - an out-of-range SIGNAL NUMBER is refused, because a clamped one names a
//     real signal the script never asked for.
//   - a bad FILE DESCRIPTOR takes the same diagnostic an already-invalid fd
//     takes; saturating is meaningless because there is no "largest fd".
//   - `test` refuses out of range, but INT64_MIN is a LEGAL operand.
//   - an arithmetic LITERAL saturates while its OPERATORS wrap (#59, settled
//     against 18 probes).
//
// So what is shared is the parse plus WHICH WAY IT FAILED, and each caller keeps
// its own answer. `numeric_result` carries both at once: a caller that clamps
// reads `value`, one that refuses reads `status`, and neither pays for the other.
//
// WHY THIS LIVES IN substrate/. It is the only layer both `syntax` and `runtime`
// can see - the parser reads a redirection fd and the builtins read everything
// else - and the whole point of the table below is that it is ONE list. A second
// copy in `runtime` for the runtime's sites is the state this header exists to
// end.
//
// NO ALLOCATION AND NO ERRNO. Everything here is constexpr over a string_view and
// two integers, which is what keeps it usable on the command path that
// tests/unit/allocation_tests.cpp gates - `strtoll` would have been the obvious
// alternative and it needs a NUL-terminated buffer that a string_view has not
// got, which is how three of these sites came to build their own digits instead.

// Which way the parse failed. The whole content of the consolidation: every
// caller needs to tell these two apart, and six of them used to answer both with
// the same value.
enum class numeric_parse : uint8_t {
	ok,
	not_a_number,   // no digits at all, or a character that is not one
	out_of_range,   // well formed, but past the range the site can hold
};

// The answer, and which way it failed.
struct numeric_result {
	numeric_parse status = numeric_parse::not_a_number;
	// CLAMPED into the site's range when `status` is out_of_range, and zero when
	// it is not_a_number. Both are deliberate: `OPTIND=<huge>` wants the clamp and
	// `shell_state::get` wants the zero, so the two policies that do not refuse
	// get their answer without a second pass over the digits.
	int64_t value = 0;
};

// Which blanks a site tolerates around its operand. Not one rule for all of them,
// because two of the sites copied dash's tolerances DELIBERATELY - `test` takes a
// newline where `kill` does not - and flattening them here would be exactly the
// kind of quiet policy change this header exists to prevent.
enum class numeric_blanks : uint8_t {
	none,
	space_tab,    // dash's `kill`: a pid may already arrive with blanks around it
	whitespace,   // dash's `getn`, which `test` copies: space, tab and newline
};

// EVERY SITE IN THE SHELL THAT READS A NUMBER, named once.
//
// This is the #35 shape, and #35's registry is the precedent that actually
// prevented recurrence: a new site cannot be added without adding an enumerator
// here, and adding an enumerator without adding its row below is a COMPILE ERROR
// from the static_asserts underneath the table. A site that wanted to skip the
// mechanism would have to write its own digits, and the guard test in
// tests/unit/builtin_registry_tests.cpp greps the tree for exactly that.
enum class numeric_site : uint8_t {
	arithmetic_literal,           // `$((99))`                     arithmetic.cpp
	variable_as_number,           // `$((x))`                    shell_state.cpp
	test_operand,                 // `test 1 -eq 1`                 builtins.cpp
	trap_signal_number,           // `trap - 2`                      signals.cpp
	kill_list_operand,            // `kill -l 9`                    builtins.cpp
	kill_pid_operand,             // `kill -s TERM 123`             builtins.cpp
	optind,                       // `OPTIND=3; getopts ...`        builtins.cpp
	loop_flow_level,              // `break 2`                      builtins.cpp
	exit_status,                  // `exit 3`                       builtins.cpp
	return_status,                // `return 3`                     builtins.cpp
	shift_count,                  // `shift 2`                      builtins.cpp
	wait_pid_operand,             // `wait 123`                     executor.cpp
	redirection_target_fd,        // `>&3`                          executor.cpp
	redirection_word_fd,          // `3>file`               parser.cpp/lexer.cpp
	positional_parameter_index,   // `${12}`                        expander.cpp
	csi_parameter,                // `ESC [ 1 ; 5 C`                  decode.cpp
	editor_repeat_count,          // `3dd` at the prompt                  vi.cpp
	count_,                       // must stay last
};

// The RANGE A SITE CAN REPRESENT, and nothing else. A semantic bound narrower
// than the representation stays with the caller: `trap` takes [0, NSIG) and
// `getopts` takes [1, $#+1], and neither of those is knowable here - NSIG is a
// platform constant this layer must not include, and $# is a runtime value. The
// division is the point. This table stops the UNDEFINED BEHAVIOUR (#59, #62);
// what a number MEANS is the caller's, and always was.
struct numeric_policy {
	numeric_site site;
	int64_t low;
	int64_t high;
	// Whether a leading `-` or `+` belongs to the operand. False is not an
	// oversight at any of the sites that have it: `shift -1` and `break -1` are
	// operands POSIX writes as positive decimal integers, and a sign there is a
	// malformed operand rather than a negative one - which is what lets
	// `read_flow_level` tell `break -1` from `break 1`.
	bool sign;
	numeric_blanks blanks;
};

// An fd, a pid, a signal number and a `break` level are all `int` (or, for a pid,
// an `int`-sized `pid_t` on every platform lesh builds for), so the widest value
// each of them can hold is the same one. Named rather than repeated, because a
// table of bare literals is a table nobody reads.
inline constexpr int64_t kNumericIntMax = 2147483647;
inline constexpr int64_t kNumericIntMin = -2147483647 - 1;
inline constexpr int64_t kNumericUnsignedMax = 4294967295;

inline constexpr numeric_policy kNumericPolicies[] = {
	// An arithmetic literal has no sign: `-1` is unary minus applied to `1`, which
	// is why `$((-9223372036854775808))` is a SATURATED INT64_MAX negated.
	{numeric_site::arithmetic_literal,         0,              INT64_MAX,      false, numeric_blanks::none},
	{numeric_site::variable_as_number,         INT64_MIN,      INT64_MAX,      true,  numeric_blanks::none},
	// INT64_MIN is a LEGAL `test` operand - dash, bash and zsh all compare it
	// without complaint - so the low bound admits magnitude 2^63 and the negation
	// below happens in the unsigned domain, where it is defined (#62).
	{numeric_site::test_operand,               INT64_MIN,      INT64_MAX,      true,  numeric_blanks::whitespace},
	{numeric_site::trap_signal_number,         0,              kNumericIntMax, false, numeric_blanks::none},
	{numeric_site::kill_list_operand,          0,              kNumericIntMax, false, numeric_blanks::none},
	{numeric_site::kill_pid_operand,           -kNumericIntMax, kNumericIntMax, true, numeric_blanks::space_tab},
	// OPTIND indexes the positional parameters, which the caller clamps against
	// $#+1; nothing narrower than the widest value it can hold belongs here.
	{numeric_site::optind,                     0,              INT64_MAX,      false, numeric_blanks::none},
	{numeric_site::loop_flow_level,            0,              kNumericIntMax, false, numeric_blanks::none},
	// `exit` and `return` take dash's range, which is an `int`. The MODULO 256
	// POSIX applies to a status is not this - `exit 256` is 0 because 256 is in
	// range and the low byte is what a parent can read - and conflating the two is
	// how `return 99999999999999999999` came to report -1.
	{numeric_site::exit_status,                kNumericIntMin, kNumericIntMax, true,  numeric_blanks::none},
	{numeric_site::return_status,              kNumericIntMin, kNumericIntMax, true,  numeric_blanks::none},
	{numeric_site::shift_count,                0,              kNumericIntMax, false, numeric_blanks::none},
	{numeric_site::wait_pid_operand,           0,              kNumericIntMax, false, numeric_blanks::none},
	{numeric_site::redirection_target_fd,      0,              kNumericIntMax, false, numeric_blanks::none},
	{numeric_site::redirection_word_fd,        0,              kNumericIntMax, false, numeric_blanks::none},
	// An index past the end is an UNSET parameter, which is what the clamp lands
	// on: `${18446744073709551617}` used to wrap a size_t onto `$1`.
	{numeric_site::positional_parameter_index, 0,              INT64_MAX,      false, numeric_blanks::none},
	// A CSI parameter is held in an `unsigned`. That no key at the terminal floor
	// sends one above 201 is the decoder's knowledge, not this table's.
	{numeric_site::csi_parameter,              0,              kNumericUnsignedMax, false, numeric_blanks::none},
	// A repeat count is typed one digit at a time and lands in an int64 numeric
	// argument. What a sane count IS - the ceiling past which `dd` stops being a
	// command and starts being a hang - is the editor's knowledge, not this
	// table's, exactly as the CSI row above says of key codes.
	{numeric_site::editor_repeat_count,        0,              kNumericIntMax, false, numeric_blanks::none},
};

// The registry guard, in #35's shape. The first catches a site added to the enum
// and forgotten here; the second catches a row inserted in the wrong place, which
// would silently hand one site another's range.
static_assert(std::size(kNumericPolicies) == static_cast<size_t>(numeric_site::count_),
              "every numeric_site needs a row in kNumericPolicies");

[[nodiscard]] constexpr bool numeric_policies_are_ordered() noexcept {
	for (size_t i = 0; i < std::size(kNumericPolicies); ++i)
		if (kNumericPolicies[i].site != static_cast<numeric_site>(i))
			return false;
	return true;
}
static_assert(numeric_policies_are_ordered(),
              "kNumericPolicies must be in numeric_site order, one row per site");

[[nodiscard]] constexpr const numeric_policy& policy_for(numeric_site site) noexcept {
	return kNumericPolicies[static_cast<size_t>(site)];
}

// The largest number this site can hold, as a MAGNITUDE. Unsigned because the
// negative limit is one past the positive one - 2^63 negated is INT64_MIN, and
// there is no int64_t for it to be until the negation has happened.
[[nodiscard]] constexpr uint64_t numeric_limit(const numeric_policy& p, bool negative) noexcept {
	return negative ? uint64_t{0} - static_cast<uint64_t>(p.low)
	                : static_cast<uint64_t>(p.high);
}

// ONE DIGIT, ACCUMULATED WITH THE OVERFLOW CHECKED. This is the atom #59 built
// and #62 copied twice: `value * base + digit` in a signed accumulator is
// undefined behaviour past the limit, and every one of the fifteen sites was
// reachable from a single line of input.
//
// False when the digit would carry `value` past `limit`, which leaves `value` AT
// `limit` - so a caller that saturates or clamps has its answer already and one
// that refuses has its flag. The digit is still the caller's to consume: an
// arithmetic literal must swallow the rest of its digits after overflowing,
// because stopping at the one that overflowed would leave the remainder behind to
// be parsed as though it were operators.
[[nodiscard]] constexpr bool accumulate_digit(uint64_t& value, uint64_t digit,
                                              uint64_t base, uint64_t limit) noexcept {
	if (digit > limit || value > (limit - digit) / base) {
		value = limit;
		return false;
	}
	value = value * base + digit;
	return true;
}

// This character's value in `base`, or -1 when it is not a digit of that base.
[[nodiscard]] constexpr int digit_value(char c, uint64_t base) noexcept {
	int digit = -1;
	if (c >= '0' && c <= '9') digit = c - '0';
	else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
	else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
	return digit >= 0 && static_cast<uint64_t>(digit) < base ? digit : -1;
}

// A run of digits read from the FRONT of `text`, for the two callers that are
// scanners rather than whole-string parsers: arithmetic's literal, which sits
// inside a larger expression, and the redirection fd, whose token may have line
// continuations between its digits. Both need to know how far they got.
struct digit_run {
	size_t consumed = 0;    // zero means there were no digits here at all
	uint64_t value = 0;     // clamped at `limit` when `overflowed`
	bool overflowed = false;
};

[[nodiscard]] constexpr digit_run scan_digits(std::string_view text, uint64_t base,
                                              uint64_t limit) noexcept {
	digit_run run;
	for (; run.consumed < text.size(); ++run.consumed) {
		const int digit = digit_value(text[run.consumed], base);
		if (digit < 0)
			break;
		if (!accumulate_digit(run.value, static_cast<uint64_t>(digit), base, limit))
			run.overflowed = true;
	}
	return run;
}

[[nodiscard]] constexpr bool is_blank(char c, numeric_blanks blanks) noexcept {
	switch (blanks) {
		case numeric_blanks::none:      return false;
		case numeric_blanks::space_tab: return c == ' ' || c == '\t';
		case numeric_blanks::whitespace: return c == ' ' || c == '\t' || c == '\n';
	}
	return false;
}

// THE WHOLE-STRING FORM, which is what twelve of the fifteen sites want: the text
// is an operand and anything left over makes it not a number. `12x` is refused
// here rather than truncated to 12, which is what `std::atoi` did and what let
// `exit 3x` exit 3.
[[nodiscard]] constexpr numeric_result parse_integer(std::string_view text,
                                                     numeric_site site) noexcept {
	const numeric_policy& policy = policy_for(site);
	numeric_result result;

	size_t at = 0;
	while (at < text.size() && is_blank(text[at], policy.blanks))
		++at;

	bool negative = false;
	if (policy.sign && at < text.size() && (text[at] == '-' || text[at] == '+'))
		negative = text[at++] == '-';

	const uint64_t limit = numeric_limit(policy, negative);
	const digit_run run = scan_digits(text.substr(at), 10, limit);
	at += run.consumed;
	if (run.consumed == 0)
		return result;   // not_a_number: a sign or a blank alone is not a number

	while (at < text.size() && is_blank(text[at], policy.blanks))
		++at;
	if (at != text.size())
		return result;   // not_a_number: trailing anything

	// NEGATED IN THE UNSIGNED DOMAIN, because -INT64_MIN has no int64_t to be and
	// the limit above deliberately admits magnitude 2^63 for `test` (#62).
	// Unsigned wrapping is defined, and 0 - 2^63 converts back to exactly INT64_MIN.
	result.value = static_cast<int64_t>(negative ? uint64_t{0} - run.value : run.value);
	result.status = run.overflowed ? numeric_parse::out_of_range : numeric_parse::ok;
	return result;
}

} // namespace lesh
