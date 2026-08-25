#include "syntax/lexer.h"

#include <gtest/gtest.h>

#include <string>
#include <tuple>
#include <vector>

using namespace lesh::syntax;

namespace {

struct Lexed {
	token_kind kind;
	std::string text;
};

std::vector<Lexed> lex_all(std::string_view src) {
	lexer lx{src};
	std::vector<Lexed> out;
	for (;;) {
		const token t = lx.next();
		if (t.kind == token_kind::end)
			break;
		out.push_back({t.kind, std::string(lx.text(t))});
	}
	return out;
}

std::vector<token_kind> kinds_of(std::string_view src) {
	std::vector<token_kind> out;
	for (const auto& l : lex_all(src))
		out.push_back(l.kind);
	return out;
}

} // namespace

TEST(Lexer, EmptyInputIsImmediatelyEnd) {
	lexer lx{""};
	EXPECT_EQ(lx.next().kind, token_kind::end);
	EXPECT_FALSE(lx.incomplete());
}

TEST(Lexer, SimpleCommandSplitsIntoWords) {
	const auto got = lex_all("echo hello world");
	ASSERT_EQ(got.size(), 3u);
	EXPECT_EQ(got[0].text, "echo");
	EXPECT_EQ(got[1].text, "hello");
	EXPECT_EQ(got[2].text, "world");
	for (const auto& l : got)
		EXPECT_EQ(l.kind, token_kind::word);
}

TEST(Lexer, DoesNotModifyItsSource) {
	// The whole point of contract 1: a highlighter runs this on the editor's own
	// buffer, on every keystroke.
	std::string src = "echo 'hi' | cat";
	const std::string before = src;
	lexer lx{src};
	while (lx.next().kind != token_kind::end) {}
	EXPECT_EQ(src, before);
}

TEST(Lexer, IsRestartableFromAnyOffset) {
	const std::string src = "echo hello";
	lexer from_start{src};
	std::ignore = from_start.next();  // consume "echo"
	const token a = from_start.next();

	lexer from_offset{src, 5};
	const token b = from_offset.next();

	EXPECT_EQ(a.offset, b.offset);
	EXPECT_EQ(a.length, b.length);
}

TEST(Lexer, RecognisesMultiCharacterOperatorsGreedily) {
	EXPECT_EQ(kinds_of("a && b"),
	          (std::vector{token_kind::word, token_kind::and_if, token_kind::word}));
	EXPECT_EQ(kinds_of("a || b"),
	          (std::vector{token_kind::word, token_kind::or_if, token_kind::word}));
	EXPECT_EQ(kinds_of("a | b"),
	          (std::vector{token_kind::word, token_kind::pipe, token_kind::word}));
	EXPECT_EQ(kinds_of("a >> b"),
	          (std::vector{token_kind::word, token_kind::dgreat, token_kind::word}));
	EXPECT_EQ(kinds_of("a <<- b"),
	          (std::vector{token_kind::word, token_kind::dless_dash, token_kind::word}));
	EXPECT_EQ(kinds_of("a ;; b"),
	          (std::vector{token_kind::word, token_kind::dsemi, token_kind::word}));
}

// POSIX.1-2024's case fallthrough. Before this it lexed as `semi` then `amp`,
// which is indistinguishable from `foo; &bar` at the token level - the parser
// would have had to glue two tokens back together to tell them apart, and the
// lexer owns no memory to do that with.
TEST(Lexer, SemicolonAmpersandIsItsOwnToken) {
	EXPECT_EQ(kinds_of("a ;& b"),
	          (std::vector{token_kind::word, token_kind::semi_and, token_kind::word}));
	EXPECT_EQ(kinds_of("a;&b"),
	          (std::vector{token_kind::word, token_kind::semi_and, token_kind::word}));
}

// A `;&` token must not eat an ordinary `;` followed by a background command,
// nor a plain `;;`. The two other separators near it - `;` alone and `&`
// alone - have to keep meaning what they meant before this token existed.
TEST(Lexer, SemicolonAmpersandDoesNotSwallowOrdinarySeparators) {
	EXPECT_EQ(kinds_of("a; &b"),
	          (std::vector{token_kind::word, token_kind::semi, token_kind::amp,
	                       token_kind::word}));
	EXPECT_EQ(kinds_of("a & b"),
	          (std::vector{token_kind::word, token_kind::amp, token_kind::word}));
	EXPECT_EQ(kinds_of("a;;b"),
	          (std::vector{token_kind::word, token_kind::dsemi, token_kind::word}));
}

TEST(Lexer, OperatorsSplitWordsWithoutBlanks) {
	EXPECT_EQ(kinds_of("a|b"),
	          (std::vector{token_kind::word, token_kind::pipe, token_kind::word}));
}

TEST(Lexer, NewlineIsItsOwnToken) {
	EXPECT_EQ(kinds_of("a\nb"),
	          (std::vector{token_kind::word, token_kind::newline, token_kind::word}));
}

TEST(Lexer, IoNumberOnlyWhenARedirectionFollowsImmediately) {
	// `2>file` redirects fd 2; `2 >file` passes 2 as an argument.
	EXPECT_EQ(kinds_of("2>f"),
	          (std::vector{token_kind::io_number, token_kind::great, token_kind::word}));
	EXPECT_EQ(kinds_of("2 >f"),
	          (std::vector{token_kind::word, token_kind::great, token_kind::word}));
}

TEST(Lexer, QuotedBlanksDoNotSplitWords) {
	const auto got = lex_all("echo 'a b' \"c d\"");
	ASSERT_EQ(got.size(), 3u);
	EXPECT_EQ(got[1].text, "'a b'");
	EXPECT_EQ(got[2].text, "\"c d\"");
}

TEST(Lexer, QuotedOperatorsAreNotOperators) {
	const auto got = lex_all("echo '|' \"&&\"");
	ASSERT_EQ(got.size(), 3u);
	EXPECT_EQ(got[1].kind, token_kind::word);
	EXPECT_EQ(got[2].kind, token_kind::word);
}

TEST(Lexer, BackslashEscapesTheNextByte) {
	const auto got = lex_all("a\\ b");
	ASSERT_EQ(got.size(), 1u) << "an escaped blank must not split the word";
	EXPECT_EQ(got[0].text, "a\\ b");
}

TEST(Lexer, CommentRunsToEndOfLine) {
	EXPECT_EQ(kinds_of("echo # not a word\nnext"),
	          (std::vector{token_kind::word, token_kind::newline, token_kind::word}));
}

TEST(Lexer, HashInsideAWordIsLiteral) {
	const auto got = lex_all("a#b");
	ASSERT_EQ(got.size(), 1u);
	EXPECT_EQ(got[0].text, "a#b");
}

TEST(Lexer, LiteralFlagMarksWordsThatNeedNoExpansion) {
	lexer plain{"hello"};
	EXPECT_TRUE(plain.next().flags & flag_literal);

	for (std::string_view src : {"$x", "a`b`", "a*", "a?", "a[b]", "~x", "'q'", "\"q\"", "a\\b"}) {
		lexer lx{src};
		EXPECT_FALSE(lx.next().flags & flag_literal) << "source: " << src;
	}
}

TEST(Lexer, BlankSeparationIsRecorded) {
	lexer lx{"a b"};
	const token first = lx.next();
	const token second = lx.next();
	EXPECT_FALSE(first.flags & flag_preceded_by_blank);
	EXPECT_TRUE(second.flags & flag_preceded_by_blank);
}

// --- errors are data, and incomplete is not an error -------------------------

TEST(Lexer, UnterminatedSingleQuoteIsIncompleteNotMalformed) {
	lexer lx{"echo 'abc"};
	std::ignore = lx.next();
	const token t = lx.next();
	EXPECT_EQ(t.kind, token_kind::word);
	EXPECT_EQ(t.error, token_error::unterminated_single_quote);
	EXPECT_TRUE(lx.incomplete()) << "an interactive shell answers this with a continuation prompt";
	EXPECT_EQ(t.error_offset, 5u) << "points at the opening quote, not the end of input";
}

TEST(Lexer, UnterminatedDoubleQuoteIsIncompleteNotMalformed) {
	lexer lx{"echo \"abc"};
	std::ignore = lx.next();
	const token t = lx.next();
	EXPECT_EQ(t.error, token_error::unterminated_double_quote);
	EXPECT_TRUE(lx.incomplete());
}

// An unterminated expansion is a DEFECT and not merely incomplete. Reported as
// incomplete alone, it reached the executor and ran: `lesh -c 'echo $('` printed
// nothing and exited zero, and `echo $((1` recursed until the stack ran out (#47).
TEST(Lexer, UnterminatedExpansionsCarryTheirOwnError) {
	const struct { const char* src; token_error error; } cases[] = {
		{"echo $(x", token_error::unterminated_command_sub},
		{"echo `x", token_error::unterminated_backquote},
		{"echo $((1", token_error::unterminated_arithmetic},
		{"echo ${x", token_error::unterminated_parameter_expansion},
	};
	for (const auto& c : cases) {
		lexer lx{c.src};
		std::ignore = lx.next();
		const token t = lx.next();
		EXPECT_EQ(t.kind, token_kind::word) << c.src;
		EXPECT_EQ(t.error, c.error) << c.src;
		EXPECT_EQ(t.error_offset, 5u) << "points at what opened it: " << c.src;
		EXPECT_TRUE(lx.incomplete()) << c.src;
	}
}

// The same claim from the interior scan, which is the one the expander and a
// highlighter run. Making it in only one of the two scans is how `echo $(`
// escaped: the word carried no error at all.
TEST(Lexer, WordInteriorReportsUnterminatedExpansionsToo) {
	const struct { const char* src; token_error error; } cases[] = {
		{"$(x", token_error::unterminated_command_sub},
		{"$((1", token_error::unterminated_arithmetic},
		{"${x", token_error::unterminated_parameter_expansion},
	};
	for (const auto& c : cases) {
		lexer lx{c.src};
		const token t = lx.next(lex_mode::word_interior);
		EXPECT_EQ(t.error, c.error) << c.src;
		EXPECT_TRUE(lx.incomplete()) << c.src;
	}
}

TEST(Lexer, TrailingBackslashIsIncompleteWithoutBeingAnError) {
	lexer lx{"echo a\\"};
	std::ignore = lx.next();
	const token t = lx.next();
	EXPECT_EQ(t.error, token_error::none) << "a line continuation is not malformed";
	EXPECT_TRUE(lx.incomplete());
}

// POSIX 2.2.1 removes `\<newline>` BEFORE the input is tokenised, so every
// lookahead has to look past one. Read literally, an operator split across a
// continuation was two operators, a reserved word was an ordinary word, and `(`
// terminated the word it was supposed to open - fifteen of quote-p.tst's cases.
TEST(Lexer, AnOperatorMayBeSplitByLineContinuations) {
	const std::vector<std::pair<std::string, token_kind>> cases = {
		{">\\\n>", token_kind::dgreat},
		{"<\\\n<", token_kind::dless},
		{"<\\\n<\\\n-", token_kind::dless_dash},
		{">\\\n|", token_kind::clobber},
		{">\\\n&", token_kind::great_and},
		{"<\\\n&", token_kind::less_and},
		{"<\\\n>", token_kind::less_great},
		{"&\\\n&", token_kind::and_if},
		{"|\\\n|", token_kind::or_if},
		{";\\\n;", token_kind::dsemi},
		{";\\\n&", token_kind::semi_and},
	};
	for (const auto& [src, kind] : cases) {
		lexer lx{src};
		const token t = lx.next();
		EXPECT_EQ(t.kind, kind) << src;
		EXPECT_EQ(t.length, src.size()) << src << ": the token SPANS the continuation";
	}
}

TEST(Lexer, ALineContinuationBetweenTokensIsNothingAtAll) {
	// Left in place it began a WORD, so `\<newline>{` lexed as one word rather than
	// as the reserved `{` and the shell looked for a command called `{`.
	const auto tokens = lex_all("\\\n{\\\n echo 1\\\n;\\\n}");
	ASSERT_GE(tokens.size(), 1u);
	EXPECT_EQ(tokens[0].kind, token_kind::word);
	EXPECT_EQ(tokens[0].text, "{\\\n") << "the word is `{` once the continuation goes";
}

TEST(Lexer, AnIoNumberSurvivesAContinuationBeforeItsOperator) {
	// `3\<newline>>\<newline>>redir` is `3>>redir`. Read as a word the `3` became
	// an ARGUMENT and the redirection landed on stdout, so a later `>&3` reported
	// "not open for output".
	lexer lx{"3\\\n>\\\n>redir"};
	const token t = lx.next();
	EXPECT_EQ(t.kind, token_kind::io_number);
	const token op = lx.next();
	EXPECT_EQ(op.kind, token_kind::dgreat);
}

TEST(Lexer, ADollarLooksPastAContinuationForItsBrace) {
	// `(` is a word TERMINATOR, so this one is not cosmetic: `$\<newline>((1+2))`
	// ended the word at the paren and parsed as a subshell.
	for (const std::string src : {"$\\\n{f}", "$\\\n(echo 1)", "$\\\n(\\\n(1+2))"}) {
		const auto tokens = lex_all(src);
		ASSERT_EQ(tokens.size(), 1u) << src;
		EXPECT_EQ(tokens[0].kind, token_kind::word) << src;
		EXPECT_EQ(tokens[0].text, src) << src << ": one word, not a word and an operator";
	}
}

// One scan looking for one delimiter walked straight through nested constructs
// that could contain it. Four scans had the defect and it cost five assertions
// across quote-p.tst, param-p.tst and cmdsub-p.tst (#42).
TEST(Lexer, AQuotedStringSurvivesQuotesInsideASubstitution) {
	for (const std::string src : {"\"$(echo \"x\")\"", "\"`echo \"x\"`\"",
	                             "\"${e=a\"b\"c}\"", "\"`echo \"a\"'b'`\""}) {
		const auto tokens = lex_all(src);
		ASSERT_EQ(tokens.size(), 1u) << src;
		EXPECT_EQ(tokens[0].text, src) << src << ": one word, not three";
		EXPECT_EQ(tokens[0].kind, token_kind::word) << src;
	}
}

TEST(Lexer, ABraceInsideQuotesDoesNotCloseAnExpansion) {
	// `${a+\}}` ends at the SECOND brace and `${e=a"b"c}` at the last: counted
	// braces alone stopped at whichever came first and left the rest of the word
	// as literal text.
	for (const std::string src : {"${a+'}'}", "${a+\"}\"}", "${a+\\}}", "${e=a\"b\"c}"}) {
		const auto tokens = lex_all(src);
		ASSERT_EQ(tokens.size(), 1u) << src;
		EXPECT_EQ(tokens[0].text, src) << src;
	}
}

TEST(Lexer, ASingleQuoteInsideBracesInsideDoubleQuotesIsAnOrdinaryByte) {
	// Not derivable from the bytes: `"${x-'}"` is a complete word - dash prints one
	// single quote at status zero - while `${x-'}` on its own is unterminated,
	// because there the quote IS a quote. The `${...}` body inherits the context;
	// a `$(...)` starts the shell language over and does not.
	lexer inside_quotes{"\"${x-'}\""};
	EXPECT_EQ(inside_quotes.next().error, token_error::none);

	lexer bare{"${x-'}"};
	const token t = bare.next();
	EXPECT_EQ(t.error, token_error::unterminated_parameter_expansion)
		<< "outside double quotes the same bytes never close";
}

TEST(Lexer, AParenInsideQuotesOrACommentDoesNotCloseASubstitution) {
	// Counted parens alone ended `$(echo ')')` inside the quotes and left the real
	// closing paren to open a new word, so lesh reported an unterminated quoted
	// string on input dash runs. A `#` where a word could begin opens a comment
	// there, and the paren inside it is text.
	for (const std::string src : {"$(echo ')')", "$(echo \")\")",
	                             "$(\necho a # ) comment\n)", "$(echo $(echo n))"}) {
		const auto tokens = lex_all(src);
		ASSERT_EQ(tokens.size(), 1u) << src;
		EXPECT_EQ(tokens[0].text, src) << src;
	}
}

// A `)` that ends a case pattern list belongs to the CASE, not to the
// substitution around it. Counting parens alone ended `$(case a in a) ...)` at
// the pattern's paren and left the rest of the clause to be read as ordinary
// words (#68). The optional leading `(` of a pattern list happened to balance,
// so only a clause written without one - `*)`, which every case has - showed it.
TEST(Lexer, ACasePatternListsParenDoesNotCloseASubstitution) {
	for (const std::string src : {
	         "$(case a in a) echo x;; esac)",
	         "$(case a in (a) echo x;; *) echo y;; esac)",
	         "$(\ncase a in\n(a) echo x;;\n *) echo not reached;;\nesac\n)",
	         "$(case a in a) case b in b) echo x;; esac;; esac)",
	         "$(case a in a|b) echo x;; esac)",
	         "$(case a in a) (echo x);; esac)",
	         "$(case a in a) echo $(echo x);; esac)",
	     }) {
		const auto tokens = lex_all(src);
		ASSERT_EQ(tokens.size(), 1u) << src;
		EXPECT_EQ(tokens[0].text, src) << src;
	}
}

// The other half of the same claim: `case` and `esac` are reserved only where a
// command could begin, so a scan that reacted to the BYTES would break input that
// works today. `$(echo case)` is one word and `$(echo esac)` is another.
TEST(Lexer, CaseAndEsacAwayFromCommandPositionAreOrdinaryWords) {
	for (const std::string src : {"$(echo case)", "$(echo esac)", "$(echo in)",
	                              "$(case)", "$(echo $(echo a) case)",
	                              "$(case a in a) echo esac;; esac)",
	                              "$(echo a; echo case in esac)"}) {
		const auto tokens = lex_all(src);
		ASSERT_EQ(tokens.size(), 1u) << src;
		EXPECT_EQ(tokens[0].text, src) << src;
	}
}

// A here-document BODY is data, so a `)` in it closes nothing. The scan skipped
// straight through the body looking for a paren and ended the substitution inside
// it, which is `cmdsub-p.tst`'s 'here-document in command substitution' (#68).
TEST(Lexer, AHereDocumentBodyInsideASubstitutionIsNotScannedForAParen) {
	for (const std::string src : {
	         "$(cat <<\\END\nfoo)\nEND\n)",
	         "$(cat <<'END'\nfoo)\nEND\n)",
	         "$(cat <<\"END\"\nfoo)\nEND\n)",
	         "$(cat <<END\nfoo)\nEND\n)",
	         "$(cat <<-END\n\tfoo)\n\tEND\n)",
	         "$(cat <<END\n$( ` ) '\nEND\n)",
	         "$(cat <<END\nENDING\nEND\n)",
	         "$(cat <<A <<B\nx)\nA\ny)\nB\n)",
	         "$(cat <<A; cat <<B\nx)\nA\ny)\nB\n)",
	     }) {
		const auto tokens = lex_all(src);
		ASSERT_EQ(tokens.size(), 1u) << src;
		EXPECT_EQ(tokens[0].text, src) << src;
	}
}

// A here-document operator INSIDE a substitution belongs to the substitution's
// own line, and one outside it belongs to the enclosing script. The scan must not
// take the outer body for its own: `cat <<OUTER; echo "$(cat <<INNER ...)"` has
// one body inside the parens and one after them.
TEST(Lexer, AnOuterHereDocumentSurvivesASubstitutionThatHasOneOfItsOwn) {
	const std::string src =
		"cat <<\\OUTER; echoraw \"$(cat <<\\INNER\ninner\nINNER\n)\"\nouter\nOUTER\n";
	bool found = false;
	for (const auto& t : lex_all(src))
		if (t.text.compare(0, 2, "\"$") == 0) {
			found = true;
			EXPECT_EQ(t.text, "\"$(cat <<\\INNER\ninner\nINNER\n)\"")
				<< "the substitution ends at its own paren, not inside INNER's body";
		}
	EXPECT_TRUE(found) << "the quoted word carrying the substitution was never lexed";
}

// Arithmetic is not a command list: `<<` there is a SHIFT and `case` cannot
// appear at all. Reading `$((1<<2))`'s operator as a here-document would have
// swallowed the rest of the input looking for a body.
TEST(Lexer, ArithmeticIsNotScannedAsACommandList) {
	for (const std::string src : {"$((1<<2))", "$((1+2))", "$((x<<y))",
	                              "$(( (1<<2) + 3 ))", "$(echo $((1<<2)))"}) {
		const auto tokens = lex_all(src);
		ASSERT_EQ(tokens.size(), 1u) << src;
		EXPECT_EQ(tokens[0].text, src) << src;
	}
}

// What the new knowledge must not cost: an unterminated `$(` is still an ERROR
// and not a word the executor runs (#47). A case clause left open and a
// here-document left open both run the scan to the end of the input.
TEST(Lexer, ACaseOrHereDocumentLeftOpenIsStillAnUnterminatedSubstitution) {
	for (const std::string src : {"echo $(case a in a) echo x",
	                              "echo $(cat <<END\nfoo",
	                              "echo $(cat <<END\nfoo)"}) {
		lexer lx{src};
		std::ignore = lx.next();
		const token t = lx.next();
		EXPECT_EQ(t.error, token_error::unterminated_command_sub) << src;
		EXPECT_TRUE(lx.incomplete()) << src;
	}
}

TEST(Lexer, AnUnterminatedSubstitutionIsStillReportedAsOne) {
	// The edge the `terminated` flag exists for: the scan's END POSITION cannot say
	// whether the construct closed, and guessing from it would report an
	// unterminated `$(` as a complete word.
	lexer lx{"echo $(echo x"};
	std::ignore = lx.next();
	const token t = lx.next();
	EXPECT_EQ(t.error, token_error::unterminated_command_sub);
	EXPECT_TRUE(lx.incomplete());
}

TEST(Lexer, NestingDeeperThanTheScanFollowsIsReportedRatherThanMisScanned) {
	// A stack that has lost its closers would leave the scan somewhere arbitrary
	// and hand the expander a word nobody wrote, so past the limit the construct is
	// reported as unterminated - a diagnostic, never a silently wrong answer.
	std::string deep;
	for (int i = 0; i < 400; ++i)
		deep += "${x-";
	deep += "hi";
	deep.append(400, '}');
	lexer lx{deep};
	const token t = lx.next();
	EXPECT_EQ(t.error, token_error::unterminated_parameter_expansion);

	std::string shallow;
	for (int i = 0; i < 200; ++i)
		shallow += "${x-";
	shallow += "hi";
	shallow.append(200, '}');
	lexer ok{shallow};
	EXPECT_EQ(ok.next().error, token_error::none) << "200 levels still lex";
}

TEST(Lexer, LexingNeverFailsOnHalfTypedInput) {
	// A highlighter sees every prefix of what the user types. None may throw, hang,
	// or fail to terminate.
	const std::string full = "echo \"a $(b) 'c\" | grep -e 'x' > out 2>&1 && done";
	for (size_t n = 0; n <= full.size(); ++n) {
		lexer lx{std::string_view(full).substr(0, n)};
		int guard = 0;
		for (;;) {
			const token t = lx.next();
			if (t.kind == token_kind::end)
				break;
			ASSERT_LT(++guard, 1000) << "did not terminate on prefix of length " << n;
		}
	}
}

TEST(Lexer, TokenStaysSmall) {
	// One token per input byte run; the size is a latency property, not a detail.
	EXPECT_EQ(sizeof(token), 16u);
}

// Inside double quotes a single quote is an ordinary byte, and a leading tilde is
// not eligible for tilde expansion. Lexing the interior in word_interior mode
// instead made `echo "it's"` print `it` - the trailing `'s"` was taken for the
// start of a single-quoted segment and its quotes were removed.
TEST(LexerDoubleQuoteInterior, SingleQuoteIsOrdinary) {
	lexer lx{"it's"};
	const token t = lx.next(lex_mode::double_quote_interior);
	EXPECT_EQ(t.kind, token_kind::seg_literal);
	EXPECT_EQ(t.length, 4u) << "the whole run is literal, quotes and all";
}

TEST(LexerDoubleQuoteInterior, SingleQuoteStartsASegmentInAPlainWord) {
	// The contrast that makes the mode necessary: outside double quotes the same
	// bytes DO begin a quoted segment.
	lexer lx{"'s'"};
	const token t = lx.next(lex_mode::word_interior);
	EXPECT_EQ(t.kind, token_kind::seg_single_quoted);
}

TEST(LexerDoubleQuoteInterior, TildeIsNotSpecial) {
	lexer lx{"~/x"};
	const token t = lx.next(lex_mode::double_quote_interior);
	EXPECT_EQ(t.kind, token_kind::seg_literal);

	lexer plain{"~/x"};
	EXPECT_EQ(plain.next(lex_mode::word_interior).kind, token_kind::seg_tilde)
		<< "outside double quotes a leading tilde is still eligible";
}

TEST(LexerDoubleQuoteInterior, ExpansionsAreStillRecognised) {
	// Only quoting changes. `$`, backtick and backslash keep their meaning.
	lexer lx{"$HOME"};
	EXPECT_EQ(lx.next(lex_mode::double_quote_interior).kind, token_kind::seg_parameter);
	lexer sub{"$(echo hi)"};
	EXPECT_EQ(sub.next(lex_mode::double_quote_interior).kind, token_kind::seg_command_sub);
}

// --- a digit run is an IO_NUMBER only when it could be a descriptor (#63) ----

TEST(Lexer, ADigitRunTooLargeToBeAFileDescriptorIsNotAnIoNumber) {
	// It accumulated into a uint32_t with nothing checking it, so
	// `4294967298>file` wrapped onto FD 2 and redirected the shell's stderr. dash,
	// zsh and ksh all read an over-long run as an ordinary word; the bound here is
	// what a descriptor can hold, which is bash's, so lesh agrees with bash below
	// it and with the other three above it.
	const std::vector redirects{token_kind::io_number, token_kind::great,
	                            token_kind::word};
	const std::vector reads_as_a_word{token_kind::word, token_kind::great,
	                                  token_kind::word};
	EXPECT_EQ(kinds_of("9>x"), redirects);
	EXPECT_EQ(kinds_of("2147483647>x"), redirects);
	EXPECT_EQ(kinds_of("2147483648>x"), reads_as_a_word);
	EXPECT_EQ(kinds_of("4294967298>x"), reads_as_a_word);
	EXPECT_EQ(kinds_of("99999999999999999999>x"), reads_as_a_word);
}

// --- $'...' : ANSI-C quoting (#75) ------------------------------------------
//
// The lexer's whole job here is to DELIMIT the construct. It decodes nothing:
// the decoded bytes are not in the source, and the lexer owns no memory. What
// it has to get right is the extent, and the extent is not a single-quoted
// run's - a backslash escapes the closing quote.

TEST(LexerDollarSingleQuote, IsItsOwnSegmentKind) {
	lexer lx{"$'a'"};
	const token t = lx.next(lex_mode::word_interior);
	EXPECT_EQ(t.kind, token_kind::seg_dollar_single_quoted);
	EXPECT_EQ(t.length, 4u) << "the `$` and both quotes belong to the segment";
	EXPECT_EQ(t.error, token_error::none);
}

TEST(LexerDollarSingleQuote, AnEscapedQuoteDoesNotEndIt) {
	// The reason this cannot reuse the single-quote scan: in `'...'` a backslash
	// is an ordinary byte, so a scan for the next `'` would stop at the one in
	// `\'` and leave `b'` outside the word.
	lexer lx{"$'a\\'b'"};
	const token t = lx.next(lex_mode::word_interior);
	EXPECT_EQ(t.kind, token_kind::seg_dollar_single_quoted);
	EXPECT_EQ(t.length, 7u);
	EXPECT_EQ(lx.text(t), "$'a\\'b'");
}

TEST(LexerDollarSingleQuote, ABlankInsideDoesNotEndTheWord) {
	// The word-extent scan is a separate code path from the segment scan, and it
	// has to know the construct too: a blank inside `$'a b'` is data, and a
	// newline inside it is data as well.
	EXPECT_EQ(kinds_of("echo $'a b'"),
	          (std::vector{token_kind::word, token_kind::word}));
	EXPECT_EQ(lex_all("echo $'a b'")[1].text, "$'a b'");
	EXPECT_EQ(lex_all("echo $'a\nb'")[1].text, "$'a\nb'")
		<< "a literal newline inside is data, not a command separator";
	EXPECT_EQ(lex_all("echo x$'a b'y")[1].text, "x$'a b'y")
		<< "it joins the word it is part of";
	EXPECT_EQ(lex_all("echo $'a\\'b c'")[1].text, "$'a\\'b c'")
		<< "the word scan honours the escape too, or the word would split at `b`";
}

TEST(LexerDollarSingleQuote, AnOperatorInsideIsData) {
	// `$'|'` is a word, not a pipe. The word scan reaches the `|` only if it
	// failed to step over the construct.
	EXPECT_EQ(kinds_of("echo $'|;&'"),
	          (std::vector{token_kind::word, token_kind::word}));
	EXPECT_EQ(lex_all("echo $'|;&'")[1].text, "$'|;&'");
}

TEST(LexerDollarSingleQuote, UnterminatedIsBothIncompleteAndDefective) {
	// The same shape as an unterminated `'...'`: more input could finish it, AND
	// as it stands the word must not run. `echo $'abc` printed an empty line at
	// status 0 before either channel was set.
	lexer lx{"$'abc"};
	const token t = lx.next(lex_mode::word_interior);
	EXPECT_EQ(t.kind, token_kind::seg_dollar_single_quoted);
	EXPECT_EQ(t.error, token_error::unterminated_single_quote);
	EXPECT_TRUE(lx.incomplete());

	lexer word{"$'abc"};
	const token w = word.next(lex_mode::command);
	EXPECT_EQ(w.error, token_error::unterminated_single_quote);
	EXPECT_TRUE(word.incomplete());
}

TEST(LexerDollarSingleQuote, ATrailingBackslashDoesNotReadPastTheEnd) {
	// `$'a\` ends with the backslash consuming a byte that is not there.
	lexer lx{"$'a\\"};
	const token t = lx.next(lex_mode::word_interior);
	EXPECT_EQ(t.error, token_error::unterminated_single_quote);
	EXPECT_TRUE(lx.incomplete());
	EXPECT_EQ(t.length, 4u) << "the whole remaining input, and not a byte more";
}

TEST(LexerDollarSingleQuote, IsNotSpecialInsideDoubleQuotesOrAHereDocumentBody) {
	// A single quote is an ordinary byte in both, so there is no `$'...'` to
	// recognise - `"$'a\nb'"` is literal text in bash and zsh alike.
	lexer dq{"$'a'"};
	EXPECT_EQ(dq.next(lex_mode::double_quote_interior).kind, token_kind::seg_literal)
		<< "inside double quotes the `$` is a lone dollar and the quotes are bytes";
	lexer body{"$'a'"};
	EXPECT_EQ(body.next(lex_mode::here_doc_body).kind, token_kind::seg_literal);
}

TEST(LexerDollarSingleQuote, ADollarQuoteInsideBracesDoesNotEndTheExpansion) {
	// The brace counter steps over quoted runs so a `}` inside one closes
	// nothing. `$'...'` is such a run, and a `}` in it is data.
	EXPECT_EQ(lex_all("echo ${x-$'}'}")[1].text, "${x-$'}'}");
	EXPECT_EQ(lex_all("echo $(echo $')')")[1].text, "$(echo $')')")
		<< "and a paren in one does not close a command substitution";
}

TEST(LexerDollarSingleQuote, ADollarNotFollowedByAQuoteIsUnchanged) {
	// The guard is narrow on purpose: `$x`, `${x}` and a lone `$` all still lex
	// the way they did.
	lexer p{"$x"};
	EXPECT_EQ(p.next(lex_mode::word_interior).kind, token_kind::seg_parameter);
	lexer lone{"$ "};
	EXPECT_EQ(lone.next(lex_mode::word_interior).kind, token_kind::seg_literal);
}

TEST(LexerDollarSingleQuote, TheWordIsNotFlaggedLiteral) {
	// flag_literal promises expansion and quote removal are provably no-ops, and
	// a `$'...'` word is neither. `$` was already in the set that clears it; this
	// asserts it rather than assuming it, because the flag is the expander's
	// licence to skip the word entirely.
	lexer lx{"$'a\\nb'"};
	const token t = lx.next(lex_mode::command);
	EXPECT_EQ(t.flags & flag_literal, 0);
}

// --- a here-document delimiter may be spelled $'...' too ---------------------

TEST(HereDocDelimiter, DollarSingleQuoteIsDecodedAndQuotes) {
	// POSIX applies quote removal to the delimiter word, and `$'...'` is one more
	// spelling of a quoted one beside `\END`, `'END'` and `"END"`. bash and zsh
	// both decode it.
	EXPECT_TRUE(here_doc_delimiter_matches("$'END'", "END"));
	EXPECT_TRUE(here_doc_delimiter_matches("$'E\\x4ED'", "END"));
	EXPECT_TRUE(here_doc_delimiter_matches("$'E\\116D'", "END"));
	EXPECT_TRUE(here_doc_delimiter_matches("$'a\\tb'", "a\tb"));
	EXPECT_FALSE(here_doc_delimiter_matches("$'E\\x4ED'", "E\\x4ED"))
		<< "the undecoded spelling must NOT match, or the body would never end";
	EXPECT_FALSE(here_doc_delimiter_matches("$'END'", "ENDX"));
	EXPECT_FALSE(here_doc_delimiter_matches("$'END", "END"))
		<< "an unterminated one cannot match anything";
	EXPECT_TRUE(here_doc_delimiter_matches("x$'E'y", "xEy"))
		<< "and it composes with the rest of the word";
}
