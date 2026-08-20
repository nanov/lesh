#include "runtime/arithmetic.h"

#include <gtest/gtest.h>

#include <map>
#include <string>

using namespace lesh::runtime;

namespace {

class FakeVars final : public arithmetic_variables {
public:
	std::map<std::string, int64_t> values;
	int64_t get(std::string_view name) const override {
		const auto it = values.find(std::string(name));
		return it == values.end() ? 0 : it->second;
	}
	void set(std::string_view name, int64_t value) override {
		values[std::string(name)] = value;
	}
	bool defined(std::string_view name) const override {
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
