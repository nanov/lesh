#pragma once

// THE VOCABULARY A PROMPT MODULE IS WRITTEN IN (#157, spec §6.10), and nothing
// else. The engine, the template language, the composer and the tick wheel are
// `prompt.h`'s; what is here is the surface a module author sees - the facts
// they read, the sink they write to, the blob their params travel in, and the
// two verbs they implement.
//
// IT MUST NOT INCLUDE `prompt.h`, and that is the whole reason it is a file.
// Every module header includes this one, `modules.h` includes those, and
// `prompt.h` includes `modules.h`; a module that could see the engine would
// close that circle and would also be able to reach for a registry it has no
// business knowing about. A module is a pure function of `state` and its params,
// and this header is what that sentence means spelled as a dependency.

#include "leshper/surface.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace lesh::leshper::prompt {

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

// What a placement answered (§6.10). A module returns the first three; a
// placement with no module at all - a literal - returns `neutral`, which is what
// "decorations do not vote" means spelled as a value rather than as a rule the
// group has to remember.
//
// `pending` is in the enum and unused by v1's modules: the async half - a
// killable `git status` child, the spinner it would drive - is #156's, and a
// status the composer can already carry is cheaper than a status added later to
// a surface a binding has started writing against. It is NOT-READY in v1: it
// emits nothing and it does not win a group's vote, because there is no
// completion path to resolve it.
enum class element_status : std::uint8_t {
	omitted = 0,
	ready = 1,
	pending = 2,
	neutral = 3,
};

[[nodiscard]] constexpr int code(element_status status) noexcept {
	return static_cast<int>(status);
}

// ---------------------------------------------------------------------------
// The facts a render is a function of
// ---------------------------------------------------------------------------

// Everything the built-in modules may read, and nothing else.
//
// A PLAIN STRUCT, NOT AN INTERFACE, and that is load bearing twice over. It
// makes a render a pure function of a value, so a test writes the facts down
// instead of building a shell; and it makes the value a literal type, so the
// default prompt renders inside a `static_assert`. The wiring site fills it from
// `ui::shell_knowledge` on the shell thread (§6.10: never by reaching across the
// link boundary), and this file has never heard of that.
//
// The views are BORROWED for the duration of one render. Nothing here is stored
// past the call - the composer copies bytes into its slots.
struct state {
	std::string_view pwd;
	std::string_view home;
	std::string_view mode;   // the vi-mode indicator's text, empty when there is none

	int status = 0;                 // `$?`
	std::size_t jobs = 0;
	std::uint64_t duration_ms = 0;  // how long the last command took

	// §6.10's virtual clock: `tick = monotonic_ms / 10`, integer. Animation state
	// is DERIVED from this and never stored, which is what makes two spinners
	// share a phase and #109's replay reproduce a frame sequence exactly.
	std::uint64_t tick = 0;

	std::uint8_t hours = 0;
	std::uint8_t minutes = 0;
	std::uint8_t seconds = 0;

	// Whether a module may touch the filesystem at all. False is what the
	// compile-time default renders under, and it is not only a constexpr trick: a
	// caller that knows the prompt is being drawn somewhere a syscall must not
	// happen sets it false and gets the same answer.
	bool fs_allowed = false;

	// §6.10's floor rule: the filesystem half blocks for a bounded stretch or
	// gives up, so prompt-appearance time never depends on an NFS mount.
	std::uint32_t fs_budget_ms = 50;

	// Variable lookup, as a function pointer rather than a `std::function`: this
	// struct has to stay a literal type. Null is "no variables reachable", which
	// `env` reports as `omitted` rather than as an error.
	bool (*getvar)(const void* ctx, std::string_view name, std::string_view& out) = nullptr;
	const void* getvar_ctx = nullptr;
};

// ---------------------------------------------------------------------------
// Digits and SGR
// ---------------------------------------------------------------------------

// A value's decimal form, in a caller-held buffer.
//
// Not `std::to_chars`, which is not `constexpr` for integers until C++23 in
// libc++'s shipping form, and not a `std::string`, which would allocate on a
// path that formats a job count. Twenty digits holds any `uint64_t`.
struct decimal {
	char digits[20]{};
	std::size_t length = 0;

	constexpr explicit decimal(std::uint64_t value) noexcept {
		char reversed[20]{};
		std::size_t count = 0;
		do {
			reversed[count++] = static_cast<char>('0' + value % 10);
			value /= 10;
		} while (value != 0);
		while (count != 0)
			digits[length++] = reversed[--count];
	}

	[[nodiscard]] constexpr std::string_view view() const noexcept {
		return std::string_view{digits, length};
	}
};

// The FORWARD direction of `sgr.h`, and its exact inverse: `apply_sgr(what this
// emits, style{})` is the style it was handed. The selftests at the bottom of
// this file assert that round trip, which is the only way the two halves stay
// honest - a reader and a writer that were merely each plausible would round-
// trip undercurl into a plain underline and nobody would notice until a theme
// did it.
//
// EVERY MAPPING IS `blit.cpp`'s `set_pen`, restated rather than shared, and the
// restatement is deliberate: `set_pen` resolves against terminal capabilities
// and against the pen already in force, and a placement has neither. The
// prompt's `style` is an ABSOLUTE statement - the pen the prompt starts in is
// the layout's business (#123, and `sgr.h`'s note about `prompt_pen`) - so the
// emission starts from reset semantics and says everything it means:
//
//   * the default style is `ESC[0m` and nothing else;
//   * anything else is ONE `ESC[...m` listing attributes, then foreground, then
//     background, exactly `set_pen`'s order, with the default colours simply
//     absent because reset already said them;
//   * undercurl is `4:3` and takes precedence over a plain underline, which is
//     the one place the two attributes are not independent at the terminal.
constexpr void emit_sgr(const style& pen, std::string& out) {
	out.append("\x1b[");
	if (pen == style{}) {
		out.append("0m");
		return;
	}

	bool wrote = false;
	const auto param = [&](std::string_view text) {
		if (wrote)
			out.push_back(';');
		out.append(text);
		wrote = true;
	};
	const auto number = [&](unsigned value) {
		param(decimal{value}.view());
	};
	const auto colour = [&](bool foreground, const color& which) {
		const unsigned base = foreground ? 30u : 40u;
		switch (which.kind) {
			case color_kind::terminal_default:
				// Never emitted: reset already means the terminal's own colour,
				// and saying 39/49 again would be a parameter with no content.
				break;
			case color_kind::indexed:
				// The palette's first sixteen slots have their own SGR ranges;
				// above that it is the extended form. Indexed stays indexed
				// either way - the user may have redefined the slot (#97).
				if (which.index < 8)
					number(base + which.index);
				else if (which.index < 16)
					number(base + 60u + which.index - 8u);
				else {
					number(base + 8u);
					number(5u);
					number(which.index);
				}
				break;
			case color_kind::truecolor:
				// No quantization here, ever. That is the blitter's, at emit
				// time, against a terminal it can see (#97).
				number(base + 8u);
				number(2u);
				number(which.r);
				number(which.g);
				number(which.b);
				break;
		}
	};

	if (has(pen.attrs, attribute::bold))
		number(1u);
	if (has(pen.attrs, attribute::dim))
		number(2u);
	if (has(pen.attrs, attribute::italic))
		number(3u);
	if (has(pen.attrs, attribute::undercurl))
		param("4:3");
	else if (has(pen.attrs, attribute::underline))
		number(4u);
	if (has(pen.attrs, attribute::reverse))
		number(7u);
	if (has(pen.attrs, attribute::strikethrough))
		number(9u);
	colour(true, pen.fg);
	colour(false, pen.bg);

	out.push_back('m');
}

// ---------------------------------------------------------------------------
// The sink
// ---------------------------------------------------------------------------

// Where a module writes, and how it asks to be woken.
//
// ONE CONCRETE CLASS, NO VIRTUALS, and it is `constexpr`-capable end to end.
// The obvious alternative - an abstract sink so a test could capture bytes -
// would have cost an indirect call per append on the one path §6.10 promises a
// number for, and would have made the compile-time default impossible: a virtual
// call through a pointer nobody can see the target of is not a constant
// expression. `std::string` IS usable inside constant evaluation, and at runtime
// the buffer keeps its capacity across renders, so the warm path allocates
// nothing.
//
// THE WAKE IS THE SMALLEST ONE ASKED FOR. Several placements may ask on the same
// render, and the composer arms one timer for the earliest (§6.10: one deadline
// list, one prompt timer, an empty list is no timer at all). `wake_in(0)` clamps
// to 1 rather than meaning "never": a module that asked for a wake meant to be
// woken, and a zero would arm a timer that fires in the past.
class sink {
public:
	constexpr void append(std::string_view text) { _bytes.append(text); }

	constexpr void append_byte(char one) { _bytes.push_back(one); }

	constexpr void write_style(const style& pen) { emit_sgr(pen, _bytes); }

	constexpr void wake_in(std::uint64_t ticks) noexcept {
		const std::uint64_t want = ticks == 0 ? 1 : ticks;
		if (_wake == 0 || want < _wake)
			_wake = want;
	}

	// Another sink's whole contribution - its bytes and its request. What a group
	// does in phase two, and what the composer does with a slot.
	constexpr void splice(const sink& other) {
		_bytes.append(other._bytes);
		if (other._wake != 0)
			wake_in(other._wake);
	}

	[[nodiscard]] constexpr std::string_view bytes() const noexcept { return _bytes; }
	[[nodiscard]] constexpr std::uint64_t wake() const noexcept { return _wake; }

	// Keeps the capacity, which is the point of reusing sinks at all.
	constexpr void reset() noexcept {
		_bytes.clear();
		_wake = 0;
	}

	// Room for a render's worth of bytes, asked for once when a surface is
	// configured. What keeps the warm render path off malloc entirely: a `sink`
	// that never grows past its reservation never calls the allocator again.
	void reserve(std::size_t bytes) { _bytes.reserve(bytes); }

private:
	std::string _bytes;
	std::uint64_t _wake = 0;
};

// ---------------------------------------------------------------------------
// A module's parameters
// ---------------------------------------------------------------------------

// Bytes a module wants to keep in its params: `env`'s variable name, `status`'s
// symbol, an ABI module's raw type slot.
//
// NO PADDING, DELIBERATELY. `char[N]` then a `uint8_t` is alignment 1 the whole
// way down, which is what makes `std::bit_cast` through `params_blob` a constant
// expression - a padding byte would be an indeterminate value read during
// constant evaluation, which is an error rather than a warning.
template <std::size_t N>
struct fixed_text {
	static_assert(N < 256, "the length is a byte");

	char data[N]{};
	std::uint8_t length = 0;

	[[nodiscard]] constexpr std::string_view view() const noexcept {
		return std::string_view{data, length};
	}

	// False when it would not fit, having changed nothing.
	constexpr bool assign(std::string_view text) noexcept {
		if (text.size() > N)
			return false;
		for (std::size_t i = 0; i < text.size(); ++i)
			data[i] = text[i];
		length = static_cast<std::uint8_t>(text.size());
		return true;
	}
};

// A module's parsed type slot, as opaque bytes.
//
// WHY A BLOB AND NOT A VARIANT, A POINTER OR A STRING. Three reasons, and each
// one on its own would have been enough:
//
//   1. THE MEMO IS A MEMCMP. §6.10's per-prompt memo is keyed on (module,
//      params), and `{env::USER}` and `{env::HOST}` differ in bytes nobody here
//      can interpret. A blob compares without knowing what it holds.
//   2. THE COMPILED DEFAULT IS A VALUE. `compile<>()` returns a `constexpr`
//      object out of a `consteval` function; anything with a pointer into
//      itself, or an allocation, could not make that trip.
//   3. A MODULE AUTHOR NEVER SEES IT. `typed_module<Params>` is the one place
//      the bridge lives, and it is nine lines.
//
// The capacity fits every built-in with room over - `env`'s 64-byte name is the
// largest at 65 bytes - and a module whose params would not fit is a compile
// error at its `typed_module` instantiation rather than a truncation at runtime.
struct params_blob {
	static constexpr std::size_t capacity = 96;

	std::array<std::byte, capacity> data{};
	std::uint32_t size = 0;

	// AN EMPTY PARAMS TYPE STORES NOTHING. `sizeof(no_params)` is 1 and that one
	// byte is padding, which `bit_cast` may not read during constant evaluation;
	// storing zero bytes for it is both correct and the only thing that compiles.
	template <class Params>
	constexpr void store(const Params& value) {
		if constexpr (std::is_empty_v<Params>) {
			size = 0;
		} else {
			const auto raw = std::bit_cast<std::array<std::byte, sizeof(Params)>>(value);
			for (std::size_t i = 0; i < sizeof(Params); ++i)
				data[i] = raw[i];
			size = static_cast<std::uint32_t>(sizeof(Params));
		}
	}

	template <class Params>
	[[nodiscard]] constexpr Params as() const {
		if constexpr (std::is_empty_v<Params>) {
			return Params{};
		} else {
			std::array<std::byte, sizeof(Params)> raw{};
			for (std::size_t i = 0; i < sizeof(Params); ++i)
				raw[i] = data[i];
			return std::bit_cast<Params>(raw);
		}
	}

	// The USED bytes only. Two blobs of different modules are never compared -
	// the memo checks the module pointer first - so this is exactly the question
	// "is this the same placement, argument and all".
	[[nodiscard]] constexpr bool operator==(const params_blob& other) const noexcept {
		if (size != other.size)
			return false;
		for (std::uint32_t i = 0; i < size; ++i)
			if (data[i] != other.data[i])
				return false;
		return true;
	}
};

// A module that takes no type slot at all. `git`, `jobs`, `mode`, `duration`.
struct no_params {};

// Why a type slot was refused, from the module that refused it.
//
// `at` IS AN OFFSET WITHIN THE TYPE SLOT, so a placement in the middle of a long
// template still points at the byte the author has to fix; the scanner adds the
// slot's own offset. `length` is how much of the slot to quote back - zero for a
// refusal with nothing to name (`git takes no argument`), the whole slot for one
// that has something (`path: unknown variant 'medum'`).
//
// THREE FIELDS AND NOT TWO. `at` alone cannot say whether there is a token to
// quote: a module that wanted no quote would have to point `at` past the end of
// the slot, and then the byte offset in the message would name the wrong byte.
// Two questions, two fields.
//
// `what` IS THE MODULE'S OWN WORDS, INCLUDING ITS SEPARATOR, and the module owns
// the whole predicate: " takes no argument", " needs a variable name",
// ": unknown variant". The sentence is `<module name><what>[' <token>']`, so
// there is no table of message shapes anywhere - the module that knows what went
// wrong is the one that says it.
struct parse_error {
	std::size_t at = 0;
	std::size_t length = 0;
	std::string_view what;
};

// ---------------------------------------------------------------------------
// The module interface
// ---------------------------------------------------------------------------

// A module: a NAMED SINGLETON that turns a type slot into params once, and the
// facts plus those params into bytes every render.
//
// VIRTUAL, WHICH THE FIRST CUT OF THIS FILE WAS NOT, and the trade is worth
// stating. What was here before was `int (*)(const state&, sink&, const void*)`
// - one indirect call, no vtable - and the price was that a function pointer
// carries no traits: it cannot be asked its name, it cannot be asked to validate
// an argument, and every question of that kind had to become a table somewhere
// else that the registration had to be kept in step with. A `module*` answers
// all three itself. The cost is one indirect call to render a placement, which
// is exactly what the function pointer cost; the second one, `parse`, happens
// once per placement at SET time and replaces a table lookup the old design paid
// there anyway.
//
// CONSTEXPR THROUGHOUT, so the built-in singletons can be `inline constexpr`
// objects and `compile<>()` can call them during constant evaluation (C++20
// P1064: a virtual call whose target the compiler can see is a constant
// expression). A module registered at run time is an ordinary object and pays
// nothing for this.
class module {
public:
	constexpr virtual ~module() = default;

	module(const module&) = delete;
	module& operator=(const module&) = delete;

	[[nodiscard]] constexpr virtual std::string_view name() const noexcept = 0;

	// The type slot, parsed ONCE at set time into `out`. False with `err` filled
	// in refuses the placement, and refusing is the point: a mistyped variant is
	// told to its author at the moment they wrote it, not rendered as the default
	// for the rest of the session.
	//
	// `type` is ALREADY UNESCAPED and is borrowed for the call only.
	constexpr virtual bool parse(std::string_view type, params_blob& out,
	                             parse_error& err) const = 0;

	// The bytes, and a LESH_PROMPT_* status. Pure in the facts and the params;
	// anything else a module remembers between renders breaks §6.10's replay.
	constexpr virtual int render(const state& facts, const params_blob& params,
	                             sink& out) const = 0;

protected:
	constexpr module() = default;
};

// The ONE place the blob is bridged, and a module author never sees it.
//
// The two `parse` overloads and the two `render` overloads differ only in their
// middle parameter, so a derived class that overrides the typed pair HIDES the
// blob pair for anyone holding the derived type. That is fine and deliberate:
// every caller in this engine holds a `const module*`, which is exactly the type
// whose lookup finds the blob pair.
template <class Params>
class typed_module : public module {
public:
	static_assert(std::is_trivially_copyable_v<Params>,
	              "a module's params travel as bytes and live in a constexpr value");
	static_assert(sizeof(Params) <= params_blob::capacity,
	              "params_blob::capacity is the ceiling; raise it or shrink the params");

	constexpr bool parse(std::string_view type, params_blob& out, parse_error& err) const final {
		Params value{};
		if (!parse(type, value, err))
			return false;
		out.store(value);
		return true;
	}

	constexpr int render(const state& facts, const params_blob& params, sink& out) const final {
		return render(facts, params.template as<Params>(), out);
	}

protected:
	constexpr virtual bool parse(std::string_view type, Params& out, parse_error& err) const = 0;
	constexpr virtual int render(const state& facts, const Params& params, sink& out) const = 0;
};

// The refusal every parameterless module gives, in one place so that four
// modules cannot come to word it four ways.
constexpr bool refuse_any_type(std::string_view type, parse_error& err) noexcept {
	if (type.empty())
		return true;
	err.at = 0;
	err.length = 0;
	err.what = " takes no argument";
	return false;
}

} // namespace lesh::leshper::prompt
