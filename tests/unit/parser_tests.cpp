#include "syntax/parser.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <tuple>
#include <utility>
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

// Here-document delimiters and quote removal. POSIX applies quote removal to the
// delimiter word, so every spelling below ends its body at a line reading `EOT`.
//
// Only the fully quoted forms used to work. `<<\EOT` did not, which meant the body
// ran to end of input - and `<<\EOT` is the spelling the yash conformance suite
// uses in every one of its here-documents, so twenty signal files scored zero for
// this and nothing else. See issue #33.
namespace {

// The body text of the first here_doc node in the tree, or nullopt when the parse
// produced none.
std::optional<std::string_view> first_here_doc_body(const tree& t) {
	for (node_index n = 0; n < t.node_count(); ++n) {
		if (t[n].kind == node_kind::here_doc && t[n].aux != 0xFFFFFFFFu)
			return t.here_doc_text(t[n].aux);
	}
	return std::nullopt;
}

} // namespace

TEST_F(ParserTest, HereDocDelimiterQuoteRemovalAcrossSpellings) {
	struct spelling {
		const char* delimiter;
		const char* what;
	};
	// Each of these must terminate at the line `EOT`.
	const spelling spellings[] = {
		{"EOT", "unquoted"},
		{"'EOT'", "fully single-quoted"},
		{"\"EOT\"", "fully double-quoted"},
		{"\\EOT", "backslash before the first character"},
		{"EO\\T", "backslash mid-word"},
		{"E'O'T", "single-quoted fragment"},
		{"\"EO\"T", "double-quoted fragment"},
	};
	for (const auto& s : spellings) {
		const std::string src = std::string("cat <<") + s.delimiter + "\nbody\nEOT\necho after\n";
		const tree t = parse_it(src);
		const auto body = first_here_doc_body(t);
		ASSERT_TRUE(body.has_value()) << s.what;
		EXPECT_EQ(*body, "body\n") << s.what << " (" << s.delimiter << ")";
		EXPECT_FALSE(t.has_errors()) << s.what;
	}
}

TEST_F(ParserTest, HereDocDelimiterDoesNotMatchAPrefixOrSuffix) {
	// A line must equal the delimiter, not merely start with it.
	const tree t = parse_it("cat <<\\EOT\nEOTX\nXEOT\nEOT\n");
	const auto body = first_here_doc_body(t);
	ASSERT_TRUE(body.has_value());
	EXPECT_EQ(*body, "EOTX\nXEOT\n");
}

// `&` inside a compound command. It is in is_separator, so a list that is not
// explicitly wrapped in an async_list has its `&` silently consumed as a `;` -
// which was fixed for the program body and left broken in every compound command.
// Running `&` in the foreground deadlocks the moment something waits on it.
TEST_F(ParserTest, AmpersandMakesAListAsynchronousInsideCompoundCommands) {
	struct wrapper {
		const char* source;
		const char* what;
	};
	const wrapper wrappers[] = {
		{"echo a &", "the program body"},
		{"{ echo a & }", "a brace group"},
		{"( echo a & )", "a subshell"},
		{"if true; then echo a & fi", "an if body"},
		{"while false; do echo a & done", "a while body"},
		{"for i in 1; do echo a & done", "a for body"},
		{"case x in x) echo a & ;; esac", "a case item"},
		{"f() { echo a & }", "a function body"},
	};
	for (const auto& w : wrappers) {
		const tree t = parse_it(w.source);
		bool found = false;
		for (node_index n = 0; n < t.node_count(); ++n)
			if (t[n].kind == node_kind::async_list)
				found = true;
		EXPECT_TRUE(found) << "no async_list in " << w.what << ": " << w.source;
	}
}

// --- reading one complete command at a time (#40) ----------------------------

namespace {

// The alias table the parser is given, as a test double. The real one is
// shell_state, which the parser deliberately does not know about (#27).
class fake_aliases final : public alias_source {
public:
	void define(std::string name, std::string value) {
		_entries.emplace_back(std::move(name), std::move(value));
	}
	bool lookup_alias(std::string_view name, std::string_view& value) const override {
		for (const auto& [n, v] : _entries)
			if (n == name) {
				value = v;
				return true;
			}
		return false;
	}
private:
	std::vector<std::pair<std::string, std::string>> _entries;
};

// The command name of the first simple command in a tree, as text.
std::string_view first_command_name(const tree& t) {
	for (node_index n = 0; n < t.node_count(); ++n)
		if (t[n].kind == node_kind::simple_command && t[n].children_count > 0) {
			const node& word = t[t.child_of(t[n], 0)];
			return t.text_of(word);
		}
	return {};
}

} // namespace

TEST_F(ParserTest, ParseNextCommandStopsAtTheEndOfTheFirstLine) {
	const std::string_view source = "echo one\necho two\n";
	size_t at = 0;
	const tree first = parse_next_command(pool, source, at);
	EXPECT_EQ(first[first.root()].children_count, 1u);
	EXPECT_EQ(first_command_name(first), "echo");
	EXPECT_EQ(at, 9u) << "the cursor must sit at the start of the second line";

	const tree second = parse_next_command(pool, source, at);
	EXPECT_EQ(second[second.root()].children_count, 1u);
	EXPECT_EQ(at, source.size()) << "nothing executable is left";
}

TEST_F(ParserTest, ParseNextCommandKeepsACompoundCommandWhole) {
	// The unit ends at a newline of the INPUT at top level - not at one inside a
	// construct, or `if` would be read without its body.
	const std::string_view source = "if true\nthen\necho hi\nfi\necho after\n";
	size_t at = 0;
	const tree first = parse_next_command(pool, source, at);
	ASSERT_EQ(first[first.root()].children_count, 1u);
	EXPECT_EQ(first[first.child_of(first[first.root()], 0)].kind, node_kind::if_clause);
	EXPECT_FALSE(first.has_errors());
	EXPECT_EQ(source.substr(at), "echo after\n");
}

TEST_F(ParserTest, ParseNextCommandSkipsAHereDocumentBody) {
	// The body is not commands, and the cursor has to land past it: the resume
	// point comes from the lexer AFTER the parser seeks it over the body.
	const std::string_view source = "cat <<\\END\nnot a command\nEND\necho after\n";
	size_t at = 0;
	const tree first = parse_next_command(pool, source, at);
	EXPECT_FALSE(first.has_errors());
	EXPECT_EQ(source.substr(at), "echo after\n");
}

TEST_F(ParserTest, ParseNextCommandTerminatesOnTrailingBlanksAndComments) {
	// Every call must ADVANCE the cursor, or a comment or blank line at the end of a
	// script is an infinite read loop. Blank lines are read as the empty commands
	// they are, one per call, and nothing is left over.
	const std::string_view source = "echo hi\n# just a comment\n\n";
	size_t at = 0;
	size_t commands = 0;
	for (int rounds = 0; at < source.size(); ++rounds) {
		ASSERT_LT(rounds, 8) << "the read loop is not making progress";
		const size_t before = at;
		const tree t = parse_next_command(pool, source, at);
		EXPECT_GT(at, before) << "a call that consumes nothing never ends";
		commands += t[t.root()].children_count;
	}
	EXPECT_EQ(commands, 1u) << "a comment and a blank line are not commands";
}

TEST_F(ParserTest, ATokenFromAnAliasBodyReadsBackAsTheAliasText) {
	// The bug this whole ticket turned on: a token from an alias body carried its
	// offset within the BODY, and the tree read that offset out of the source - so
	// `alias e=echo` produced a command called `alia` and alias substitution could
	// never actually run anything.
	fake_aliases aliases;
	aliases.define("e", "echo");
	const tree t = parse(pool, "e hi", &aliases);
	EXPECT_EQ(first_command_name(t), "echo");
}

TEST_F(ParserTest, AliasReplacementIsRescannedSoItCanYieldAKeyword) {
	fake_aliases aliases;
	aliases.define("i", "if true");
	const tree t = parse(pool, "i; then echo hi; fi", &aliases);
	ASSERT_EQ(t[t.root()].children_count, 1u);
	EXPECT_EQ(t[t.child_of(t[t.root()], 0)].kind, node_kind::if_clause);
}

TEST_F(ParserTest, AReservedWordIsNeverReplacedByAnAlias) {
	// POSIX checks a word against the reserved words BEFORE the alias table, so an
	// alias cannot shadow `if`.
	fake_aliases aliases;
	aliases.define("if", ":");
	const tree t = parse(pool, "if true; then echo hi; fi", &aliases);
	ASSERT_EQ(t[t.root()].children_count, 1u);
	EXPECT_EQ(t[t.child_of(t[t.root()], 0)].kind, node_kind::if_clause);
}

TEST_F(ParserTest, ADefinitionEndingInABlankMakesTheNextWordEligible) {
	// `alias e='echo ' c=cat` makes `e c` run cat, and the word after THAT is not
	// eligible - one word, not the rest of the line.
	fake_aliases aliases;
	aliases.define("e", "echo ");
	aliases.define("c", "cat");
	const tree t = parse(pool, "e c c", &aliases);
	node_index command = no_node;
	for (node_index n = 0; n < t.node_count(); ++n)
		if (t[n].kind == node_kind::simple_command)
			command = n;
	ASSERT_NE(command, no_node);
	ASSERT_EQ(t[command].children_count, 3u);
	EXPECT_EQ(t.text_of(t[t.child_of(t[command], 0)]), "echo");
	EXPECT_EQ(t.text_of(t[t.child_of(t[command], 1)]), "cat");
	EXPECT_EQ(t.text_of(t[t.child_of(t[command], 2)]), "c");
}

TEST_F(ParserTest, TheCommandWordAfterAnAssignmentPrefixIsSubstituted) {
	// POSIX substitutes for the COMMAND WORD, which is the first word that is not
	// an assignment.
	fake_aliases aliases;
	aliases.define("s", "sh");
	const tree t = parse(pool, "a=A s -c :", &aliases);
	node_index command = no_node;
	for (node_index n = 0; n < t.node_count(); ++n)
		if (t[n].kind == node_kind::simple_command)
			command = n;
	ASSERT_NE(command, no_node);
	ASSERT_EQ(t[command].children_count, 4u);
	EXPECT_EQ(t[t.child_of(t[command], 0)].kind, node_kind::assignment);
	EXPECT_EQ(t.text_of(t[t.child_of(t[command], 1)]), "sh");
}

TEST_F(ParserTest, ANewlineAfterAPipeContinuesThePipeline) {
	// POSIX spells it `linebreak` in the grammar. Without it, reading one command
	// at a time ended the unit at the newline and the right-hand side was lost.
	const std::string_view source = "echo foo |\ncat\n";
	size_t at = 0;
	const tree t = parse_next_command(pool, source, at);
	ASSERT_EQ(t[t.root()].children_count, 1u);
	EXPECT_EQ(t[t.child_of(t[t.root()], 0)].kind, node_kind::pipeline);
	EXPECT_EQ(at, source.size());
}
