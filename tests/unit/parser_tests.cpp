#include "syntax/parser.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace lesh::syntax;

namespace {

class ParserTest : public ::testing::Test {
protected:
	lesh::buffer_pool pool{1024 * 64};
	tree parse_it(std::string_view src) { return parse(pool, src); }
};

// Collects node kinds in the order the parser created them, for shape assertions
// that do not depend on internal indices.
std::vector<node_kind> kinds_under(const tree& t, node_index at) {
	std::vector<node_kind> out;
	const node& n = t[at];
	for (uint32_t i = 0; i < n.children_count; ++i)
		out.push_back(t[t.child_of(n, i)].kind);
	return out;
}

} // namespace

TEST_F(ParserTest, EmptyInputStillProducesATree) {
	const tree t = parse_it("");
	EXPECT_NE(t.root(), no_node) << "there is always a tree";
	EXPECT_EQ(t[t.root()].kind, node_kind::program);
	EXPECT_EQ(t[t.root()].children_count, 0u);
	EXPECT_FALSE(t.has_errors());
}

TEST_F(ParserTest, SimpleCommandCollectsItsWords) {
	const tree t = parse_it("echo hello world");
	const node& program = t[t.root()];
	ASSERT_EQ(program.children_count, 1u);

	const node_index cmd = t.child_of(program, 0);
	EXPECT_EQ(t[cmd].kind, node_kind::simple_command);
	EXPECT_EQ(kinds_under(t, cmd),
	          (std::vector{node_kind::word, node_kind::word, node_kind::word}));
	EXPECT_EQ(t.text_of(t[cmd]), "echo hello world");
}

TEST_F(ParserTest, PipelineHasOneChildPerCommand) {
	const tree t = parse_it("a | b | c");
	const node_index pipe = t.child_of(t[t.root()], 0);
	EXPECT_EQ(t[pipe].kind, node_kind::pipeline);
	EXPECT_EQ(t[pipe].children_count, 3u);
	for (uint32_t i = 0; i < 3; ++i)
		EXPECT_EQ(t[t.child_of(t[pipe], i)].kind, node_kind::simple_command);
}

TEST_F(ParserTest, PipelineOfOneIsNotWrapped) {
	// Every consumer walks this tree per keystroke; a wrapper node that carries no
	// information is depth nobody should pay for.
	const tree t = parse_it("echo hi");
	EXPECT_EQ(t[t.child_of(t[t.root()], 0)].kind, node_kind::simple_command);
}

TEST_F(ParserTest, AndOrIsBinaryAndLeftAssociative) {
	const tree t = parse_it("a && b || c");
	const node_index top = t.child_of(t[t.root()], 0);
	ASSERT_EQ(t[top].kind, node_kind::and_or);
	EXPECT_EQ(t[top].children_count, 2u);

	const node_index left = t.child_of(t[top], 0);
	EXPECT_EQ(t[left].kind, node_kind::and_or) << "(a && b) || c, not a && (b || c)";
	EXPECT_EQ(t[t.child_of(t[top], 1)].kind, node_kind::simple_command);
}

TEST_F(ParserTest, AndOrRecordsWhichOperatorJoined) {
	const tree t = parse_it("a && b");
	const node& top = t[t.child_of(t[t.root()], 0)];
	ASSERT_EQ(top.kind, node_kind::and_or);
	EXPECT_EQ(t.token_at(top.aux).kind, token_kind::and_if);

	const tree u = parse_it("a || b");
	const node& top2 = u[u.child_of(u[u.root()], 0)];
	EXPECT_EQ(u.token_at(top2.aux).kind, token_kind::or_if);
}

TEST_F(ParserTest, SeparatorsSplitCommands) {
	const tree t = parse_it("a; b\nc");
	EXPECT_EQ(t[t.root()].children_count, 3u);
}

TEST_F(ParserTest, AssignmentsAreDistinguishedFromWords) {
	const tree t = parse_it("FOO=bar echo hi");
	const node_index cmd = t.child_of(t[t.root()], 0);
	EXPECT_EQ(kinds_under(t, cmd),
	          (std::vector{node_kind::assignment, node_kind::word, node_kind::word}));
}

TEST_F(ParserTest, AssignmentSyntaxAfterTheCommandNameIsAnArgument) {
	// POSIX: only the command prefix carries assignments. `echo FOO=bar` passes
	// FOO=bar as an argument.
	const tree t = parse_it("echo FOO=bar");
	const node_index cmd = t.child_of(t[t.root()], 0);
	EXPECT_EQ(kinds_under(t, cmd), (std::vector{node_kind::word, node_kind::word}));
}

TEST_F(ParserTest, WhatLooksLikeAnAssignmentButIsNotAName) {
	// The part before '=' must be a NAME, so this is an ordinary word.
	const tree t = parse_it("a-b=c echo");
	const node_index cmd = t.child_of(t[t.root()], 0);
	EXPECT_EQ(kinds_under(t, cmd)[0], node_kind::word);
}

TEST_F(ParserTest, RedirectionCarriesItsFileDescriptor) {
	const tree t = parse_it("echo hi 2>err");
	const node_index cmd = t.child_of(t[t.root()], 0);
	const auto kinds = kinds_under(t, cmd);
	ASSERT_EQ(kinds.size(), 3u);
	EXPECT_EQ(kinds[2], node_kind::redirect);
	EXPECT_EQ(t[t.child_of(t[cmd], 2)].aux, 2u) << "2>err redirects fd 2";
}

// --- the parser never fails --------------------------------------------------

TEST_F(ParserTest, UnexpectedOperatorBecomesAnErrorNodeAndParsingContinues) {
	const tree t = parse_it("echo ;; hi");
	EXPECT_TRUE(t.has_errors());
	EXPECT_NE(t.root(), no_node) << "a tree is still produced";
	EXPECT_GT(t[t.root()].children_count, 0u) << "recovery continued past the error";
}

TEST_F(ParserTest, RedirectionWithNoTargetIsAnErrorNodeNotAFailure) {
	const tree t = parse_it("echo >");
	EXPECT_TRUE(t.has_errors());
	const node_index cmd = t.child_of(t[t.root()], 0);
	bool found = false;
	for (uint32_t i = 0; i < t[cmd].children_count; ++i) {
		const node& child = t[t.child_of(t[cmd], i)];
		if (child.error == parse_error::missing_operand)
			found = true;
	}
	EXPECT_TRUE(found);
}

TEST_F(ParserTest, UnterminatedQuoteIsIncompleteNotAnError) {
	const tree t = parse_it("echo 'abc");
	EXPECT_TRUE(t.incomplete()) << "an interactive shell answers this with a continuation prompt";
}

TEST_F(ParserTest, ParsingNeverFailsOnHalfTypedInput) {
	// The line-editor contract. Every prefix of a realistic command line must
	// produce a tree, without throwing, hanging, or looping.
	const std::string full = "FOO=1 echo \"a b\" | grep -e x > out 2>&1 && ls ;; done";
	for (size_t n = 0; n <= full.size(); ++n) {
		const tree t = parse_it(std::string_view(full).substr(0, n));
		ASSERT_NE(t.root(), no_node) << "no tree for prefix of length " << n;
	}
}

// --- spans, which are the point ----------------------------------------------

TEST_F(ParserTest, EveryNodeCarriesItsSourceRange) {
	const tree t = parse_it("echo hello");
	const node_index cmd = t.child_of(t[t.root()], 0);
	EXPECT_EQ(t.text_of(t[cmd]), "echo hello");
	EXPECT_EQ(t.text_of(t[t.child_of(t[cmd], 0)]), "echo");
	EXPECT_EQ(t.text_of(t[t.child_of(t[cmd], 1)]), "hello");
}

TEST_F(ParserTest, NodeAtFindsTheDeepestNodeContainingAnOffset) {
	// This is the query completion asks: "what am I inside of?". zsh cannot ask it
	// - struct eprog carries no offsets - which is why it maintains a second
	// parser for its line editor.
	const std::string src = "echo hello";
	const tree t = parse_it(src);

	const node_index at_echo = t.node_at(1);
	ASSERT_NE(at_echo, no_node);
	EXPECT_EQ(t.text_of(t[at_echo]), "echo");

	const node_index at_hello = t.node_at(7);
	ASSERT_NE(at_hello, no_node);
	EXPECT_EQ(t.text_of(t[at_hello]), "hello");
}

TEST_F(ParserTest, NodeAtReturnsNothingPastTheEnd) {
	const tree t = parse_it("echo");
	EXPECT_EQ(t.node_at(999), no_node);
}

TEST_F(ParserTest, SourceIsNotModified) {
	std::string src = "echo 'hi' | cat > out";
	const std::string before = src;
	const tree t = parse_it(src);
	EXPECT_EQ(src, before) << "the editor owns this buffer";
}

TEST_F(ParserTest, NodeStaysSmall) {
	EXPECT_EQ(sizeof(node), 24u);
}
