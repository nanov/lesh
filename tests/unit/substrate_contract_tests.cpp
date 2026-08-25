#include "substrate/numeric.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>

// Contract tests for the substrate.
//
// The frozen containers - hybrid_vector and inline_vector - were the first half of
// this file. #13 froze them rather than repair them because their only caller was
// src/legacy/; #28 deleted that caller and #100 threw the containers after it. The
// two DISABLED tests that recorded their defects went too: a permanently disabled
// test is a comment with a build cost, and the defects belong to code that is gone.
//
// ---------------------------------------------------------------------------
// THE ONE NUMERIC-OPERAND PARSER (substrate/numeric.h, issue #63).
//
// The whole of this file, and an unusual contract: what the parser answers
// matters less than what it REPORTS, because fifteen call sites each choose
// their own answer from it. So
// these assert the three-way outcome and the exact boundaries, and the callers'
// policies are asserted where the callers are.
//
// Constexpr wherever it can be, which is not decoration: a static_assert that
// fails is a build that stops, and every one of these inputs used to be a value
// some site read wrongly at run time.

using lesh::accumulate_digit;
using lesh::digit_run;
using lesh::numeric_parse;
using lesh::numeric_result;
using lesh::numeric_site;
using lesh::parse_integer;
using lesh::policy_for;
using lesh::scan_digits;

namespace {

constexpr numeric_result parsed(std::string_view text, numeric_site site) {
	return parse_integer(text, site);
}

} // namespace

TEST(NumericParse, AccumulationStopsAtTheLimitInsteadOfOverflowing) {
	// The atom #59 built and #62 copied twice. Past the limit the value is left AT
	// the limit, which is what makes a saturating caller and a refusing caller read
	// the same call differently and both be right.
	uint64_t value = 0;
	EXPECT_TRUE(accumulate_digit(value, 9, 10, 100));
	EXPECT_EQ(value, 9u);
	EXPECT_TRUE(accumulate_digit(value, 9, 10, 100));
	EXPECT_EQ(value, 99u);
	EXPECT_FALSE(accumulate_digit(value, 9, 10, 100));
	EXPECT_EQ(value, 100u) << "an overflowed accumulation must land on the limit";

	// A limit smaller than the digit itself is the edge the check has to survive,
	// because `limit - digit` underflows an unsigned subtraction otherwise.
	uint64_t small = 0;
	EXPECT_FALSE(accumulate_digit(small, 9, 10, 3));
	EXPECT_EQ(small, 3u);
}

TEST(NumericParse, ADigitRunKeepsReadingPastTheOverflow) {
	// An arithmetic literal MUST swallow the rest of its digits after overflowing:
	// stopping at the one that overflowed would leave the remainder behind to be
	// parsed as operators, turning `$((99999999999999999999))` into a syntax error.
	constexpr digit_run run = scan_digits("99999999999999999999+1", 10, INT64_MAX);
	static_assert(run.consumed == 20, "every digit of the literal is consumed");
	static_assert(run.overflowed);
	static_assert(run.value == static_cast<uint64_t>(INT64_MAX));

	// Bases, for the one caller that has them.
	static_assert(scan_digits("ff", 16, INT64_MAX).value == 255);
	static_assert(scan_digits("17", 8, INT64_MAX).value == 15);
	// 8 is no octal digit, so the run ends at the 1.
	static_assert(scan_digits("18", 8, INT64_MAX).consumed == 1);
}

TEST(NumericParse, TheThreeOutcomesAreToldApart) {
	// The whole content of the consolidation. Six sites used to answer
	// not_a_number and out_of_range with the SAME value, which is how
	// `exit notanumber` came to report success.
	EXPECT_EQ(parsed("12", numeric_site::test_operand).status, numeric_parse::ok);
	EXPECT_EQ(parsed("notanumber", numeric_site::test_operand).status,
	          numeric_parse::not_a_number);
	EXPECT_EQ(parsed("99999999999999999999", numeric_site::test_operand).status,
	          numeric_parse::out_of_range);
}

TEST(NumericParse, TrailingTextIsNotANumberRatherThanATruncatedOne) {
	// `std::atoi("3x")` is 3, which is how `exit 3x` exited 3. An operand is the
	// WHOLE word or it is not a number.
	EXPECT_EQ(parsed("3x", numeric_site::exit_status).status, numeric_parse::not_a_number);
	EXPECT_EQ(parsed("0x10", numeric_site::exit_status).status, numeric_parse::not_a_number);
	EXPECT_EQ(parsed("--", numeric_site::exit_status).status, numeric_parse::not_a_number);
	EXPECT_EQ(parsed("", numeric_site::exit_status).status, numeric_parse::not_a_number);
	EXPECT_EQ(parsed("-", numeric_site::exit_status).status, numeric_parse::not_a_number);
	EXPECT_EQ(parsed("%1", numeric_site::wait_pid_operand).status,
	          numeric_parse::not_a_number);
}

TEST(NumericParse, ANotANumberCarriesZeroAndAnOutOfRangeCarriesTheClamp) {
	// The two policies that do NOT refuse read `value` straight out, so both have
	// to be right without a branch at the call site: shell_state::get wants the
	// zero and OPTIND wants the clamp.
	EXPECT_EQ(parsed("notanumber", numeric_site::variable_as_number).value, 0);
	EXPECT_EQ(parsed("99999999999999999999", numeric_site::variable_as_number).value,
	          INT64_MAX);
	EXPECT_EQ(parsed("-99999999999999999999", numeric_site::variable_as_number).value,
	          INT64_MIN);
	EXPECT_EQ(parsed("99999999999999999999", numeric_site::loop_flow_level).value,
	          lesh::kNumericIntMax);
}

TEST(NumericParse, TheSignedLimitsRoundTripAtBothEnds) {
	// INT64_MIN is the value the whole unsigned-magnitude idiom exists for: its
	// magnitude is 2^63, which does not fit the signed accumulator the old code
	// built it in, and negating the signed conversion of it was undefined
	// behaviour on the one operand the range check admits (#62).
	static_assert(parse_integer("-9223372036854775808", numeric_site::test_operand).value
	                  == INT64_MIN);
	static_assert(parse_integer("-9223372036854775808", numeric_site::test_operand).status
	                  == numeric_parse::ok);
	static_assert(parse_integer("9223372036854775807", numeric_site::test_operand).value
	                  == INT64_MAX);
	// One past either end is out of range, not a wrapped value.
	static_assert(parse_integer("-9223372036854775809", numeric_site::test_operand).status
	                  == numeric_parse::out_of_range);
	static_assert(parse_integer("9223372036854775808", numeric_site::test_operand).status
	                  == numeric_parse::out_of_range);
}

TEST(NumericParse, ASiteWithoutASignRefusesOneRatherThanNegating) {
	// `shift -1` and `break -1` are malformed operands rather than negative counts,
	// which is what keeps a negative from reaching consume_loop_flow as something
	// indistinguishable from 1.
	EXPECT_FALSE(policy_for(numeric_site::shift_count).sign);
	EXPECT_EQ(parsed("-1", numeric_site::shift_count).status, numeric_parse::not_a_number);
	EXPECT_EQ(parsed("-1", numeric_site::loop_flow_level).status,
	          numeric_parse::not_a_number);
	// And one WITH a sign takes both spellings, because dash does.
	EXPECT_EQ(parsed("-1", numeric_site::test_operand).value, -1);
	EXPECT_EQ(parsed("+1", numeric_site::test_operand).value, 1);
}

TEST(NumericParse, BlanksAreToleratedOnlyWhereTheSiteSaysSo) {
	// dash's tolerances, copied deliberately rather than flattened: `test` takes a
	// newline round its operand and `kill` takes only spaces and tabs, while a
	// variable read as a number takes none at all.
	EXPECT_EQ(parsed(" 1 ", numeric_site::test_operand).value, 1);
	EXPECT_EQ(parsed("\n1\n", numeric_site::test_operand).value, 1);
	EXPECT_EQ(parsed(" 1 ", numeric_site::kill_pid_operand).value, 1);
	EXPECT_EQ(parsed("\n1\n", numeric_site::kill_pid_operand).status,
	          numeric_parse::not_a_number);
	EXPECT_EQ(parsed(" 1 ", numeric_site::variable_as_number).status,
	          numeric_parse::not_a_number);
}

TEST(NumericParse, EverySiteHasExactlyOneRowAndItIsItsOwn) {
	// The runtime half of the registry guard the header static_asserts. A row
	// silently shifted by one would hand a site another's range, and every one of
	// these ranges is a decision some ticket argued.
	for (size_t i = 0; i < static_cast<size_t>(numeric_site::count_); ++i) {
		const auto site = static_cast<numeric_site>(i);
		EXPECT_EQ(policy_for(site).site, site) << "policy row " << i << " names another site";
		EXPECT_LE(policy_for(site).low, policy_for(site).high);
	}
}
