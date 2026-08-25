#include "runtime/builtins.h"

#include "runtime/executor.h"
#include "runtime/shell_state.h"
#include "syntax/parser.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>

using namespace lesh::runtime;
using namespace lesh::syntax;

namespace {

// `read`, and the input source it reads from. See issue #31.
//
// The builtin was implemented and read `std::fgetc(stdin)`, which is the one
// place it must not read from: `main` slurps a script with read_all(std::cin),
// draining fd 0 and latching the stdio EOF indicator, so every `read` in a script
// fed on STANDARD INPUT failed with nothing assigned - read-p.tst scored 1/32
// while the same script run as a FILE passed. The yash harness always pipes the
// script in, which is why the whole file went with it.
//
// These tests put the input on FD 0 through a pipe, because that is the seam:
// a here-document, a pipeline stage and a piped-in script all reach `read` as fd
// 0, and only one of the three used to work.
class ReadTest : public ::testing::Test {
protected:
	lesh::buffer_pool pool{1024 * 64};
	shell_state state;
	int _original_stdin = -1;

	void SetUp() override { _original_stdin = ::dup(STDIN_FILENO); }

	void TearDown() override {
		if (_original_stdin >= 0) {
			::dup2(_original_stdin, STDIN_FILENO);
			::close(_original_stdin);
		}
	}

	// Puts `text` on fd 0 and returns nothing else: the point is that `read` sees a
	// descriptor, never a FILE*.
	void feed(std::string_view text) {
		int fds[2] = {-1, -1};
		ASSERT_EQ(::pipe(fds), 0);
		ASSERT_EQ(::write(fds[1], text.data(), text.size()),
		          static_cast<ssize_t>(text.size()));
		::close(fds[1]);
		ASSERT_EQ(::dup2(fds[0], STDIN_FILENO), STDIN_FILENO);
		::close(fds[0]);
	}

	// What is still unread on fd 0 - the assertion behind "read does not read more
	// than needed", which no examination of the variables can make.
	[[nodiscard]] std::string rest_of_input() const {
		std::string out;
		char c = '\0';
		while (::read(STDIN_FILENO, &c, 1) == 1)
			out += c;
		return out;
	}

	int run(std::string_view src) {
		const tree t = parse(pool, src);
		tree_walking_executor ex{pool, state};
		return ex.run(t);
	}

	[[nodiscard]] std::string value_of(std::string_view name) const {
		std::string_view text;
		return state.lookup(name, text) ? std::string{text} : std::string{"<unset>"};
	}
};

TEST_F(ReadTest, ReadsFromDescriptorZeroAfterTheScriptWasSlurpedFromIt) {
	// The bug, reproduced by its mechanism rather than by its symptom: fd 0 carries
	// the SCRIPT, `main` drains it through std::cin exactly as it does for a piped-in
	// script, and the here-document then puts a fresh pipe on fd 0. Reading the
	// drained FILE* reports EOF forever - a stdio EOF indicator is sticky until
	// clearerr and the buffer position no longer matches the descriptor - so this
	// used to leave `a` unset and return 1.
	feed("read a <<END\nA\nEND\n");
	std::string script;
	std::string line;
	while (std::getline(std::cin, line)) {
		script += line;
		script += '\n';
	}
	EXPECT_EQ(run(script), 0);
	EXPECT_EQ(value_of("a"), "A");
}

TEST_F(ReadTest, ConsumesNoMoreOfItsInputThanTheLineItNeeds) {
	// POSIX requires it, and read-p.tst asserts it by running `cat` afterwards. A
	// buffered FILE* would swallow the whole block from the pipe, so the rest of the
	// input is the only witness. The first line continues, so `read` must consume
	// two of the three lines and not a byte of the third.
	feed("\\\nA\nC\n");
	EXPECT_EQ(run("read a"), 0);
	EXPECT_EQ(value_of("a"), "A");
	EXPECT_EQ(rest_of_input(), "C\n");
}

TEST_F(ReadTest, AssignsAtEndOfInputAndStillFails) {
	// read-p.tst's "EOF fails read" and "variables are assigned even if EOF is
	// reached without newline". dash returns 1 and assigns; returning early on EOF
	// left every variable UNSET, which is what made `${a-unset}` say unset where
	// dash says empty.
	feed("");
	EXPECT_EQ(run("read a"), 1);
	EXPECT_EQ(value_of("a"), "") << "assigned empty, not left unset";

	feed("foo bar baz");
	EXPECT_EQ(run("read b c"), 1) << "data without a newline still fails";
	EXPECT_EQ(value_of("b"), "foo");
	EXPECT_EQ(value_of("c"), "bar baz");

	feed("foo\\");
	EXPECT_EQ(run("read d"), 1);
	EXPECT_EQ(value_of("d"), "foo") << "an orphan backslash is ignored";
}

TEST_F(ReadTest, SplitsOnIfsWithTheLastVariableTakingTheRemainder) {
	feed("A B C D \n");
	EXPECT_EQ(run("read a b"), 0);
	EXPECT_EQ(value_of("a"), "A");
	EXPECT_EQ(value_of("b"), "B C D") << "the remainder, trailing IFS space removed";

	// Leading and trailing IFS WHITE SPACE is not a delimiter; a non-white-space
	// IFS character is, at the start of the line as much as anywhere else.
	feed("  A  \n");
	EXPECT_EQ(run("read c"), 0);
	EXPECT_EQ(value_of("c"), "A");
	feed(" - A - \n");
	EXPECT_EQ(run("read d"), 0);
	EXPECT_EQ(value_of("d"), "- A -") << "`-` is not in the default IFS";

	// Fewer fields than variables: the surplus are assigned the empty string.
	feed("A B\n");
	EXPECT_EQ(run("read e f g"), 0);
	EXPECT_EQ(value_of("g"), "");
}

TEST_F(ReadTest, ADelimiterThatRunsToTheEndOfTheLineYieldsNoEmptyField) {
	// read-p.tst's "exact number of fields with non-whitespace IFS". Three fields
	// and three variables, so the third takes its own field - NOT the remainder of
	// the line with the trailing ` -` still on it. The distinction is the whole
	// difference between this case and "too many fields are joined": there, the
	// remainder keeps a trailing non-white-space delimiter and loses only the
	// spaces.
	feed("A-B-C - \n");
	EXPECT_EQ(run("IFS=' -' read a b c"), 0);
	EXPECT_EQ(value_of("c"), "C");

	feed("A B C-C D  \n");
	EXPECT_EQ(run("IFS=' -' read d e f"), 0);
	EXPECT_EQ(value_of("f"), "C-C D");
	feed("A B C-C D -  \n");
	EXPECT_EQ(run("IFS=' -' read g h i"), 0);
	EXPECT_EQ(value_of("i"), "C-C D -") << "the trailing delimiter stays in a remainder";

	// A non-white-space IFS character at the START does yield an empty field, and
	// two of them yield two - only one end of the line is special.
	feed("--CC--\n");
	EXPECT_EQ(run("IFS=' -' read j k l m n"), 0);
	EXPECT_EQ(value_of("j"), "");
	EXPECT_EQ(value_of("k"), "");
	EXPECT_EQ(value_of("l"), "CC");
	EXPECT_EQ(value_of("m"), "");
	EXPECT_EQ(value_of("n"), "");
}

TEST_F(ReadTest, ABackslashPreventsFieldSplittingUnlessRawModeIsAsked) {
	// Why the resolved line carries a flag per character. Resolving the escapes and
	// splitting the result afterwards would split `A\ A` into two fields, because by
	// then the space is indistinguishable from one the input supplied.
	feed("A\\ A B\n");
	EXPECT_EQ(run("read a b"), 0);
	EXPECT_EQ(value_of("a"), "A A");
	EXPECT_EQ(value_of("b"), "B");

	// The same escaped space with the backslash IN IFS: still not a delimiter, and
	// the backslash itself is not one either.
	feed("A\\ A B\n");
	EXPECT_EQ(run("IFS=' \\' read c d"), 0);
	EXPECT_EQ(value_of("c"), "A A");

	// -r takes every backslash literally, which makes it a delimiter when IFS says
	// so and an ordinary character when IFS does not.
	feed("A\\ A B\n");
	EXPECT_EQ(run("read -r e f"), 0);
	EXPECT_EQ(value_of("e"), "A\\");
	EXPECT_EQ(value_of("f"), "A B");
	feed("A\\B\n");
	EXPECT_EQ(run("IFS='\\' read -r g h"), 0);
	EXPECT_EQ(value_of("g"), "A");
	EXPECT_EQ(value_of("h"), "B");
}

TEST_F(ReadTest, AnEmptyIfsSplitsNothingAndStripsNothing) {
	feed(" A\\ B  C \n");
	EXPECT_EQ(run("IFS= read a b"), 0);
	EXPECT_EQ(value_of("a"), " A B  C ") << "leading and trailing space are data";
	EXPECT_EQ(value_of("b"), "");
}

TEST_F(ReadTest, LineContinuationJoinsTheNextLineUnlessRawModeIsAsked) {
	feed("A\\\nB\nC\n");
	EXPECT_EQ(run("read a"), 0);
	EXPECT_EQ(value_of("a"), "AB");
	EXPECT_EQ(rest_of_input(), "C\n");

	feed("A\\\nB\n");
	EXPECT_EQ(run("read -r b"), 0);
	EXPECT_EQ(value_of("b"), "A\\") << "-r ends the line at the newline";
}

TEST_F(ReadTest, DelimiterOptionReplacesTheNewline) {
	// `-d` is POSIX Issue 8. dash predates it and fails read-p.tst's "non-default
	// delimiters" for that reason alone - the one case it does not pass.
	feed("A B:C D ExF\n");
	EXPECT_EQ(run("read -d : a b"), 0);
	EXPECT_EQ(value_of("a"), "A");
	EXPECT_EQ(value_of("b"), "B");
	EXPECT_EQ(run("read -d x c d"), 0);
	EXPECT_EQ(value_of("c"), "C");
	EXPECT_EQ(value_of("d"), "D E");
	// Attached, clustered with -r, and after `--`.
	feed("A:B\n");
	EXPECT_EQ(run("read -rd: e"), 0);
	EXPECT_EQ(value_of("e"), "A");
	feed("A B\n");
	EXPECT_EQ(run("read -- f g"), 0);
	EXPECT_EQ(value_of("f"), "A");
}

TEST_F(ReadTest, AnUnknownOptionIsAnErrorRatherThanAVariableName) {
	feed("A\n");
	EXPECT_EQ(run("read -Z a 2>/dev/null"), 2);
	EXPECT_EQ(value_of("a"), "<unset>") << "nothing ran, so nothing was assigned";
	EXPECT_EQ(run("read -d 2>/dev/null"), 2) << "-d needs its delimiter";
}

TEST_F(ReadTest, ANameTheShellCouldNeverAssignIsRefused) {
	// dash reads the LINE first and fails on the operand afterwards, assigning the
	// names ahead of the bad one: `printf 'A\nB\n' | { read 1bad; read x; }` leaves
	// x as B, so the check cannot move ahead of the read.
	feed("A B\n");
	EXPECT_EQ(run("read a 2bad 2>/dev/null"), 2);
	EXPECT_EQ(value_of("a"), "A");
	EXPECT_EQ(rest_of_input(), "") << "the line was consumed before the refusal";
}

TEST_F(ReadTest, WithNoOperandsTheLineGoesToREPLY) {
	// bash, ksh and zsh do this; dash makes it an error ("read: arg count"). Kept as
	// a deliberate divergence: no conformance assertion covers it, `while read; do`
	// is common, and the alternative is refusing a line the shell has already read.
	feed("hello  there \n");
	EXPECT_EQ(run("read"), 0);
	EXPECT_EQ(value_of("REPLY"), "hello  there ") << "unsplit and unstripped";
}

TEST_F(ReadTest, AReadonlyTargetFailsWithoutAssigning) {
	// dash reports `read: a: is read only`, returns 2, and - being a REGULAR builtin
	// - carries on rather than exiting the shell.
	feed("B\n");
	EXPECT_EQ(run("readonly a=A; read a 2>/dev/null"), 2);
	EXPECT_EQ(value_of("a"), "A");
}

TEST_F(ReadTest, AnAssignmentPrefixIsVisibleToTheBuiltinAndDoesNotOutliveIt) {
	// The other half of the fix. A regular builtin's prefix assignments were applied
	// after the call and only when they were meant to persist, so `IFS=' -' read a b`
	// split on the default IFS - twelve of read-p.tst's field-splitting cases.
	// POSIX 2.9.1 performs them before the command and, for a regular builtin, ends
	// them with it.
	feed("A-B\n");
	EXPECT_EQ(run("IFS=' -' read a b"), 0);
	EXPECT_EQ(value_of("a"), "A");
	EXPECT_EQ(value_of("b"), "B");
	// The default rather than unset: POSIX makes IFS a variable the shell SETS at
	// startup, so there is a previous value to restore to (#42). Before that it was
	// genuinely unset, and this line read "<unset>".
	EXPECT_EQ(value_of("IFS"), " \t\n") << "restored to what it was: the default";

	feed("A-B\n");
	EXPECT_EQ(run("IFS=:; IFS=' -' read c d"), 0);
	EXPECT_EQ(value_of("IFS"), ":") << "restored to its previous VALUE";

	// A SPECIAL builtin's prefix persists, which is the distinction the branch is
	// there to make.
	EXPECT_EQ(run("keep=1 export other"), 0);
	EXPECT_EQ(value_of("keep"), "1");
}

} // namespace
