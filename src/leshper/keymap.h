#pragma once

// Keymaps as data (#117, #118, architecture spec §6.4).
//
// A keymap is a flat table from a SEQUENCE OF DECODED KEY EVENTS to an action
// NAME. Three words in that sentence are decisions:
//
//   SEQUENCE, because F-12 binds key sequences and not keys, and because the
//   prefix-hold that makes `<C-x><C-e>` possible needs the table to answer "is
//   anything longer than this bound" as cheaply as it answers "is this bound".
//
//   DECODED KEY EVENTS, never raw bytes. zle's model reversed deliberately: the
//   decoder (#111) owns escape sequences exactly once, so a binding is written
//   against `<Up>` rather than against whatever `ESC O A` your terminal happens
//   to send. `<C-w>` is NOTATION that parses to U+0017 - at the #97 floor that
//   is all the wire carries - and the `ctrl` modifier bit is set only where a
//   terminal says so in a CSI parameter (`<C-Left>`), exactly as event.h
//   describes.
//
//   ACTION NAME, never another key sequence. That is why vim's map/noremap
//   distinction has nothing to distinguish here: a binding cannot expand to keys
//   and so cannot recursively re-enter the map.
//
// THE KEY IS A CANONICAL ENCODING, and this is the one shape decision worth
// arguing. Each key event encodes to a fixed six bytes - kind, modifier bits,
// and a big-endian value - so a sequence is a byte string, ordered
// lexicographically exactly as it would be if compared key by key, and "is this
// a prefix of that" is `starts_with`. What that buys: the table is a sorted
// vector searched with `std::lower_bound` and no allocation, and `keymap_stack`
// can live in state.h holding nothing but bytes, which is what keeps state.h
// from having to include event.h (event.h includes state.h - the cycle is real).
//
// It is NOT a retreat to zle's byte strings. What is encoded is the symbolic
// event - a named-key enumerator or a Unicode scalar, plus modifier bits - after
// the decoder has resolved the terminal's escape sequence. Nothing here can see
// a terminal's bytes and nothing here would know what to do with them.

#include "leshper/event.h"
#include "leshper/registry.h"
#include "leshper/state.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lesh::leshper {

// How many bytes one key event occupies in an encoded sequence. Fixed, so that
// a byte prefix is a key prefix and never half a key.
inline constexpr std::size_t encoded_key_size = 6;

// Appends the canonical encoding of one key event.
void encode_key(const key_event& key, std::string& into);

[[nodiscard]] std::string encode_key(const key_event& key);

// Reads one key event out of an encoded sequence, advancing `at`. False when
// there are not `encoded_key_size` bytes left or the record is malformed - which
// only a corrupted table could produce, and is answered rather than asserted so
// that `bind`'s listing of a table it did not build cannot crash the shell.
[[nodiscard]] bool decode_key(std::string_view encoded, std::size_t& at, key_event& out);

// ---------------------------------------------------------------------------
// Vim notation (#117 decision 1): `<C-w>`, `<A-Left>`, `<Up>`, `<S-Tab>`, and
// literal printables. The neovim feel, because that is what the owner types.
// ---------------------------------------------------------------------------

// Parses notation into an encoded sequence. False - with `into` left untouched -
// when the text is not notation this vocabulary can express, so that a typo in
// an rc file is reported rather than silently bound to something else.
//
// THE RULES, in the order they surprise people:
//
//   `C-` on a codepoint folds to the C0 control: `<C-w>` IS U+0017, and binding
//   it is binding what the terminal sends. A codepoint with no C0 spelling
//   (`<C-1>`) is refused rather than given a modifier bit no terminal will ever
//   set at the #97 floor.
//
//   `C-` on a NAMED key sets the ctrl bit, because `<C-Left>` has no control
//   character at all and arrives as a CSI parameter (`ESC [ 1 ; 5 D`).
//
//   `S-` on an ASCII letter uppercases it - `<S-a>` is `A`, which is what the
//   keyboard sends - and on anything else sets the shift bit.
//
//   `A-` and `M-` both mean Alt, and set the alt bit the decoder's ESC-prefix
//   rule sets.
//
// `<S-Tab>` parses today and nothing produces it yet: the decoder does not read
// CSI Z. The notation is honest about what a keymap can hold, and the decoder
// grows into it additively.
[[nodiscard]] bool parse_key_notation(std::string_view text, std::string& into);

// The inverse, for `bind`'s listing. Round-trips through parse_key_notation.
[[nodiscard]] std::string render_key_notation(std::string_view encoded);

// The bytes a key sequence would have typed, for `lesh_invocation::keys`.
//
// Codepoints only: a named key is not text and contributes nothing, which is
// what makes `self_insert` bound to `<Up>` insert nothing rather than garbage.
void encoded_keys_as_text(std::string_view encoded, std::string& into);

// ---------------------------------------------------------------------------
// The keymap.
// ---------------------------------------------------------------------------

// A flat table, plus the three per-keymap knobs #117 decided.
//
// FIRST-CLASS DATA (F-11): copyable, assignable, and mutable through ordinary
// operations. `bind -N vi_visual vi_command` is a copy construction; there is no
// keymap-building DSL and no second dispatch system.
class keymap {
public:
	struct entry {
		std::string keys;    // encoded, never notation
		std::string action;

		friend bool operator==(const entry&, const entry&) noexcept = default;
	};

	// Binds, replacing any existing binding of the same sequence - so re-sourcing
	// an rc file is idempotent, the same rule #101 gives action registration.
	// An empty action name UNBINDS, which is how `bind '<C-w>' ''` removes one.
	void bind(std::string_view keys, std::string_view action);

	bool unbind(std::string_view keys);

	// The action bound to exactly this sequence, or null.
	[[nodiscard]] const std::string* action_for(std::string_view keys) const noexcept;

	// True when some binding STRICTLY extends `keys` - the question that decides
	// whether dispatch holds. Strictly: an exact match is not its own prefix, or
	// every complete binding would hold forever.
	[[nodiscard]] bool has_longer(std::string_view keys) const noexcept;

	[[nodiscard]] const std::vector<entry>& entries() const noexcept { return _entries; }
	[[nodiscard]] bool empty() const noexcept { return _entries.empty(); }

	// STOPS LOOKUP, and stops the `self_insert` floor with it (F-29). An opaque
	// keymap is the bottom of the stack for as long as it is pushed: nothing
	// below it is consulted, and neither is the global floor - which is the whole
	// of what the completion pager and vi command mode need. The pager routes
	// unbound printables to its filter through `default_action`; vi command mode
	// declares no default and so swallows them, which is what vi does.
	bool opaque = false;

	// What an unmatched SINGLE key runs, if anything. Topmost declaring keymap
	// wins. Deliberately not consulted for an unmatched multi-key sequence: a
	// held prefix that went nowhere is a mistake, and feeding its last key to the
	// pager's filter would type the `<C-x>` the user was in the middle of.
	std::string default_action;

	// F-40's mode indicator, empty when this keymap does not claim one.
	std::string indicator;

	friend bool operator==(const keymap&, const keymap&) noexcept = default;

private:
	// Sorted by `keys`. Fixed-width key records make byte order key order, so
	// `lower_bound` answers both questions above.
	std::vector<entry> _entries;
};

// ---------------------------------------------------------------------------
// The keymap registry: name -> keymap, beside the action registry (#117
// decision 8).
//
// The NAME is the identity. A keymap stack holds names rather than pointers, so
// re-creating `emacs` re-points every stack that names it, a state stays
// comparable for N-3's replay, and no stack can outlive the table it points
// into.
// ---------------------------------------------------------------------------
class keymap_registry {
public:
	// The four default keymaps' names, spelled once.
	static constexpr std::string_view emacs = "emacs";
	static constexpr std::string_view vi_insert = "vi_insert";
	static constexpr std::string_view vi_command = "vi_command";

	// Creates (or replaces) `name`, optionally as a copy of `copy_from`. Null
	// `copy_from` makes an empty one; a non-null name that does not exist is a
	// failure and nothing is created.
	keymap* create(std::string_view name, std::string_view copy_from = {});

	[[nodiscard]] keymap* find(std::string_view name) noexcept;
	[[nodiscard]] const keymap* find(std::string_view name) const noexcept;

	bool erase(std::string_view name);

	// Sorted, because `bind -l` is read by a human.
	void names(std::vector<std::string>& into) const;

	[[nodiscard]] std::size_t size() const noexcept { return _maps.size(); }

	// `emacs`, `vi_insert` and `vi_command`, as data.
	//
	// The emacs table is the hardcoded switch that used to live in editor.cpp,
	// moved rather than rewritten. The two vi tables are SKELETONS: enough that
	// the stack has something to swap its base to and that the opaque/indicator
	// machinery is exercised by a real map, and no more - vi's repertoire is
	// #119's, and writing it here would be writing that ticket badly.
	void install_defaults();

private:
	std::map<std::string, keymap, std::less<>> _maps;
};

// ---------------------------------------------------------------------------
// The dispatch environment.
//
// WHO OWNS THE REGISTRIES (#110 left this open, #118 answers it). Not a global,
// and not `state`'s by value: a registry is the ENVIRONMENT - what the user has
// bound and registered - where `state` is what a turn of the machine mutates.
// Copying a state must not fork the user's bindings, and N-3's equality must not
// compare them, so they cannot be members. So `state` holds a shared pointer to
// one of these and the last state referring to it frees it (ADR-0007), which
// keeps "one loop, one registry" a fact about the object graph rather than a
// comment on a file-scope variable.
// ---------------------------------------------------------------------------

// How long a prefix-hold waits, when the loop arms one (F-5).
//
// 400 ms, zsh's KEYTIMEOUT. Longer than the decoder's 25 ms ESC window on
// purpose: that one is a race with the terminal's own bytes, this one is a race
// with a human's second keystroke.
inline constexpr std::chrono::milliseconds default_key_timeout{400};

class editing_context {
public:
	editing_context();

	editing_context(const editing_context&) = delete;
	editing_context& operator=(const editing_context&) = delete;

	[[nodiscard]] registry& actions() noexcept { return _actions; }
	[[nodiscard]] keymap_registry& keymaps() noexcept { return _keymaps; }
	[[nodiscard]] loop_harness& loop() noexcept { return _loop; }

	// The hold timeout the LOOP reads to arm its poll deadline. leshper never
	// asks a clock (F-5); it is told when.
	std::chrono::milliseconds key_timeout{default_key_timeout};

private:
	registry _actions;
	keymap_registry _keymaps;
	loop_harness _loop{_actions};
};

// The context a state dispatches through, created on first use.
//
// Lazy so that `state s;` in a test still edits - the default environment is the
// nine built-ins and the three default keymaps, which is what every test that
// pre-dates this ticket already assumed. The real loop constructs one explicitly
// and shares it across the states it owns.
editing_context& context_of(state& current);

// ---------------------------------------------------------------------------
// Resolution: what the stack says about one candidate sequence.
// ---------------------------------------------------------------------------

struct resolution {
	enum class kind : std::uint8_t {
		unbound,   // nothing matches and nothing longer could
		dispatch,  // run `action`
		hold,      // something longer is bound: wait for it (F-5)
	};

	kind what = kind::unbound;
	std::string action;   // set for `dispatch`, and for `unbound` never
};

// Walks the stack top-down against `candidate` (an encoded sequence).
//
// #117 decision 4, in one function: first exact match wins, any prefix match
// anywhere holds, an opaque keymap ends the walk, the topmost `default_action`
// catches an unmatched single key, and `self_insert` is the floor beneath all of
// it for a printable - unless an opaque keymap took the floor away.
//
// `printable` is the caller's answer to "would this single key type itself",
// asked here rather than derived, because the floor is about the LAST key and
// this function only sees bytes.
[[nodiscard]] resolution resolve_keys(const keymap_registry& maps, const keymap_stack& stack,
                                      std::string_view candidate, bool printable);

// The same walk, with the prefix question answered by the clock rather than by
// the table: nothing longer can arrive, so a held sequence resolves to its own
// exact match if it has one, and a lone printable held only because something
// longer was bound falls to the floor and types itself. Called by
// `keymap_expire` and by nothing else.
[[nodiscard]] resolution resolve_expired_keys(const keymap_registry& maps,
                                              const keymap_stack& stack,
                                              std::string_view held);

// True when a lone key event should fall through to `self_insert` (#117's
// floor): a printable scalar, not a named key, and not modified. Alt-a is not a
// character, and Ctrl-A already IS one - U+0001 - which is below the floor.
[[nodiscard]] bool is_self_inserting(const key_event& key) noexcept;

// F-40: the indicator the topmost keymap declaring one asks for, empty when
// none does. VISUAL shows while pushed; the pager, declaring none, hides
// nothing.
[[nodiscard]] std::string_view indicator_of(const keymap_registry& maps,
                                            const keymap_stack& stack) noexcept;

} // namespace lesh::leshper
