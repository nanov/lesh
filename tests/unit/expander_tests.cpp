#include "runtime/expander.h"

#include "syntax/parser.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <tuple>
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

	// `~user`. A fake table, so a test can name a user that exists on no machine -
	// which is the only way to assert what a MISS does without depending on the
	// password database of whoever runs the suite.
	std::map<std::string, std::string> user_homes;
	bool home_directory_of(std::string_view user, std::string_view& out) const override {
		const auto it = user_homes.find(std::string(user));
		if (it == user_homes.end())
			return false;
		out = it->second;
		return true;
	}
	std::string_view ifs() const override { return separators; }

	// Special and positional parameters.
	std::vector<std::string> args;
	int status = 0;

	int last_status_value() const override { return status; }
	int process_id_value() const override { return 4242; }
	size_t positional_count() const override { return args.size(); }
	bool positional_at(size_t index, std::string_view& out) const override {
		if (index == 0 || index > args.size())
			return false;
		out = args[index - 1];
		return true;
	}
	std::string_view script_name_value() const override { return "lesh"; }

	std::string flags;
	std::string_view option_flags() const override { return flags; }
};

// Records ${x=default} assignments so tests can assert they happened.
class FakeAssigner final : public parameter_assigner {
public:
	std::map<std::string, std::string> assigned;
	bool assign_parameter(std::string_view name, std::string_view value) override {
		assigned[std::string(name)] = std::string(value);
		return true;
	}
};

// A runner that records what it was asked to execute, so tests can assert the
// expander asked rather than assuming.
//
// `answer` is what it reports back. The default is `ok`, which is every existing
// test; the other two are what the real runner says when the body will not parse
// or when there is no process to run it in, and the expander has to tell all
// three apart (#57).
class FakeRunner final : public command_runner {
public:
	std::vector<std::string> asked;
	std::string reply;
	substitution_result answer = substitution_result::ok;

	substitution_result run_and_capture(std::string_view code,
	                                    lesh::arena_array<char>& out) override {
		asked.emplace_back(code);
		if (answer != substitution_result::ok)
			return answer;
		for (const char c : reply)
			out.push(c);
		return answer;
	}
};

// The mutable state arithmetic needs. Separate from FakeParams because
// arithmetic ASSIGNS, and the expander refuses to evaluate at all without it -
// which is completion's mode and a case these tests have to be able to pick.
class FakeVars final : public arithmetic_variables {
public:
	std::map<std::string, int64_t> values;
	bool refuse = false;

	int64_t get(std::string_view name) const override {
		const auto it = values.find(std::string(name));
		return it == values.end() ? 0 : it->second;
	}
	bool set(std::string_view name, int64_t value) override {
		if (refuse)
			return false;
		values[std::string(name)] = value;
		return true;
	}
	bool defined(std::string_view name) const override {
		return values.count(std::string(name)) != 0;
	}
};

class ExpanderTest : public ::testing::Test {
protected:
	lesh::buffer_pool pool{1024 * 64};
	FakeParams params;

	// Expands the first word of a command and returns its fields.
	//
	// `fatal_out` reports expander::fatal_error(), which is a SEPARATE channel from
	// the returned status and the only one a defect inside a parameter default can
	// travel on: expand_value returns a value, so it has no status to
	// hand back to expand_text. It is also the channel the executor acts on.
	//
	// `vars` supplies the mutable state arithmetic needs; nullptr is completion's
	// mode, where arithmetic is not evaluated at all.
	std::vector<std::string> expand(std::string_view src, command_runner* runner = nullptr,
	                                expansion_status* status_out = nullptr,
	                                bool* fatal_out = nullptr,
	                                arithmetic_variables* vars = nullptr) {
		const tree t = parse(pool, src);
		const node_index cmd = t.child_of(t[t.root()], 0);
		expander ex{pool, params, runner, true, vars};
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
		if (fatal_out != nullptr)
			*fatal_out = ex.fatal_error();

		std::vector<std::string> out;
		for (const auto& f : fields)
			out.emplace_back(f);
		return out;
	}

	// Expands the patterns of the first item of a `case` clause.
	//
	// Goes through the same expand_word() every other word takes, deliberately:
	// what makes a case pattern a pattern is the ROLE the parser recorded on the
	// node, so a helper that passed the property itself would assert nothing about
	// the mechanism under test.
	std::vector<std::string> patterns(std::string_view src) {
		const tree t = parse(pool, src);
		const node_index clause = t.child_of(t[t.root()], 0);
		const node& item = t[t.child_of(t[clause], 1)];
		expander ex{pool, params, nullptr, /*glob_enabled=*/true};
		lesh::arena_array<std::string_view> fields{pool, 8};
		for (uint32_t i = 0; i < item.aux; ++i)
			std::ignore = ex.expand_word(t, t.child_of(item, i), fields);

		std::vector<std::string> out;
		for (const auto& f : fields)
			out.emplace_back(f);
		return out;
	}

	// Expands the SUBJECT of a `case` clause - the word it matches against.
	std::vector<std::string> subject(std::string_view src) {
		const tree t = parse(pool, src);
		const node_index clause = t.child_of(t[t.root()], 0);
		expander ex{pool, params, nullptr, /*glob_enabled=*/true};
		lesh::arena_array<std::string_view> fields{pool, 4};
		std::ignore = ex.expand_word(t, t.child_of(t[clause], 0), fields);

		std::vector<std::string> out;
		for (const auto& f : fields)
			out.emplace_back(f);
		return out;
	}

	// Expands text into a single VALUE in one of the three value contexts. The
	// three differ in their quoting rules, which is the whole reason
	// value_context exists (#42).
	std::string value(std::string_view text, value_context context) {
		expander ex{pool, params};
		return std::string(ex.expand_value(text, context));
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

// POSIX 2.6.5 gives a separator a shape - IFS white space, then at most one
// non-white-space IFS character, then more IFS white space - and the consequence
// is EMPTY fields, which the previous "drop every IFS byte" loop could not
// produce at all. Ten of fsplit-p.tst's twelve failures were this one rule (#42).
TEST_F(ExpanderTest, SuccessiveNonWhitespaceSeparatorsLeaveAnEmptyFieldBetweenThem) {
	params.separators = "-";
	params.vars["P"] = "1--2";
	EXPECT_EQ(expand("echo $P"), (std::vector<std::string>{"echo", "1", "", "2"}));
}

TEST_F(ExpanderTest, ALeadingNonWhitespaceSeparatorLeavesAnEmptyFieldBeforeIt) {
	params.separators = "-";
	params.vars["P"] = "-1";
	EXPECT_EQ(expand("echo $P"), (std::vector<std::string>{"echo", "", "1"}));
}

TEST_F(ExpanderTest, ALeadingWhitespaceSeparatorDoesNot) {
	// The asymmetry is the point: white space at the start of the input is not a
	// separator, so there is no field for it to end.
	params.separators = " ";
	params.vars["P"] = " 1";
	EXPECT_EQ(expand("echo $P"), (std::vector<std::string>{"echo", "1"}));
}

TEST_F(ExpanderTest, WhitespaceAroundANonWhitespaceSeparatorIsPartOfIt) {
	// ` - ` is ONE separator, not three, so this is two fields rather than four.
	params.separators = " -";
	params.vars["P"] = "1 - 2";
	EXPECT_EQ(expand("echo $P"), (std::vector<std::string>{"echo", "1", "2"}));
}

TEST_F(ExpanderTest, ASeparatorThatHasUsedItsSlotStartsANewOne) {
	// fsplit-p.tst's 'complex field splitting with successive non-whitespace IFS':
	// leading white space has no field to close, so the first `-` produces the
	// empty field and the second produces another.
	params.separators = " -";
	params.vars["P"] = "  --33";
	EXPECT_EQ(expand("echo $P"), (std::vector<std::string>{"echo", "", "", "33"}));
}

TEST_F(ExpanderTest, ATrailingSeparatorDoesNotAddAnEmptyLastField) {
	// "empty last field is ignored": one separator at the end yields nothing extra,
	// two yield one empty field. Achieved by never emitting a field until content
	// arrives, so the end of the word simply drops what is owed.
	params.separators = "-";
	params.vars["P"] = "1-";
	EXPECT_EQ(expand("echo $P"), (std::vector<std::string>{"echo", "1"}));
	params.vars["P"] = "1--";
	EXPECT_EQ(expand("echo $P"), (std::vector<std::string>{"echo", "1", ""}));
	params.vars["P"] = "-";
	EXPECT_EQ(expand("echo $P"), (std::vector<std::string>{"echo", ""}));
}

TEST_F(ExpanderTest, OneSeparatorMaySpanTwoExpansions) {
	// The separator state has to outlive a segment: `$a$b` is `1  2` here, which is
	// ONE separator and two fields. Resetting per segment made it three.
	params.separators = " ";
	params.vars["A"] = "1 ";
	params.vars["B"] = " 2";
	EXPECT_EQ(expand("echo $A$B"), (std::vector<std::string>{"echo", "1", "2"}));
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

// The three value contexts, which one `quoted` flag could not tell apart. It meant
// "no field splitting" and "double-quoted backslash rules" at once, so suppressing
// the first switched the second (#42).
TEST_F(ExpanderTest, AnAssignmentValueUsesUNQUOTEDBackslashRules) {
	// `x=\!` assigned `\!`, because the flag that turned field splitting off also
	// said "inside double quotes", where a backslash before `!` is literal.
	EXPECT_EQ(value("\\!", value_context::assignment), "!");
	EXPECT_EQ(value("a\\ b", value_context::assignment), "a b") << "and still one value";
}

TEST_F(ExpanderTest, ARedirectionOperandUsesUNQUOTEDBackslashRulesToo) {
	// `cat <\i'n'"0"` looked for a file called `\in0` - redir-p.tst:73.
	EXPECT_EQ(value("\\i'n'\"0\"", value_context::redirection_operand), "in0");
}

TEST_F(ExpanderTest, AHereDocumentBodyKeepsItsQuotes) {
	// POSIX 2.7.4: the body behaves as if double-quoted EXCEPT that `"` is not
	// special - so neither quote character is. Lexed as a word interior instead,
	// `it's` came out as `its` and `a"b` as `ab`: quote removal on text that never
	// had quotes.
	EXPECT_EQ(value("it's", value_context::here_document_body), "it's");
	EXPECT_EQ(value("a\"b\"c", value_context::here_document_body), "a\"b\"c");
	// The escapes that DO apply there, and the one that does not.
	EXPECT_EQ(value("\\$ \\\\ \\\" \\z", value_context::here_document_body), "$ \\ \\\" \\z");
}

TEST_F(ExpanderTest, AnUnquotedDollarAtInAValueJoinsRatherThanLosingEveryFieldButTheLast) {
	// `x=$@` assigned `c` for `set a b c`: the branch produced a field list into an
	// array the value path discards. dash joins on IFS, and so does `x="$@"`.
	params.args = {"a", "b", "c"};
	EXPECT_EQ(value("$@", value_context::assignment), "a b c");
	EXPECT_EQ(value("\"$@\"", value_context::assignment), "a b c");
	params.separators = ":";
	EXPECT_EQ(value("$@", value_context::assignment), "a:b:c");
}

TEST_F(ExpanderTest, AQuotedDollarAtInACommandWordStillMakesOneFieldPerParameter) {
	// The other half of the same distinction: inside double quotes in a command's
	// argument, field splitting is off while `"$@"` still yields a field each.
	params.args = {"a b", "c"};
	EXPECT_EQ(expand("echo \"$@\""), (std::vector<std::string>{"echo", "a b", "c"}));
}

// The argument of a ${...} operator is part of the WORD, so it is expanded in the
// context of the expansion rather than flattened to a value and appended.
TEST_F(ExpanderTest, ADefaultsArgumentIsExpandedInContext) {
	// Unquoted, so the backslash escapes anything and the escaped blank does not
	// separate while the bare one does. quote-p.tst's eleven 'backslashes in
	// substitution of expansion' cases are this.
	EXPECT_EQ(expand("echo ${u-\\!a b}"),
	          (std::vector<std::string>{"echo", "!a", "b"}));
	// Inside double quotes, the double-quote rules and no splitting at all.
	EXPECT_EQ(expand("echo \"${u-\\!a b}\""),
	          (std::vector<std::string>{"echo", "\\!a b"}));
}

TEST_F(ExpanderTest, ASingleQuoteInADefaultIsAQuoteOutsideDoubleQuotesAndAByteInside) {
	EXPECT_EQ(expand("echo ${u-a'b c'd}"),
	          (std::vector<std::string>{"echo", "ab cd"}));
	EXPECT_EQ(expand("echo \"${u-a'b}\""),
	          (std::vector<std::string>{"echo", "a'b"}));
}

TEST_F(ExpanderTest, AssignDefaultRemovesQuotesBeforeAssigningAndThenSplitsTheValue) {
	// The one operator that needs a value AND a substitution, and they are not the
	// same string: quote removal happens before the assignment, so the value holds
	// a literal blank where the word held `\ ` - and the substitution then splits
	// the VALUE, leading blank and all. quote-p.tst's 'quotes in substitution of
	// expansion ${a=b}': `${a=\ x}` substitutes `[x]` while `${a+\ x}` gives `[ x]`.
	FakeAssigner assigner;
	const tree t = parse(pool, "echo ${a=\\ x}");
	const node_index cmd = t.child_of(t[t.root()], 0);
	expander ex{pool, params, nullptr, true, nullptr, &assigner};
	lesh::arena_array<std::string_view> fields{pool, 4};
	ex.expand_word(t, t.child_of(t[cmd], 1), fields);
	ASSERT_EQ(fields.size(), 1u);
	EXPECT_EQ(std::string(fields[0]), "x") << "the substitution is split, so the blank goes";
	EXPECT_EQ(assigner.assigned["a"], " x") << "the value keeps it, quotes removed";
}

TEST_F(ExpanderTest, OnlyHashAndPercentHaveADoubledForm) {
	// `${u--x}` is the default `-x`, not a `--` operator. Testing every operator
	// for a repeated character ate the first byte of the argument and substituted
	// `x`, which is three of fsplit-p.tst's cases - their defaults all begin with a
	// hyphen.
	EXPECT_EQ(expand("echo ${u--x}"), (std::vector<std::string>{"echo", "-x"}));
	EXPECT_EQ(expand("echo ${u-=x}"), (std::vector<std::string>{"echo", "=x"}));
	params.vars["v"] = "aab";
	EXPECT_EQ(expand("echo ${v##a*}"), (std::vector<std::string>{"echo"}))
		<< "and ## is still the longest-prefix form: it consumes everything, and an "
		   "unquoted expansion that yields one empty field yields no field at all";
	EXPECT_EQ(expand("echo ${v#a*}"), (std::vector<std::string>{"echo", "ab"}));
}

// `"$@"` with no positional parameters is ZERO fields, quotes and all.
TEST_F(ExpanderTest, AnAbsentDollarAtInDoubleQuotesDoesNotEvenStartAField) {
	// The quotes normally start a field, which is why `echo ""` passes one empty
	// argument. POSIX exempts an absent `"$@"`, and forcing the field here printed
	// one empty one where dash, bash and zsh all print nothing (param-p.tst:560).
	EXPECT_EQ(expand("echo \"$@\""), (std::vector<std::string>{"echo"}));
	EXPECT_EQ(expand("echo \"$@\"\"$@\""), (std::vector<std::string>{"echo"}));
}

TEST_F(ExpanderTest, ButAnythingElseInThoseQuotesStillStartsIt) {
	// The line either side of the exemption. An empty VARIABLE is content and an
	// absent `$@` is not, so `"$null$@"` is one empty field and `"$@"` is none -
	// which is why this takes two flags rather than one.
	params.vars["null"] = "";
	EXPECT_EQ(expand("echo \"$null\"\"$@\""), (std::vector<std::string>{"echo", ""}));
	EXPECT_EQ(expand("echo \"=$@=\""), (std::vector<std::string>{"echo", "=="}));
	EXPECT_EQ(expand("echo \"\""), (std::vector<std::string>{"echo", ""}));
}

TEST_F(ExpanderTest, AssigningToAPositionalOrSpecialParameterIsRefusedNotIgnored) {
	// A stub that succeeded: `${1:=}` reported nothing and quietly substituted the
	// default. dash says `1: bad variable name` and exits 2 (param-p.tst:198, :202).
	FakeAssigner assigner;
	for (const std::string_view form : {"${1:=x}", "${*:=x}", "${@:=x}", "${1=x}"}) {
		const std::string src = std::string("echo ") + std::string(form);
		const tree t = parse(pool, src);
		const node_index cmd = t.child_of(t[t.root()], 0);
		expander ex{pool, params, nullptr, true, nullptr, &assigner};
		lesh::arena_array<std::string_view> fields{pool, 4};
		ex.expand_word(t, t.child_of(t[cmd], 1), fields);
		EXPECT_TRUE(ex.fatal_error()) << form;
	}
	EXPECT_TRUE(assigner.assigned.empty()) << "and nothing was assigned";
}

TEST_F(ExpanderTest, ButOnlyWhenTheAssignmentWouldActuallyHappen) {
	// dash's rule rather than an approximation of it: `set a; echo ${1=x}`
	// substitutes `a` at status zero, because a set parameter needs no assignment.
	params.args = {"a"};
	bool fatal = false;
	EXPECT_EQ(expand("echo ${1=x}", nullptr, nullptr, &fatal),
	          (std::vector<std::string>{"echo", "a"}));
	EXPECT_FALSE(fatal);
}

TEST_F(ExpanderTest, DollarAtAndDollarStarAreAlwaysSet) {
	// `${@-unset}` is EMPTY in dash, not `unset`: only the colon forms treat an
	// empty `$@` as absent. Reported unset, `${@=x}` took the assignment path and
	// refused a name dash accepts.
	EXPECT_EQ(expand("echo \"${@-unset}\""), (std::vector<std::string>{"echo", ""}));
	EXPECT_EQ(expand("echo \"${@:-unset}\""),
	          (std::vector<std::string>{"echo", "unset"}));
}

TEST_F(ExpanderTest, TheLengthFormNeedsAnAmeAfterTheHash) {
	// POSIX 2.6.2's disambiguation. `${#+y}` read as a length gave the length of an
	// unset parameter called `+y` - zero - where dash prints `y`, and `${#?}` read
	// as an operator gave the count where dash gives the length of `$?`.
	params.args = {"a", "b"};
	EXPECT_EQ(expand("echo ${#+y}"), (std::vector<std::string>{"echo", "y"}));
	EXPECT_EQ(expand("echo ${#-y}"), (std::vector<std::string>{"echo", "2"}));
	EXPECT_EQ(expand("echo ${#?}"), (std::vector<std::string>{"echo", "1"}))
		<< "the length of $?, which is one digit";
	EXPECT_EQ(expand("echo ${#?X}"), (std::vector<std::string>{"echo", "2"}))
		<< "but with a message it is the count again";
	params.vars["v"] = "hello";
	EXPECT_EQ(expand("echo ${#v}"), (std::vector<std::string>{"echo", "5"}));
}

// A pattern keeps its quoting, as escapes. Removing the quotes made every quoted
// metacharacter a metacharacter again.
TEST_F(ExpanderTest, AQuotedMetacharacterInATrimPatternIsData) {
	params.vars["s"] = "***";
	EXPECT_EQ(expand("echo ${s#'*'}"), (std::vector<std::string>{"echo", "**"}))
		<< "one literal asterisk, not all three";
	EXPECT_EQ(expand("echo ${s##'*'}"), (std::vector<std::string>{"echo", "**"}));
	EXPECT_EQ(expand("echo ${s#\\*}"), (std::vector<std::string>{"echo", "**"}));
}

TEST_F(ExpanderTest, AnUnquotedMetacharacterInATrimPatternStillWildcards) {
	// The other half: `pattern` must not turn the pattern into a literal string.
	params.vars["a"] = "1-2-3-4";
	EXPECT_EQ(expand("echo ${a#*-}"), (std::vector<std::string>{"echo", "2-3-4"}));
	EXPECT_EQ(expand("echo ${a##*-}"), (std::vector<std::string>{"echo", "4"}));
	EXPECT_EQ(expand("echo \"${a#*1}\""), (std::vector<std::string>{"echo", "-2-3-4"}))
		<< "and the outer double quotes do not quote the pattern";
}

TEST_F(ExpanderTest, AnExpansionInAPatternIsAPatternUnlessItWasQuoted) {
	// param-p.tst's 'parameter expansion in embedded pattern': the value of `a` is
	// an asterisk, so unquoted it wildcards and quoted it is one literal asterisk
	// that `ab\bc` does not contain.
	params.vars["w"] = "ab\\bc";
	params.vars["a"] = "*";
	EXPECT_EQ(expand("echo ${w#${a}b}"), (std::vector<std::string>{"echo", "\\bc"}));
	EXPECT_EQ(expand("echo ${w#\"${a}b\"}"),
	          (std::vector<std::string>{"echo", "ab\\bc"}));
}

// A `case` pattern is the same PATTERN property, reached through the role the
// parser recorded rather than through a `#` operator. It was not reached at all:
// the pattern went through expand_word like a command argument, so quote removal
// threw the quoting away and every quoted metacharacter became a metacharacter
// again - `case '*ab' in '***')` matched.
TEST_F(ExpanderTest, AQuotedMetacharacterInACasePatternIsData) {
	EXPECT_EQ(patterns("case x in \\*\\*\\*|'***'|\"***\") :; esac"),
	          (std::vector<std::string>{"\\*\\*\\*", "\\*\\*\\*", "\\*\\*\\*"}))
		<< "all three quotings say the same thing to the matcher";
	EXPECT_EQ(patterns("case x in \\?\\?) :; esac"), (std::vector<std::string>{"\\?\\?"}));
	EXPECT_EQ(patterns("case x in '[['abc]) :; esac"),
	          (std::vector<std::string>{"\\[\\[abc]"}));
}

TEST_F(ExpanderTest, AnUnquotedMetacharacterInACasePatternStillWildcards) {
	// The other half, and the reason the escaping cannot simply be applied to the
	// whole pattern: `*)` has to stay a wildcard.
	EXPECT_EQ(patterns("case x in \\**) :; esac"), (std::vector<std::string>{"\\**"}));
	EXPECT_EQ(patterns("case x in a?c|[ab]*) :; esac"),
	          (std::vector<std::string>{"a?c", "[ab]*"}));
}

TEST_F(ExpanderTest, ABackslashArrivingFromAnExpansionInACasePatternIsSpecial) {
	// XCU 2.9.4: an unquoted backslash in the pattern is special even when it came
	// out of an expansion, so `$bs` wildcards nothing and escapes the `a`. Quoting
	// the expansion makes both backslashes data. Same rule append_value already
	// applies to `${w#${a}b}`.
	params.vars["bs"] = "\\a\\z";
	EXPECT_EQ(patterns("case x in $bs) :; esac"), (std::vector<std::string>{"\\a\\z"}));
	EXPECT_EQ(patterns("case x in \"$bs\") :; esac"),
	          (std::vector<std::string>{"\\\\a\\\\z"}));
}

TEST_F(ExpanderTest, ACasePatternIsOneFieldEvenWhenItExpandsToNothing) {
	// A command argument that expands to nothing is NO argument; a pattern that
	// expands to nothing is the EMPTY pattern, which matches an empty subject. The
	// executor skipped an item whose pattern list came back empty, so
	// `case $(true) in $(true))` never ran - case-p.tst's 'redirection on case
	// command', which is not about redirection at all.
	params.separators = " \t\n";
	EXPECT_EQ(patterns("case x in $nope) :; esac"), (std::vector<std::string>{""}));
	EXPECT_EQ(patterns("case x in \"\") :; esac"), (std::vector<std::string>{""}));
}

TEST_F(ExpanderTest, ACasePatternIsNotFieldSplit) {
	// POSIX subjects a case pattern to the expansions but not to field splitting,
	// so an IFS that appears in the value must not break the pattern in two - and
	// the second field would have been read as the item's SECOND pattern.
	params.vars["p"] = "a:b";
	params.separators = ":";
	EXPECT_EQ(patterns("case x in $p) :; esac"), (std::vector<std::string>{"a:b"}));
}

TEST_F(ExpanderTest, ACaseSubjectIsOneValueAndNotAPattern) {
	// POSIX subjects the word a `case` matches against to the expansions and NOT
	// to field splitting. It was split like a command argument, and the executor
	// compares against the first field - so `IFS=:; case a:b in 'a:b')` compared
	// the pattern against `a` and matched nothing, and a subject holding a single
	// blank expanded to no field at all.
	params.vars["v"] = "a:b";
	params.separators = ":";
	EXPECT_EQ(subject("case $v in x) :; esac"), (std::vector<std::string>{"a:b"}));

	params.separators = " \t\n";
	params.vars["blank"] = " ";
	EXPECT_EQ(subject("case $blank in x) :; esac"), (std::vector<std::string>{" "}));
	EXPECT_EQ(subject("case $nope in x) :; esac"), (std::vector<std::string>{""}))
		<< "and a subject that expands to nothing is the empty subject";

	// Text, not a pattern: a backslash the subject HOLDS is one of its bytes, and
	// escaping it would make the subject match patterns it does not equal.
	params.vars["bs"] = "\\a";
	EXPECT_EQ(subject("case \"$bs\" in x) :; esac"), (std::vector<std::string>{"\\a"}));
	EXPECT_EQ(subject("case '*' in x) :; esac"), (std::vector<std::string>{"*"}));
}

TEST_F(ExpanderTest, ABackslashEscapesABraceInsideBracesEvenInDoubleQuotes) {
	// The one byte double-quote rules do NOT cover, and they cannot: a `}` would end
	// the expansion, so `\}` is the only way to write a literal one there. dash
	// prints `a}b` for `"${a+a\}b}"` and lesh printed `a\}b` - the whole of
	// quote-p.tst's remaining three cases, one character each.
	params.vars["a"] = "set";
	EXPECT_EQ(expand("echo \"${a+a\\}b}\""),
	          (std::vector<std::string>{"echo", "a}b"}));
	EXPECT_EQ(expand("echo \"${a+\\{}\""), (std::vector<std::string>{"echo", "\\{"}))
		<< "and an opening brace is NOT special, so its backslash stays";
}

TEST_F(ExpanderTest, ABackquotedSubstitutionHasItsEscapesRemovedFirst) {
	// POSIX 2.6.3: a backslash inside backquotes keeps its literal meaning except
	// before `$`, a backquote or another backslash - so the body handed to the
	// parser is not the body as written. Unconditionally, not quote-aware:
	// `` `echoraw '\$y'` `` prints `$y`, so the escape inside SINGLE quotes was
	// removed too, before the body was ever parsed.
	FakeRunner runner;
	runner.reply = "out\n";
	expand("echo `a \\` b \\$c \\\\ \\z`", &runner);
	ASSERT_EQ(runner.asked.size(), 1u);
	EXPECT_EQ(runner.asked[0], "a ` b $c \\ \\z");
}

TEST_F(ExpanderTest, AndDoubleQuotesAddTheQuoteToThatSet) {
	// `"`echoraw \"1\"`"` prints `1`: the `\"` is in the source only to stop the
	// quoted string ending, so it comes off. Outside double quotes it does not -
	// `` `echoraw \"1\"` `` prints `"1"` there.
	FakeRunner runner;
	runner.reply = "out\n";
	expand("echo \"`a \\\"1\\\"`\"", &runner);
	ASSERT_EQ(runner.asked.size(), 1u);
	EXPECT_EQ(runner.asked[0], "a \"1\"");

	FakeRunner bare;
	bare.reply = "out\n";
	expand("echo `a \\\"1\\\"`", &bare);
	ASSERT_EQ(bare.asked.size(), 1u);
	EXPECT_EQ(bare.asked[0], "a \\\"1\\\"") << "outside quotes the backslash stays";
}

TEST_F(ExpanderTest, TildeExpandsToHome) {
	EXPECT_EQ(expand("echo ~"), (std::vector<std::string>{"echo", "/home/tester"}));
}

TEST_F(ExpanderTest, ANamedTildeExpandsToThatUsersHome) {
	params.user_homes["known"] = "/home/known";
	EXPECT_EQ(expand("echo ~known"), (std::vector<std::string>{"echo", "/home/known"}));
	EXPECT_EQ(expand("echo ~known/sub"),
	          (std::vector<std::string>{"echo", "/home/known/sub"}));
}

TEST_F(ExpanderTest, AnUnknownUserLeavesTheWordAlone) {
	// dash prints `~nosuchuser` at status zero rather than reporting anything, and
	// POSIX leaves the case unspecified. Reporting would break `mkdir ~tmp` on a
	// machine where the shell is right and the user is not.
	EXPECT_EQ(expand("echo ~nobodyhere"),
	          (std::vector<std::string>{"echo", "~nobodyhere"}));
}

TEST_F(ExpanderTest, AQuotedNameIsNotALoginNameButItsQuotesStillCome0ff) {
	// POSIX 2.6.1: if ANY character of the tilde-prefix is quoted, none of them is
	// part of a login name - so the tilde stays and quote removal still runs. dash
	// prints `~known` for all three, and lesh printed `~"known"` with the quotes.
	params.user_homes["known"] = "/home/known";
	EXPECT_EQ(expand("echo ~\"known\""), (std::vector<std::string>{"echo", "~known"}));
	EXPECT_EQ(expand("echo ~'known'"), (std::vector<std::string>{"echo", "~known"}));
	EXPECT_EQ(expand("echo ~kno\\wn"), (std::vector<std::string>{"echo", "~known"}));
	EXPECT_EQ(expand("echo ~\\/"), (std::vector<std::string>{"echo", "~/"}))
		<< "tilde-p.tst's `~\\/`, which kept its backslash";
}

TEST_F(ExpanderTest, ATildeIsEligibleAfterAnUnquotedColonInAnAssignmentValueOnly) {
	// POSIX confines the after-colon rule to assignments, which is why it is a lex
	// MODE rather than a property of the word: `PATH=~/bin:~/sbin` expands both,
	// and `echo ~:~` expands only the first.
	params.user_homes["known"] = "/home/known";
	EXPECT_EQ(value("a:~:~known", value_context::assignment),
	          "a:/home/tester:/home/known");
	EXPECT_EQ(value("a:~", value_context::redirection_operand), "a:~")
		<< "not in a redirection operand: dash does not do it there either";
	EXPECT_EQ(expand("echo a:~"), (std::vector<std::string>{"echo", "a:~"}))
		<< "and not in an ordinary word";
}

TEST_F(ExpanderTest, AQuotedColonDoesNotMakeTheFollowingTildeEligible) {
	// `x=':'~` assigns `:~` in dash: the colon has to be UNQUOTED for the tilde
	// after it to be a tilde-prefix.
	EXPECT_EQ(value("':'~", value_context::assignment), ":~");
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

// --- malformed nesting the word scan cannot see (#48) ------------------------

TEST_F(ExpanderTest, UnterminatedArithmeticInsideAParameterDefaultIsRefused) {
	// The whole of #48. `${x-$((1}` is well formed AT THE COMMAND LEVEL - the word
	// scan counts braces, the `}` closes the parameter expansion - so no defect
	// reaches the parser and the damage is entirely inside the expansion. The
	// arithmetic case then strips `$((` and `))` from a segment too short to hold
	// them, so expand_text and expand_to_value re-expanded the same four
	// bytes until the stack ran out. Before the check below this test did not fail,
	// it CRASHED: ASan reported a stack overflow alternating between the two.
	bool fatal = false;
	const auto fields = expand("echo ${x-$((1}", nullptr, nullptr, &fatal);
	EXPECT_TRUE(fatal);
	EXPECT_EQ(fields, (std::vector<std::string>{"echo"}));
}

TEST_F(ExpanderTest, EveryParameterOperatorRefusesAnUnterminatedArithmetic) {
	// Not one shape but nine: every operator that EXPANDS its argument reached the
	// same recursion, and each was confirmed to crash on its own rather than
	// trusted to share a code path with `-`.
	//
	// Which operators need x SET is the point of the split. `${x-d}` expands its
	// default only when x is absent, while `${x+d}` and the trim forms expand
	// theirs only when it is present - so an argument the operator never reaches is
	// never lexed, and never refused. That is the visible edge of reporting this at
	// expansion time rather than at parse time, and it is asserted below.
	for (const std::string_view form : {"${x-$((1}", "${x:-$((1}", "${x=$((1}",
	                                    "${x:=$((1}", "${x?$((1}", "${x:?$((1}"}) {
		params.vars.erase("x");
		bool fatal = false;
		const std::string src = std::string("echo ") + std::string(form);
		const auto fields = expand(src, nullptr, nullptr, &fatal);
		EXPECT_TRUE(fatal) << form;
		EXPECT_EQ(fields, (std::vector<std::string>{"echo"})) << form;
	}

	// Only the diagnostic is asserted for these: the trim forms go on to trim with
	// the pattern they could not expand, so the field they leave behind is whatever
	// that produced. It never reaches a command - fatal_error() stops it first -
	// and asserting it would pin a value nothing reads.
	for (const std::string_view form : {"${x+$((1}", "${x:+$((1}", "${x#$((1}",
	                                    "${x%%$((1}"}) {
		params.vars["x"] = "value";
		bool fatal = false;
		const std::string src = std::string("echo ") + std::string(form);
		expand(src, nullptr, nullptr, &fatal);
		EXPECT_TRUE(fatal) << form;
	}
}

TEST_F(ExpanderTest, AnArgumentTheOperatorNeverReachesIsRefusedTOO) {
	// #48 recorded this as a divergence: dash reports `${x-$((1}` at PARSE time and
	// so refuses it whether or not x is set, while lesh's word scan counted braces,
	// could not see inside `${...}`, and only found the defect when the default was
	// expanded - which a set x never does. The scan now follows the nesting, so the
	// word itself carries the defect and the command is refused before anything
	// runs. Kept as a test because it is the case that closes the divergence.
	params.vars["x"] = "value";
	bool fatal = false;
	EXPECT_EQ(expand("echo ${x-$((1}", nullptr, nullptr, &fatal),
	          (std::vector<std::string>{"echo"}));
	EXPECT_TRUE(fatal);
}

TEST_F(ExpanderTest, UnterminatedCommandSubstitutionInsideADefaultIsRefused) {
	// This one never recursed - substitution_body returns nothing for a segment too
	// short - so it expanded to an empty field at status zero, which is the silent
	// half of the same bug: dash reports a syntax error.
	bool fatal = false;
	EXPECT_EQ(expand("echo ${x-$(}", nullptr, nullptr, &fatal),
	          (std::vector<std::string>{"echo"}));
	EXPECT_TRUE(fatal);
}

TEST_F(ExpanderTest, UnterminatedQuoteInsideADefaultIsNotRefused) {
	// The DELIBERATE edge of the check, asserted so it cannot be widened by
	// accident. Whether a quote inside `${x-...}` is a quote at all depends on the
	// double-quote context POSIX 2.6.2 gives it: inside double quotes it is an
	// ordinary byte, and dash prints it - `dash -c 'echo "${x-'"'"'}"'` writes one
	// single quote at status zero. Only the constructs whose delimiters the
	// expander STRIPS are refused.
	//
	// The expected value used to be the empty string, because the default was
	// re-lexed without the enclosing context and the quote ate the rest (#42). The
	// default is now expanded in place, so the byte survives.
	bool fatal = false;
	EXPECT_EQ(expand("echo \"${x-'}\"", nullptr, nullptr, &fatal),
	          (std::vector<std::string>{"echo", "'"}));
	EXPECT_FALSE(fatal);
}

TEST(ExpanderDepthTest, DeeplyNestedExpansionIsRefusedRatherThanExhaustingTheStack) {
	// Well formed, and still unbounded. Refusing an unterminated construct bounds
	// the recursion by the length of the input, which is not a bound: every level
	// of `${x-...}` re-enters expand_text, so
	// nesting in the input is nesting on the C++ stack. Measured on the debug build
	// before the limit: 1500 levels expanded, 2000 overflowed the stack. dash gets
	// further on the same input and then does the same thing - `hi` at 16000,
	// SIGSEGV at 18000.
	//
	// Its own pool: 5000 levels of source is 25 KB, and the fixture's is sized for
	// one command line.
	lesh::buffer_pool pool{1024 * 1024};
	FakeParams params;
	expander ex{pool, params};

	std::string text;
	for (int i = 0; i < 5000; ++i)
		text += "${x-";
	text += "hi";
	text.append(5000, '}');

	EXPECT_TRUE(ex.expand_value(text, value_context::assignment).empty());
	EXPECT_TRUE(ex.fatal_error());
}

// --- an expansion that cannot be performed is an ERROR, not an empty field (#39)

TEST_F(ExpanderTest, ArithmeticThatWillNotEvaluateIsAFatalExpansionError) {
	// The line #39 named. The evaluator already refused every one of these -
	// `division by zero`, `unexpected end of expression` - and the refusal was
	// turned into `unsupported_construct` above it, which nothing treats as fatal.
	// So `echo $((1/0))` printed an empty line and reported SUCCESS, which is a
	// wrong answer rather than a missing feature. dash reports and exits 2.
	FakeVars vars;
	for (const std::string_view form : {"$((1/0))", "$((--))", "$((1%0))",
	                                    "$(( 0 && + ))", "$((1+))"}) {
		bool fatal = false;
		expansion_status status = expansion_status::ok;
		const std::string src = std::string("echo ") + std::string(form);
		const auto fields = expand(src, nullptr, &status, &fatal, &vars);
		EXPECT_TRUE(fatal) << form;
		EXPECT_EQ(status, expansion_status::expansion_error) << form;
		EXPECT_EQ(fields, (std::vector<std::string>{"echo"})) << form;
	}
}

TEST_F(ExpanderTest, AnErrorAndAnUnimplementedConstructAreNoLongerTheSameAnswer) {
	// The half of #39 that is about the enum rather than about the flag.
	// `unsupported_construct` was standing for BOTH "lesh has not built that" and
	// "this expansion failed", and collapsing the two is how a real error came to
	// travel on a value nothing acts on. Completion is the case that keeps the
	// distinction honest: with no mutable state there is nothing to evaluate
	// against, so `$((1/0))` is genuinely unsupported THERE and must not be fatal -
	// a line editor drawing a suggestion may not kill the shell.
	bool fatal = false;
	expansion_status status = expansion_status::ok;
	expand("echo $((1/0))", nullptr, &status, &fatal, nullptr);
	EXPECT_FALSE(fatal) << "completion's mode expands without the power to fail";
	EXPECT_EQ(status, expansion_status::unsupported_construct);
}

TEST_F(ExpanderTest, ArithmeticThatEvaluatesIsUntouchedByAnyOfThis) {
	// The line the check must not cross, asserted so it cannot widen by accident.
	FakeVars vars;
	vars.values["i"] = 7;
	bool fatal = false;
	expansion_status status = expansion_status::ok;
	EXPECT_EQ(expand("echo $((6/2)) $((i+1)) $((0 && 1/0))", nullptr, &status, &fatal, &vars),
	          (std::vector<std::string>{"echo", "3", "8", "0"}));
	EXPECT_FALSE(fatal);
	EXPECT_EQ(status, expansion_status::ok)
		<< "and a short-circuited division by zero is not evaluated at all (#56)";
}

TEST_F(ExpanderTest, ARefusedArithmeticAssignmentStaysTheVariableAssignmentError) {
	// It was already fatal, and it must not start printing an ARITHMETIC
	// diagnostic on top of the readonly one shell_state has already produced: the
	// expression is fine, the variable refused the write. Two errors for one cause
	// is what the branch order prevents.
	FakeVars vars;
	vars.refuse = true;
	bool fatal = false;
	expansion_status status = expansion_status::ok;
	expand("echo $((x=1))", nullptr, &status, &fatal, &vars);
	EXPECT_TRUE(fatal);
	EXPECT_EQ(status, expansion_status::expansion_error);
}

// --- a syntax error inside a substitution crosses the fork (#57) --------------

TEST_F(ExpanderTest, ASubstitutionBodyThatWillNotParseIsAFatalExpansionError) {
	// The whole of #57 at this layer. The answer used to be a bool, so the runner
	// could say only "I produced output" or "I could not run at all" - and a child
	// that refused `if true` and _exit(2)ed was indistinguishable from one that ran
	// `exit 2`. The refusal therefore had nowhere to go, and `echo $(if true)`
	// printed the diagnostic and then reported success.
	FakeRunner runner;
	runner.answer = substitution_result::malformed;
	bool fatal = false;
	expansion_status status = expansion_status::ok;
	const auto fields = expand("echo $(if true)", &runner, &status, &fatal);
	EXPECT_TRUE(fatal);
	EXPECT_EQ(status, expansion_status::malformed_expansion)
		<< "the INPUT is at fault, which is what malformed_expansion means (#48)";
	EXPECT_EQ(fields, (std::vector<std::string>{"echo"}));
	ASSERT_EQ(runner.asked.size(), 1u);
	EXPECT_EQ(runner.asked[0], "if true");
}

TEST_F(ExpanderTest, ASubstitutionThatCouldNotBeRunAtAllIsFatalToo) {
	// No pipe or no process: not the input's fault, and not a substitution that
	// produced nothing either - which is what discarding the old `false` made it
	// look like.
	//
	// The FIELD is still built out of the literal bytes around it - `a$(x)b` is
	// `ab` - and that is exactly the wrong answer the flag exists to stop: nothing
	// in the field says the middle went missing. It never reaches a command,
	// because fatal_error() is consulted first.
	FakeRunner runner;
	runner.answer = substitution_result::unavailable;
	bool fatal = false;
	expansion_status status = expansion_status::ok;
	EXPECT_EQ(expand("echo a$(x)b", &runner, &status, &fatal),
	          (std::vector<std::string>{"echo", "ab"}));
	EXPECT_TRUE(fatal);
	EXPECT_EQ(status, expansion_status::expansion_error);
}

TEST_F(ExpanderTest, ASubstitutionThatRanIsNotAnErrorWhateverItsCommandDid) {
	// The boundary. A body that parses and then FAILS at run time - a missing
	// command, an `exit 7` - is an ordinary result: POSIX gives its status to the
	// command with no command name (#50) and the shell carries on. dash agrees:
	// `x=$(eval "if true")` reports 2 through $? and runs the next line.
	FakeRunner runner;
	runner.reply = "out\n";
	bool fatal = false;
	expansion_status status = expansion_status::ok;
	EXPECT_EQ(expand("echo $(x)", &runner, &status, &fatal),
	          (std::vector<std::string>{"echo", "out"}));
	EXPECT_FALSE(fatal);
	EXPECT_EQ(status, expansion_status::ok);
}
