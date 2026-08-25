#include "leshper/abi.h"
#include "leshper/registry.h"
#include "leshper/state.h"
#include "substrate/arena.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace lesh::leshper;

// The highlighter reactor (#124: F-20/F-21/F-22, on #93's ABI).
//
// Every test below drives the highlighter the way the loop will: build a state,
// hand it to the harness fake, and read the batch back. Nothing here calls into
// builtin_reactors.cpp directly, because there IS no other entry point - the
// reactor is a function pointer in the registry and a `void*` beside it, exactly
// as a Lua reactor would be, and asserting on it through the ABI is asserting on
// the property that matters (A-11).
//
// The one thing these tests reach for that a plugin could not is the STYLE NAME:
// `lesh_style_name` turns an id back into what it was interned under, so an
// assertion reads `command.unknown` rather than `7`. That is the vocabulary
// under test, not an implementation detail - the theme is the other half of
// F-21 and is deliberately not this ticket's.

namespace {

// One registry, one highlighter, one loop - and the highlighter outlives the
// registry it registered into, which is the lifetime the ABI requires of any
// reactor's context pointer.
struct highlight_fixture {
	owned_highlighter self;
	registry reg;
	loop_harness loop{reg};

	highlight_fixture() { register_builtin_reactors(reg, self.get()); }

	[[nodiscard]] reactor_batch paint(std::string_view line) {
		lesh::leshper::state s;
		s.buffer.replace(s.buffer.begin_position(), s.buffer.begin_position(),
		                 std::string(line));
		s.gen.bump();
		std::vector<reactor_batch> batches = loop.react(s, LESH_EVENT_BUFFER_CHANGED);
		EXPECT_EQ(batches.size(), 1u);
		if (batches.empty())
			return reactor_batch{};
		return std::move(batches[0]);
	}

	[[nodiscard]] std::string style_name(std::uint32_t id) {
		char out[64] = {};
		std::size_t length = 0;
		if (lesh_style_name(&reg, id, out, sizeof(out), &length) != LESH_OK)
			return "<none>";
		return std::string(out, length);
	}

	// Every span, as a reader sees it: what the bytes are and what they were
	// called. Order is preserved, because the order IS part of the contract -
	// outermost first, so a later span refines an earlier one it overlaps.
	[[nodiscard]] std::vector<std::pair<std::string, std::string>> painted(
		std::string_view line) {
		const reactor_batch batch = paint(line);
		std::vector<std::pair<std::string, std::string>> out;
		for (const decoration_span& one : batch.spans)
			out.emplace_back(std::string(line.substr(one.start, one.end - one.start)),
			                 style_name(one.style_id));
		return out;
	}

	[[nodiscard]] bool has(std::string_view line, std::string_view text,
	                       std::string_view style) {
		for (const auto& [bytes, name] : painted(line))
			if (bytes == text && name == style)
				return true;
		return false;
	}

	[[nodiscard]] bool any_of_style(std::string_view line, std::string_view style) {
		for (const auto& [bytes, name] : painted(line))
			if (name == style)
				return true;
		return false;
	}
};

// A PATH with exactly one directory in it, restored on the way out, so that
// "resolves against PATH" is a property of the test and not of the machine.
class scoped_path {
public:
	explicit scoped_path(const char* value) {
		if (const char* old = ::getenv("PATH")) {
			_had = true;
			_old = old;
		}
		::setenv("PATH", value, 1);
	}
	~scoped_path() {
		if (_had)
			::setenv("PATH", _old.c_str(), 1);
		else
			::unsetenv("PATH");
	}

	scoped_path(const scoped_path&) = delete;
	scoped_path& operator=(const scoped_path&) = delete;

private:
	bool _had = false;
	std::string _old;
};

} // namespace

TEST(LeshperHighlight, ItRegistersThroughTheAbiExactlyAsAPluginWould) {
	// A-11: built-in reactors MUST use the subscription interface, not a special
	// path. The evidence is that the ABI can see it - the name is in the reactor
	// registry, and there is no second table for native ones.
	highlight_fixture fixture;
	std::int32_t exists = 0;
	EXPECT_EQ(lesh_reactor_exists(&fixture.reg, "highlighter", &exists), LESH_OK);
	EXPECT_EQ(exists, 1);
	EXPECT_EQ(lesh_reactor_exists(&fixture.reg, "nobody", &exists), LESH_OK);
	EXPECT_EQ(exists, 0);
}

TEST(LeshperHighlight, ACursorMoveDoesNotAskForARepaint) {
	// A highlight is a function of the text. Subscribing to cursor_moved would be
	// a full re-parse per arrow key for a byte-identical answer.
	highlight_fixture fixture;
	lesh::leshper::state s;
	EXPECT_TRUE(fixture.loop.react(s, LESH_EVENT_CURSOR_MOVED).empty());
	EXPECT_EQ(fixture.loop.react(s, LESH_EVENT_BUFFER_CHANGED).size(), 1u);
}

TEST(LeshperHighlight, TheStyleVocabularyIsInternedSemanticNames) {
	// F-21's "independently themeable", stated as the thing that makes it true:
	// the reactor emits `command.unknown`, never a colour. The theme maps these
	// at render, which is the other half of the feature and not this ticket.
	highlight_fixture fixture;
	for (const char* name : {"command.unknown", "command.path", "keyword", "comment",
	                         "string.single", "string.double", "string.ansi_c",
	                         "expansion.parameter", "expansion.command",
	                         "expansion.arithmetic", "expansion.tilde",
	                         "redirect.target", "error.syntax"}) {
		std::uint32_t id = LESH_STYLE_NONE;
		EXPECT_EQ(lesh_style_intern(&fixture.reg, name, &id), LESH_OK) << name;
		// Interning is idempotent, so an id already present comes back rather
		// than a second one being minted: the reactor interned these at
		// registration and this is the same id, not a new one.
		EXPECT_NE(id, LESH_STYLE_NONE) << name;
		EXPECT_EQ(fixture.style_name(id), name);
	}
}

TEST(LeshperHighlight, OneLineWhole) {
	// The batch for one ordinary line, in order, as a reader would describe it.
	// Here so that a change to the emission contract has to be stated rather than
	// discovered - the ordering IS the contract, since the renderer that consumes
	// it does not exist yet.
	highlight_fixture fixture;
	scoped_path path{"/nonexistent"};
	const std::string_view line = "/bin/sh -c 'a' \"b $x\" # note";
	const std::vector<std::pair<std::string, std::string>> want = {
		{"'a'", "string.single"},
		{"\"b $x\"", "string.double"},
		{"$x", "expansion.parameter"},     // refines the string it sits in
		{"/bin/sh", "command.path"},
		{"# note", "comment"},
	};
	EXPECT_EQ(fixture.painted(line), want);
}

TEST(LeshperHighlight, AnUnresolvableCommandNameIsUnknownAndAResolvableOneIsAPath) {
	highlight_fixture fixture;
	scoped_path path{"/bin"};
	EXPECT_TRUE(fixture.has("sh -c x", "sh", "command.path"));
	EXPECT_TRUE(fixture.has("no_such_command_xyzzy a", "no_such_command_xyzzy",
	                        "command.unknown"));
}

TEST(LeshperHighlight, ANameWithASlashIsAPathAndNotAPathLookup) {
	// POSIX 2.9.1.1. With PATH emptied, an absolute name still resolves and a
	// bare one cannot - which is the difference the rule makes.
	highlight_fixture fixture;
	scoped_path path{""};
	EXPECT_TRUE(fixture.has("/bin/sh -c x", "/bin/sh", "command.path"));
	EXPECT_TRUE(fixture.has("/bin/definitely_not_here x", "/bin/definitely_not_here",
	                        "command.unknown"));
}

TEST(LeshperHighlight, ADirectoryIsNotACommand) {
	// access(X_OK) says yes for a directory, so the mode test is what keeps
	// `/tmp` from painting as something exec would run.
	highlight_fixture fixture;
	EXPECT_TRUE(fixture.has("/tmp arg", "/tmp", "command.unknown"));
}

TEST(LeshperHighlight, AWordThatIsNotProvablyLiteralIsNotClassifiedAtAll) {
	// `$cmd` names a command only after expansion. Painting it red for being a
	// variable is the behaviour that makes highlighting untrustworthy, so the
	// classifier declines and the expansion segment paints instead.
	highlight_fixture fixture;
	scoped_path path{"/bin"};
	EXPECT_FALSE(fixture.any_of_style("$cmd a", "command.unknown"));
	EXPECT_FALSE(fixture.any_of_style("$cmd a", "command.path"));
	EXPECT_TRUE(fixture.has("$cmd a", "$cmd", "expansion.parameter"));
	// Quoted, too: `'sh'` is `sh` only after quote removal.
	EXPECT_FALSE(fixture.any_of_style("'sh' -c x", "command.path"));
	EXPECT_TRUE(fixture.has("'sh' -c x", "'sh'", "string.single"));
}

TEST(LeshperHighlight, QuotedStringsArePaintedByKind) {
	// F-21 asks for quoted string BY KIND, and #95 found the syntax layer
	// already answers it: re-lexing a word interior through C-6 yields the kinds
	// with exact spans, no second scanner involved.
	highlight_fixture fixture;
	const std::string_view line = "echo 'a' \"b\" $'c'";
	EXPECT_TRUE(fixture.has(line, "'a'", "string.single"));
	EXPECT_TRUE(fixture.has(line, "\"b\"", "string.double"));
	EXPECT_TRUE(fixture.has(line, "$'c'", "string.ansi_c"));
}

TEST(LeshperHighlight, ExpansionsAndSubstitutionsArePaintedByKind) {
	highlight_fixture fixture;
	const std::string_view line = "echo $x $(ls) $((1+2)) ~/d";
	EXPECT_TRUE(fixture.has(line, "$x", "expansion.parameter"));
	EXPECT_TRUE(fixture.has(line, "$(ls)", "expansion.command"));
	EXPECT_TRUE(fixture.has(line, "$((1+2))", "expansion.arithmetic"));
	EXPECT_TRUE(fixture.has(line, "~", "expansion.tilde"));
}

TEST(LeshperHighlight, ExpansionsInsideDoubleQuotesStillPaint) {
	// A single quote inside double quotes is an ordinary byte, so the interior
	// has to be re-lexed in the right mode; lexed as a plain word interior,
	// `"it's $x"` loses the parameter behind a phantom single-quoted run.
	highlight_fixture fixture;
	const std::string_view line = "echo \"it's $x\"";
	EXPECT_TRUE(fixture.has(line, "\"it's $x\"", "string.double"));
	EXPECT_TRUE(fixture.has(line, "$x", "expansion.parameter"));
}

TEST(LeshperHighlight, ItDescendsIntoCommandSubstitutionInteriors) {
	// #104's sub-parses, which is the whole reason `$(ls -l foo)` is not one
	// blob. The outer span comes FIRST and the interior's classification second,
	// which is the emission order the batch promises: later refines earlier.
	highlight_fixture fixture;
	scoped_path path{"/bin"};
	const std::string_view line = "echo $(sh -c x)";
	const std::vector<std::pair<std::string, std::string>> spans = fixture.painted(line);

	std::size_t outer = spans.size();
	std::size_t inner = spans.size();
	for (std::size_t i = 0; i < spans.size(); ++i) {
		if (spans[i].first == "$(sh -c x)" && spans[i].second == "expansion.command")
			outer = i;
		if (spans[i].first == "sh" && spans[i].second == "command.path")
			inner = i;
	}
	ASSERT_LT(outer, spans.size()) << "the substitution itself was never painted";
	ASSERT_LT(inner, spans.size()) << "nothing painted inside the substitution";
	EXPECT_LT(outer, inner) << "the container must be emitted before what it contains";
}

TEST(LeshperHighlight, CommentsComeFromTheSideListAndNothingElseMoves) {
	// #103 made comments tokens the parser records beside the tree. A painter
	// greys them from that list; the grammar never sees one, so `ls` is still a
	// command name with a comment after it.
	highlight_fixture fixture;
	scoped_path path{"/bin"};
	const std::string_view line = "sh # a comment";
	EXPECT_TRUE(fixture.has(line, "# a comment", "comment"));
	EXPECT_TRUE(fixture.has(line, "sh", "command.path"));
}

TEST(LeshperHighlight, RedirectTargetsArePainted) {
	// #103 gave the target a word node with a role, which is the span this
	// paints and the footing completion stands on.
	highlight_fixture fixture;
	EXPECT_TRUE(fixture.has("echo x > /tmp/out", "/tmp/out", "redirect.target"));
	EXPECT_TRUE(fixture.has("echo x >> /tmp/out", "/tmp/out", "redirect.target"));
}

TEST(LeshperHighlight, KeywordsArePaintedWherePositionMakesThemKeywords) {
	// #105's flag, and the reason it has to be a flag the PARSER sets: the bytes
	// are the same either way, and only the grammar knows which `done` is which.
	highlight_fixture fixture;
	const std::string_view loop = "for i in a; do echo $i; done";
	EXPECT_TRUE(fixture.has(loop, "for", "keyword"));
	EXPECT_TRUE(fixture.has(loop, "do", "keyword"));
	EXPECT_TRUE(fixture.has(loop, "done", "keyword"));
	// An argument spelled like a keyword is not one.
	EXPECT_FALSE(fixture.any_of_style("echo done", "keyword"));
}

TEST(LeshperHighlight, AMalformedLineGetsASquiggleAndAnIncompleteOneDoesNot) {
	// C-2's tristate, which is the whole reason it is a tristate rather than a
	// boolean. `echo "x` is incomplete AND defective; an editor answers that
	// with a continuation prompt, not with red, because more input fixes it.
	// Painting it the moment the opening quote is typed is what makes live
	// highlighting hated.
	highlight_fixture fixture;
	EXPECT_TRUE(fixture.any_of_style("echo ;;", "error.syntax"));
	EXPECT_FALSE(fixture.any_of_style("echo \"x", "error.syntax"));
	EXPECT_FALSE(fixture.any_of_style("echo a\\", "error.syntax"));
	EXPECT_FALSE(fixture.any_of_style("if true; then echo a", "error.syntax"));
	// And the string still paints while it is being typed - the span exists, it
	// is simply not an error yet.
	EXPECT_TRUE(fixture.has("echo \"x", "\"x", "string.double"));
	// A clean line has no squiggle at all.
	EXPECT_FALSE(fixture.any_of_style("echo hi", "error.syntax"));
}

TEST(LeshperHighlight, ADefectInsideACommandSubstitutionIsNotThisLinesDefect) {
	// #104's watermark, read forward. The expander reports an interior defect at
	// expansion time, at status 2, and has_errors() declines to see it - so
	// painting it here would put the tree and the paint in disagreement about
	// what is wrong with the line.
	highlight_fixture fixture;
	EXPECT_FALSE(fixture.any_of_style("echo $(if true)", "error.syntax"));
	EXPECT_TRUE(fixture.has("echo $(if true)", "$(if true)", "expansion.command"));
}

TEST(LeshperHighlight, AliasesAreNotSubstitutedSoSpansPointAtWhatWasTyped) {
	// The highlight parse passes no alias table - parse()'s default, and #95's
	// finding. There is no table to pass here, which is exactly the point: the
	// reactor cannot reach shell state through the ABI, so it could not paint
	// through an alias even if it wanted to. Every span is a real input offset.
	highlight_fixture fixture;
	const reactor_batch batch = fixture.paint("echo 'a' $(x) # c");
	for (const decoration_span& one : batch.spans) {
		EXPECT_LE(one.start, one.end);
		EXPECT_LE(one.end, std::string_view{"echo 'a' $(x) # c"}.size());
	}
}

TEST(LeshperHighlight, ThePollIsCheckedAndTheReactorGivesUpCooperatively) {
	// ADR-0008's cancellation. The harness resets the flag at the top of react(),
	// so the way to be superseded mid-sweep is to be superseded by something
	// running in the same sweep - a reactor whose name sorts before
	// "highlighter", which is the order the registry's map iterates.
	highlight_fixture fixture;
	static loop_harness* which = nullptr;
	which = &fixture.loop;
	ASSERT_EQ(lesh_reactor_register(&fixture.reg, "a_typist", LESH_EVENT_BUFFER_CHANGED,
	          [](lesh_request*, void*) -> std::int32_t {
		          which->supersede();
		          return LESH_OK;
	          }, nullptr), LESH_OK);

	lesh::leshper::state s;
	s.buffer.replace(s.buffer.begin_position(), s.buffer.begin_position(),
	                 std::string("echo one; echo two; echo three"));
	s.gen.bump();
	const std::vector<reactor_batch> batches =
		fixture.loop.react(s, LESH_EVENT_BUFFER_CHANGED);
	ASSERT_EQ(batches.size(), 2u);
	const reactor_batch* highlighter_batch = nullptr;
	for (const reactor_batch& one : batches)
		if (one.reactor == "highlighter")
			highlighter_batch = &one;
	ASSERT_NE(highlighter_batch, nullptr);
	EXPECT_EQ(highlighter_batch->status, LESH_ERR_SUPERSEDED);
}

TEST(LeshperHighlight, AStaleHighlightHasNowhereToBeApplied) {
	// N-4 through the highlighter rather than through a toy reactor: the batch
	// was computed against a generation the buffer has left behind, and there is
	// no apply function in abi.h for it to reach.
	highlight_fixture fixture;
	lesh::leshper::state s;
	s.buffer.replace(s.buffer.begin_position(), s.buffer.begin_position(),
	                 std::string("echo hi"));
	s.gen.bump();
	std::vector<reactor_batch> batches = fixture.loop.react(s, LESH_EVENT_BUFFER_CHANGED);
	ASSERT_EQ(batches.size(), 1u);

	s.buffer.replace(s.buffer.end_position(), s.buffer.end_position(), std::string("!"));
	s.gen.bump();
	EXPECT_FALSE(fixture.loop.apply(s, std::move(batches[0])));
	EXPECT_TRUE(fixture.loop.applied().empty());

	batches = fixture.loop.react(s, LESH_EVENT_BUFFER_CHANGED);
	ASSERT_EQ(batches.size(), 1u);
	EXPECT_TRUE(fixture.loop.apply(s, std::move(batches[0])));
	EXPECT_EQ(fixture.loop.applied().size(), 1u);
}

TEST(LeshperHighlight, TheComputePathTakesNothingFromTheHeap) {
	// #90's rule: a reactor computes out of the arena its request hands it, and
	// nothing else. The instrument is the repo's own - `heap_allocations` counts
	// ONLY the arena's malloc fallback, so a non-zero reading means the parse
	// outgrew the pool, which is the failure this design exists to avoid. The
	// harness's own copies (the snapshot into a std::string, the emitted spans
	// into a std::vector) are the LOOP's allocations and are invisible here, as
	// they should be: they are not on the reactor's path.
	highlight_fixture fixture;
	lesh::leshper::state s;
	std::string line;
	while (line.size() < 4096)
		line += "for f in a b c; do echo \"$f-$(date)\" >> log; done; ";
	line.resize(4096);
	s.buffer.replace(s.buffer.begin_position(), s.buffer.begin_position(), line);
	s.gen.bump();

	// Warm once: the first parse is where a lazily-grown arena_array would grow.
	std::vector<reactor_batch> warm = fixture.loop.react(s, LESH_EVENT_BUFFER_CHANGED);
	ASSERT_EQ(warm.size(), 1u);
	EXPECT_FALSE(warm[0].spans.empty());

	auto& counters = lesh::metrics::allocations();
	const std::size_t heap_before = counters.heap_allocations;
	const std::vector<reactor_batch> again =
		fixture.loop.react(s, LESH_EVENT_BUFFER_CHANGED);
	EXPECT_EQ(counters.heap_allocations, heap_before);
	EXPECT_EQ(again.size(), 1u);
}

TEST(LeshperHighlight, AnEmptyBufferPaintsNothingAndDoesNotFail) {
	highlight_fixture fixture;
	const reactor_batch batch = fixture.paint("");
	EXPECT_EQ(batch.status, LESH_OK);
	EXPECT_TRUE(batch.spans.empty());
	EXPECT_TRUE(batch.texts.empty());
	EXPECT_TRUE(batch.proposals.empty());
}

TEST(LeshperHighlight, TheHighlighterEmitsSpansAndNothingElse) {
	// The emitting reactor IS the decoration namespace, and this one's whole
	// output is decoration: virtual text and proposals belong to the
	// autosuggester (F-24), which is a different reactor and a different ticket.
	highlight_fixture fixture;
	const reactor_batch batch = fixture.paint("echo 'a' $(x) # c");
	EXPECT_FALSE(batch.spans.empty());
	EXPECT_TRUE(batch.texts.empty());
	EXPECT_TRUE(batch.proposals.empty());
}
