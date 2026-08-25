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

TEST_F(ParserTest, UnterminatedQuoteIsBothIncompleteAndADefect) {
	// The two channels are orthogonal, and #47 turned on reading them as a
	// hierarchy. Incomplete is for the reader: more input would fix this, so an
	// interactive shell prompts. has_errors() is for the executor: as it stands the
	// tree must not run, which is why `lesh -c "echo it's"` printed `it` at status
	// zero for as long as only the node KIND was consulted.
	const tree t = parse_it("echo 'abc");
	EXPECT_TRUE(t.incomplete()) << "an interactive shell answers this with a continuation prompt";
	EXPECT_TRUE(t.has_errors()) << "a shell with the whole input in hand has nothing to continue";
}

TEST_F(ParserTest, IncompleteWithoutADefectIsNotAnError) {
	// dash runs both of these and exits zero, so neither may become a syntax
	// error. They are what stops the fix for #47 from being "incomplete implies
	// error" - and an unterminated here-document is the case #21 depends on.
	for (const std::string_view src : {"echo a\\", "cat <<EOF\nbody"}) {
		const tree t = parse_it(src);
		EXPECT_TRUE(t.incomplete()) << src;
		EXPECT_FALSE(t.has_errors()) << "more input would complete it; it is not malformed: " << src;
	}
}

TEST_F(ParserTest, AWordCarriesItsTokensDefectInEveryPosition) {
	// Only a simple command's own words recorded the defect, so `for i in "a` and
	// `case "a in` were accepted in silence. The word node is built in one place
	// now precisely so a new position cannot forget.
	const std::string_view cases[] = {
		"echo \"abc",                      // argument
		"x=\"abc",                         // assignment prefix
		"cat > \"abc",                     // redirection target
		"cat <<\"EOF\nx\nEOF",             // here-document delimiter
		"for i in \"a; do echo; done",     // for list
		"case \"a in b) echo;; esac",      // case subject
		"case a in \"b) echo;; esac",      // case pattern
		"echo $(",                         // command substitution
		"echo ${x",                        // parameter expansion
		"echo $((1",                       // arithmetic expansion
		"echo `",                          // backquote
	};
	for (const std::string_view src : cases) {
		const tree t = parse_it(src);
		EXPECT_TRUE(t.has_errors()) << src;
	}
}

// --- an unterminated compound command (#49) ----------------------------------
//
// The parser's half of #47's defect. `accept(reserved::kw_fi)` returning false is
// what makes recovery possible, and five of the six compound commands DISCARDED
// that false - so `{ echo x` ran and reported success where dash reports a syntax
// error. The two channels stay orthogonal, and which of them a shape lands in is
// what these three tests pin down.

TEST_F(ParserTest, AnUnterminatedCompoundCommandIsBothIncompleteAndADefect) {
	// Ran out of INPUT with a construct open. Incomplete, so an interactive reader
	// answers it with a continuation prompt; defective, so a shell holding the
	// whole input diagnoses instead. dash reports 2 for every one of these.
	const std::string_view cases[] = {
		"( echo x",                    // subshell
		"{ echo x",                    // brace group
		"if true",                     // missing `then`
		"if true; then echo hi",       // missing `fi`
		"if true; then a; elif b",     // missing the second `then`
		"if true; then a; else b",     // missing `fi` after `else`
		"while true",                  // missing `do`
		"while true; do echo hi",      // missing `done`
		"until true",                  // missing `do`
		"until true; do echo hi",      // missing `done`
		"for i in 1",                  // missing `do`
		"for i in 1; do echo hi",      // missing `done`
		"for i",                       // missing `do`, with no list at all
		"case a in",                   // missing `esac`
		"case a in b) echo hi;;",      // missing `esac`
		"case a in b",                 // missing `)`
		"case a",                      // missing `in`
		"f() { echo x",                // the body of a function definition
	};
	for (const std::string_view src : cases) {
		const tree t = parse_it(src);
		EXPECT_TRUE(t.has_errors()) << "a shell holding the whole input must refuse: " << src;
		EXPECT_TRUE(t.incomplete()) << "more input would complete it: " << src;
	}
}

TEST_F(ParserTest, AConstructClosedByTheWrongWordIsADefectWithoutBeingIncomplete) {
	// The other half of the split. `if true; fi` has a `fi` where `then` belongs,
	// and no amount of further input helps - so it is a defect that is NOT
	// incomplete, which is what stops the fix from being "missing terminator
	// implies incomplete". dash reports 2 for these too.
	const std::string_view cases[] = {
		"if true; fi",                     // `fi` where `then` belongs
		"if true; then a; done",           // a loop's terminator on an if
		"while true; done",                // `done` where `do` belongs
		"for i in 1; done",                // the same, for a for loop
		"case a in b echo hi;; esac",      // a pattern list with no `)`
		"{ echo x; )",                     // a subshell's closer on a brace group
		// Nested, and the OUTER construct is closed: the `fi` the subshell ran into
		// is the if's own, so the defect is the subshell's and no continuation
		// reaches it.
		"if true; then ( echo x; fi",
	};
	for (const std::string_view src : cases) {
		const tree t = parse_it(src);
		EXPECT_TRUE(t.has_errors()) << src;
		EXPECT_FALSE(t.incomplete()) << "no continuation completes this: " << src;
	}
}

TEST_F(ParserTest, AWellFormedCompoundCommandCarriesNoDefect) {
	// The over-eager half of the same change, and the reason it is a test rather
	// than a hope: `error-p.tst` asserts 220 error behaviours and this is where an
	// over-eager terminator check shows up first. The two `linebreak` forms are the
	// ones that were only accepted because the missing `in` was being discarded.
	const std::string_view cases[] = {
		"( echo x )",
		"{ echo x; }",
		"{ echo x\n}",
		"if true; then echo hi; fi",
		"if true; then a; elif b; then c; else d; fi",
		"if true\nthen\necho hi\nfi",
		"while false; do echo hi; done",
		"until true; do echo hi; done",
		"for i in 1 2; do echo $i; done",
		"for i; do echo $i; done",
		"for i\ndo echo $i; done",
		"for i\nin 1 2\ndo echo $i; done",   // linebreak before `in`
		"case a in b) echo hi;; esac",
		"case a in (b) echo hi;; esac",
		"case a in b|c) echo hi;; esac",
		"case a in esac",
		"case a\nin\nb) echo hi;;\nesac",    // linebreak before `in`
		"f() { echo x; }",
		"if true; then ( echo x ); fi",
		"{ if true; then echo hi; fi; }",
	};
	for (const std::string_view src : cases) {
		const tree t = parse_it(src);
		EXPECT_FALSE(t.has_errors()) << "this is valid POSIX: " << src;
		EXPECT_FALSE(t.incomplete()) << src;
	}
}

TEST_F(ParserTest, ACaseClauseGivesItsWordsTheirRoles) {
	// Which words are PATTERNS is knowledge only the parser has: `*)` and the `*`
	// in `echo *` are the same token and the same node kind, and the expander sees
	// no difference. Recorded on the node so the expander cannot be handed one and
	// treat it as a command argument, which is what threw the quoting away.
	const tree t = parse_it("case \"$x\" in a|b) echo *;; esac");
	const node& clause = t[t.child_of(t[t.root()], 0)];
	ASSERT_EQ(clause.kind, node_kind::case_clause);

	const node& subject = t[t.child_of(clause, 0)];
	EXPECT_EQ(static_cast<word_role>(subject.aux), word_role::case_subject)
		<< "the subject is one value, but text rather than a pattern";

	const node& item = t[t.child_of(clause, 1)];
	ASSERT_EQ(item.aux, 2u) << "two alternative patterns";
	for (uint32_t i = 0; i < item.aux; ++i)
		EXPECT_EQ(static_cast<word_role>(t[t.child_of(item, i)].aux), word_role::pattern);

	// The body's words are ordinary too, or `echo *` inside a case would stop
	// globbing.
	const node& body = t[t.child_of(item, item.aux)];
	const node& command = t[t.child_of(body, 0)];
	for (uint32_t i = 0; i < command.children_count; ++i) {
		const node& w = t[t.child_of(command, i)];
		if (w.kind == node_kind::word)
			EXPECT_EQ(static_cast<word_role>(w.aux), word_role::ordinary);
	}
}

// The high bit of a case_item's `aux` records whether `;&` closed it, the same
// packing for_loop uses for its name token and `in` flag (#64).
constexpr uint32_t kCaseItemFallsThrough = 0x80000000u;
constexpr uint32_t kCaseItemPatternMask = 0x7FFFFFFFu;

TEST_F(ParserTest, ASemicolonAmpersandMarksTheItemItCloses) {
	// POSIX.1-2024 fallthrough: `;&` runs the NEXT item's body without testing
	// its pattern, so the executor needs to know which terminator closed this
	// item. `;;` must leave the flag clear - it is the case the plain pattern
	// count assertion above already covers, and this pins down the opposite.
	const tree t = parse_it("case i in i) ;& j) echo x;; esac");
	EXPECT_FALSE(t.has_errors());
	const node& clause = t[t.child_of(t[t.root()], 0)];
	ASSERT_EQ(clause.children_count, 3u) << "subject plus two items";

	const node& first = t[t.child_of(clause, 1)];
	EXPECT_EQ(first.aux & kCaseItemPatternMask, 1u);
	EXPECT_TRUE(first.aux & kCaseItemFallsThrough) << "closed by ;&";

	const node& second = t[t.child_of(clause, 2)];
	EXPECT_EQ(second.aux & kCaseItemPatternMask, 1u);
	EXPECT_FALSE(second.aux & kCaseItemFallsThrough) << "closed by ;;, not ;&";
}

TEST_F(ParserTest, ASemicolonAmpersandOnTheLastItemIsStillValid) {
	// `;&` with no item after it is legal - POSIX only says the NEXT item's body
	// runs if there is one - and case-p.tst:203 ends exactly this way.
	const tree t = parse_it("case i in i) echo x;& esac");
	EXPECT_FALSE(t.has_errors());
	const node& clause = t[t.child_of(t[t.root()], 0)];
	const node& item = t[t.child_of(clause, 1)];
	EXPECT_TRUE(item.aux & kCaseItemFallsThrough);
}

TEST_F(ParserTest, SemicolonAmpersandDoesNotDisturbOrdinaryAmpersandUses) {
	// A new token for `;&` must not change how `;` and `&` behave on their own -
	// a background command before `;;`, or a bare `&` list separator, are
	// unrelated constructs that happen to share bytes with the new operator.
	for (const std::string_view src : {"case x in x) foo &;; esac", "a & b",
	                                   "case x in x) foo&;;esac"}) {
		const tree t = parse_it(src);
		EXPECT_FALSE(t.has_errors()) << src;
	}
}

TEST_F(ParserTest, AnUnterminatedCompoundCommandStillEndsTheReadUnit) {
	// The progress guarantee, for the parser's channel this time: the incremental
	// reader calls parse_next_command in a loop until the cursor reaches the end,
	// and a call that consumes nothing is a hang rather than a diagnostic.
	const std::string_view sources[] = {"echo one\nif true\n", "echo one\n{ echo x\n",
	                                    "echo one\ncase a in\n"};
	for (const std::string_view src : sources) {
		size_t at = 0;
		const tree first = parse_next_command(pool, src, at);
		EXPECT_FALSE(first.has_errors()) << "the first line is well formed: " << src;
		ASSERT_GT(at, 0u) << src;

		const size_t before = at;
		const tree second = parse_next_command(pool, src, at);
		EXPECT_TRUE(second.has_errors()) << src;
		EXPECT_GT(at, before) << "a call that consumes nothing never ends: " << src;
	}
}

TEST_F(ParserTest, ADefectiveNodeNamesWhatWasLeftUnterminated) {
	// The diagnostic is worth the lookup: `lesh: syntax error` for `echo it's`
	// reads as a complaint about the script rather than about the apostrophe.
	const struct { std::string_view src; std::string_view detail; } cases[] = {
		{"echo it's", "unterminated quoted string"},
		{"echo \"x", "unterminated quoted string"},
		{"echo $(", "unterminated command substitution"},
		{"echo `", "unterminated command substitution"},
		{"echo $((1", "unterminated arithmetic expansion"},
		{"echo ${x", "unterminated parameter expansion"},
		// The defect is the TARGET, which is not a redirect node's first token.
		{"cat > \"x", "unterminated quoted string"},
		// A missing terminator has no defective TOKEN to name it - the construct's
		// own kind does, which is why error_detail asks the node first.
		{"( echo x", "unterminated subshell"},
		{"{ echo x", "unterminated brace group"},
		{"if true", "unterminated if command"},
		{"while true", "unterminated while loop"},
		{"until true", "unterminated until loop"},
		{"for i in 1", "unterminated for loop"},
		{"case a in", "unterminated case command"},
		{"case a in b", "unterminated case pattern list"},
	};
	for (const auto& c : cases) {
		const tree t = parse_it(c.src);
		const node_index at = t.first_error();
		ASSERT_NE(at, no_node) << c.src;
		ASSERT_NE(t.error_detail(t[at]), nullptr) << c.src;
		EXPECT_EQ(std::string_view(t.error_detail(t[at])), c.detail) << c.src;
	}
}

// --- a compound command with no operand (#58) --------------------------------
//
// #49's defect one production earlier: there the closing keyword was missing, here
// the thing between the keywords is. `compound_list` reduces to `term` and `term`
// to at least one `and_or`, so an EMPTY list is a syntax error at every position
// the grammar spells one - and lesh accepted all of them at status 0, TWO OF THEM
// by looping forever.

TEST_F(ParserTest, ACompoundCommandWithAnEmptyOperandIsADefect) {
	// Not incomplete: the token the parser wanted is present and is not end of
	// input, so no continuation helps. That is the same split record_missing draws
	// for a terminator, reached through the same call. dash reports 2 for every one.
	const std::string_view cases[] = {
		// The four from #58's table. An empty list runs to status 0, and 0 is exactly
		// what keeps a `while` going, which is why the second of them HUNG.
		"if; then echo x; fi",
		"while; do :; done",
		"until; do echo x; done",
		"for ; do echo x; done",
		// An empty BODY is the same production as an empty condition, and one of
		// these hung for the same reason.
		"if true; then fi",
		"if false; then a; else fi",         // the `else` part carries one too
		"if false; then a; elif b; then fi", // and so does every `elif`
		"while true; do done",
		"until true; do done",
		"for i in a; do done",
		"{ }",                               // `{ compound_list }` with no list
		"( )",
		"f() { }",                           // a function body is one of those two
		"f() ( )",
		"if true; then { }; fi",             // nested: the INNER construct is defective
	};
	for (const std::string_view src : cases) {
		const tree t = parse_it(src);
		EXPECT_TRUE(t.has_errors()) << "dash refuses this: " << src;
		EXPECT_FALSE(t.incomplete()) << "no continuation completes this: " << src;
	}
}

TEST_F(ParserTest, AnEmptyListIsLegitimateWhereThePOSIXGrammarAllowsOne) {
	// The over-eager half, and where `error-p.tst`'s 220 assertions would notice
	// first. Two positions take an empty list on purpose - a `case` item and
	// `program` itself - and a group holding only a REDIRECTION is a simple command
	// with no words rather than an empty list at all. dash runs all of these.
	const std::string_view cases[] = {
		"case a in b) ;; esac",        // the optional case_item compound_list
		"case a in b);; esac",
		"case a in b) esac",
		"case a in esac",              // no items at all
		"",                            // `program` may be empty
		"# nothing but a comment",
		"\n\n",
		"{ >/dev/null; }",             // only a redirection, which IS an and_or
		"( >/dev/null )",
		"for i in; do echo x; done",   // an empty WORDLIST is not an empty list
		"for i; do echo $i; done",     // ... and neither is no wordlist
		"while :; do :; done",
		"if if true; then true; fi; then echo x; fi",  // a list holding one compound
	};
	for (const std::string_view src : cases) {
		const tree t = parse_it(src);
		EXPECT_FALSE(t.has_errors()) << "this is valid POSIX: " << src;
	}
}

TEST_F(ParserTest, AMissingOperandNamesTheConstructItBelongsTo) {
	// The phrase lookup is also what keeps the two carriers of missing_operand
	// apart. On a compound node the kind names the construct. On the error node
	// that a bad redirection becomes, the kind names nothing, the lookup declines,
	// and `echo >` falls through to the token scan exactly as it did before - which
	// is why ADefectiveNodeNamesWhatWasLeftUnterminated needed no change.
	const struct { std::string_view src; std::string_view detail; } cases[] = {
		{"if; then echo x; fi",    "if command with an empty condition or body"},
		{"while; do :; done",      "while loop with an empty condition or body"},
		{"until; do echo x; done", "until loop with an empty condition or body"},
		{"for ; do echo x; done",  "for loop with no variable name or an empty body"},
		{"{ }",                    "empty brace group"},
		{"( )",                    "empty subshell"},
	};
	for (const auto& c : cases) {
		const tree t = parse_it(c.src);
		const node_index at = t.first_error();
		ASSERT_NE(at, no_node) << c.src;
		ASSERT_NE(t.error_detail(t[at]), nullptr) << c.src;
		EXPECT_EQ(std::string_view(t.error_detail(t[at])), c.detail) << c.src;
	}
}

// --- a word-list item spelled `do` or `in` (#65) -----------------------------
//
// The word list of a `for` is a list of WORDS, and `do`/`in` are reserved words
// only by POSITION - #19's rule, the same one that makes `echo done` print
// `done`. The word-list position is not a reserved-word position at all: only
// the token that follows a SEPARATOR after the list is, because that is where
// the grammar's `do_group` actually begins.

TEST_F(ParserTest, AWordListItemSpelledLikeAReservedWordIsAnOrdinaryWord) {
	// Every one of these is a word list holding a word that also spells a
	// reserved word elsewhere. dash, bash, zsh and yash all run every one of
	// them; lesh refused the first two before this fix - for-p.tst:35, :44 and
	// :117.
	const struct { std::string_view src; std::vector<std::string_view> words; } cases[] = {
		{"for i in in; do echo $i; done",            {"in"}},
		{"for i in done esac fi; do echo $i; done",  {"done", "esac", "fi"}},
		// `do` right after `in`, separated from the REAL `do` by a `;` rather
		// than a newline.
		{"for i in do; do echo $i; done",            {"do"}},
		// The same, separated by a newline instead - for-p.tst:35's shape.
		{"for i in do\ndo\necho $i\ndone",           {"do"}},
		// Two reserved spellings back to back - for-p.tst:44's shape.
		{"for word in do done\ndo\necho $word\ndone", {"do", "done"}},
	};
	for (const auto& c : cases) {
		const tree t = parse_it(c.src);
		EXPECT_FALSE(t.has_errors()) << "dash runs this: " << c.src;
		const node_index loop = t.child_of(t[t.root()], 0);
		ASSERT_EQ(t[loop].kind, node_kind::for_loop) << c.src;
		ASSERT_EQ(t[loop].children_count, c.words.size() + 1)
			<< "the word list plus the body: " << c.src;
		for (size_t i = 0; i < c.words.size(); ++i) {
			const node& w = t[t.child_of(t[loop], i)];
			EXPECT_EQ(w.kind, node_kind::word) << c.src;
			EXPECT_EQ(t.text_of(w), c.words[i]) << c.src;
		}
	}
}

TEST_F(ParserTest, AForLoopWithoutASeparatorBeforeDoIsStillASyntaxError) {
	// POSIX requires a `sequential_sep` (`;` or a newline) between the word list
	// and `do`, so an unseparated `do` is swallowed as one more WORD rather than
	// closing the list - and nothing then terminates the list, so dash, bash, zsh
	// and yash all refuse it. Verified against dash directly rather than guessed:
	// `for i in a b do echo $i; done` is `rc=2` there, with no output.
	for (const std::string_view src : {
	         "for i in a b do echo $i; done",
	         "for i in a b do; echo hi; done",
	     }) {
		const tree t = parse_it(src);
		EXPECT_TRUE(t.has_errors()) << "dash refuses this: " << src;
	}
}

TEST_F(ParserTest, TheOverEagerFixWouldHaveBrokenTheseAndMustNot) {
	// The boundary in the other direction: an ordinary word list, no `in` at
	// all, and a newline standing in for the `;` must all keep working exactly
	// as before.
	const std::string_view cases[] = {
		"for i in a b; do echo $i; done",   // still an ordinary word list
		"for i; do echo $i; done",          // no `in` - iterates $@
		"for i in a b\ndo echo $i; done",   // newline where `;` could stand
		"for i in a b; done",               // still missing `do` (#58)
	};
	for (const std::string_view src : cases) {
		const tree t = parse_it(src);
		if (src == "for i in a b; done")
			EXPECT_TRUE(t.has_errors()) << "#58's fix must hold: " << src;
		else
			EXPECT_FALSE(t.has_errors()) << "dash runs this: " << src;
	}
}

TEST_F(ParserTest, RunningOutOfInputBeforeAnOperandIsStillIncomplete) {
	// The third combination, and the one an interactive reader depends on: the
	// operand is empty AND the input ended, so more of it would help. A missing
	// operand must not cost the continuation prompt that ADR-0002 made the parser
	// total for - `while` on a line of its own is half-typed, not malformed.
	for (const std::string_view src : {"if", "while", "until", "for", "{", "(",
	                                   "while ;", "if ;", "for ;"}) {
		const tree t = parse_it(src);
		EXPECT_TRUE(t.has_errors()) << "a shell holding the whole input must refuse: " << src;
		EXPECT_TRUE(t.incomplete()) << "more input would complete it: " << src;
	}
}

TEST_F(ParserTest, AnUnterminatedWordStillEndsTheReadUnit) {
	// A parser that consumes nothing and returns is a hang, not a diagnostic, and
	// the incremental reader (#40) calls this in a loop until the cursor reaches
	// the end. The unterminated construct swallows the rest of the input, so the
	// cursor must land at the end in ONE call.
	for (const std::string_view src : {"echo one\necho \"two\necho three\n",
	                                   "echo one\necho $(\necho three\n"}) {
		size_t at = 0;
		const tree first = parse_next_command(pool, src, at);
		EXPECT_FALSE(first.has_errors()) << "the first line is well formed: " << src;
		ASSERT_GT(at, 0u) << src;

		const size_t before = at;
		const tree second = parse_next_command(pool, src, at);
		EXPECT_TRUE(second.has_errors()) << src;
		EXPECT_GT(at, before) << "a call that consumes nothing never ends: " << src;
		EXPECT_EQ(at, src.size()) << "the defect ran to the end of the input: " << src;
	}
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

TEST_F(ParserTest, TheForLoopNameIsNotAReservedWordPosition) {
	// POSIX 2.4 recognises a reserved word only where the grammar accepts one, and
	// the word after `for` is a NAME. So an alias called `in` really does apply
	// there, while the `in` the loop itself needs is still the keyword.
	fake_aliases aliases;
	aliases.define("f", " for ");
	aliases.define("w", " in ");
	aliases.define("in", " x ");
	const tree t = parse(pool, "f w in 1; do :; done", &aliases);
	node_index loop = no_node;
	for (node_index n = 0; n < t.node_count(); ++n)
		if (t[n].kind == node_kind::for_loop)
			loop = n;
	ASSERT_NE(loop, no_node);
	// aux packs the name token with the has_in flag in the top bit.
	EXPECT_NE(t[loop].aux & 0x80000000u, 0u) << "the loop must have an `in` clause";
	EXPECT_EQ(t.text_of_token(t.token_at(t[loop].aux & 0x7FFFFFFFu)), "x");
}

TEST_F(ParserTest, AKeywordTheGrammarCannotAcceptIsStillNotAnAlias) {
	// The converse, and the limit of the rule above: in a position where the word
	// IS reserved, the alias table is not consulted at all. yash substitutes here
	// and no other shell does - see the cases in tests/spec/harder.spec.
	fake_aliases aliases;
	aliases.define("forx", "for x ");
	aliases.define("do", ";");
	const tree t = parse(pool, "forx do echo $x; done", &aliases);
	node_index loop = no_node;
	for (node_index n = 0; n < t.node_count(); ++n)
		if (t[n].kind == node_kind::for_loop)
			loop = n;
	ASSERT_NE(loop, no_node);
	EXPECT_EQ(t[loop].aux & 0x80000000u, 0u) << "`do` stayed the keyword, so no `in`";
}

TEST_F(ParserTest, AFunctionDefinitionMayTakeItsParenthesesFromAnotherAlias) {
	// The name ends one body and `()` is a second alias the trailing blank makes
	// eligible, so the two-token lookahead has to cross the boundary AND substitute
	// as it goes. Probing the raw input saw the word `p` and refused the shape.
	fake_aliases aliases;
	aliases.define("f", "f ");
	aliases.define("p", "()");
	const tree t = parse(pool, "f p\n{ :; }\n", &aliases);
	bool found = false;
	for (node_index n = 0; n < t.node_count(); ++n)
		found = found || t[n].kind == node_kind::function_definition;
	EXPECT_TRUE(found) << "`f p` followed by a brace group defines a function";
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
