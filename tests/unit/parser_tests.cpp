#include "syntax/parser.h"
#include "syntax/source_map.h"

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

	// The body's command name carries its role (#103); the arguments stay
	// ordinary, and `echo *` inside a case still globs because the expander
	// treats command_name exactly as ordinary - the guarantee moved from "every
	// body word is ordinary" to the expander's enumerated one-value test.
	const node& body = t[t.child_of(item, item.aux)];
	const node& command = t[t.child_of(body, 0)];
	EXPECT_EQ(static_cast<word_role>(t[t.child_of(command, 0)].aux),
	          word_role::command_name);
	for (uint32_t i = 1; i < command.children_count; ++i) {
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

TEST_F(ParserTest, AHereDocumentBodyMayLiveInsideAnAliasBody) {
	// The operator is in one definition and the body in another, so the body's
	// lines follow a newline drawn from the alias rather than from the script. The
	// body's bytes then live in a registered text region above the input, which is
	// why here_doc_text reads through text_at rather than the source.
	fake_aliases aliases;
	aliases.define("c", "cat <<\\END");
	aliases.define("d", "c\nhere-document\nEND");
	const tree t = parse(pool, "d\n", &aliases);
	node_index doc = no_node;
	for (node_index n = 0; n < t.node_count(); ++n)
		if (t[n].kind == node_kind::here_doc)
			doc = n;
	ASSERT_NE(doc, no_node) << "the operator inside the alias body opened one";
	EXPECT_EQ(t.here_doc_text(t[doc].aux), "here-document\n");
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

// --- the offset -> (line, column) mapper (#76) --------------------------------
//
// Tested DIRECTLY rather than only through the shell. Three consumers ride on
// this one answer - `$LINENO`, the line in a runtime diagnostic and the column in
// it - so a defect here surfaces three times over, in three places that each look
// like a different bug.

TEST(SourceMapTest, TheFirstByteIsLineOneColumnOne) {
	const source_map map{"echo hi\n"};
	EXPECT_EQ(map.at(0).line, 1u);
	EXPECT_EQ(map.at(0).column, 1u);
}

TEST(SourceMapTest, ColumnsCountAlongTheLine) {
	const source_map map{"echo hi\n"};
	EXPECT_EQ(map.at(5).line, 1u);
	EXPECT_EQ(map.at(5).column, 6u);
}

TEST(SourceMapTest, TheByteAfterANewlineOpensTheNextLine) {
	const source_map map{"a\nbb\nccc\n"};
	EXPECT_EQ(map.at(2).line, 2u);
	EXPECT_EQ(map.at(2).column, 1u);
	EXPECT_EQ(map.at(5).line, 3u);
	EXPECT_EQ(map.at(5).column, 1u);
	EXPECT_EQ(map.at(7).line, 3u);
	EXPECT_EQ(map.at(7).column, 3u);
}

TEST(SourceMapTest, AnEmptyLineStillCounts) {
	// The `LINENO increments for each line` assertion turns on exactly this: the
	// blank third line of the yash case has no command on it and the fourth line
	// is still line 4.
	const source_map map{"echo $LINENO\necho $LINENO\n\necho $LINENO\n"};
	EXPECT_EQ(map.at(0).line, 1u);
	EXPECT_EQ(map.at(13).line, 2u);
	EXPECT_EQ(map.at(27).line, 4u);
}

TEST(SourceMapTest, AnOffsetPastTheEndClampsToTheEnd) {
	// A defect can be reported at a virtual offset above the input - see
	// tree::add_text_region - and a diagnostic that walked off the buffer looking
	// for its line would be worse than one that names the last line.
	const source_map map{"a\nb\n"};
	EXPECT_EQ(map.at(4).line, 3u);
	EXPECT_EQ(map.at(4).column, 1u);
	EXPECT_EQ(map.at(4000).line, 3u);
}

TEST(SourceMapTest, TheEmptySourceIsOneOne) {
	const source_map map{""};
	EXPECT_EQ(map.at(0).line, 1u);
	EXPECT_EQ(map.at(0).column, 1u);
}

TEST(SourceMapTest, ColumnsCountCHARACTERSRatherThanBytes) {
	// The one case that distinguishes the two. `é` is two bytes, so the space
	// after it is at BYTE 2 and at CHARACTER 2, and the `x` at byte 3 is the third
	// character rather than the fourth. Every tool that reads `file:line:col`
	// wants the second answer. See source_map's comment.
	const source_map map{"\xc3\xa9 x\n"};
	EXPECT_EQ(map.at(2).column, 2u) << "the space is the second character";
	EXPECT_EQ(map.at(3).column, 3u) << "and x the third";
}

TEST(SourceMapTest, ARepeatedQueryIsAnsweredFromWhereverExecutionLeftIt) {
	// The memo has to survive movement in BOTH directions, because a loop body run
	// twice asks about the same offsets again after the answer has moved past them.
	// Correctness first; the reason the mapper carries a memo at all is that
	// without one the backward step rescans from byte zero every iteration.
	const source_map map{"a\nb\nc\nd\ne\n"};
	EXPECT_EQ(map.at(8).line, 5u);
	EXPECT_EQ(map.at(2).line, 2u);
	EXPECT_EQ(map.at(8).line, 5u);
	EXPECT_EQ(map.at(0).line, 1u);
	EXPECT_EQ(map.at(6).line, 4u);
	EXPECT_EQ(map.at(4).line, 3u);
}

TEST_F(ParserTest, AnOffsetInsideAnAliasBodyResolvesToTheInvocationSite) {
	// #40's regions put typed text in [0, source.size()) and each alias body ABOVE
	// it, so "did this come from an alias?" is a comparison rather than a guess.
	// That is what makes the fallback an explicit rule: a token with no position in
	// the script is reported where the user can see it, at the word that ran.
	fake_aliases aliases;
	aliases.define("a", "nosuchcmd");
	const std::string_view source = "echo one\na\n";
	const tree t = parse(pool, source, &aliases);
	const node& cmd = t[t.child_of(t[t.root()], 1)];
	const uint32_t word = t.span_of(cmd).offset;
	ASSERT_GE(word, source.size()) << "the command word came from the alias body";

	const invocation_site site = t.invocation_of(word);
	EXPECT_EQ(site.offset, 9u) << "the `a` the user typed, on line 2";
	ASSERT_EQ(site.depth, 1u);
	EXPECT_EQ(site.chain[0], "a");
}

TEST_F(ParserTest, ANestedAliasReportsTheWholeChainOutermostFirst) {
	fake_aliases aliases;
	aliases.define("a", "b");
	aliases.define("b", "nosuchcmd");
	const tree t = parse(pool, "a\n", &aliases);
	const node& cmd = t[t.child_of(t[t.root()], 0)];
	const invocation_site site = t.invocation_of(t.span_of(cmd).offset);
	EXPECT_EQ(site.offset, 0u);
	ASSERT_EQ(site.depth, 2u);
	EXPECT_EQ(site.chain[0], "a");
	EXPECT_EQ(site.chain[1], "b");
}

TEST_F(ParserTest, AnOffsetInTheInputPassesThroughUnchanged) {
	const tree t = parse(pool, "echo hi\n");
	const invocation_site site = t.invocation_of(5);
	EXPECT_EQ(site.offset, 5u);
	EXPECT_EQ(site.depth, 0u);
}


// --- #103: what the highlighter reads ----------------------------------------

TEST_F(ParserTest, TheCommandNameCarriesItsRole) {
	const tree t = parse_it("x=1 echo a b");
	const node_index cmd = t.child_of(t[t.root()], 0);
	ASSERT_EQ(t[cmd].kind, node_kind::simple_command);
	ASSERT_EQ(t[cmd].children_count, 4u);
	EXPECT_EQ(t[t.child_of(t[cmd], 0)].kind, node_kind::assignment);
	const node& name = t[t.child_of(t[cmd], 1)];
	EXPECT_EQ(name.kind, node_kind::word);
	EXPECT_EQ(static_cast<word_role>(name.aux), word_role::command_name);
	EXPECT_EQ(t.text_of(name), "echo");
	// The arguments stay ordinary - the role marks exactly one word.
	EXPECT_EQ(static_cast<word_role>(t[t.child_of(t[cmd], 2)].aux), word_role::ordinary);
	EXPECT_EQ(static_cast<word_role>(t[t.child_of(t[cmd], 3)].aux), word_role::ordinary);
}

TEST_F(ParserTest, ARedirectTargetIsAWordNodeWithItsOwnRole) {
	const tree t = parse_it("echo x > out");
	const node_index cmd = t.child_of(t[t.root()], 0);
	ASSERT_EQ(t[cmd].children_count, 3u);
	const node& redirect = t[t.child_of(t[cmd], 2)];
	ASSERT_EQ(redirect.kind, node_kind::redirect);
	ASSERT_EQ(redirect.children_count, 1u) << "the target is a child word now";
	const node& target = t[t.child_of(redirect, 0)];
	EXPECT_EQ(target.kind, node_kind::word);
	EXPECT_EQ(static_cast<word_role>(target.aux), word_role::redirect_target);
	EXPECT_EQ(t.text_of(target), "out");
}

TEST_F(ParserTest, ACommentIsRecordedBesideTheTreeAndNotAsAToken) {
	const tree t = parse_it("echo hi # note");
	ASSERT_EQ(t.comment_count(), 1u);
	const span c = t.comment_at(0);
	EXPECT_EQ(t.source().substr(c.offset, c.length), "# note");
	for (uint32_t i = 0; i < t.token_count(); ++i)
		EXPECT_NE(t.token_at(i).kind, token_kind::comment)
			<< "trivia must stay out of the token array";
	// The command's own span is untouched by the trivia after it.
	const node_index cmd = t.child_of(t[t.root()], 0);
	EXPECT_EQ(t.text_of(t[cmd]), "echo hi");
	EXPECT_FALSE(t.has_errors());
}

TEST_F(ParserTest, ACommentAloneIsAnEmptyProgramWithOneRecordedSpan) {
	const tree t = parse_it("# only a comment");
	EXPECT_EQ(t[t.root()].children_count, 0u);
	ASSERT_EQ(t.comment_count(), 1u);
	EXPECT_FALSE(t.has_errors());
}

TEST_F(ParserTest, ReadingOneCommandAtATimeRecordsACommentExactlyOnce) {
	const std::string_view src = "echo a # c\necho b";
	size_t position = 0;
	size_t comments = 0;
	size_t commands = 0;
	while (position < src.size()) {
		const tree t = parse_next_command(pool, src, position);
		comments += t.comment_count();
		for (uint32_t i = 0; i < t[t.root()].children_count; ++i)
			commands += t[t.child_of(t[t.root()], i)].kind != node_kind::error;
	}
	EXPECT_EQ(commands, 2u);
	EXPECT_EQ(comments, 1u) << "the resume cursor must sit past the comment";
}

TEST_F(ParserTest, TheLexerEmitsACommentTokenWhereAWordCouldBegin) {
	lexer lx("echo # tail");
	EXPECT_EQ(lx.next().kind, token_kind::word);
	const token c = lx.next();
	EXPECT_EQ(c.kind, token_kind::comment);
	EXPECT_EQ(c.offset, 5u);
	EXPECT_EQ(c.length, 6u);
	EXPECT_EQ(lx.next().kind, token_kind::end);
	// Inside a word, `#` stays an ordinary byte.
	lexer inside("foo#bar");
	const token w = inside.next();
	EXPECT_EQ(w.kind, token_kind::word);
	EXPECT_EQ(w.length, 7u);
}

// --- #104: a command substitution's interior is parsed ------------------------
//
// `$(ls -l foo | grep bar)` used to reach the tree as one opaque segment of one
// word token: the pipeline inside painted as a blob. The interior is parsed now
// and recorded on a side table, the here-document and comment shape - see
// sub_parse in ast.h for why it is not a child of the word node.

namespace {

// The simple_command node of a one-command program.
const node& only_command(const tree& t) {
	return t[t.child_of(t[t.root()], 0)];
}

// True when any node of a recorded interior carries a defect.
bool interior_is_defective(const tree& t, const sub_parse& s) {
	for (uint32_t n = s.node_begin; n < s.node_end; ++n)
		if (tree::is_defective(t[n]))
			return true;
	return false;
}

// The text one recorded interior spans, out of the input it points into.
std::string_view interior_text(const tree& t, uint32_t i) {
	const sub_parse& s = t.sub_parse_at(i);
	return t.source().substr(s.interior.offset, s.interior.length);
}

} // namespace

TEST_F(ParserTest, ACommandSubstitutionInteriorIsParsedIntoTheSameTree) {
	const std::string_view src = "echo $(ls -l foo | grep bar)";
	const tree t = parse_it(src);
	ASSERT_EQ(t.sub_parse_count(), 1u);
	const sub_parse& s = t.sub_parse_at(0);

	// The span is the text BETWEEN the delimiters, at its real offset in the input.
	EXPECT_EQ(interior_text(t, 0), "ls -l foo | grep bar");
	EXPECT_EQ(s.depth, 0u);

	// The subtree is a program, and the pipeline the user typed is inside it.
	ASSERT_NE(s.root, no_node);
	ASSERT_EQ(t[s.root].kind, node_kind::program);
	ASSERT_EQ(t[s.root].children_count, 1u);
	const node& pipe = t[t.child_of(t[s.root], 0)];
	ASSERT_EQ(pipe.kind, node_kind::pipeline);
	ASSERT_EQ(pipe.children_count, 2u);
	EXPECT_EQ(t.text_of(t[t.child_of(pipe, 0)]), "ls -l foo");
	EXPECT_EQ(t.text_of(t[t.child_of(pipe, 1)]), "grep bar");

	// Every span in the subtree is an offset into what the user typed, so a
	// decoration lands on the right bytes.
	const node& grep = t[t.child_of(t[t.child_of(pipe, 1)], 0)];
	EXPECT_EQ(static_cast<word_role>(grep.aux), word_role::command_name);
	EXPECT_EQ(t.span_of(grep).offset, src.find("grep"));

	// And the word that holds it is still one word to everybody else.
	const node& cmd = only_command(t);
	ASSERT_EQ(cmd.children_count, 2u);
	const node& word = t[t.child_of(cmd, 1)];
	EXPECT_EQ(word.kind, node_kind::word);
	EXPECT_EQ(word.children_count, 0u) << "a word node stays a leaf";
	EXPECT_EQ(s.word_token, word.first_token);
	EXPECT_FALSE(t.has_errors());
	EXPECT_FALSE(t.incomplete());
}

TEST_F(ParserTest, ASubstitutionInsideASubstitutionIsParsedToo) {
	const tree t = parse_it("echo $(echo $(date))");
	ASSERT_EQ(t.sub_parse_count(), 2u);
	EXPECT_EQ(interior_text(t, 0), "echo $(date)");
	EXPECT_EQ(t.sub_parse_at(0).depth, 0u);
	EXPECT_EQ(interior_text(t, 1), "date");
	EXPECT_EQ(t.sub_parse_at(1).depth, 1u) << "one substitution encloses it";

	// The runs of nodes never overlap: an interior is parsed after the one that
	// contains it has finished, so a painter can walk either linearly.
	EXPECT_LE(t.sub_parse_at(0).node_end, t.sub_parse_at(1).node_begin);
	const node& inner = t[t.sub_parse_at(1).root];
	ASSERT_EQ(inner.children_count, 1u);
	EXPECT_EQ(t.text_of(t[t.child_of(inner, 0)]), "date");
}

TEST_F(ParserTest, ASubstitutionInsideDoubleQuotesIsFound) {
	const tree t = parse_it("echo \"a $(ls) b\"");
	ASSERT_EQ(t.sub_parse_count(), 1u);
	EXPECT_EQ(interior_text(t, 0), "ls");
}

TEST_F(ParserTest, TwoSubstitutionsInOneWordAreBothRecorded) {
	const tree t = parse_it("echo $(a)$(b)");
	ASSERT_EQ(t.sub_parse_count(), 2u);
	EXPECT_EQ(t.sub_parse_at(0).word_token, t.sub_parse_at(1).word_token)
		<< "one word holds both";
	EXPECT_EQ(interior_text(t, 0), "a");
	EXPECT_EQ(interior_text(t, 1), "b");
}

TEST_F(ParserTest, ASubstitutionInAnAssignmentOrARedirectTargetIsFound) {
	const tree assigned = parse_it("x=$(ls)");
	ASSERT_EQ(assigned.sub_parse_count(), 1u);
	EXPECT_EQ(interior_text(assigned, 0), "ls");

	const tree target = parse_it("echo x > $(dir)/f");
	ASSERT_EQ(target.sub_parse_count(), 1u);
	EXPECT_EQ(interior_text(target, 0), "dir");
}

TEST_F(ParserTest, ADefectInsideAnInteriorIsNotThisCommandsDefect) {
	// The interior is a syntax error and the command containing it is not. The
	// executor re-parses interiors through the expander and reports this at
	// expansion time, at status 2 (#57); letting it into has_errors() would stop
	// the outer command from running at all.
	const tree t = parse_it("echo $(if true)");
	EXPECT_FALSE(t.has_errors()) << "the outer command is well formed";
	EXPECT_FALSE(t.incomplete()) << "the input the user typed is finished";
	ASSERT_EQ(t.sub_parse_count(), 1u);
	EXPECT_TRUE(interior_is_defective(t, t.sub_parse_at(0)))
		<< "the defect is still recorded, on the interior's own nodes";
}

TEST_F(ParserTest, AnUnterminatedSubstitutionKeepsItsDefectAndStillParsesWhatIsThere) {
	// `echo $(ls -l` - the WORD carries the defect, exactly as before, and the
	// interior is parsed anyway so a line editor has something to paint.
	const tree t = parse_it("echo $(ls -l");
	EXPECT_TRUE(t.has_errors()) << "the word token is unterminated";
	EXPECT_TRUE(t.incomplete()) << "more input would close it";
	EXPECT_STREQ(t.error_detail(t[t.first_error()]), "unterminated command substitution");
	ASSERT_EQ(t.sub_parse_count(), 1u);
	const sub_parse& s = t.sub_parse_at(0);
	EXPECT_EQ(interior_text(t, 0), "ls -l")
		<< "the interior runs to the end of what was typed";
	ASSERT_NE(s.root, no_node);
	ASSERT_EQ(t[s.root].children_count, 1u);
	EXPECT_EQ(t.text_of(t[t.child_of(t[s.root], 0)]), "ls -l");
}

TEST_F(ParserTest, ABackquotedInteriorIsParsedUntilABackslashMakesItsSpansALie) {
	// POSIX 2.6.3 removes a `\` before `$`, a backquote or another `\` BEFORE the
	// body is shell input, so an escaped body is a rewritten string rather than a
	// run of input bytes - and every span here has to be a real input offset.
	const tree t = parse_it("echo `ls -l`");
	ASSERT_EQ(t.sub_parse_count(), 1u);
	EXPECT_EQ(interior_text(t, 0), "ls -l");
	EXPECT_EQ(t[t.sub_parse_at(0).root].children_count, 1u);

	// With a backslash in it, the segment stays opaque - the answer it gave before
	// this existed.
	EXPECT_EQ(parse_it("echo `echo \\`date\\``").sub_parse_count(), 0u);
}

TEST_F(ParserTest, ArithmeticAndParameterInteriorsStayOpaque) {
	// `$(( ))`'s mini-parser lives in the expander (#30/#56) and is not the shell
	// grammar, so this parser has no subtree to hang there.
	EXPECT_EQ(parse_it("echo $((1 + 2))").sub_parse_count(), 0u);
	// `${x:-$(ls)}` is a limitation of this version: the `${...}` interior has its
	// own grammar the parser does not model, and guessing where the word begins
	// would put spans on text by position rather than by rule.
	EXPECT_EQ(parse_it("echo ${x:-$(ls)}").sub_parse_count(), 0u);
}

TEST_F(ParserTest, NestingStopsAtTheDepthCeiling) {
	std::string src = "echo ";
	for (uint32_t i = 0; i < kMaxSubParseDepth + 8; ++i)
		src += "$(";
	src += "x";
	for (uint32_t i = 0; i < kMaxSubParseDepth + 8; ++i)
		src += ")";
	const tree t = parse_it(src);
	EXPECT_EQ(t.sub_parse_count(), kMaxSubParseDepth)
		<< "the parser stops following where the lexer and the expander do";
	for (uint32_t i = 0; i < t.sub_parse_count(); ++i)
		EXPECT_EQ(t.sub_parse_at(i).depth, i);
}

TEST_F(ParserTest, AWordWithNoSubstitutionRecordsNothing) {
	// flag_literal keeps the scan off the common path entirely.
	EXPECT_EQ(parse_it("echo hello world").sub_parse_count(), 0u);
	EXPECT_EQ(parse_it("echo 'a $(b)'").sub_parse_count(), 0u)
		<< "single quotes suppress the substitution";
	EXPECT_EQ(parse_it("echo $x").sub_parse_count(), 0u);
}

TEST_F(ParserTest, ReadingOneCommandAtATimeParsesInteriorsAndResumesCorrectly) {
	// The interior parse runs after the command has been read, so it must not move
	// the cursor the next read starts from.
	const std::string_view src = "echo $(a b)\necho second\n";
	size_t position = 0;
	const tree one = parse_next_command(pool, src, position);
	EXPECT_EQ(one.sub_parse_count(), 1u);
	EXPECT_EQ(position, 12u) << "past the newline, not past the interior";
	const tree two = parse_next_command(pool, src, position);
	EXPECT_EQ(two.sub_parse_count(), 0u);
	EXPECT_EQ(two.text_of(only_command(two)), "echo second");
}

TEST_F(ParserTest, TheInteriorExtentIsAskedOfTheLexerDirectly) {
	// C-6: a client holding a token and the bytes it came from can ask without a
	// parse. The `$\<newline>(` spelling is why the opener is looked for rather
	// than assumed two bytes wide.
	const std::string_view src = "$\\\n(ls)";
	lexer lx(src);
	const token seg = lx.next(lex_mode::word_interior);
	ASSERT_EQ(seg.kind, token_kind::seg_command_sub);
	uint32_t begin = 0, end = 0;
	ASSERT_TRUE(command_sub_interior(src, seg, begin, end));
	EXPECT_EQ(src.substr(begin, end - begin), "ls");
}

// --- #105: keyword tokens are marked where the parser recognises them --------

namespace {

// The text of every token the parser accepted as a reserved word, in the order
// the tokens were recorded. Source order for a top-level parse; a command
// substitution's interior is parsed afterwards, so its keywords come last.
std::vector<std::string_view> keywords_of(const tree& t) {
	std::vector<std::string_view> out;
	for (uint32_t i = 0; i < static_cast<uint32_t>(t.token_count()); ++i)
		if ((t.token_at(i).flags & flag_keyword) != 0)
			out.push_back(t.text_of_token(t.token_at(i)));
	return out;
}

} // namespace

TEST_F(ParserTest, EveryKeywordPositionIsFlagged) {
	EXPECT_EQ(keywords_of(parse_it("if a; then b; elif c; then d; else e; fi")),
	          (std::vector<std::string_view>{"if", "then", "elif", "then", "else", "fi"}));
	EXPECT_EQ(keywords_of(parse_it("while a; do b; done")),
	          (std::vector<std::string_view>{"while", "do", "done"}));
	EXPECT_EQ(keywords_of(parse_it("until a; do b; done")),
	          (std::vector<std::string_view>{"until", "do", "done"}));
	EXPECT_EQ(keywords_of(parse_it("for i in 1; do :; done")),
	          (std::vector<std::string_view>{"for", "in", "do", "done"}));
	EXPECT_EQ(keywords_of(parse_it("{ :; }")),
	          (std::vector<std::string_view>{"{", "}"}));
	EXPECT_EQ(keywords_of(parse_it("f() { :; }")),
	          (std::vector<std::string_view>{"{", "}"})) << "the name is not one";
}

TEST_F(ParserTest, AReservedWordUsedAsAnArgumentIsNotFlagged) {
	// The whole reason this is a flag and not a token kind: `echo done` prints
	// `done`, and the lexer cannot tell the two apart because the bytes are the
	// same. Only position decides, which is knowledge the parser has and nobody
	// downstream of it does.
	EXPECT_TRUE(keywords_of(parse_it("echo done")).empty());
	EXPECT_TRUE(keywords_of(parse_it("echo if then else fi esac do")).empty());
	EXPECT_TRUE(keywords_of(parse_it("echo { }")).empty());
	// A quoted word in COMMAND position is not a keyword either - it is a command
	// called `if`, which is what peek_reserved's flag_literal test already says.
	EXPECT_TRUE(keywords_of(parse_it("\"if\" true")).empty());
}

TEST_F(ParserTest, ACaseClauseFlagsItsCaseInAndEsac) {
	EXPECT_EQ(keywords_of(parse_it("case x in a) ;; esac")),
	          (std::vector<std::string_view>{"case", "in", "esac"}));
	// A subject and a pattern are WORDS wherever they sit: `case in in in) ;; esac`
	// spells `in` three times and only the middle one is the grammar's.
	EXPECT_EQ(keywords_of(parse_it("case in in in) ;; esac")),
	          (std::vector<std::string_view>{"case", "in", "esac"}));
	// A body puts its words back in command position, so a keyword there is one.
	EXPECT_EQ(keywords_of(parse_it("case x in a) if b; then c; fi ;; esac")),
	          (std::vector<std::string_view>{"case", "in", "if", "then", "fi", "esac"}));
}

TEST_F(ParserTest, AForLoopsWordListHoldsWordsAndNotKeywords) {
	// `for i in in do done` names three words; the `do` that follows is the loop's.
	EXPECT_EQ(keywords_of(parse_it("for i in in do done; do :; done")),
	          (std::vector<std::string_view>{"for", "in", "do", "done"}));
}

TEST_F(ParserTest, TheNegationWordIsAKeyword) {
	EXPECT_EQ(keywords_of(parse_it("! true")),
	          (std::vector<std::string_view>{"!"}));
	EXPECT_EQ(keywords_of(parse_it("! a | b")),
	          (std::vector<std::string_view>{"!"})) << "one Bang, on the pipeline";
	EXPECT_TRUE(keywords_of(parse_it("echo !")).empty());
}

TEST_F(ParserTest, KeywordsInsideACommandSubstitutionAreFlaggedToo) {
	// #104 parses an interior with the same code, so this costs nothing - but it is
	// the case a painter of `$(...)` needs, and the one a second mechanism bolted
	// on beside the parser would have missed.
	EXPECT_EQ(keywords_of(parse_it("echo $(if a; then b; fi)")),
	          (std::vector<std::string_view>{"if", "then", "fi"}));
	EXPECT_EQ(keywords_of(parse_it("x=$(while a; do b; done)")),
	          (std::vector<std::string_view>{"while", "do", "done"}));
	EXPECT_TRUE(keywords_of(parse_it("echo $(echo done)")).empty());
	// Nesting: the top-level parse is recorded first, the interiors after it.
	EXPECT_EQ(keywords_of(parse_it("if a; then echo $(until b; do c; done); fi")),
	          (std::vector<std::string_view>{"if", "then", "fi", "until", "do", "done"}));
}

TEST_F(ParserTest, AWordTheParserNeverAcceptedIsNotFlagged) {
	// at_list_terminator and parse_case's `esac` test both PEEK at a keyword and
	// leave it for a later accept to take. A defective construct is where that
	// distinction shows: nothing accepts the second `fi`, so nothing marks it.
	EXPECT_EQ(keywords_of(parse_it("if a; then b; fi fi")),
	          (std::vector<std::string_view>{"if", "then", "fi"}));
	EXPECT_TRUE(keywords_of(parse_it("esac")).empty())
		<< "a bare `esac` is a command called esac, as dash runs it";
}

TEST_F(ParserTest, TheKeywordFlagIsAdditiveAndLeavesTheOtherBitsAlone) {
	// A new bit must not disturb the two the lexer sets - every read of `flags` in
	// the tree is a masked test, and this pins that it stays that way.
	const tree t = parse_it("if a; then b; fi");
	const token& kw = t.token_at(0);
	EXPECT_EQ(t.text_of_token(kw), "if");
	EXPECT_NE(kw.flags & flag_keyword, 0);
	EXPECT_NE(kw.flags & flag_literal, 0) << "still a plain literal word";
	EXPECT_EQ(sizeof(token), 16u);
}
