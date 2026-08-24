#include "runtime/arithmetic.h"

#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>

using namespace lesh::runtime;

namespace {

class FakeVars final : public arithmetic_variables {
public:
	std::map<std::string, int64_t> values;
	// Names that refuse a write, standing in for a readonly variable.
	std::set<std::string> refusing;
	int64_t get(std::string_view name) const override {
		const auto it = values.find(std::string(name));
		return it == values.end() ? 0 : it->second;
	}
	bool set(std::string_view name, int64_t value) override {
		if (refusing.count(std::string(name)) != 0)
			return false;
		values[std::string(name)] = value;
		return true;
	}
	bool defined(std::string_view name) const override {
		return values.find(std::string(name)) != values.end();
	}
	[[nodiscard]] bool assigned(std::string_view name) const {
		return values.find(std::string(name)) != values.end();
	}
};

class ArithmeticTest : public ::testing::Test {
protected:
	FakeVars vars;
	int64_t eval(std::string_view e) {
		const auto r = evaluate(e, vars);
		EXPECT_TRUE(r.ok) << e << ": " << (r.error ? r.error : "");
		return r.value;
	}
	bool fails(std::string_view e) { return !evaluate(e, vars).ok; }
};

} // namespace

TEST_F(ArithmeticTest, Literals) {
	EXPECT_EQ(eval("0"), 0);
	EXPECT_EQ(eval("42"), 42);
	EXPECT_EQ(eval("  7  "), 7) << "blanks are insignificant";
}

TEST_F(ArithmeticTest, BasesFollowC) {
	EXPECT_EQ(eval("0x1f"), 31);
	EXPECT_EQ(eval("0X1F"), 31);
	EXPECT_EQ(eval("010"), 8) << "a leading zero is octal";
	EXPECT_EQ(eval("10"), 10);
}

TEST_F(ArithmeticTest, PrecedenceIsCs) {
	EXPECT_EQ(eval("2 + 3 * 4"), 14) << "multiplication binds tighter";
	EXPECT_EQ(eval("(2 + 3) * 4"), 20);
	EXPECT_EQ(eval("2 * 3 + 4"), 10);
	EXPECT_EQ(eval("1 + 2 < 4"), 1) << "additive binds tighter than relational";
	EXPECT_EQ(eval("1 | 2 & 3"), 3) << "& binds tighter than |";
}

TEST_F(ArithmeticTest, LeftAssociativity) {
	EXPECT_EQ(eval("10 - 3 - 2"), 5) << "(10-3)-2, not 10-(3-2)";
	EXPECT_EQ(eval("100 / 5 / 2"), 10);
}

TEST_F(ArithmeticTest, UnaryOperators) {
	EXPECT_EQ(eval("-5"), -5);
	EXPECT_EQ(eval("+5"), 5);
	EXPECT_EQ(eval("!0"), 1);
	EXPECT_EQ(eval("!5"), 0);
	EXPECT_EQ(eval("~0"), -1);
	EXPECT_EQ(eval("- -5"), 5);
}

TEST_F(ArithmeticTest, ComparisonYieldsOneOrZero) {
	EXPECT_EQ(eval("1 < 2"), 1);
	EXPECT_EQ(eval("2 < 1"), 0);
	EXPECT_EQ(eval("2 <= 2"), 1);
	EXPECT_EQ(eval("1 == 1"), 1);
	EXPECT_EQ(eval("1 != 1"), 0);
}

TEST_F(ArithmeticTest, LogicalOperators) {
	EXPECT_EQ(eval("1 && 1"), 1);
	EXPECT_EQ(eval("1 && 0"), 0);
	EXPECT_EQ(eval("0 || 1"), 1);
	EXPECT_EQ(eval("0 || 0"), 0);
	EXPECT_EQ(eval("5 && 3"), 1) << "the result is 1, not the operand";
}

TEST_F(ArithmeticTest, BitwiseAndShifts) {
	EXPECT_EQ(eval("7 & 3"), 3);
	EXPECT_EQ(eval("4 | 1"), 5);
	EXPECT_EQ(eval("5 ^ 3"), 6);
	EXPECT_EQ(eval("1 << 4"), 16);
	EXPECT_EQ(eval("16 >> 2"), 4);
}

TEST_F(ArithmeticTest, Conditional) {
	EXPECT_EQ(eval("1 ? 42 : 7"), 42);
	EXPECT_EQ(eval("0 ? 42 : 7"), 7);
	EXPECT_EQ(eval("1 ? 2 : 3 ? 4 : 5"), 2) << "nests to the right";
}

TEST_F(ArithmeticTest, VariablesAreReadWithoutADollar) {
	vars.values["i"] = 5;
	EXPECT_EQ(eval("i"), 5);
	EXPECT_EQ(eval("i + 1"), 6);
	EXPECT_EQ(eval("i * i"), 25);
}

TEST_F(ArithmeticTest, UnsetVariableIsZeroNotAnError) {
	// What makes `i=$((i+1))` work without initialising i.
	EXPECT_EQ(eval("nothing"), 0);
	EXPECT_EQ(eval("nothing + 1"), 1);
}

TEST_F(ArithmeticTest, AssignmentWritesBack) {
	EXPECT_EQ(eval("x = 7"), 7);
	EXPECT_EQ(vars.values["x"], 7);
}

TEST_F(ArithmeticTest, CompoundAssignment) {
	vars.values["i"] = 5;
	EXPECT_EQ(eval("i += 3"), 8);
	EXPECT_EQ(vars.values["i"], 8);
	EXPECT_EQ(eval("i *= 2"), 16);
	EXPECT_EQ(vars.values["i"], 16);
	EXPECT_EQ(eval("i <<= 1"), 32);
}

TEST_F(ArithmeticTest, AssignmentIsNotConfusedWithEquality) {
	vars.values["i"] = 5;
	EXPECT_EQ(eval("i == 5"), 1);
	EXPECT_EQ(vars.values["i"], 5) << "== must not assign";
}

TEST_F(ArithmeticTest, RelationalIsNotConfusedWithShift) {
	EXPECT_EQ(eval("1 < 2"), 1);
	EXPECT_EQ(eval("1 << 2"), 4);
	EXPECT_EQ(eval("8 > 2"), 1);
	EXPECT_EQ(eval("8 >> 2"), 2);
}

TEST_F(ArithmeticTest, DivisionByZeroIsAnErrorNotUndefinedBehaviour) {
	EXPECT_TRUE(fails("1 / 0"));
	EXPECT_TRUE(fails("1 % 0"));
}

TEST_F(ArithmeticTest, MalformedExpressionsFailRatherThanGuess) {
	EXPECT_TRUE(fails("1 +"));
	EXPECT_TRUE(fails("(1 + 2"));
	EXPECT_TRUE(fails("1 ? 2"));
	EXPECT_TRUE(fails("@"));
}

TEST_F(ArithmeticTest, EmptyExpressionFails) {
	EXPECT_TRUE(fails(""));
}

// Short-circuiting is about the operand NOT taken. The value was always right -
// `0 && (x=1)` answered 0 - and the assignment happened anyway, because the
// evaluator computes as it parses. See issue #56.

TEST_F(ArithmeticTest, LogicalAndDoesNotEvaluateTheOperandItSkips) {
	EXPECT_EQ(eval("0 && (x = 1)"), 0);
	EXPECT_FALSE(vars.assigned("x")) << "the skipped operand must not assign";
}

TEST_F(ArithmeticTest, LogicalOrDoesNotEvaluateTheOperandItSkips) {
	EXPECT_EQ(eval("1 || (x = 1)"), 1);
	EXPECT_FALSE(vars.assigned("x")) << "the skipped operand must not assign";
}

TEST_F(ArithmeticTest, ConditionalEvaluatesOnlyTheBranchItTakes) {
	EXPECT_EQ(eval("1 ? (a = 2) : (b = 3)"), 2);
	EXPECT_EQ(vars.values["a"], 2);
	EXPECT_FALSE(vars.assigned("b"));

	EXPECT_EQ(eval("0 ? (c = 2) : (d = 3)"), 3);
	EXPECT_FALSE(vars.assigned("c"));
	EXPECT_EQ(vars.values["d"], 3);
}

TEST_F(ArithmeticTest, AnOperandThatIsReachedStillAssigns) {
	// The other half of the fix, and the half a suppression that overreached
	// would break: `$((x = 1))` is POSIX, and these operators depend on it.
	EXPECT_EQ(eval("1 && (x = 1)"), 1);
	EXPECT_EQ(vars.values["x"], 1);
	EXPECT_EQ(eval("0 || (y = 2)"), 1);
	EXPECT_EQ(vars.values["y"], 2);
	EXPECT_EQ(eval("z = 3"), 3);
	EXPECT_EQ(vars.values["z"], 3);
	EXPECT_EQ(eval("z += 4"), 7);
	EXPECT_EQ(vars.values["z"], 7);
}

TEST_F(ArithmeticTest, SkippingNestsRatherThanResuming) {
	// The inner `||` would take its right operand on its own account. The outer
	// `&&` skipped the whole thing, so nothing inside it runs.
	EXPECT_EQ(eval("0 && (0 || (x = 1))"), 0);
	EXPECT_FALSE(vars.assigned("x"));
	EXPECT_EQ(eval("1 || (1 ? (y = 1) : (z = 1))"), 1);
	EXPECT_FALSE(vars.assigned("y"));
	EXPECT_FALSE(vars.assigned("z"));
}

TEST_F(ArithmeticTest, SkippingEndsWhereTheSkippedOperandEnds) {
	// `0 && (x=1)` is 0, so `||` does reach its right operand. A suppression that
	// leaked past the operand it was for would lose the assignment to y.
	EXPECT_EQ(eval("0 && (x = 1) || (y = 2)"), 1);
	EXPECT_FALSE(vars.assigned("x"));
	EXPECT_EQ(vars.values["y"], 2);
}

TEST_F(ArithmeticTest, ASkippedOperandIsStillParsed) {
	// Not evaluating it is not the same as not reading it: a malformed operand is
	// a malformed expression however the condition came out, and the parse is the
	// only pass that can say so.
	EXPECT_TRUE(fails("0 && +"));
	EXPECT_TRUE(fails("1 || (1"));
	EXPECT_TRUE(fails("1 ? 2 :"));
	EXPECT_TRUE(fails("0 && @"));
	// And it is consumed to its end, or the text left over would itself fail.
	EXPECT_EQ(eval("0 && 1 / 0 * 2 + 3"), 0);
}

TEST_F(ArithmeticTest, DivisionByZeroInASkippedOperandIsNotAnError) {
	EXPECT_EQ(eval("0 && 1 / 0"), 0);
	EXPECT_EQ(eval("1 || 1 % 0"), 1);
	EXPECT_EQ(eval("1 ? 2 : 1 / 0"), 2);
	vars.values["i"] = 4;
	EXPECT_EQ(eval("0 && (i /= 0)"), 0);
	EXPECT_EQ(vars.values["i"], 4);
}

TEST_F(ArithmeticTest, ASkippedReadIsNotAReadForNounset) {
	// unset_name is what the caller turns into a `set -u` error. An operand that
	// was never evaluated read nothing, so there is nothing to report.
	EXPECT_EQ(evaluate("missing", vars).unset_name, "missing");
	EXPECT_TRUE(evaluate("0 && missing", vars).unset_name.empty());
	EXPECT_TRUE(evaluate("1 || missing", vars).unset_name.empty());
	EXPECT_TRUE(evaluate("1 ? 0 : missing", vars).unset_name.empty());
}

TEST_F(ArithmeticTest, ASkippedAssignmentCannotBeRefused) {
	// A readonly variable refuses the write, and POSIX makes that fatal. An
	// assignment that never happened cannot be refused.
	vars.refusing.insert("r");
	EXPECT_TRUE(evaluate("r = 1", vars).assignment_refused);
	const auto skipped = evaluate("0 && (r = 1)", vars);
	EXPECT_TRUE(skipped.ok) << (skipped.error ? skipped.error : "");
	EXPECT_FALSE(skipped.assignment_refused);
	EXPECT_EQ(skipped.value, 0);
}
