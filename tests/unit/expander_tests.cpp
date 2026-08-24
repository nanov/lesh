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
	//
	// `fatal_out` reports expander::fatal_error(), which is a SEPARATE channel from
	// the returned status and the only one a defect inside a parameter default can
	// travel on: expand_value returns a value, so it has no status to
	// hand back to expand_text. It is also the channel the executor acts on.
	std::vector<std::string> expand(std::string_view src, command_runner* runner = nullptr,
	                                expansion_status* status_out = nullptr,
	                                bool* fatal_out = nullptr) {
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
		if (fatal_out != nullptr)
			*fatal_out = ex.fatal_error();

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

TEST_F(ExpanderTest, AnArgumentTheOperatorNeverReachesIsNeverRefused) {
	// The measured cost of decision 2, pinned rather than left to be discovered.
	// dash reports `${x-$((1}` at PARSE time, so it refuses the command whether or
	// not x is set. lesh's word scan cannot see inside `${...}` - it counts braces,
	// which is #42's territory to change - so the defect is found when the default
	// is expanded, and a default that is not needed is not expanded. Recorded as a
	// divergence in tests/spec/syntax_errors.spec too.
	params.vars["x"] = "value";
	bool fatal = false;
	EXPECT_EQ(expand("echo ${x-$((1}", nullptr, nullptr, &fatal),
	          (std::vector<std::string>{"echo", "value"}));
	EXPECT_FALSE(fatal);
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
