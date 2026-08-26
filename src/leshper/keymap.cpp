#include "leshper/keymap.h"

#include "substrate/assert.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace lesh::leshper {
namespace {

// The two record kinds. Values rather than an enum class because they are
// serialized bytes: changing one changes what a saved table means, and a name
// in a switch would hide that.
constexpr char kind_codepoint = 1;
constexpr char kind_named = 2;

constexpr unsigned char modifier_shift = 1u;
constexpr unsigned char modifier_alt = 2u;
constexpr unsigned char modifier_ctrl = 4u;

constexpr char32_t delete_character = 0x7F;

[[nodiscard]] unsigned char modifier_bits(const key_modifiers& of) noexcept {
	unsigned char bits = 0;
	if (of.shift)
		bits |= modifier_shift;
	if (of.alt)
		bits |= modifier_alt;
	if (of.ctrl)
		bits |= modifier_ctrl;
	return bits;
}

[[nodiscard]] key_modifiers modifiers_of(unsigned char bits) noexcept {
	return key_modifiers{.shift = (bits & modifier_shift) != 0,
	                     .alt = (bits & modifier_alt) != 0,
	                     .ctrl = (bits & modifier_ctrl) != 0};
}

// The last named_key enumerator, so decode_key can refuse a record from a
// corrupted table rather than cast a wild number into the enum.
constexpr auto last_named_key = static_cast<std::uint32_t>(named_key::f12);

[[nodiscard]] bool is_ascii_letter(char32_t codepoint) noexcept {
	return (codepoint >= U'a' && codepoint <= U'z') || (codepoint >= U'A' && codepoint <= U'Z');
}

// ASCII case folding, and ASCII only: a keymap NAME like `<C-W>` is matched
// case-insensitively, and doing that with the C locale's towlower would make
// the answer depend on the user's locale.
[[nodiscard]] char lowered(char byte) noexcept {
	return byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte - 'A' + 'a') : byte;
}

[[nodiscard]] bool same_name(std::string_view a, std::string_view b) noexcept {
	if (a.size() != b.size())
		return false;
	for (std::size_t i = 0; i < a.size(); ++i)
		if (lowered(a[i]) != lowered(b[i]))
			return false;
	return true;
}

// --- The notation vocabulary ----------------------------------------------

struct named_spelling {
	std::string_view text;
	named_key key;
};

// The first spelling of each key is the one `render_key_notation` writes back.
constexpr std::array<named_spelling, 24> named_spellings{{
	{"Up", named_key::up},
	{"Down", named_key::down},
	{"Left", named_key::left},
	{"Right", named_key::right},
	{"Home", named_key::home},
	{"End", named_key::end},
	{"PageUp", named_key::page_up},
	{"PageDown", named_key::page_down},
	{"Insert", named_key::insert},
	{"Del", named_key::delete_forward},
	{"Delete", named_key::delete_forward},
	{"F1", named_key::f1},
	{"F2", named_key::f2},
	{"F3", named_key::f3},
	{"F4", named_key::f4},
	{"F5", named_key::f5},
	{"F6", named_key::f6},
	{"F7", named_key::f7},
	{"F8", named_key::f8},
	{"F9", named_key::f9},
	{"F10", named_key::f10},
	{"F11", named_key::f11},
	{"F12", named_key::f12},
	// event.h's `backspace` is the named key a terminal that sends BS rather
	// than DEL produces. The DEL spelling below is the ordinary one.
	{"BSKey", named_key::backspace},
}};

struct codepoint_spelling {
	std::string_view text;
	char32_t codepoint;
};

// Codepoints with a name, so that a binding can be written without embedding a
// control character in an rc file. `BS` is DEL because that is what terminals
// actually send for the Backspace key (event.h says so, and binds both).
constexpr std::array<codepoint_spelling, 13> codepoint_spellings{{
	{"Space", U' '},
	{"Tab", U'\t'},
	{"CR", U'\r'},
	{"Enter", U'\r'},
	{"Return", U'\r'},
	{"NL", U'\n'},
	{"LF", U'\n'},
	{"Esc", 0x1B},
	{"BS", delete_character},
	{"Backspace", delete_character},
	{"lt", U'<'},
	{"gt", U'>'},
	{"Bslash", U'\\'},
}};

// `C-` folded onto a codepoint (#117 decision 2). Answers false for a codepoint
// with no C0 spelling, which is refused rather than given a bit no terminal at
// the #97 floor will ever set.
[[nodiscard]] bool control_of(char32_t codepoint, char32_t& out) noexcept {
	if (is_ascii_letter(codepoint)) {
		out = codepoint & 0x1F;
		return true;
	}
	switch (codepoint) {
	case U'@':
	case U' ':
		out = 0;
		return true;
	case U'[':
		out = 0x1B;
		return true;
	case U'\\':
		out = 0x1C;
		return true;
	case U']':
		out = 0x1D;
		return true;
	case U'^':
		out = 0x1E;
		return true;
	case U'_':
		out = 0x1F;
		return true;
	case U'?':
		out = delete_character;
		return true;
	default:
		return false;
	}
}

// One UTF-8 scalar out of notation text. Notation is written by a human in a
// file, so a malformed byte is a refusal rather than a U+FFFD - the decoder's
// graceful degradation is for what a terminal sends, not for what an rc file
// says.
[[nodiscard]] bool next_scalar(std::string_view text, std::size_t& at, char32_t& out) noexcept {
	if (at >= text.size())
		return false;
	const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
	const auto continues = [&](std::size_t i) {
		return i < text.size() && (byte(i) & 0xC0) == 0x80;
	};
	const unsigned char lead = byte(at);
	if (lead < 0x80) {
		out = lead;
		at += 1;
		return true;
	}
	if ((lead & 0xE0) == 0xC0 && continues(at + 1)) {
		out = static_cast<char32_t>(((lead & 0x1F) << 6) | (byte(at + 1) & 0x3F));
		at += 2;
		return true;
	}
	if ((lead & 0xF0) == 0xE0 && continues(at + 1) && continues(at + 2)) {
		out = static_cast<char32_t>(((lead & 0x0F) << 12) | ((byte(at + 1) & 0x3F) << 6)
		                            | (byte(at + 2) & 0x3F));
		at += 3;
		return true;
	}
	if ((lead & 0xF8) == 0xF0 && continues(at + 1) && continues(at + 2) && continues(at + 3)) {
		out = static_cast<char32_t>(((lead & 0x07) << 18) | ((byte(at + 1) & 0x3F) << 12)
		                            | ((byte(at + 2) & 0x3F) << 6) | (byte(at + 3) & 0x3F));
		at += 4;
		return true;
	}
	return false;
}

void encode_utf8(char32_t codepoint, std::string& out) {
	if (codepoint < 0x80) {
		out.push_back(static_cast<char>(codepoint));
	} else if (codepoint < 0x800) {
		out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
		out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
	} else if (codepoint < 0x10000) {
		out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
		out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
	} else {
		out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
		out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
	}
}

// One `<...>` group. `at` points past the `<` and lands past the `>`.
[[nodiscard]] bool parse_angle_group(std::string_view text, std::size_t& at, std::string& into) {
	const std::size_t close = text.find('>', at);
	if (close == std::string_view::npos || close == at)
		return false;
	std::string_view body = text.substr(at, close - at);
	at = close + 1;

	key_modifiers modifiers;
	// Modifier prefixes, in any order, each two bytes and a dash. Stopped as
	// soon as what is left is not a prefix, so `<C-->` binds Ctrl-minus and
	// `<S-Left>` does not read `S-L` as a modifier on `eft`.
	for (;;) {
		if (body.size() < 3 || body[1] != '-')
			break;
		const char which = lowered(body[0]);
		if (which == 'c')
			modifiers.ctrl = true;
		else if (which == 'a' || which == 'm')
			modifiers.alt = true;
		else if (which == 's')
			modifiers.shift = true;
		else
			break;
		body.remove_prefix(2);
	}
	if (body.empty())
		return false;

	for (const named_spelling& one : named_spellings) {
		if (!same_name(one.text, body))
			continue;
		// Ctrl on a named key is the CSI-parameter bit: `<C-Left>` has no
		// control-character spelling at all.
		encode_key(key_event::of(one.key, modifiers), into);
		return true;
	}

	char32_t codepoint = 0;
	bool found = false;
	for (const codepoint_spelling& one : codepoint_spellings) {
		if (same_name(one.text, body)) {
			codepoint = one.codepoint;
			found = true;
			break;
		}
	}
	if (!found) {
		std::size_t scan = 0;
		if (!next_scalar(body, scan, codepoint) || scan != body.size())
			return false;   // more than one character, and none of the names
	}

	if (modifiers.shift && is_ascii_letter(codepoint)) {
		// What the keyboard sends: `<S-a>` is `A`, with no bit set.
		codepoint = codepoint & ~char32_t{0x20};
		modifiers.shift = false;
	}
	if (modifiers.ctrl) {
		char32_t folded = 0;
		if (!control_of(codepoint, folded))
			return false;
		codepoint = folded;
		modifiers.ctrl = false;
	}
	encode_key(key_event::of(codepoint, modifiers), into);
	return true;
}

// --- Table search ----------------------------------------------------------

struct by_keys {
	bool operator()(const keymap::entry& a, std::string_view b) const noexcept {
		return std::string_view{a.keys} < b;
	}
	bool operator()(std::string_view a, const keymap::entry& b) const noexcept {
		return a < std::string_view{b.keys};
	}
};

} // namespace

// ---------------------------------------------------------------------------
// Encoding.
// ---------------------------------------------------------------------------

void encode_key(const key_event& key, std::string& into) {
	const auto value = key.named ? static_cast<std::uint32_t>(key.key)
	                             : static_cast<std::uint32_t>(key.codepoint);
	into.push_back(key.named ? kind_named : kind_codepoint);
	into.push_back(static_cast<char>(modifier_bits(key.modifiers)));
	// Big-endian, so byte order is numeric order and `lower_bound` over the
	// encoded table sorts the way a key-by-key comparison would.
	into.push_back(static_cast<char>((value >> 24) & 0xFF));
	into.push_back(static_cast<char>((value >> 16) & 0xFF));
	into.push_back(static_cast<char>((value >> 8) & 0xFF));
	into.push_back(static_cast<char>(value & 0xFF));
}

std::string encode_key(const key_event& key) {
	std::string out;
	encode_key(key, out);
	return out;
}

bool decode_key(std::string_view encoded, std::size_t& at, key_event& out) {
	if (at + encoded_key_size > encoded.size())
		return false;
	const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(encoded[at + i]); };
	const char kind = encoded[at];
	if (kind != kind_codepoint && kind != kind_named)
		return false;
	const auto value = static_cast<std::uint32_t>((byte(2) << 24) | (byte(3) << 16)
	                                              | (byte(4) << 8) | byte(5));
	if (kind == kind_named && value > last_named_key)
		return false;
	out = kind == kind_named
	          ? key_event::of(static_cast<named_key>(value), modifiers_of(byte(1)))
	          : key_event::of(static_cast<char32_t>(value), modifiers_of(byte(1)));
	at += encoded_key_size;
	return true;
}

bool parse_key_notation(std::string_view text, std::string& into) {
	if (text.empty())
		return false;
	std::string built;
	std::size_t at = 0;
	while (at < text.size()) {
		if (text[at] == '<') {
			const std::size_t opened = at;
			++at;
			if (parse_angle_group(text, at, built))
				continue;
			// Not a group this vocabulary knows. A bare `<` is a literal, which
			// is what makes `a<b` bindable without `<lt>`; anything that looked
			// like a group and was not one is the caller's typo.
			at = opened;
			if (text.find('>', at) != std::string_view::npos)
				return false;
		}
		char32_t codepoint = 0;
		if (!next_scalar(text, at, codepoint))
			return false;
		encode_key(key_event::of(codepoint), built);
	}
	if (built.empty())
		return false;
	into = std::move(built);
	return true;
}

std::string render_key_notation(std::string_view encoded) {
	std::string out;
	std::size_t at = 0;
	key_event key;
	while (decode_key(encoded, at, key)) {
		std::string body;
		if (key.named) {
			for (const named_spelling& one : named_spellings) {
				if (one.key == key.key) {
					body = std::string(one.text);
					break;
				}
			}
		} else if (key.codepoint == U'<') {
			body = "lt";
		} else if (key.codepoint == U' ') {
			// Written as a name rather than as a literal space, or a listing of
			// two bindings would be indistinguishable from one binding of two keys.
			body = "Space";
		} else if (key.codepoint < 0x20 || key.codepoint == delete_character) {
			// Written back as the C0 name a human reads, which is the spelling
			// that parses to exactly these bytes again.
			bool named_it = false;
			for (const codepoint_spelling& one : codepoint_spellings) {
				if (one.codepoint == key.codepoint) {
					body = std::string(one.text);
					named_it = true;
					break;
				}
			}
			if (!named_it) {
				// The inverse of `control_of`: a letter comes back lowercase, and
				// everything else comes back as the ASCII character 0x40 above it -
				// `@ [ \\ ] ^ _`, which is exactly the set that folds to a control.
				body = "C-";
				body.push_back(key.codepoint >= 0x01 && key.codepoint <= 0x1A
				                   ? static_cast<char>(key.codepoint + 0x60)
				                   : static_cast<char>(key.codepoint + 0x40));
			}
		} else if (!key.modifiers.any()) {
			encode_utf8(key.codepoint, out);   // a literal printable, written literally
			continue;
		} else {
			encode_utf8(key.codepoint, body);
		}
		if (body.empty())
			body = "?";

		std::string prefix;
		if (key.modifiers.ctrl)
			prefix += "C-";
		if (key.modifiers.alt)
			prefix += "A-";
		if (key.modifiers.shift)
			prefix += "S-";
		out.push_back('<');
		out += prefix;
		out += body;
		out.push_back('>');
	}
	return out;
}

void encoded_keys_as_text(std::string_view encoded, std::string& into) {
	std::size_t at = 0;
	key_event key;
	while (decode_key(encoded, at, key)) {
		if (!key.named && !key.modifiers.alt && !key.modifiers.ctrl)
			encode_utf8(key.codepoint, into);
	}
}

bool encoded_keys_are_text(std::string_view encoded) noexcept {
	std::size_t at = 0;
	key_event key;
	bool any = false;
	while (decode_key(encoded, at, key)) {
		if (key.named || key.modifiers.alt || key.modifiers.ctrl)
			return false;
		any = true;
	}
	return any;
}

// ---------------------------------------------------------------------------
// keymap
// ---------------------------------------------------------------------------

void keymap::bind(std::string_view keys, std::string_view action) {
	if (keys.empty())
		return;
	const auto at = std::lower_bound(_entries.begin(), _entries.end(), keys, by_keys{});
	if (at != _entries.end() && std::string_view{at->keys} == keys) {
		if (action.empty())
			_entries.erase(at);
		else
			at->action.assign(action);
		return;
	}
	if (action.empty())
		return;
	_entries.insert(at, entry{std::string(keys), std::string(action)});
}

bool keymap::unbind(std::string_view keys) {
	const auto at = std::lower_bound(_entries.begin(), _entries.end(), keys, by_keys{});
	if (at == _entries.end() || std::string_view{at->keys} != keys)
		return false;
	_entries.erase(at);
	return true;
}

const std::string* keymap::action_for(std::string_view keys) const noexcept {
	const auto at = std::lower_bound(_entries.begin(), _entries.end(), keys, by_keys{});
	if (at == _entries.end() || std::string_view{at->keys} != keys)
		return nullptr;
	return &at->action;
}

bool keymap::has_longer(std::string_view keys) const noexcept {
	auto at = std::lower_bound(_entries.begin(), _entries.end(), keys, by_keys{});
	// An exact match is not its own prefix: if it were, a complete binding would
	// hold forever waiting for a longer one that does not exist.
	if (at != _entries.end() && std::string_view{at->keys} == keys)
		++at;
	return at != _entries.end() && std::string_view{at->keys}.starts_with(keys);
}

// ---------------------------------------------------------------------------
// keymap_registry
// ---------------------------------------------------------------------------

keymap* keymap_registry::create(std::string_view name, std::string_view copy_from) {
	if (name.empty())
		return nullptr;
	if (!copy_from.empty()) {
		const keymap* source = find(copy_from);
		if (source == nullptr)
			return nullptr;
		// Copied by value, because a keymap is data (F-11): `bind -N vi_visual
		// vi_command` produces a second table that then diverges freely.
		const keymap copied = *source;
		const auto placed = _maps.insert_or_assign(std::string(name), copied);
		return &placed.first->second;
	}
	const auto placed = _maps.insert_or_assign(std::string(name), keymap{});
	return &placed.first->second;
}

keymap* keymap_registry::find(std::string_view name) noexcept {
	const auto at = _maps.find(name);
	return at == _maps.end() ? nullptr : &at->second;
}

const keymap* keymap_registry::find(std::string_view name) const noexcept {
	const auto at = _maps.find(name);
	return at == _maps.end() ? nullptr : &at->second;
}

bool keymap_registry::erase(std::string_view name) {
	const auto at = _maps.find(name);
	if (at == _maps.end())
		return false;
	_maps.erase(at);
	return true;
}

void keymap_registry::names(std::vector<std::string>& into) const {
	into.clear();
	into.reserve(_maps.size());
	for (const auto& [name, unused] : _maps)
		into.push_back(name);   // std::map is already ordered
}

namespace {

// A binding, written the way a user would write it, so the default tables read
// as tables rather than as encoding calls.
void bind_notation(keymap& into, std::string_view notation, std::string_view action) {
	std::string keys;
	const bool parsed = parse_key_notation(notation, keys);
	LESH_ASSERT(parsed);   // a default table with a typo in it is a build defect
	if (parsed)
		into.bind(keys, action);
}

// The motions, in one place because three keymaps want the same ones (#119).
//
// vi_command has them because a user moves; vi_operator_pending has them because
// `dw` is a motion after a verb and the map is opaque; writing them twice would
// be the kind of duplication that drifts by one binding and is then a bug
// nobody can see.
void bind_vi_motions(keymap& into) {
	bind_notation(into, "h", "vi_backward_char");
	bind_notation(into, "l", "vi_forward_char");
	bind_notation(into, " ", "vi_forward_char");
	bind_notation(into, "j", "vi_line_down");
	bind_notation(into, "k", "vi_line_up");
	bind_notation(into, "^", "vi_first_nonblank");
	bind_notation(into, "$", "end_of_line");
	bind_notation(into, "w", "vi_word_next");
	bind_notation(into, "b", "vi_word_prev");
	bind_notation(into, "e", "vi_word_end");
	bind_notation(into, "W", "vi_blank_word_next");
	bind_notation(into, "B", "vi_blank_word_prev");
	bind_notation(into, "E", "vi_blank_word_end");
	bind_notation(into, "f", "vi_find_forward");
	bind_notation(into, "F", "vi_find_backward");
	bind_notation(into, "t", "vi_till_forward");
	bind_notation(into, "T", "vi_till_backward");
	bind_notation(into, ";", "vi_find_repeat");
	bind_notation(into, ",", "vi_find_repeat_reverse");
	bind_notation(into, "<Left>", "vi_backward_char");
	bind_notation(into, "<Right>", "vi_forward_char");
	bind_notation(into, "<Up>", "vi_line_up");
	bind_notation(into, "<Down>", "vi_line_down");
}

// The text objects (#99 answer 2), in one place because operator-pending and
// visual both want them: `diw` and `viw` are the same action reached from two
// stacks, which is exactly what "an object is one action that sets the
// selection" bought.
void bind_vi_objects(keymap& into) {
	bind_notation(into, "iw", "vi_object_inner_word");
	bind_notation(into, "aw", "vi_object_a_word");
	bind_notation(into, "iW", "vi_object_inner_blank_word");
	bind_notation(into, "aW", "vi_object_a_blank_word");
	static constexpr std::string_view delimiters[] = {"(", ")", "b", "[", "]",
	                                                  "{", "}", "B", "\"", "'", "`"};
	std::string inner;
	std::string around;
	for (const std::string_view one : delimiters) {
		inner.assign("i").append(one);
		around.assign("a").append(one);
		bind_notation(into, inner, "vi_object_inner_pair");
		bind_notation(into, around, "vi_object_a_pair");
	}
}

} // namespace

void keymap_registry::install_defaults() {
	// --- emacs: #107's hardcoded switch, moved in and not rewritten ---------
	//
	// The bindings are the same ones editor.cpp's `binding_for` returned, key for
	// key. Two spellings of Backspace, because terminals disagree about which one
	// they send and event.h binds both. Enter is deliberately absent: F-35 makes
	// it a decision the parser takes part in, and binding it to self_insert here
	// would answer that question wrongly and quietly. `redo` is deliberately
	// unbound - emacs has no second key for it and inventing one is not this
	// ticket's call.
	keymap* emacs_map = create(emacs);
	bind_notation(*emacs_map, "<BS>", "delete_backward_char");
	bind_notation(*emacs_map, "<C-h>", "delete_backward_char");
	bind_notation(*emacs_map, "<BSKey>", "delete_backward_char");
	bind_notation(*emacs_map, "<C-w>", "delete_backward_word");
	bind_notation(*emacs_map, "<C-a>", "beginning_of_line");
	bind_notation(*emacs_map, "<C-e>", "end_of_line");
	bind_notation(*emacs_map, "<C-b>", "backward_char");
	bind_notation(*emacs_map, "<C-f>", "forward_char");
	bind_notation(*emacs_map, "<C-_>", "undo");
	bind_notation(*emacs_map, "<Left>", "backward_char");
	bind_notation(*emacs_map, "<Right>", "forward_char");
	bind_notation(*emacs_map, "<Home>", "beginning_of_line");
	bind_notation(*emacs_map, "<End>", "end_of_line");

	// The emacs side of the ONE kill store (#99 answer 3): `C-y` reads what a
	// kill wrote, which is the same table vi's `p` reads.
	bind_notation(*emacs_map, "<C-y>", "yank");

	// --- vi_insert ---------------------------------------------------------
	//
	// The editing keys that work the same in every mode, plus F-40's indicator,
	// plus the one key that makes it a mode: Escape back to command.
	keymap* insert_map = create(vi_insert);
	insert_map->indicator = "INSERT";
	bind_notation(*insert_map, "<BS>", "delete_backward_char");
	bind_notation(*insert_map, "<C-h>", "delete_backward_char");
	bind_notation(*insert_map, "<BSKey>", "delete_backward_char");
	bind_notation(*insert_map, "<C-w>", "delete_backward_word");
	bind_notation(*insert_map, "<Left>", "backward_char");
	bind_notation(*insert_map, "<Right>", "forward_char");
	bind_notation(*insert_map, "<Home>", "beginning_of_line");
	bind_notation(*insert_map, "<End>", "end_of_line");
	bind_notation(*insert_map, "<Esc>", "vi_command_mode");

	// --- vi_command: the repertoire (#99, #119, spec §6.5) ------------------
	//
	// Opaque is what makes command mode command mode. The `self_insert` floor is
	// the bottom of the stack, and a mode where an unbound `z` types a `z` is not
	// vi - so the map that must swallow unbound printables says exactly that,
	// through the same flag the completion pager will use (F-29), with no default
	// action to catch them.
	keymap* command_map = create(vi_command);
	command_map->indicator = "NORMAL";
	command_map->opaque = true;
	bind_vi_motions(*command_map);
	bind_notation(*command_map, "0", "vi_digit_or_line_start");
	bind_notation(*command_map, "1", "vi_digit_argument");
	bind_notation(*command_map, "2", "vi_digit_argument");
	bind_notation(*command_map, "3", "vi_digit_argument");
	bind_notation(*command_map, "4", "vi_digit_argument");
	bind_notation(*command_map, "5", "vi_digit_argument");
	bind_notation(*command_map, "6", "vi_digit_argument");
	bind_notation(*command_map, "7", "vi_digit_argument");
	bind_notation(*command_map, "8", "vi_digit_argument");
	bind_notation(*command_map, "9", "vi_digit_argument");
	bind_notation(*command_map, "d", "vi_delete_operator");
	bind_notation(*command_map, "c", "vi_change_operator");
	bind_notation(*command_map, "y", "vi_yank_operator");
	bind_notation(*command_map, "x", "vi_delete_char");
	bind_notation(*command_map, "s", "vi_substitute_char");
	bind_notation(*command_map, "r", "vi_replace_char");
	bind_notation(*command_map, "~", "vi_toggle_case");
	bind_notation(*command_map, "D", "vi_delete_to_line_end");
	bind_notation(*command_map, "C", "vi_change_to_line_end");
	bind_notation(*command_map, "Y", "vi_yank_line");
	bind_notation(*command_map, "p", "vi_put_after");
	bind_notation(*command_map, "P", "vi_put_before");
	bind_notation(*command_map, "i", "vi_insert_mode");
	bind_notation(*command_map, "I", "vi_insert_at_line_start");
	bind_notation(*command_map, "a", "vi_append");
	bind_notation(*command_map, "A", "vi_append_at_line_end");
	bind_notation(*command_map, "o", "vi_open_below");
	bind_notation(*command_map, "O", "vi_open_above");
	bind_notation(*command_map, "v", "vi_visual_mode");
	bind_notation(*command_map, ".", "vi_repeat");
	bind_notation(*command_map, "<Esc>", "vi_normal_reset");
	bind_notation(*command_map, "u", "undo");
	bind_notation(*command_map, "<C-r>", "redo");
	bind_notation(*command_map, "<Home>", "beginning_of_line");
	bind_notation(*command_map, "<End>", "end_of_line");

	// --- vi_operator_pending: zle's `viopp` ---------------------------------
	//
	// OPAQUE, and that is the decision worth naming. Non-opaque would have let
	// the motions fall through to vi_command for free - and would have let `p`
	// and `i` fall through with them, so `dp` would paste and `di` would enter
	// insert mode with a delete still pending. A map that is the whole vocabulary
	// of what may follow an operator is what vi means by operator-pending, and it
	// costs one call to the shared motion table.
	keymap* pending_map = create("vi_operator_pending");
	pending_map->indicator = "PENDING";
	pending_map->opaque = true;
	bind_vi_motions(*pending_map);
	bind_vi_objects(*pending_map);
	bind_notation(*pending_map, "0", "vi_digit_or_line_start");
	bind_notation(*pending_map, "1", "vi_digit_argument");
	bind_notation(*pending_map, "2", "vi_digit_argument");
	bind_notation(*pending_map, "3", "vi_digit_argument");
	bind_notation(*pending_map, "4", "vi_digit_argument");
	bind_notation(*pending_map, "5", "vi_digit_argument");
	bind_notation(*pending_map, "6", "vi_digit_argument");
	bind_notation(*pending_map, "7", "vi_digit_argument");
	bind_notation(*pending_map, "8", "vi_digit_argument");
	bind_notation(*pending_map, "9", "vi_digit_argument");
	// The doubled forms. All three name one action, which refuses a mismatched
	// pair - `dc` is not `dd`, and vi says so.
	bind_notation(*pending_map, "d", "vi_line_object");
	bind_notation(*pending_map, "c", "vi_line_object");
	bind_notation(*pending_map, "y", "vi_line_object");
	bind_notation(*pending_map, "<Esc>", "vi_operator_abort");

	// --- vi_visual ----------------------------------------------------------
	//
	// NOT opaque: the motions below it in vi_command move the head, and a
	// selection whose head is the cursor follows for free (#96). What is bound
	// here is only what visual mode means DIFFERENTLY - the verbs, which act on
	// the region instead of starting an operator, and `o`, which flips it.
	keymap* visual_map = create("vi_visual");
	visual_map->indicator = "VISUAL";
	bind_vi_objects(*visual_map);
	bind_notation(*visual_map, "d", "vi_visual_delete");
	bind_notation(*visual_map, "x", "vi_visual_delete");
	bind_notation(*visual_map, "c", "vi_visual_change");
	bind_notation(*visual_map, "s", "vi_visual_change");
	bind_notation(*visual_map, "y", "vi_visual_yank");
	bind_notation(*visual_map, "o", "vi_visual_swap_ends");
	bind_notation(*visual_map, "v", "vi_visual_exit");
	bind_notation(*visual_map, "<Esc>", "vi_visual_exit");

	// --- The two one-shot maps ----------------------------------------------
	//
	// `f`, `F`, `t`, `T` and `r` all need the NEXT key as an argument rather than
	// as a binding, and #117 already built the shape for it: an opaque keymap
	// whose default action catches whatever arrives. No second input mode, no
	// "read a character" call anywhere - the same machinery the completion pager
	// uses to route unbound printables to its filter (F-29).
	keymap* find_map = create("vi_find_char");
	find_map->opaque = true;
	find_map->default_action = "vi_find_char_target";

	keymap* replace_map = create("vi_replace_char");
	replace_map->opaque = true;
	replace_map->default_action = "vi_replace_char_with";

	// --- pager: the map #117 was shaped for (#138, F-28 to F-30) -------------
	//
	// OPAQUE WITH A DEFAULT ACTION, which is the whole of the pager's dispatch.
	// #117 decision 4 promised "the pager needs zero special dispatch" and this
	// is what cashes it: opaque stops the lookup AND takes the `self_insert`
	// floor with it, so a printable typed over an open pager cannot reach the
	// buffer; the default action then routes it to the filter, which is F-29.
	// editor.cpp has no branch for any of it.
	//
	// NO INDICATOR, deliberately. F-40's indicator reads the topmost keymap that
	// declares one, and a pager is a thing you can SEE - it is on the screen
	// under the line. Declaring one would blank VISUAL while a pager was open
	// over visual mode, which is the case keymap.h's note about this map names.
	keymap* pager_map = create(pager);
	pager_map->opaque = true;
	pager_map->default_action = "pager_filter_key";
	// Tab cycles, which is what Tab has meant in a menu since tcsh. `<S-Tab>`
	// parses today and the decoder does not read CSI Z yet (keymap.h says so);
	// binding it is the notation and the decoder growing into each other.
	bind_notation(*pager_map, "<Tab>", "pager_next");
	bind_notation(*pager_map, "<S-Tab>", "pager_previous");
	// Left and Right walk the row, Up and Down walk the column. The grid is
	// row-major - candidates fill across then down - so a row step is the grid's
	// width, which is a question about the terminal that `lesh_pager_move`'s
	// axis argument asks the editor rather than guessing here.
	bind_notation(*pager_map, "<Right>", "pager_next");
	bind_notation(*pager_map, "<Left>", "pager_previous");
	bind_notation(*pager_map, "<Down>", "pager_next_row");
	bind_notation(*pager_map, "<Up>", "pager_previous_row");
	// Enter accepts, and BOTH spellings of it, for the reason read.cpp's
	// `bind_line_keys` gives: the key sends U+000D and a raw-mode terminal may
	// hand back either. This is not F-35's question - there is no line to decide
	// about, only a candidate to take - so binding it here answers nothing
	// quietly.
	bind_notation(*pager_map, "<C-m>", "pager_accept");
	bind_notation(*pager_map, "<C-j>", "pager_accept");
	bind_notation(*pager_map, "<Esc>", "pager_close");
	bind_notation(*pager_map, "<C-g>", "pager_close");
	// Three spellings of Backspace, the same three every other default table
	// carries.
	bind_notation(*pager_map, "<BS>", "pager_filter_backspace");
	bind_notation(*pager_map, "<C-h>", "pager_filter_backspace");
	bind_notation(*pager_map, "<BSKey>", "pager_filter_backspace");
}

// ---------------------------------------------------------------------------
// editing_context
// ---------------------------------------------------------------------------

editing_context::editing_context() {
	register_builtin_actions(_actions);
	// The vi repertoire, registered through the same ABI and by the same rule
	// (#119). Its context is a member above, which is what gives the userdata
	// pointer in every one of those registrations an owner (ADR-0007).
	register_vi_actions(_actions, _vi.get());
	// The pager's actions (#138), through the same ABI and by the same rule.
	// Registered with a null context because they have none to need: everything
	// the pager remembers is editor state, which the handle reaches.
	register_pager_actions(_actions);
	_keymaps.install_defaults();
}

editing_context& context_of(state& current) {
	if (!current.context)
		current.context = std::make_shared<editing_context>();
	return *current.context;
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------

bool is_self_inserting(const key_event& key) noexcept {
	if (key.named)
		return false;
	// Alt-a is not the character `a`, and Ctrl-A already IS a character - U+0001 -
	// which is below the floor rather than on it.
	if (key.modifiers.alt || key.modifiers.ctrl)
		return false;
	return key.codepoint >= 0x20 && key.codepoint != delete_character;
}

namespace {

// What one walk of the stack found. Shared by the two resolutions below, which
// differ in exactly one thing: whether a longer binding may still arrive.
struct found {
	const std::string* exact = nullptr;
	const std::string* fallback = nullptr;
	bool prefix = false;
	bool floored = true;
};

found walk_stack(const keymap_registry& maps, const keymap_stack& stack,
                 std::string_view candidate) {
	found seen;
	for (std::size_t depth = stack.layers.size(); depth-- > 0;) {
		const keymap* map = maps.find(stack.layers[depth]);
		if (map == nullptr)
			continue;   // a stack naming a keymap `bind` deleted: skipped, not fatal
		if (seen.exact == nullptr)
			seen.exact = map->action_for(candidate);
		if (!seen.prefix)
			seen.prefix = map->has_longer(candidate);
		if (seen.fallback == nullptr && !map->default_action.empty())
			seen.fallback = &map->default_action;
		if (map->opaque) {
			// Nothing below is consulted, and that includes the floor: the floor
			// is conceptually the bottom of the stack, and a keymap that stops
			// lookup stops it too. This is what lets vi command mode swallow an
			// unbound printable and the pager route one to its filter.
			seen.floored = false;
			break;
		}
	}
	return seen;
}

// What to do once no longer binding can arrive.
//
// The default action and the floor both apply to a LONE key and deliberately
// not to a sequence. An abandoned `<C-x>q` is a mistake, and typing the `q` out
// of it - or handing it to the pager's filter - is the wrong recovery from one.
// zle's answer, not vim's: vim replays an unmatched prefix as literal input,
// which needs dispatch to re-enter itself, and zsh is the lineage F-11 names.
resolution settle(const found& seen, std::string_view candidate, bool printable) {
	if (seen.exact != nullptr)
		return resolution{resolution::kind::dispatch, *seen.exact};
	if (candidate.size() != encoded_key_size)
		return resolution{};
	if (seen.fallback != nullptr)
		return resolution{resolution::kind::dispatch, *seen.fallback};
	if (seen.floored && printable)
		return resolution{resolution::kind::dispatch, std::string("self_insert")};
	return resolution{};
}

} // namespace

resolution resolve_keys(const keymap_registry& maps, const keymap_stack& stack,
                        std::string_view candidate, bool printable) {
	const found seen = walk_stack(maps, stack, candidate);
	// A prefix beats an exact match, which is what "resolves to the LONGEST exact
	// match" means: `<C-x>` bound and `<C-x><C-e>` bound too must wait to find out
	// which the user meant.
	if (seen.prefix)
		return resolution{resolution::kind::hold, {}};
	return settle(seen, candidate, printable);
}

resolution resolve_expired_keys(const keymap_registry& maps, const keymap_stack& stack,
                                std::string_view held) {
	// The same settlement, with the prefix question answered by the clock instead
	// of by the table: nothing longer can arrive now. So a lone printable held
	// only because something longer WAS bound types itself after the timeout,
	// which is what zle does and what makes `bind gg ...` survivable for anyone
	// who wanted to type a single `g`.
	bool printable = false;
	if (held.size() == encoded_key_size) {
		std::size_t at = 0;
		key_event key;
		if (decode_key(held, at, key))
			printable = is_self_inserting(key);
	}
	return settle(walk_stack(maps, stack, held), held, printable);
}

std::string_view indicator_of(const keymap_registry& maps, const keymap_stack& stack) noexcept {
	for (std::size_t depth = stack.layers.size(); depth-- > 0;) {
		const keymap* map = maps.find(stack.layers[depth]);
		if (map != nullptr && !map->indicator.empty())
			return map->indicator;
	}
	return {};
}

} // namespace lesh::leshper
