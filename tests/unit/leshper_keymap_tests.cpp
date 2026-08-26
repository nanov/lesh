#include "leshper/keymap.h"

#include "leshper/abi.h"
#include "leshper/editor.h"
#include "leshper/event.h"
#include "leshper/registry.h"
#include "leshper/state.h"
#include "runtime/builtins.h"
#include "runtime/shell_state.h"

#include "temp_path.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

// The keymap stack, in its own file (#118).
//
// Wave 3's rule, kept: every lane gets its own test file and leshper_tests.cpp
// no longer grows. What lives here is everything the stack owns - the notation,
// the encoding, the table, the registry, dispatch through it, and the `bind`
// builtin's reach into it across the link boundary.

using namespace lesh::leshper;

namespace {

// Helpers named for what a user does, so a test reads as a session.
effects press(state& s, char32_t codepoint, key_modifiers modifiers = {}) {
	return step(s, key_event::of(codepoint, modifiers));
}
effects press(state& s, named_key key, key_modifiers modifiers = {}) {
	return step(s, key_event::of(key, modifiers));
}

effects type(state& s, std::string_view text) {
	effects all;
	for (const char byte : text) {
		effects one = press(s, static_cast<char32_t>(static_cast<unsigned char>(byte)));
		all.insert(all.end(), one.begin(), one.end());
	}
	return all;
}

std::string text_of(const state& s) { return std::string(s.buffer.text()); }
size_t cursor_of(const state& s) { return s.cursor.byte_offset(); }

// The encoded form of one written key sequence. ASSERTs rather than returns a
// failure, because a test whose notation does not parse is a broken test.
std::string keys(std::string_view notation) {
	std::string encoded;
	EXPECT_TRUE(parse_key_notation(notation, encoded)) << notation;
	return encoded;
}

constexpr char32_t control_w = 0x17;
constexpr char32_t control_x = 0x18;
constexpr char32_t delete_character = 0x7F;

} // namespace

// ---------------------------------------------------------------------------
// The notation (#117 decision 1): vim's, because that is what the owner types.
// ---------------------------------------------------------------------------

TEST(LeshperKeyNotation, ControlNotationParsesToTheC0Codepoint) {
	// The decision the spec states twice: `<C-w>` is NOTATION for U+0017, not a
	// `w` with a bit set, because U+0017 is literally what the terminal sends at
	// the #97 floor. Binding the notation and binding the raw control character
	// must therefore produce the SAME table row, or a user who wrote one and a
	// terminal that sent the other would never meet.
	EXPECT_EQ(keys("<C-w>"), encode_key(key_event::of(control_w)));
	EXPECT_EQ(keys("<C-a>"), encode_key(key_event::of(static_cast<char32_t>(0x01))));
	EXPECT_EQ(keys("<C-_>"), encode_key(key_event::of(static_cast<char32_t>(0x1F))));
	EXPECT_EQ(keys("<C-@>"), encode_key(key_event::of(static_cast<char32_t>(0x00))));
	EXPECT_EQ(keys("<C-?>"), encode_key(key_event::of(delete_character)));
	// Upper and lower case fold to the same control character, as they do on the
	// wire: there is no U+0037 to tell `<C-W>` from `<C-w>` with.
	EXPECT_EQ(keys("<C-W>"), keys("<C-w>"));
}

TEST(LeshperKeyNotation, ControlOnANamedKeyIsTheModifierBitBecauseThereIsNoC0ForIt) {
	// event.h's asymmetry, and the reason it exists: `<C-Left>` has no control
	// character at all - a terminal spells it `ESC [ 1 ; 5 D`, which #111 decodes
	// into a named key with `ctrl` set. So the notation has to mean two different
	// things by `C-`, and it does, decided by what it is applied to.
	EXPECT_EQ(keys("<C-Left>"),
	          encode_key(key_event::of(named_key::left, key_modifiers{.ctrl = true})));
	// A codepoint with no C0 spelling is REFUSED rather than given a bit no
	// terminal at this floor will ever set.
	std::string unused;
	EXPECT_FALSE(parse_key_notation("<C-1>", unused));
	EXPECT_FALSE(parse_key_notation("<C-->", unused));
}

TEST(LeshperKeyNotation, AltIsABitAndBothSpellingsMeanIt) {
	// `A-` and `M-`, because vim accepts both and the owner's muscle memory may
	// hold either. The bit is the one #111's ESC-prefix rule sets.
	EXPECT_EQ(keys("<A-Left>"),
	          encode_key(key_event::of(named_key::left, key_modifiers{.alt = true})));
	EXPECT_EQ(keys("<M-a>"), keys("<A-a>"));
	EXPECT_EQ(keys("<A-a>"), encode_key(key_event::of(U'a', key_modifiers{.alt = true})));
	// And Alt-a is NOT `a`: a binding of one must not answer for the other.
	EXPECT_NE(keys("<A-a>"), keys("a"));
}

TEST(LeshperKeyNotation, ShiftUppercasesALetterAndIsABitOnEverythingElse) {
	// What the keyboard sends. There is no shifted `a` on the wire - there is
	// `A` - so `<S-a>` has to be `A` or a binding written that way would be
	// unreachable. `<S-Tab>` is the other case: a terminal that speaks CSI Z
	// sends a modifier, and the notation can express it even though #111's
	// decoder does not read that sequence yet.
	EXPECT_EQ(keys("<S-a>"), keys("A"));
	EXPECT_EQ(keys("<S-Tab>"), encode_key(key_event::of(U'\t', key_modifiers{.shift = true})));
	EXPECT_EQ(keys("<S-Left>"),
	          encode_key(key_event::of(named_key::left, key_modifiers{.shift = true})));
}

TEST(LeshperKeyNotation, LiteralPrintablesAreThemselvesAndSequencesConcatenate) {
	EXPECT_EQ(keys("x"), encode_key(key_event::of(U'x')));
	EXPECT_EQ(keys("gg"), keys("g") + keys("g"));
	EXPECT_EQ(keys("<C-x><C-e>"), keys("<C-x>") + keys("<C-e>"));
	// Multi-byte scalars survive: a keymap is over Unicode, not over bytes.
	EXPECT_EQ(keys("\xC3\xA9"), encode_key(key_event::of(static_cast<char32_t>(0xE9))));
	// Six bytes a key, fixed, which is what makes a byte prefix a key prefix.
	EXPECT_EQ(keys("gg").size(), 2 * encoded_key_size);
}

TEST(LeshperKeyNotation, NotationThatMeansNothingIsRefusedRatherThanGuessedAt) {
	// A typo in an rc file must be reported. The decoder's graceful degradation
	// is for what a TERMINAL sends; what a human wrote is either notation or a
	// mistake.
	std::string unused;
	EXPECT_FALSE(parse_key_notation("", unused));
	EXPECT_FALSE(parse_key_notation("<Nonesuch>", unused));
	EXPECT_FALSE(parse_key_notation("<>", unused));
	EXPECT_FALSE(parse_key_notation("<A->", unused));
	EXPECT_FALSE(parse_key_notation("<Ctrl-w>", unused));

	// A `<` with no `>` after it anywhere is a LITERAL, which is what lets `a<b`
	// be bound without escaping - and is why `<C-` is three literal keys rather
	// than a refusal. One rule, applied both times: an unclosed bracket was never
	// a group, and only a group that closed and did not resolve is a mistake.
	std::string angle;
	EXPECT_TRUE(parse_key_notation("a<b", angle));
	EXPECT_EQ(angle, keys("a") + keys("<lt>") + keys("b"));
	std::string unclosed;
	EXPECT_TRUE(parse_key_notation("<C-", unclosed));
	EXPECT_EQ(unclosed, keys("<lt>") + keys("C") + keys("-"));
}

TEST(LeshperKeyNotation, EveryRenderedSequenceParsesBackToItself) {
	// `bind`'s listing has to be re-inputtable, the same property `alias`'s
	// listing has: `bind -m emacs > f` then reading `f` back must rebuild the
	// same table. That is only true if rendering is the exact inverse of parsing.
	for (const char* written : {"<C-w>", "<C-a>", "<Up>", "<A-Left>", "<C-Left>",
	                            "<S-Tab>", "<BS>", "<Esc>", "<Tab>", "<CR>",
	                            "<Space>", "<lt>", "x", "gg", "<C-x><C-e>",
	                            "<F5>", "<PageUp>", "<Del>"}) {
		const std::string encoded = keys(written);
		const std::string rendered = render_key_notation(encoded);
		std::string again;
		ASSERT_TRUE(parse_key_notation(rendered, again)) << written << " -> " << rendered;
		EXPECT_EQ(again, encoded) << written << " -> " << rendered;
	}
}

TEST(LeshperKeyNotation, ADecodedRecordRoundTripsAndACorruptOneIsRefused) {
	const std::string encoded = keys("<A-Left>x");
	size_t at = 0;
	key_event first;
	ASSERT_TRUE(decode_key(encoded, at, first));
	EXPECT_EQ(first, key_event::of(named_key::left, key_modifiers{.alt = true}));
	key_event second;
	ASSERT_TRUE(decode_key(encoded, at, second));
	EXPECT_EQ(second, key_event::of(U'x'));
	EXPECT_FALSE(decode_key(encoded, at, second));

	// A table `bind` did not build - a truncated record, a kind byte from a
	// newer writer - is ANSWERED rather than cast into the enum and dispatched.
	std::string truncated = encoded.substr(0, 3);
	size_t nowhere = 0;
	EXPECT_FALSE(decode_key(truncated, nowhere, first));
	std::string wrong_kind = encoded;
	wrong_kind[0] = 9;
	nowhere = 0;
	EXPECT_FALSE(decode_key(wrong_kind, nowhere, first));
}

// ---------------------------------------------------------------------------
// The keymap: a flat table, and first-class data (F-11).
// ---------------------------------------------------------------------------

TEST(LeshperKeymap, BindingReplacesAndAnEmptyActionUnbinds) {
	// Registration replaces, the same rule #101 gives the action registry, so
	// re-sourcing an rc file is idempotent rather than an error or a duplicate.
	keymap map;
	map.bind(keys("<C-w>"), "delete_backward_word");
	map.bind(keys("<C-w>"), "kill_region");
	ASSERT_NE(map.action_for(keys("<C-w>")), nullptr);
	EXPECT_EQ(*map.action_for(keys("<C-w>")), "kill_region");
	EXPECT_EQ(map.entries().size(), 1u);

	map.bind(keys("<C-w>"), "");
	EXPECT_EQ(map.action_for(keys("<C-w>")), nullptr);
	EXPECT_TRUE(map.empty());
	EXPECT_FALSE(map.unbind(keys("<C-w>")));
}

TEST(LeshperKeymap, ThePrefixQuestionIsAnsweredWithoutAnExactMatchAnsweringIt) {
	// The whole of what makes `<C-x><C-e>` possible, and the trap in it: an exact
	// match is NOT its own prefix. If it were, every complete binding would hold
	// forever waiting for a longer one that does not exist.
	keymap map;
	map.bind(keys("<C-x><C-e>"), "edit_command_line");
	EXPECT_TRUE(map.has_longer(keys("<C-x>")));
	EXPECT_EQ(map.action_for(keys("<C-x>")), nullptr);
	EXPECT_FALSE(map.has_longer(keys("<C-x><C-e>")));
	ASSERT_NE(map.action_for(keys("<C-x><C-e>")), nullptr);

	map.bind(keys("<C-x>"), "something_shorter");
	EXPECT_TRUE(map.has_longer(keys("<C-x>")));   // still, and this is the hold
	EXPECT_NE(map.action_for(keys("<C-x>")), nullptr);
}

TEST(LeshperKeymap, TheTableStaysSortedSoLookupIsASearchAndNotAScan) {
	keymap map;
	for (const char* written : {"z", "a", "<C-x><C-e>", "<C-x>", "m"})
		map.bind(keys(written), "noop");
	ASSERT_EQ(map.entries().size(), 5u);
	for (size_t i = 1; i < map.entries().size(); ++i)
		EXPECT_LT(map.entries()[i - 1].keys, map.entries()[i].keys);
	// And a prefix sorts immediately before what extends it, which is why
	// lower_bound answers both questions.
	EXPECT_EQ(map.entries()[0].keys, keys("<C-x>"));
	EXPECT_EQ(map.entries()[1].keys, keys("<C-x><C-e>"));
}

TEST(LeshperKeymap, AKeymapIsDataAndACopyDivergesFromItsOriginal) {
	// F-11: created, copied and modified as data, with no keymap-building DSL and
	// no second dispatch system. `bind -N vi_visual vi_command` is this.
	keymap original;
	original.bind(keys("h"), "backward_char");
	original.indicator = "NORMAL";
	original.opaque = true;

	keymap copied = original;
	EXPECT_TRUE(copied == original);
	copied.bind(keys("h"), "select_backward_char");
	copied.indicator = "VISUAL";
	EXPECT_FALSE(copied == original);
	EXPECT_EQ(*original.action_for(keys("h")), "backward_char");
}

// ---------------------------------------------------------------------------
// The registry: the name is the identity (#117 decision 8).
// ---------------------------------------------------------------------------

TEST(LeshperKeymapRegistry, TheDefaultsAreTheModesPlusTheirSubModes) {
	// Three when #118 wrote this, seven since #119 filled the vi repertoire in,
	// eight since #138 added the pager's: the two vi MODES gained the four
	// sub-modes they push - operator-pending, visual, and the two one-shot maps
	// that catch the argument key of `f` and `r` - and the pager pushes a fifth.
	// Every one of them is an ordinary keymap, which is the point: nothing in
	// dispatch knows that five of these are pushed rather than swapped.
	keymap_registry maps;
	maps.install_defaults();
	std::vector<std::string> names;
	maps.names(names);
	EXPECT_EQ(names, (std::vector<std::string>{"emacs", "pager", "vi_command",
	                                           "vi_find_char", "vi_insert",
	                                           "vi_operator_pending",
	                                           "vi_replace_char", "vi_visual"}));
}

TEST(LeshperKeymapRegistry, TheEmacsDefaultsAreTheHardcodedTableMovedIn) {
	// #107's switch, key for key. Two spellings of Backspace, because terminals
	// disagree about which one they send and event.h binds both. Enter is
	// deliberately absent - F-35 makes it a decision the parser takes part in.
	//
	// THE FOUR FORWARD KEYS RUN A WRAPPER NOW (#140 decision 2, #147), and that
	// is the whole visible difference: `<Right>` is `forward_char` with nothing
	// suggested and an accept at the end of the buffer, under one name that says
	// both. The backward keys are untouched - a suggestion is forward of the
	// cursor - and so is `<C-a>`.
	keymap_registry maps;
	maps.install_defaults();
	const keymap* emacs = maps.find(keymap_registry::emacs);
	ASSERT_NE(emacs, nullptr);

	const std::pair<const char*, const char*> expected[] = {
		{"<BS>", "delete_backward_char"},   {"<C-h>", "delete_backward_char"},
		{"<BSKey>", "delete_backward_char"}, {"<C-w>", "delete_backward_word"},
		{"<C-a>", "beginning_of_line"},
		{"<C-e>", "accept_suggestion_or_end_of_line"},
		{"<C-b>", "backward_char"},
		{"<C-f>", "accept_suggestion_or_forward_char"},
		{"<C-_>", "undo"},                  {"<Left>", "backward_char"},
		{"<Right>", "accept_suggestion_or_forward_char"},
		{"<Home>", "beginning_of_line"},
		{"<End>", "accept_suggestion_or_end_of_line"},
		{"<A-f>", "accept_suggestion_or_forward_word"},
		{"<A-Right>", "accept_suggestion_or_forward_word"},
	};
	for (const auto& [written, action] : expected) {
		const std::string* bound = emacs->action_for(keys(written));
		ASSERT_NE(bound, nullptr) << written;
		EXPECT_EQ(*bound, action) << written;
	}
	EXPECT_EQ(emacs->action_for(keys("<CR>")), nullptr);
	EXPECT_FALSE(emacs->opaque);
	EXPECT_TRUE(emacs->indicator.empty());
}

TEST(LeshperKeymapRegistry, TheAcceptTableIsTheSameSixInEmacsAndViInsert) {
	// #140 decision 4's table, read off the tables themselves: "same four, same
	// two" for the two INSERTING keymaps, and neither a whole-line accept nor a
	// dismissal anywhere in vi_command.
	keymap_registry maps;
	maps.install_defaults();
	const keymap* emacs = maps.find(keymap_registry::emacs);
	const keymap* insert = maps.find(keymap_registry::vi_insert);
	ASSERT_NE(emacs, nullptr);
	ASSERT_NE(insert, nullptr);

	for (const char* written : {"<Right>", "<C-f>", "<End>", "<C-e>", "<A-f>", "<A-Right>"}) {
		const std::string* in_emacs = emacs->action_for(keys(written));
		const std::string* in_insert = insert->action_for(keys(written));
		ASSERT_NE(in_emacs, nullptr) << written;
		ASSERT_NE(in_insert, nullptr) << written;
		EXPECT_EQ(*in_emacs, *in_insert) << written << " differs between the two modes";
	}
}

TEST(LeshperKeymapRegistry, ViCommandAcceptsWordsOnWAndEAndKeepsBPure) {
	// #140's vi row. `w` and `e` are the wrappers zsh-autosuggestions binds for
	// partial accept; `b` is a pure motion because a suggestion lives forward of
	// the cursor; `$` and `l` are pure because command mode has no whole-line
	// accept at all.
	keymap_registry maps;
	maps.install_defaults();
	const keymap* command = maps.find(keymap_registry::vi_command);
	ASSERT_NE(command, nullptr);

	ASSERT_NE(command->action_for(keys("w")), nullptr);
	EXPECT_EQ(*command->action_for(keys("w")), "accept_suggestion_or_word_start_next");
	ASSERT_NE(command->action_for(keys("e")), nullptr);
	EXPECT_EQ(*command->action_for(keys("e")), "accept_suggestion_or_word_end_next");
	ASSERT_NE(command->action_for(keys("b")), nullptr);
	EXPECT_EQ(*command->action_for(keys("b")), "vi_word_prev");
	EXPECT_EQ(*command->action_for(keys("$")), "end_of_line");
	EXPECT_EQ(*command->action_for(keys("l")), "vi_forward_char");
}

TEST(LeshperKeymapRegistry, TheOperatorPendingMapBindsPureMotionsAndNothingThatAccepts) {
	// #140 decision 2's safety argument, as a fact about the table: the `w` in
	// `dw` and the `$` in `d$` dispatch HERE, inside an opaque keymap that got
	// its bindings from `bind_vi_motions` and from nowhere else. There is no
	// condition inside an action to get wrong, so this is where the property
	// lives and where it is asserted. The end-to-end proof is in the vi suite.
	keymap_registry maps;
	maps.install_defaults();
	const keymap* pending = maps.find("vi_operator_pending");
	ASSERT_NE(pending, nullptr);

	for (const keymap::entry& one : pending->entries())
		EXPECT_EQ(one.action.find("accept_suggestion"), std::string::npos)
			<< render_key_notation(one.keys) << " can accept a suggestion mid-operator";

	ASSERT_NE(pending->action_for(keys("w")), nullptr);
	EXPECT_EQ(*pending->action_for(keys("w")), "vi_word_next");
	ASSERT_NE(pending->action_for(keys("e")), nullptr);
	EXPECT_EQ(*pending->action_for(keys("e")), "vi_word_end");
	EXPECT_EQ(*pending->action_for(keys("$")), "end_of_line");
}

TEST(LeshperKeymapRegistry, DismissIsBoundInNoDefaultKeymapAtAll) {
	// #140 decision 4, and it is a decision rather than an omission: a
	// suggestion changes as you type and Ctrl-C clears the line, so the key is
	// not spent. The action is registered and one `bind` away, which is what the
	// note beside it in builtin_actions.cpp says.
	keymap_registry maps;
	maps.install_defaults();
	std::vector<std::string> names;
	maps.names(names);
	for (const std::string& name : names) {
		const keymap* map = maps.find(name);
		ASSERT_NE(map, nullptr);
		for (const keymap::entry& one : map->entries())
			EXPECT_NE(one.action, "dismiss_autosuggestion")
				<< name << " bound " << render_key_notation(one.keys) << " to the dismissal";
	}
}

TEST(LeshperKeymapRegistry, ViCommandIsOpaqueAndViInsertIsNot) {
	// The skeleton's one load-bearing property. Command mode must swallow an
	// unbound printable, and the flag that does it is the same one the completion
	// pager will set (F-29) - not a mode-specific rule in dispatch.
	keymap_registry maps;
	maps.install_defaults();
	ASSERT_NE(maps.find(keymap_registry::vi_command), nullptr);
	EXPECT_TRUE(maps.find(keymap_registry::vi_command)->opaque);
	EXPECT_EQ(maps.find(keymap_registry::vi_command)->indicator, "NORMAL");
	EXPECT_FALSE(maps.find(keymap_registry::vi_insert)->opaque);
	EXPECT_EQ(maps.find(keymap_registry::vi_insert)->indicator, "INSERT");
}

TEST(LeshperKeymapRegistry, CreatingCopiesWhenAskedAndRefusesASourceThatIsNotThere) {
	keymap_registry maps;
	maps.install_defaults();
	ASSERT_NE(maps.create("vi_visual", keymap_registry::vi_command), nullptr);
	EXPECT_TRUE(*maps.find("vi_visual") == *maps.find(keymap_registry::vi_command));
	maps.find("vi_visual")->indicator = "VISUAL";
	EXPECT_EQ(maps.find(keymap_registry::vi_command)->indicator, "NORMAL");

	EXPECT_EQ(maps.create("hopeless", "nonesuch"), nullptr);
	EXPECT_EQ(maps.find("hopeless"), nullptr);
	EXPECT_TRUE(maps.erase("vi_visual"));
	EXPECT_FALSE(maps.erase("vi_visual"));
}

// ---------------------------------------------------------------------------
// The stack: the mode is the base, sub-modes are pushes (#117 decision 5).
// ---------------------------------------------------------------------------

TEST(LeshperKeymapStack, EveryStateStartsInEmacsWithNothingPushed) {
	const state s;
	EXPECT_EQ(s.keymaps.mode(), "emacs");
	EXPECT_EQ(s.keymaps.layers.size(), 1u);
	EXPECT_FALSE(s.keymaps.holding());
	EXPECT_TRUE(s.keymaps.pending_operator.empty());
}

TEST(LeshperKeymapStack, SetModeSwapsTheBaseAndTakesThePushesWithIt) {
	// The decision, and it is a decision: a sub-mode is a modifier ON a mode, so
	// leaving one stranded over a base it was never pushed onto would shadow the
	// new mode with bindings that no longer mean anything. `i` out of visual mode
	// lands in insert mode, not in visual-over-insert.
	keymap_stack stack;
	stack.push("vi_visual");
	stack.pending_operator = "delete_region";
	ASSERT_EQ(stack.layers.size(), 2u);

	stack.set_mode(keymap_registry::vi_insert);
	EXPECT_EQ(stack.layers.size(), 1u);
	EXPECT_EQ(stack.mode(), "vi_insert");
	EXPECT_TRUE(stack.pending_operator.empty());
}

TEST(LeshperKeymapStack, PushAndPopAreSymmetricAndTheBaseCannotBePoppedAway) {
	keymap_stack stack;
	stack.push("vi_visual");
	stack.push("vi_operator_pending");
	EXPECT_EQ(stack.layers.size(), 3u);
	EXPECT_TRUE(stack.pop());
	EXPECT_TRUE(stack.pop());
	// A mode is not something one pops out of, only something one swaps.
	EXPECT_FALSE(stack.pop());
	EXPECT_EQ(stack.mode(), "emacs");
}

TEST(LeshperKeymapStack, TheIndicatorIsTheTopmostKeymapThatClaimsOne) {
	// F-40: VISUAL shows while pushed, and the pager - declaring none - hides
	// nothing that was showing beneath it.
	keymap_registry maps;
	maps.install_defaults();
	maps.create("vi_visual");
	maps.find("vi_visual")->indicator = "VISUAL";
	maps.create("pager");   // declares none on purpose

	keymap_stack stack;
	stack.set_mode(keymap_registry::vi_command);
	EXPECT_EQ(indicator_of(maps, stack), "NORMAL");
	stack.push("vi_visual");
	EXPECT_EQ(indicator_of(maps, stack), "VISUAL");
	stack.push("pager");
	EXPECT_EQ(indicator_of(maps, stack), "VISUAL");
	stack.pop();
	stack.pop();
	EXPECT_EQ(indicator_of(maps, stack), "NORMAL");

	stack.set_mode(keymap_registry::emacs);
	EXPECT_TRUE(indicator_of(maps, stack).empty());
}

TEST(LeshperKeymapStack, ThePendingOperatorIsASlotWithPushPopAndClearAroundIt) {
	// zle's `viopp` written down (#117 decision 6). This ticket owns the
	// MECHANICS - the slot, and the push and pop around it - and #119 owns the vi
	// verbs that use them. Written as the sequence a `d` then a motion makes, so
	// that the ticket that adds `d` has the shape to fill in.
	keymap_stack stack;
	stack.set_mode(keymap_registry::vi_command);

	// The verb stores itself and pushes.
	stack.pending_operator = "vi_delete";
	stack.push("vi_operator_pending");
	EXPECT_EQ(stack.layers.size(), 2u);

	// Dispatch pops, invokes, and clears.
	EXPECT_TRUE(stack.pop());
	stack.pending_operator.clear();
	EXPECT_TRUE(stack.pending_operator.empty());
	EXPECT_EQ(stack.mode(), "vi_command");

	// And Escape pops AND clears, which is why the two are separate operations
	// rather than one that always does both.
	stack.pending_operator = "vi_change";
	stack.push("vi_operator_pending");
	stack.set_mode(keymap_registry::vi_command);
	EXPECT_TRUE(stack.pending_operator.empty());
	EXPECT_EQ(stack.layers.size(), 1u);
}

// ---------------------------------------------------------------------------
// Dispatch (#117 decision 4).
// ---------------------------------------------------------------------------

TEST(LeshperKeymapDispatch, TheEmacsDefaultsEditTheLineTheWayTheHardcodedTableDid) {
	// The behavioural half of "the table moved in rather than being rewritten".
	state s;
	type(s, "echo hi");
	EXPECT_EQ(text_of(s), "echo hi");
	press(s, named_key::home);
	EXPECT_EQ(cursor_of(s), 0u);
	press(s, named_key::end);
	EXPECT_EQ(cursor_of(s), 7u);
	press(s, control_w);
	EXPECT_EQ(text_of(s), "echo ");
	press(s, static_cast<char32_t>(0x01));   // Ctrl-A
	EXPECT_EQ(cursor_of(s), 0u);
	press(s, static_cast<char32_t>(0x1F));   // Ctrl-_, undo
	EXPECT_EQ(text_of(s), "echo hi");
}

TEST(LeshperKeymapDispatch, MotionOverACombiningMarkMovesByTheWholeCluster) {
	// What `MotionIsGraphemeWiseWhereTheEnumPathIsStillScalarWise` used to pin as
	// a DISAGREEMENT, asserted here as the single answer. The enum switch stepped
	// scalar values and stopped between the `e` and its accent; dispatch now runs
	// the registered `backward_char`, which asks the editor, which asks #108's
	// segmenter. F-3, finally, on the path a user's arrow key takes.
	state s;
	type(s, "x");
	press(s, U'e');
	press(s, static_cast<char32_t>(0x0301));   // COMBINING ACUTE ACCENT
	ASSERT_EQ(text_of(s), "xe\xCC\x81");
	ASSERT_EQ(cursor_of(s), 4u);

	press(s, named_key::left);
	EXPECT_EQ(cursor_of(s), 1u);   // over the whole cluster, not into it
	press(s, named_key::right);
	EXPECT_EQ(cursor_of(s), 4u);
	press(s, delete_character);
	EXPECT_EQ(text_of(s), "x");    // and one backspace takes the whole cluster
}

TEST(LeshperKeymapDispatch, ThePrintableFloorIsSelfInsertAndNothingElseIs) {
	// #117's floor: the one binding that is a rule rather than a table row. Alt-a
	// is not the character `a`, and Ctrl-A already IS a character - U+0001 - which
	// is below the floor rather than on it.
	EXPECT_TRUE(is_self_inserting(key_event::of(U'x')));
	EXPECT_TRUE(is_self_inserting(key_event::of(static_cast<char32_t>(0xE9))));
	EXPECT_FALSE(is_self_inserting(key_event::of(U'a', key_modifiers{.alt = true})));
	EXPECT_FALSE(is_self_inserting(key_event::of(named_key::up)));
	EXPECT_FALSE(is_self_inserting(key_event::of(U'\n')));
	EXPECT_FALSE(is_self_inserting(key_event::of(delete_character)));

	state s;
	press(s, U'a', key_modifiers{.alt = true});
	EXPECT_TRUE(s.buffer.empty());   // and an unbound Alt-a types nothing
	press(s, U'a');
	EXPECT_EQ(text_of(s), "a");
}

TEST(LeshperKeymapDispatch, TheTopmostKeymapWinsAndTheOneBelowStillAnswersForTheRest) {
	// Top-down, first exact match. The push shadows one binding and leaves the
	// rest of the base reachable, which is what makes a sub-mode a sub-mode rather
	// than a replacement.
	state s;
	editing_context& context = context_of(s);
	keymap* over = context.keymaps().create("shouty");
	over->bind(keys("<C-a>"), "end_of_line");

	type(s, "echo hi");
	s.keymaps.push("shouty");
	press(s, static_cast<char32_t>(0x01));   // Ctrl-A, rebound above
	EXPECT_EQ(cursor_of(s), 7u);
	press(s, static_cast<char32_t>(0x02));   // Ctrl-B, only the base binds it
	EXPECT_EQ(cursor_of(s), 6u);
}

TEST(LeshperKeymapDispatch, AnOpaqueKeymapStopsLookupAndTakesTheFloorWithIt) {
	// F-29's hatch, and the rule that makes vi command mode possible without a
	// mode-specific branch anywhere in dispatch: an opaque keymap is the bottom of
	// the stack while it is pushed, and the `self_insert` floor is conceptually
	// beneath the bottom - so it goes too. A pager whose map swallowed keys but
	// still let printables type themselves would be no pager at all.
	state s;
	editing_context& context = context_of(s);
	keymap* pager = context.keymaps().create("pager");
	pager->opaque = true;
	pager->bind(keys("<C-b>"), "backward_char");

	type(s, "echo");
	s.keymaps.push("pager");
	const effects nothing = press(s, U'z');
	EXPECT_EQ(text_of(s), "echo");   // swallowed, not typed
	EXPECT_TRUE(nothing.empty());
	press(s, static_cast<char32_t>(0x01));   // Ctrl-A: the base is unreachable
	EXPECT_EQ(cursor_of(s), 4u);
	press(s, static_cast<char32_t>(0x02));   // but the pager's own binding runs
	EXPECT_EQ(cursor_of(s), 3u);
}

TEST(LeshperKeymapDispatch, AnOpaqueKeymapsDefaultActionCatchesWhatItSwallowed) {
	// The other half of F-29: the pager routes unbound printables to its filter
	// action. Nothing in dispatch knows what a pager is.
	state s;
	editing_context& context = context_of(s);
	keymap* pager = context.keymaps().create("pager");
	pager->opaque = true;
	pager->default_action = "self_insert";   // stands in for the filter action

	s.keymaps.push("pager");
	type(s, "zq");
	EXPECT_EQ(text_of(s), "zq");
}

TEST(LeshperKeymapDispatch, ViCommandModeSwallowsAnUnboundPrintableAndStillMoves) {
	state s;
	type(s, "echo hi");
	s.keymaps.set_mode(keymap_registry::vi_command);
	type(s, "zqx");
	EXPECT_EQ(text_of(s), "echo hi");   // nothing typed itself
	press(s, U'0');
	EXPECT_EQ(cursor_of(s), 0u);
	press(s, U'l');
	EXPECT_EQ(cursor_of(s), 1u);
	press(s, U'$');
	EXPECT_EQ(cursor_of(s), 7u);
}

TEST(LeshperKeymapDispatch, AMultiKeySequenceRunsOnItsLastKey) {
	state s;
	editing_context& context = context_of(s);
	context.keymaps().find(keymap_registry::emacs)
		->bind(keys("<C-x><C-e>"), "beginning_of_line");

	type(s, "echo hi");
	const effects held = press(s, control_x);
	EXPECT_TRUE(held.empty());              // nothing ran, and nothing redrew
	EXPECT_TRUE(s.keymaps.holding());
	EXPECT_EQ(cursor_of(s), 7u);

	press(s, static_cast<char32_t>(0x05));  // Ctrl-E completes the sequence
	EXPECT_FALSE(s.keymaps.holding());
	EXPECT_EQ(cursor_of(s), 0u);            // beginning_of_line, not end_of_line
}

TEST(LeshperKeymapDispatch, APrefixHoldsEvenWhenTheShorterSequenceIsBoundToo) {
	// "resolves to the LONGEST exact match": with both `<C-x>` and `<C-x><C-e>`
	// bound, the short one must WAIT rather than fire, or the long one could
	// never be reached.
	state s;
	editing_context& context = context_of(s);
	keymap* emacs = context.keymaps().find(keymap_registry::emacs);
	emacs->bind(keys("<C-x>"), "end_of_line");
	emacs->bind(keys("<C-x><C-e>"), "beginning_of_line");

	type(s, "echo hi");
	press(s, named_key::home);
	press(s, control_x);
	EXPECT_TRUE(s.keymaps.holding());
	EXPECT_EQ(cursor_of(s), 0u);   // the short binding did NOT fire
}

TEST(LeshperKeymapDispatch, TheHoldResolvesToTheShorterBindingWhenTheDeadlinePasses) {
	// F-5, and the deadline is a PARAMETER: leshper never reads a clock. The loop
	// hands `now` in with the key, asks when to wake, and calls back at that
	// instant - the same three-call shape input_decoder has, for the same reason.
	using clock = std::chrono::steady_clock;
	state s;
	editing_context& context = context_of(s);
	keymap* emacs = context.keymaps().find(keymap_registry::emacs);
	emacs->bind(keys("<C-x>"), "end_of_line");
	emacs->bind(keys("<C-x><C-e>"), "beginning_of_line");

	type(s, "echo hi");
	press(s, named_key::home);

	const clock::time_point pressed_at = clock::now();
	EXPECT_TRUE(step(s, key_event::of(control_x), pressed_at).empty());
	const auto deadline = keymap_deadline(s);
	ASSERT_TRUE(deadline.has_value());
	EXPECT_EQ(*deadline, pressed_at + context.key_timeout);

	// An early wake - poll(2) returning on a signal - must not resolve a hold
	// that is still legitimately in flight.
	EXPECT_TRUE(keymap_expire(s, pressed_at).empty());
	EXPECT_TRUE(s.keymaps.holding());
	EXPECT_EQ(cursor_of(s), 0u);

	const effects resolved = keymap_expire(s, *deadline);
	EXPECT_FALSE(resolved.empty());
	EXPECT_FALSE(s.keymaps.holding());
	EXPECT_EQ(cursor_of(s), 7u);          // end_of_line, the longest exact match
	EXPECT_FALSE(keymap_deadline(s).has_value());
}

TEST(LeshperKeymapDispatch, ALonePrintableHeldOnlyByALongerBindingTypesItselfOnTheTimeout) {
	// The other half of the timeout, and the one that keeps a multi-key binding
	// from stealing a letter. `gg` bound means `g` holds; waiting must then type
	// the `g` the user asked for, because nothing longer can arrive and the floor
	// is the shorter match. zle's rule; the alternative - swallowing it - would
	// make binding any sequence over a printable unusable.
	state s;
	editing_context& context = context_of(s);
	context.keymaps().find(keymap_registry::emacs)->bind(keys("gg"), "beginning_of_line");

	type(s, "echo");
	press(s, U'g');
	EXPECT_TRUE(s.keymaps.holding());
	EXPECT_EQ(text_of(s), "echo");

	const effects typed = keymap_expire(s, std::chrono::steady_clock::now());
	EXPECT_FALSE(typed.empty());
	EXPECT_EQ(text_of(s), "echog");
	EXPECT_FALSE(s.keymaps.holding());

	// And the sequence itself still wins when it is completed in time.
	press(s, U'g');
	press(s, U'g');
	EXPECT_EQ(cursor_of(s), 0u);
	EXPECT_EQ(text_of(s), "echog");
}

TEST(LeshperKeymapDispatch, AHoldThatMatchesNothingIsDroppedRatherThanTyped) {
	// The recovery from a mistake. Typing the `q` out of an abandoned `<C-x>q`
	// would put a character in the line the user never asked for, so an unmatched
	// SEQUENCE runs nothing - the floor and the default action both apply to a
	// lone key and deliberately not to this.
	state s;
	editing_context& context = context_of(s);
	context.keymaps().find(keymap_registry::emacs)
		->bind(keys("<C-x><C-e>"), "beginning_of_line");

	type(s, "echo");
	press(s, control_x);
	ASSERT_TRUE(s.keymaps.holding());
	const effects nothing = press(s, U'q');
	EXPECT_TRUE(nothing.empty());
	EXPECT_EQ(text_of(s), "echo");
	EXPECT_FALSE(s.keymaps.holding());

	// And an expiry with nothing bound at the held sequence drops it too.
	press(s, control_x);
	ASSERT_TRUE(s.keymaps.holding());
	EXPECT_TRUE(keymap_expire(s, std::chrono::steady_clock::now()).empty());
	EXPECT_FALSE(s.keymaps.holding());
	EXPECT_EQ(text_of(s), "echo");
}

TEST(LeshperKeymapDispatch, AKeymapNamingAnActionNobodyRegisteredIsAMissAndNotACrash) {
	// The same rule dispatch through an unregistered name follows on the ABI side
	// (ADR-0008): a miss leaves the state alone. An rc file that binds a typo must
	// not take the shell down with it.
	state s;
	editing_context& context = context_of(s);
	context.keymaps().find(keymap_registry::emacs)->bind(keys("<C-b>"), "backwrad_char");

	type(s, "echo");
	const state before = s;
	const effects nothing = press(s, static_cast<char32_t>(0x02));
	EXPECT_TRUE(nothing.empty());
	EXPECT_TRUE(s == before);
}

TEST(LeshperKeymapDispatch, ARebindingTakesEffectOnTheVeryNextKeystroke) {
	// F-13's point: every built-in behaviour is a named action, and naming it is
	// what makes it rebindable. There is no enum between the key and the action
	// any more, so this is a table edit and nothing else.
	state s;
	editing_context& context = context_of(s);
	type(s, "echo hi");
	press(s, static_cast<char32_t>(0x01));
	ASSERT_EQ(cursor_of(s), 0u);

	context.keymaps().find(keymap_registry::emacs)->bind(keys("<C-a>"), "end_of_line");
	press(s, static_cast<char32_t>(0x01));
	EXPECT_EQ(cursor_of(s), 7u);
}

TEST(LeshperKeymapDispatch, InjectedInputRunsThroughTheKeymapAndNotAroundIt) {
	// F-7, zle's `zle -U`: text pushed onto the input stack is read back AS THOUGH
	// TYPED, so a control character in it invokes its binding. Splicing it into
	// the buffer would be quicker and would break A-12.
	state s;
	type(s, "echo hi");
	step(s, injected_input{std::string("\x01") + "X"});
	EXPECT_EQ(text_of(s), "Xecho hi");   // Ctrl-A moved home, then X typed there
}

TEST(LeshperKeymapDispatch, ACandidateSequenceIsBuiltInsideTheStringsSmallBuffer) {
	// N-2's constraint, as the property that satisfies it rather than as a timing.
	// Dispatch builds `pending + this key` on every keystroke, and that string
	// must not reach the heap. Six bytes a key is what buys it: the small-string
	// buffer holds three keys, and no default keymap binds a sequence longer than
	// two - so a held prefix costs an allocation only where a user has bound one
	// deliberately, and never on the ordinary keystroke.
	std::string probe;
	const size_t small = probe.capacity();
	EXPECT_GE(small, 3 * encoded_key_size);

	encode_key(key_event::of(control_x), probe);
	encode_key(key_event::of(U'e'), probe);
	encode_key(key_event::of(named_key::left), probe);
	EXPECT_EQ(probe.capacity(), small) << "building a three-key candidate reached the heap";

	// And the longest action name a default table names still fits, so the
	// resolution that carries it back does not allocate either.
	EXPECT_LE(std::string_view("delete_backward_word").size(), small);
}

TEST(LeshperKeymapDispatch, TwoStatesShareNothingAndAContextIsSharedByCopy) {
	// The ownership decision, asserted. A registry is ENVIRONMENT: copying a state
	// must not fork the user's bindings, and N-3's equality must not compare them.
	state first;
	context_of(first).keymaps().find(keymap_registry::emacs)
		->bind(keys("<C-b>"), "end_of_line");

	state second;
	type(second, "echo hi");
	press(second, static_cast<char32_t>(0x02));
	EXPECT_EQ(cursor_of(second), 6u);   // its own default table, unaffected

	// A copy shares the context and stays EQUAL to its original, because the
	// context is not part of what a state is.
	const state copied = first;
	EXPECT_TRUE(copied == first);
	EXPECT_EQ(copied.context, first.context);
}

// ---------------------------------------------------------------------------
// `bind`, and the link boundary (#117 decision 7, #118).
// ---------------------------------------------------------------------------

namespace {

// THE ADAPTER, and it lives in a TEST because of where the link graph puts it.
//
// `lesh_runtime` does not link `lesh_leshper` - `lesh` is built on
// `lesh_runtime lesh_syntax lesh_ui` and nothing else - so a builtin cannot call
// a keymap function directly, and making it able to would drag the whole editor
// into every `lesh -c`. `binding_console` is the narrow interface that crosses,
// declared by the runtime and implemented wherever both sides are linked. This
// class is that implementation; the loop will need the same twenty lines when it
// is wired up, and this test proves they are enough.
class leshper_binding_console final : public lesh::runtime::binding_console {
public:
	explicit leshper_binding_console(editing_context& context) : _context(&context) {}

	void keymap_names(std::vector<std::string>& into) const override {
		_context->keymaps().names(into);
	}

	outcome create_keymap(std::string_view name, std::string_view from) override {
		return _context->keymaps().create(name, from) != nullptr ? outcome::ok
		                                                         : outcome::no_such_keymap;
	}

	outcome bind_key(std::string_view name, std::string_view notation,
	                 std::string_view action) override {
		keymap* map = keymap_for(name);
		if (map == nullptr)
			return outcome::no_such_keymap;
		std::string encoded;
		if (!parse_key_notation(notation, encoded))
			return outcome::bad_notation;
		if (!action.empty()) {
			// Bound to something that exists, or the binding is a typo that only
			// shows up as a dead key months later.
			int32_t exists = 0;
			const std::string name_of_action{action};
			if (lesh_action_exists(&_context->actions(), name_of_action.c_str(), &exists)
			        != LESH_OK
			    || exists == 0)
				return outcome::no_such_action;
		}
		map->bind(encoded, action);
		return outcome::ok;
	}

	outcome lookup_key(std::string_view name, std::string_view notation,
	                   std::string& action_out) const override {
		const keymap* map = keymap_for(name);
		if (map == nullptr)
			return outcome::no_such_keymap;
		std::string encoded;
		if (!parse_key_notation(notation, encoded))
			return outcome::bad_notation;
		const std::string* bound = map->action_for(encoded);
		action_out = bound != nullptr ? *bound : std::string{};
		return outcome::ok;
	}

	outcome list_bindings(std::string_view name,
	                      std::vector<std::pair<std::string, std::string>>& into) const override {
		const keymap* map = keymap_for(name);
		if (map == nullptr)
			return outcome::no_such_keymap;
		into.clear();
		for (const keymap::entry& one : map->entries())
			into.emplace_back(render_key_notation(one.keys), one.action);
		return outcome::ok;
	}

private:
	[[nodiscard]] keymap* keymap_for(std::string_view name) const {
		return _context->keymaps().find(name.empty() ? keymap_registry::emacs : name);
	}

	editing_context* _context;
};

// Runs one `bind` command line and answers what it wrote and what it returned.
//
// A redirection of the real descriptors rather than a stub stream, because the
// builtin writes with printf and the point is to test the builtin.
struct bind_run {
	int status = 0;
	std::string output;
};

// The console the next `run_bind` hands its shell. A file-scope pointer HERE
// rather than in the shell (#134 moved the real seam onto `shell_state`) only
// because `run_bind` builds its own shell per call: the guard below installs
// one for the duration of a test and the shell picks it up.
lesh::runtime::binding_console* g_test_console = nullptr;

bind_run run_bind(const std::vector<std::string>& words) {
	std::vector<char*> argv;
	std::vector<std::string> owned = words;
	for (std::string& one : owned)
		argv.push_back(one.data());
	argv.push_back(nullptr);

	const lesh::testing::temp_path scratch;
	const std::string path = scratch.file("out");
	std::fflush(stdout);
	std::fflush(stderr);
	const int saved_out = ::dup(STDOUT_FILENO);
	const int saved_err = ::dup(STDERR_FILENO);
	const int into = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	EXPECT_GE(into, 0);
	::dup2(into, STDOUT_FILENO);
	::dup2(into, STDERR_FILENO);
	::close(into);

	lesh::runtime::shell_state shell;
	shell.set_binding_console(g_test_console);
	lesh::runtime::builtin_result result;
	const bool ran = lesh::runtime::try_run_builtin(shell, argv.data(), result, false);

	std::fflush(stdout);
	std::fflush(stderr);
	::dup2(saved_out, STDOUT_FILENO);
	::dup2(saved_err, STDERR_FILENO);
	::close(saved_out);
	::close(saved_err);

	EXPECT_TRUE(ran) << "bind is not in the handler table";
	std::ifstream in{path};
	std::ostringstream text;
	text << in.rdbuf();
	return bind_run{result.status, text.str()};
}

// Installs a console for the duration of one test and takes it away afterwards,
// so a test that leaves one behind cannot make the next test's `bind` succeed.
class console_guard {
public:
	explicit console_guard(editing_context& context) : _console(context) {
		g_test_console = &_console;
	}
	~console_guard() { g_test_console = nullptr; }

	console_guard(const console_guard&) = delete;
	console_guard& operator=(const console_guard&) = delete;

private:
	leshper_binding_console _console;
};

} // namespace

TEST(LeshperKeymapBind, WithNoLineEditorItSaysSoRatherThanPretending) {
	// A non-interactive shell has no keymaps to mutate, and `bind` in an rc file
	// guarded for both kinds of shell must not end the script - so this is an
	// OPERATIONAL failure and not a usage error.
	g_test_console = nullptr;
	const bind_run ran = run_bind({"bind", "-l"});
	EXPECT_EQ(ran.status, 1);
	EXPECT_NE(ran.output.find("no line editor"), std::string::npos);
}

TEST(LeshperKeymapBind, ListsTheKeymapsItHas) {
	state s;
	const console_guard guard{context_of(s)};
	const bind_run ran = run_bind({"bind", "-l"});
	EXPECT_EQ(ran.status, 0);
	EXPECT_EQ(ran.output,
	          "emacs\npager\nvi_command\nvi_find_char\nvi_insert\n"
	          "vi_operator_pending\nvi_replace_char\nvi_visual\n");
}

TEST(LeshperKeymapBind, BindsAndThenAnswersWhatItBound) {
	state s;
	editing_context& context = context_of(s);
	const console_guard guard{context};

	EXPECT_EQ(run_bind({"bind", "<C-t>", "end_of_line"}).status, 0);
	const bind_run asked = run_bind({"bind", "<C-t>"});
	EXPECT_EQ(asked.status, 0);
	EXPECT_EQ(asked.output, "<C-t> end_of_line\n");

	// And the shell's edit really changed the editor's dispatch.
	type(s, "echo hi");
	press(s, named_key::home);
	press(s, static_cast<char32_t>(0x14));   // Ctrl-T
	EXPECT_EQ(cursor_of(s), 7u);

	// An unbound sequence prints nothing and answers 1, so it is a test.
	// `<C-q>` rather than `<C-y>`: #119 gave `<C-y>` to `yank`, the emacs side of
	// the one kill store.
	const bind_run missing = run_bind({"bind", "<C-q>"});
	EXPECT_EQ(missing.status, 1);
	EXPECT_TRUE(missing.output.empty());
}

TEST(LeshperKeymapBind, TheMinusMOptionSelectsWhichKeymapTheOperandsApplyTo) {
	state s;
	editing_context& context = context_of(s);
	const console_guard guard{context};

	EXPECT_EQ(run_bind({"bind", "-m", "vi_command", "H", "beginning_of_line"}).status, 0);
	EXPECT_EQ(run_bind({"bind", "-m", "vi_command", "H"}).output, "H beginning_of_line\n");
	// The default keymap is emacs, and the binding did not land there.
	EXPECT_EQ(run_bind({"bind", "H"}).status, 1);

	const bind_run nonesuch = run_bind({"bind", "-m", "nonesuch", "H", "undo"});
	EXPECT_EQ(nonesuch.status, 1);
	EXPECT_NE(nonesuch.output.find("no such keymap"), std::string::npos);
}

TEST(LeshperKeymapBind, MinusNCreatesAKeymapAndCopiesOneWhenAsked) {
	state s;
	editing_context& context = context_of(s);
	const console_guard guard{context};

	EXPECT_EQ(run_bind({"bind", "-N", "vi_visual", "vi_command"}).status, 0);
	ASSERT_NE(context.keymaps().find("vi_visual"), nullptr);
	EXPECT_TRUE(*context.keymaps().find("vi_visual")
	            == *context.keymaps().find(keymap_registry::vi_command));

	EXPECT_EQ(run_bind({"bind", "-N", "empty"}).status, 0);
	ASSERT_NE(context.keymaps().find("empty"), nullptr);
	EXPECT_TRUE(context.keymaps().find("empty")->empty());

	const bind_run nonesuch = run_bind({"bind", "-N", "hopeless", "nonesuch"});
	EXPECT_EQ(nonesuch.status, 1);
	EXPECT_EQ(context.keymaps().find("hopeless"), nullptr);
}

TEST(LeshperKeymapBind, TheListingIsReInputtable) {
	// `alias`'s property, and for the same reason: `bind -m emacs > f` and reading
	// `f` back has to rebuild the same table, which is only true if every line is
	// notation the parser accepts.
	state s;
	editing_context& context = context_of(s);
	const console_guard guard{context};

	const bind_run listed = run_bind({"bind"});
	ASSERT_EQ(listed.status, 0);
	ASSERT_FALSE(listed.output.empty());

	keymap rebuilt;
	std::istringstream lines{listed.output};
	std::string written;
	std::string action;
	size_t rows = 0;
	while (lines >> written >> action) {
		std::string encoded;
		ASSERT_TRUE(parse_key_notation(written, encoded)) << written;
		rebuilt.bind(encoded, action);
		++rows;
	}
	EXPECT_EQ(rows, context.keymaps().find(keymap_registry::emacs)->entries().size());
	EXPECT_TRUE(rebuilt == *context.keymaps().find(keymap_registry::emacs));
}

TEST(LeshperKeymapBind, ARefusedCommandLineIsAUsageErrorAndAWrongNameIsNot) {
	state s;
	const console_guard guard{context_of(s)};

	// Usage errors: status 2, the answer every other builtin gives for a command
	// line that is not the shape the utility accepts.
	EXPECT_EQ(run_bind({"bind", "-z"}).status, 2);
	EXPECT_EQ(run_bind({"bind", "-m"}).status, 2);
	EXPECT_EQ(run_bind({"bind", "-l", "extra"}).status, 2);
	EXPECT_EQ(run_bind({"bind", "a", "b", "c"}).status, 2);

	// A name that does not resolve is a failure of the operation, not of the
	// command line.
	const bind_run typo = run_bind({"bind", "<C-t>", "backwrad_char"});
	EXPECT_EQ(typo.status, 1);
	EXPECT_NE(typo.output.find("no such action"), std::string::npos);
	const bind_run garbage = run_bind({"bind", "<Nonesuch>", "undo"});
	EXPECT_EQ(garbage.status, 1);
	EXPECT_NE(garbage.output.find("not a key sequence"), std::string::npos);
}
