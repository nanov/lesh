#include "runtime/expander.h"

#include "syntax/parser.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

using namespace lesh::runtime;
using namespace lesh::syntax;

namespace {

// A fake shell state. The expander depends on a port rather than a concrete
// state type precisely so this is possible - #12 has not designed the real one
// yet, and this ticket did not have to wait for it.
class FakeParams final : public parameter_source {
public:
	std::map<std::string, std::string> vars;
	std::string home = "/home/tester";
	std::string separators = " \t\n";

	bool lookup(std::string_view name, std::string_view& value) const override {
		const auto it = vars.find(std::string(name));
		if (it == vars.end())
			return false;
		value = it->second;
		return true;
	}
	std::string_view home_directory() const override { return home; }
	std::string_view ifs() const override { return separators; }
};

// A runner that records what it was asked to execute, so tests can assert the
// expander asked rather than assuming.
class FakeRunner final : public command_runner {
public:
	std::vector<std::string> asked;
	std::string reply;

	bool run_and_capture(std::string_view code, lesh::arena_array<char>& out) override {
		asked.emplace_back(code);
		for (const char c : reply)
			out.push(c);
		return true;
	}
};

class ExpanderTest : public ::testing::Test {
protected:
	lesh::buffer_pool pool{1024 * 64};
	FakeParams params;

	// Expands the first word of a command and returns its fields.
	std::vector<std::string> expand(std::string_view src, command_runner* runner = nullptr,
	                                expansion_status* status_out = nullptr) {
		const tree t = parse(pool, src);
		const node_index cmd = t.child_of(t[t.root()], 0);
		expander ex{pool, params, runner};
		lesh::arena_array<std::string_view> fields{pool, 8};

		expansion_status last = expansion_status::ok;
		for (uint32_t i = 0; i < t[cmd].children_count; ++i) {
			const node_index child = t.child_of(t[cmd], i);
			if (t[child].kind != node_kind::word)
				continue;
			const expansion_status s = ex.expand_word(t, child, fields);
			if (s != expansion_status::ok)
				last = s;
		}
		if (status_out != nullptr)
			*status_out = last;

		std::vector<std::string> out;
		for (const auto& f : fields)
			out.emplace_back(f);
		return out;
	}
};

} // namespace

TEST_F(ExpanderTest, LiteralWordsExpandToThemselves) {
	EXPECT_EQ(expand("echo hello world"),
	          (std::vector<std::string>{"echo", "hello", "world"}));
}

TEST_F(ExpanderTest, LiteralWordsAreZeroCopyViewsIntoTheSource) {
	// The point of the lexer's flag_literal: most words need no work at all, and
	// paying an allocation and a copy for them would defeat the whole constraint.
	const std::string src = "echo hello";
	const tree t = parse(pool, src);
	const node_index cmd = t.child_of(t[t.root()], 0);
	expander ex{pool, params};
	lesh::arena_array<std::string_view> fields{pool, 4};
	ex.expand_word(t, t.child_of(t[cmd], 1), fields);

	ASSERT_EQ(fields.size(), 1u);
	const char* source_begin = t.source().data();
	EXPECT_GE(fields[0].data(), source_begin);
	EXPECT_LT(fields[0].data(), source_begin + t.source().size())
		<< "a literal field should point into the original source, not a copy";
}

TEST_F(ExpanderTest, ParameterExpansionSubstitutesTheValue) {
	params.vars["NAME"] = "world";
	EXPECT_EQ(expand("echo $NAME"), (std::vector<std::string>{"echo", "world"}));
	EXPECT_EQ(expand("echo ${NAME}"), (std::vector<std::string>{"echo", "world"}));
}

TEST_F(ExpanderTest, ParameterWithPrefixAndSuffix) {
	// The defect that started this whole effort: `echo a$HOME-b` printed just "a".
	params.vars["X"] = "MID";
	EXPECT_EQ(expand("echo a${X}b"), (std::vector<std::string>{"echo", "aMIDb"}));
	EXPECT_EQ(expand("echo a$X-b"), (std::vector<std::string>{"echo", "aMID-b"}));
	EXPECT_EQ(expand("echo $X/sub"), (std::vector<std::string>{"echo", "MID/sub"}));
}

TEST_F(ExpanderTest, TwoParametersInOneWordConcatenate) {
	// The other half of that defect: the second expansion used to overwrite the
	// first rather than append to it.
	params.vars["A"] = "one";
	params.vars["B"] = "two";
	EXPECT_EQ(expand("echo $A$B"), (std::vector<std::string>{"echo", "onetwo"}));
	EXPECT_EQ(expand("echo ${A}x${B}"), (std::vector<std::string>{"echo", "onextwo"}));
}

TEST_F(ExpanderTest, UnsetParameterExpandsToNothingAtAll) {
	// POSIX: an unquoted unset parameter produces ZERO fields, not one empty one.
	// `echo $nope` passes no argument.
	EXPECT_EQ(expand("echo $nope"), (std::vector<std::string>{"echo"}));
}

TEST_F(ExpanderTest, QuotedUnsetParameterStillProducesAnEmptyField) {
	// ...but quoted, it produces one empty field. This is the distinction that
	// makes `[ -z "$x" ]` work and `[ -z $x ]` a syntax error.
	EXPECT_EQ(expand("echo \"$nope\""), (std::vector<std::string>{"echo", ""}));
}

TEST_F(ExpanderTest, UnquotedExpansionIsFieldSplit) {
	params.vars["LIST"] = "a b c";
	EXPECT_EQ(expand("echo $LIST"), (std::vector<std::string>{"echo", "a", "b", "c"}));
}

TEST_F(ExpanderTest, QuotedExpansionIsNotFieldSplit) {
	params.vars["LIST"] = "a b c";
	EXPECT_EQ(expand("echo \"$LIST\""), (std::vector<std::string>{"echo", "a b c"}));
}

TEST_F(ExpanderTest, LiteralBlanksAreNeverSplitByExpansion) {
	// Only the RESULT of an expansion is split. Literal text is already words.
	params.vars["X"] = "v";
	EXPECT_EQ(expand("echo a$X"), (std::vector<std::string>{"echo", "av"}));
}

TEST_F(ExpanderTest, FieldSplittingHonoursIfs) {
	params.separators = ":";
	params.vars["P"] = "a:b:c";
	EXPECT_EQ(expand("echo $P"), (std::vector<std::string>{"echo", "a", "b", "c"}));
}

TEST_F(ExpanderTest, SingleQuotesSuppressEverything) {
	params.vars["X"] = "value";
	EXPECT_EQ(expand("echo '$X'"), (std::vector<std::string>{"echo", "$X"}));
	EXPECT_EQ(expand("echo 'a b'"), (std::vector<std::string>{"echo", "a b"}));
}

TEST_F(ExpanderTest, DoubleQuotesExpandButDoNotSplit) {
	params.vars["X"] = "a b";
	EXPECT_EQ(expand("echo \"pre $X post\""),
	          (std::vector<std::string>{"echo", "pre a b post"}));
}

TEST_F(ExpanderTest, QuoteRemovalHappens) {
	EXPECT_EQ(expand("echo \"quoted\""), (std::vector<std::string>{"echo", "quoted"}));
	EXPECT_EQ(expand("echo 'quoted'"), (std::vector<std::string>{"echo", "quoted"}));
}

TEST_F(ExpanderTest, EmptyQuotesStillProduceAField) {
	EXPECT_EQ(expand("echo ''"), (std::vector<std::string>{"echo", ""}));
	EXPECT_EQ(expand("echo \"\""), (std::vector<std::string>{"echo", ""}));
}

TEST_F(ExpanderTest, BackslashEscapesAreRemoved) {
	EXPECT_EQ(expand("echo a\\ b"), (std::vector<std::string>{"echo", "a b"}));
}

TEST_F(ExpanderTest, TildeExpandsToHome) {
	EXPECT_EQ(expand("echo ~"), (std::vector<std::string>{"echo", "/home/tester"}));
}

// --- the port that breaks the cycle ------------------------------------------

TEST_F(ExpanderTest, CommandSubstitutionGoesThroughTheRunner) {
	FakeRunner runner;
	runner.reply = "captured\n";
	const auto fields = expand("echo $(inner cmd)", &runner);
	EXPECT_EQ(fields, (std::vector<std::string>{"echo", "captured"}));
	ASSERT_EQ(runner.asked.size(), 1u);
	EXPECT_EQ(runner.asked[0], "inner cmd");
}

TEST_F(ExpanderTest, CommandSubstitutionStripsTrailingNewlines) {
	FakeRunner runner;
	runner.reply = "out\n\n\n";
	EXPECT_EQ(expand("echo $(x)", &runner), (std::vector<std::string>{"echo", "out"}));
}

TEST_F(ExpanderTest, CommandSubstitutionWithoutARunnerIsRefusedNotExecuted) {
	// Completion's mode. Expanding `$(...)` merely to offer a suggestion would run
	// arbitrary commands as a side effect of typing. With no runner supplied there
	// is nothing that COULD execute - it is prevented by construction rather than
	// by remembering to check a flag.
	expansion_status status = expansion_status::ok;
	const auto fields = expand("echo $(anything)", nullptr, &status);
	EXPECT_EQ(status, expansion_status::command_substitution_unavailable);
	EXPECT_EQ(fields, (std::vector<std::string>{"echo"}));
}

TEST_F(ExpanderTest, CommandSubstitutionResultIsFieldSplitWhenUnquoted) {
	FakeRunner runner;
	runner.reply = "a b";
	EXPECT_EQ(expand("echo $(x)", &runner), (std::vector<std::string>{"echo", "a", "b"}));
}

TEST_F(ExpanderTest, CommandSubstitutionResultIsNotSplitWhenQuoted) {
	FakeRunner runner;
	runner.reply = "a b";
	EXPECT_EQ(expand("echo \"$(x)\"", &runner), (std::vector<std::string>{"echo", "a b"}));
}
