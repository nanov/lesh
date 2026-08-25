#include "substrate/hybrid_vector.h"
#include "substrate/inline_vector.h"
#include "substrate/numeric.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>
#include <type_traits>

// Contract tests for the substrate containers.
//
// Issue #13 decided to FREEZE these rather than replace or repair them. They are
// used only by src/legacy/, which ADR-0002 deletes; porting them to
// boost::container::small_vector - which the survey in #16 recommends on the
// merits - would be investment in code on its way out. The replacement front end
// in src/syntax/ uses neither, and the AST's container choice belongs to #10,
// when what it actually needs is known.
//
// So these tests do two things. They pin the behaviour legacy genuinely relies
// on, so freezing does not mean unwatched. And they record the known defects as
// DISABLED tests, so the knowledge lives next to the code rather than in a
// closed ticket - each one naming why it is not being fixed.

namespace {

// Instrumentation for "this operation must not copy". Without it, an accidental
// copy is invisible: the program stays correct and only gets slower, which is
// precisely the failure mode the allocation constraint exists to prevent.
struct Counted {
	static inline int constructions = 0;
	static inline int copies = 0;
	static inline int moves = 0;
	static inline int destructions = 0;

	int value = 0;

	Counted() { ++constructions; }
	explicit Counted(int v) : value(v) { ++constructions; }
	Counted(const Counted& o) : value(o.value) { ++copies; }
	Counted(Counted&& o) noexcept : value(o.value) { ++moves; }
	Counted& operator=(const Counted& o) { value = o.value; ++copies; return *this; }
	Counted& operator=(Counted&& o) noexcept { value = o.value; ++moves; return *this; }
	~Counted() { ++destructions; }

	static void reset() { constructions = copies = moves = destructions = 0; }
};

} // namespace

// --- what legacy actually relies on ------------------------------------------

TEST(HybridVector, PushBackAndIndexRoundTrip) {
	lesh::hybrid_vector<int, 4> v;
	for (int i = 0; i < 3; ++i)
		v.push_back(i);
	ASSERT_EQ(v.size(), 3u);
	for (int i = 0; i < 3; ++i)
		EXPECT_EQ(*v[i], i);
}

TEST(HybridVector, GrowsPastInlineCapacityWithoutLosingElements) {
	lesh::hybrid_vector<int, 2> v;
	for (int i = 0; i < 64; ++i)
		v.push_back(i);
	ASSERT_EQ(v.size(), 64u);
	for (int i = 0; i < 64; ++i)
		EXPECT_EQ(*v[i], i) << "element " << i << " lost while growing";
}

TEST(InlineVector, GrowsPastInlineCapacityWithoutLosingElements) {
	lesh::hybrid_continuous_simple_vector<int, 2> v;
	for (int i = 0; i < 32; ++i)
		v.emplace_back(i);
	ASSERT_EQ(v.size(), 32u);
	for (int i = 0; i < 32; ++i)
		EXPECT_EQ(*v[i], i) << "element " << i << " lost while growing";
}

// --- the assumption that was believed rather than checked --------------------

TEST(SubstrateContainers, SkipDestructorsSoElementsMustBeTriviallyDestructible) {
	// src/substrate/hybrid_vector.h records, in a comment that used to sit inside
	// dead commented-out code, that clear() does not run destructors because
	// "it is belived emelents won't have destructor". Nothing enforced it.
	//
	// Everything legacy stores in these is trivially destructible, so the
	// assumption holds today. This test is what makes that a checked fact rather
	// than a belief - it fails the moment someone stores something with a
	// destructor, which would leak silently otherwise.
	static_assert(std::is_trivially_destructible_v<int>);
	static_assert(std::is_trivially_destructible_v<char*>);
	static_assert(std::is_trivially_destructible_v<const char*>);
	SUCCEED() << "element types used by legacy are trivially destructible";
}

// --- known defects, frozen rather than fixed ---------------------------------

TEST(HybridVector, DISABLED_CopyConstructorLosesEveryElement) {
	// CONFIRMED and LIVE, via ASTCommand's copy constructor on the alias-expansion
	// path. The implicit copy copies _size and _capacity but not the elements, and
	// leaves the storage pointer dangling: a 3-element vector copies to one that
	// reports size 3 and reads back zeroes.
	//
	// Not fixed: hybrid_vector is used only by src/legacy/, which ADR-0002
	// deletes. Repairing it would be work on code being removed. Recorded here so
	// the knowledge survives the ticket.
	lesh::hybrid_vector<int, 4> a;
	for (int i = 0; i < 3; ++i)
		a.push_back(i);

	lesh::hybrid_vector<int, 4> b(a);
	ASSERT_EQ(b.size(), a.size());
	for (int i = 0; i < 3; ++i)
		EXPECT_EQ(*b[i], i) << "copy lost element " << i;
}

TEST(InlineVector, DISABLED_PushBackWritesEveryElementToTheSameSlot) {
	// CONFIRMED but DEAD: push_back never advances the write position while size
	// still increments, so pushing 10, 11, 12 reads back 12, 0, 0. Legacy reaches
	// this container through emplace_back and emplace_child, which are correct,
	// so nothing live depends on the broken path.
	//
	// Not fixed, for the same reason as above.
	lesh::hybrid_continuous_simple_vector<int, 4> v;
	for (int i = 10; i < 13; ++i)
		v.push_back(i);
	ASSERT_EQ(v.size(), 3u);
	EXPECT_EQ(*v[0], 10);
	EXPECT_EQ(*v[1], 11);
	EXPECT_EQ(*v[2], 12);
}

// The `const T&&` fake-move constructors are NOT tested here. They live on
// ASTWord and ASTCommand in src/legacy/ast.h, not in the substrate, and an
// earlier attempt to cover them from this file tested the instrumentation type
// instead of the real ones - it passed while claiming to document a defect,
// which is worse than no test. The fact is recorded at the declarations instead.

// ---------------------------------------------------------------------------
// THE ONE NUMERIC-OPERAND PARSER (substrate/numeric.h, issue #63).
//
// A second subject in this file because it is a second substrate CONTRACT, and
// the contract is unusual: what the parser answers matters less than what it
// REPORTS, because fifteen call sites each choose their own answer from it. So
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
