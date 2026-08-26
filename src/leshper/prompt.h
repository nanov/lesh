#pragma once

// The prompt engine (#157, architecture spec §6.10): one element kind, two-phase
// groups, a tick wheel, and a `constexpr` default table.
//
// ONE ELEMENT KIND, AND THAT IS THE WHOLE DESIGN. A module, a literal, a style
// and a group are all `int f(const state&, sink&, const void* data)` - a
// function of the facts into a byte sink, answering with a status. Nothing here
// is a class hierarchy and nothing here is virtual: a prompt render is a loop
// over a handful of function pointers, which is what makes §6.10's "well under
// 100 µs" a property of the shape rather than of an optimizer's mood.
//
// WHY THE AUTHORING SIDE IS TYPES AND THE STORED SIDE IS POINTERS. `{fn, data,
// kind}` is the record the ABI, a Lua binding and a config string all produce,
// and a function POINTER carries no traits - `seg` cannot ask a pointer whether
// it is a module. So a compile-time element is a small TYPE with a
// `static constexpr element_kind k` and a `static constexpr int call(...)`, and
// `make<E>()` adapts it to a record through a thunk. The combinators then read
// `k` with `if constexpr`, the two-phase vote happens at compile time, and the
// default prompt is a `constexpr std::array` with no initializer running at
// startup. A runtime element is the same record with the traits it needs
// carried as data instead.
//
// WHAT MAKES A CONSTEXPR DEFAULT TABLE LEGAL. `state::fs_allowed`. The memory-
// only modules are pure functions of the struct; `git` is the one that touches
// the filesystem, and it answers `omitted` on a false `fs_allowed` BEFORE
// reaching `read_git_head`. C++23 lets a `constexpr` function contain a call it
// never evaluates, so the same `module_git` serves the compile-time table and
// the running shell, and the static_asserts at the bottom of this file render
// the real default prompt rather than a paper copy of it.
//
// v1 STYLES ARE VALUES. `style_of<kCyan>` takes a `leshper::style` as a
// non-type template argument; there is no style-string parser here and that is
// deliberate (§6.10's last paragraph) - the `cyan.bold` grammar arrives with its
// first string source and is then shared with the highlighter's theme (#141).
// What v1 has is the SGR emission, `emit_sgr` below, and it is the exact inverse
// of `sgr.h`'s reader, asserted as such.
//
// WHAT THIS FILE DOES NOT DO. It does not know about the loop, the layout, the
// blitter or `shell_state`. `state` is a plain struct of facts somebody else
// gathers, so a test - and a constant expression - can hand it a synthetic one.

#include "leshper/abi.h"
#include "leshper/git_head.h"
#include "leshper/sgr.h"
#include "leshper/surface.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace lesh::leshper::prompt {

// ---------------------------------------------------------------------------
// Status and kind
// ---------------------------------------------------------------------------

// What an element answered (§6.10). Modules return the first three; literals
// and styles return `neutral`, which is what "decorations do not vote" means
// spelled as a value rather than as a rule the group has to remember.
//
// `pending` is in the enum and unused by v1's modules: the async half - a
// killable `git status` child, the spinner it would drive - is #156's, and a
// status the composer can already carry is cheaper than a status added later to
// a surface a binding has started writing against.
enum class element_status : std::uint8_t {
	omitted = 0,
	ready = 1,
	pending = 2,
	neutral = 3,
};

// What an element IS, for the group vote. The three roles §6.10 names: a module
// reads a shell fact, a decoration is grammar, a group composes.
enum class element_kind : std::uint8_t {
	module,
	decoration,
	group,
};

[[nodiscard]] constexpr int code(element_status status) noexcept {
	return static_cast<int>(status);
}

// An element's `int` answer, read as a status. Anything outside the four is
// `omitted` - which is the whole of the ABI's error handling on this path: a C
// module that returned LESH_ERR_INVAL contributes nothing and the prompt still
// draws (N-4: malformed degrades, it does not abort).
[[nodiscard]] constexpr element_status status_of(int raw) noexcept {
	switch (raw) {
		case 0:  return element_status::omitted;
		case 1:  return element_status::ready;
		case 2:  return element_status::pending;
		case 3:  return element_status::neutral;
		default: return element_status::omitted;
	}
}

// ---------------------------------------------------------------------------
// The facts a render is a function of
// ---------------------------------------------------------------------------

// Everything the built-in modules may read, and nothing else.
//
// A PLAIN STRUCT, NOT AN INTERFACE, and that is load bearing twice over. It
// makes a render a pure function of a value, so a test writes the facts down
// instead of building a shell; and it makes the value a literal type, so the
// default table renders inside a `static_assert`. The wiring site fills it from
// `shell_knowledge` on the shell thread (§6.10: never by reaching across the
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
	// compile-time table renders under, and it is not only a constexpr trick: a
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
// and against the pen already in force, and a prompt element has neither. The
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

// Where an element writes, and how it asks to be woken.
//
// ONE CONCRETE CLASS, NO VIRTUALS, and it is `constexpr`-capable end to end.
// The obvious alternative - an abstract sink so a test could capture bytes -
// would have cost an indirect call per append on the one path §6.10 promises a
// number for, and would have made the compile-time table impossible: a virtual
// call is not a constant expression. `std::string` IS usable inside constant
// evaluation, and at runtime the buffer keeps its capacity across renders, so
// the warm path allocates nothing.
//
// THE WAKE IS THE SMALLEST ONE ASKED FOR. Several elements may ask on the same
// render, and the composer arms one timer for the earliest (§6.10: one deadline
// list, one prompt timer, an empty list is no timer at all). `wake_in(0)`
// clamps to 1 rather than meaning "never": an element that asked for a wake
// meant to be woken, and a zero would arm a timer that fires in the past.
class sink {
public:
	constexpr void append(std::string_view text) { _bytes.append(text); }

	constexpr void write_style(const style& pen) { emit_sgr(pen, _bytes); }

	constexpr void wake_in(std::uint64_t ticks) noexcept {
		const std::uint64_t want = ticks == 0 ? 1 : ticks;
		if (_wake == 0 || want < _wake)
			_wake = want;
	}

	// Another sink's whole contribution - its bytes and its request. What a
	// group does in phase two, and what the composer does with a slot.
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

private:
	std::string _bytes;
	std::uint64_t _wake = 0;
};

// ---------------------------------------------------------------------------
// The stored element
// ---------------------------------------------------------------------------

using element_fn = int (*)(const state&, sink&, const void* data);

// `{fn, data, kind}` - §6.10's record, and the only shape the composer walks.
//
// `data` is null for everything authored in C++: the argument is baked into the
// instantiation by the NTTP, so `env<"USER">` is a pure function pointer with
// nothing to carry. It is non-null only for a RUNTIME element - an ABI
// placement, later a Lua one or a config string - where it points at the
// `binding` the engine owns beside the placement.
struct element {
	element_fn fn = nullptr;
	const void* data = nullptr;
	element_kind kind = element_kind::module;
};

// What a runtime placement hands its function.
//
// Two fields and not one: the ARGUMENT belongs to the placement (`{env:USER}`
// and `{env:HOST}` are two placements of one module), and the USERDATA belongs
// to the registration (§6.10: modules are singletons in the registry with free
// placement). Both have to reach the function, and the ABI's module signature
// takes the second separately, so the pair travels as one pointer.
struct binding {
	std::string_view arg;
	void* userdata = nullptr;
};

[[nodiscard]] constexpr std::string_view arg_of(const void* data) noexcept {
	return data == nullptr ? std::string_view{} : static_cast<const binding*>(data)->arg;
}

[[nodiscard]] constexpr void* userdata_of(const void* data) noexcept {
	return data == nullptr ? nullptr : static_cast<const binding*>(data)->userdata;
}

// ---------------------------------------------------------------------------
// The built-in modules: the memory-only set, plus the budgeted `git`
//
// Every one is a pure function of `state` and, where it has one, of its bound
// argument. `constexpr` throughout - `git` included, see its comment - so the
// default table renders at compile time and the running shell calls exactly the
// same code.
//
// THE OMISSION RULE IS EACH MODULE'S OWN, and it is checked BEFORE any bytes
// are written. `status` omits on zero, `jobs` omits on none, `duration` omits
// under its floor. That is what makes `seg` able to vanish a literal: the
// module said nothing at all, so there is nothing to unsay.
// ---------------------------------------------------------------------------

// `$PWD`, with the home directory contracted to `~`.
//
// A PREFIX MATCH THAT RESPECTS COMPONENTS: `/home/user` contracts under
// `/home/user`, and so does `/home/user/src`, but `/home/username` does not.
// The cheap `starts_with` would have turned the third into `~name`, which is a
// path that does not exist.
constexpr int module_path(const state& facts, sink& out, const void*) {
	if (facts.pwd.empty())
		return code(element_status::omitted);

	std::string_view rest = facts.pwd;
	if (!facts.home.empty() && rest.size() >= facts.home.size()
	    && rest.substr(0, facts.home.size()) == facts.home
	    && (rest.size() == facts.home.size() || rest[facts.home.size()] == '/')) {
		out.append("~");
		rest.remove_prefix(facts.home.size());
	}
	out.append(rest);
	return code(element_status::ready);
}

// `$?`, and nothing at all when the last command succeeded - which is the
// module that makes the whole design worth having, because the brackets around
// it are a literal that has to vanish with it.
constexpr int module_status(const state& facts, sink& out, const void*) {
	if (facts.status == 0)
		return code(element_status::omitted);

	// A negative status is not a shell exit status, but a `state` filled from
	// somewhere unusual can hold one, and printing `18446744073709551615` for it
	// would be a worse answer than a minus sign.
	std::uint64_t magnitude = 0;
	if (facts.status < 0) {
		out.append("-");
		magnitude = static_cast<std::uint64_t>(-static_cast<std::int64_t>(facts.status));
	} else {
		magnitude = static_cast<std::uint64_t>(facts.status);
	}
	out.append(decimal{magnitude}.view());
	return code(element_status::ready);
}

constexpr int module_jobs(const state& facts, sink& out, const void*) {
	if (facts.jobs == 0)
		return code(element_status::omitted);
	out.append(decimal{static_cast<std::uint64_t>(facts.jobs)}.view());
	return code(element_status::ready);
}

// The vi-mode indicator (F-40). The TEXT arrives on the facts rather than being
// chosen here: #117 says the indicator is whatever the topmost keymap declares,
// so a module that mapped modes to strings would be a second, disagreeing
// answer to that question.
constexpr int module_mode(const state& facts, sink& out, const void*) {
	if (facts.mode.empty())
		return code(element_status::omitted);
	out.append(facts.mode);
	return code(element_status::ready);
}

constexpr void append_two_digits(sink& out, unsigned value) {
	const char pair[2] = {
		static_cast<char>('0' + (value / 10u) % 10u),
		static_cast<char>('0' + value % 10u),
	};
	out.append(std::string_view{pair, 2});
}

// `HH:MM:SS`, and the one v1 element that asks to be woken.
//
// THE REQUEST IS DERIVED FROM THE TICK, NEVER STORED: the next second on the
// 10 ms grid is `100 - tick % 100` ticks away, which is a function of the facts
// and of nothing this module remembers. That is §6.10's "the tick is the state"
// at its smallest - a clock parked through a long command re-arms from the fire
// rather than catching up on the seconds it slept through.
constexpr int module_time(const state& facts, sink& out, const void*) {
	append_two_digits(out, facts.hours);
	out.append(":");
	append_two_digits(out, facts.minutes);
	out.append(":");
	append_two_digits(out, facts.seconds);
	out.wake_in(100 - facts.tick % 100);
	return code(element_status::ready);
}

// Under this, the last command was fast enough that saying so is noise. Two
// seconds is starship's default and prmt's, and agreeing with them costs
// nothing.
inline constexpr std::uint64_t kDurationFloorMs = 2000;

// How long the last command took, humanized. Integer seconds throughout: a
// prompt that reported `2.317s` would be inviting the eye to read a digit that
// changes every run and means nothing.
constexpr int module_duration(const state& facts, sink& out, const void*) {
	if (facts.duration_ms < kDurationFloorMs)
		return code(element_status::omitted);

	const std::uint64_t total = facts.duration_ms / 1000;
	if (total < 60) {
		out.append(decimal{total}.view());
		out.append("s");
	} else if (total < 3600) {
		out.append(decimal{total / 60}.view());
		out.append("m");
		out.append(decimal{total % 60}.view());
		out.append("s");
	} else {
		out.append(decimal{total / 3600}.view());
		out.append("h");
		out.append(decimal{(total / 60) % 60}.view());
		out.append("m");
		out.append(decimal{total % 60}.view());
		out.append("s");
	}
	return code(element_status::ready);
}

// One environment variable, by name. The name is the module's ARGUMENT, which
// is where the two authoring sides differ and the only place they do: the NTTP
// form bakes it in, the runtime form reads it out of the binding.
//
// AN EMPTY VALUE OMITS. `{env:HOST}@` should vanish on a machine with no
// `$HOST` rather than render a bare `@`, and "set but empty" is what that
// machine actually has.
constexpr int render_env(const state& facts, sink& out, std::string_view name) {
	if (name.empty() || facts.getvar == nullptr)
		return code(element_status::omitted);

	std::string_view value;
	if (!facts.getvar(facts.getvar_ctx, name, value) || value.empty())
		return code(element_status::omitted);
	out.append(value);
	return code(element_status::ready);
}

constexpr int module_env(const state& facts, sink& out, const void* data) {
	return render_env(facts, out, arg_of(data));
}

// The branch, or the short object name on a detached HEAD - v1's one budgeted
// module.
//
// THE GUARD IS FIRST AND IT IS TWO THINGS AT ONCE. At runtime it is §6.10's
// floor rule: a caller that has not allowed filesystem work gets no syscall,
// not a fast one. During constant evaluation it is what makes this function
// legal at all - C++23 permits a `constexpr` function to CONTAIN a call it never
// evaluates, and `fs_allowed` false is what keeps the evaluation off the
// `read_git_head` line. One function, two worlds, no paper copy of the module
// for the compile-time table to render instead.
constexpr int module_git(const state& facts, sink& out, const void*) {
	if (!facts.fs_allowed || facts.pwd.empty())
		return code(element_status::omitted);

	const git_head head = read_git_head(facts.pwd, git_probe_options{
		.budget_ms = facts.fs_budget_ms,
		.allow_spawn = true,
		.git_command = "git",
	});
	if (!head.found)
		return code(element_status::omitted);

	out.append(head.detached ? head.short_sha : head.branch);
	return code(element_status::ready);
}

// A literal placed at runtime: its bytes ARE its bound argument, so the ABI's
// `add_literal` needs no second storage mechanism and no second element shape.
constexpr int decoration_literal(const state&, sink& out, const void* data) {
	out.append(arg_of(data));
	return code(element_status::neutral);
}

// ---------------------------------------------------------------------------
// Authoring at compile time
// ---------------------------------------------------------------------------

// A string literal as a non-type template argument. Nothing in the repo had
// one; this is the minimum that works - a char array, its size, and a view.
template <std::size_t N>
struct fixed_string {
	char data[N]{};

	constexpr fixed_string(const char (&literal)[N]) noexcept {
		for (std::size_t i = 0; i < N; ++i)
			data[i] = literal[i];
	}

	// Without the terminating NUL: these bytes go into a prompt, not into a C
	// string.
	[[nodiscard]] constexpr std::string_view view() const noexcept {
		return std::string_view{data, N - 1};
	}
};

template <std::size_t N>
fixed_string(const char (&)[N]) -> fixed_string<N>;

// Whether an element type PAINTS - the one trait a group needs beyond `kind`,
// because a group that contained a style has to put the pen back when it ends.
// Absent on every type that does not set it, which is all of them but one.
template <class E, class = void>
struct is_styling : std::false_type {};
template <class E>
struct is_styling<E, std::void_t<decltype(E::styles)>> : std::bool_constant<E::styles> {};

// The adapter from a type to a record. `data` is unused and stays null: the
// whole reason a compile-time element is a type is that its argument is already
// inside it.
template <class E>
constexpr int element_thunk(const state& facts, sink& out, const void*) {
	return E::call(facts, out);
}

template <class E>
[[nodiscard]] constexpr element make() noexcept {
	return element{&element_thunk<E>, nullptr, E::k};
}

template <class... Es>
[[nodiscard]] constexpr std::array<element, sizeof...(Es)> table() noexcept {
	return std::array<element, sizeof...(Es)>{make<Es>()...};
}

// --- Decorations -----------------------------------------------------------

template <fixed_string S>
struct literal {
	static constexpr element_kind k = element_kind::decoration;

	static constexpr int call(const state&, sink& out) {
		out.append(S.view());
		return code(element_status::neutral);
	}
};

// A style is an ABSOLUTE statement, emitted whole (see `emit_sgr`). `styles` is
// the flag a group reads to know it owes a reset.
template <style V>
struct style_of {
	static constexpr element_kind k = element_kind::decoration;
	static constexpr bool styles = true;

	static constexpr int call(const state&, sink& out) {
		out.write_style(V);
		return code(element_status::neutral);
	}
};

// --- Modules ---------------------------------------------------------------
//
// Trailing underscores on `time_` and `mode_` and a leading module-ish `_t` on
// the rest: `::time_t` and `::mode_t` are macros' and headers' names that a
// bare `time` or `mode` in this namespace would collide with the moment
// somebody included <sys/types.h> before this file. The names are ugly on
// purpose - a header that only compiles depending on include order is the
// defect the self-containment build exists to catch.

struct path_t {
	static constexpr element_kind k = element_kind::module;
	static constexpr int call(const state& s, sink& o) { return module_path(s, o, nullptr); }
};

struct status_t {
	static constexpr element_kind k = element_kind::module;
	static constexpr int call(const state& s, sink& o) { return module_status(s, o, nullptr); }
};

struct jobs_t {
	static constexpr element_kind k = element_kind::module;
	static constexpr int call(const state& s, sink& o) { return module_jobs(s, o, nullptr); }
};

struct mode_t_ {
	static constexpr element_kind k = element_kind::module;
	static constexpr int call(const state& s, sink& o) { return module_mode(s, o, nullptr); }
};

struct time_t_ {
	static constexpr element_kind k = element_kind::module;
	static constexpr int call(const state& s, sink& o) { return module_time(s, o, nullptr); }
};

struct duration_t {
	static constexpr element_kind k = element_kind::module;
	static constexpr int call(const state& s, sink& o) { return module_duration(s, o, nullptr); }
};

struct git_t {
	static constexpr element_kind k = element_kind::module;
	static constexpr int call(const state& s, sink& o) { return module_git(s, o, nullptr); }
};

template <fixed_string Name>
struct env {
	static constexpr element_kind k = element_kind::module;
	static constexpr int call(const state& s, sink& o) { return render_env(s, o, Name.view()); }
};

// --- Combinators -----------------------------------------------------------

// §6.10's group: shown iff a module inside it is ready, decorations do not vote,
// and evaluation is two-phase so a module that will not be shown is never asked
// for its bytes.
//
// PHASE ONE renders only the module children, each into its own scratch. PHASE
// TWO runs at all only if one of them said `ready` - and then it runs every
// child in DECLARED ORDER, splicing the module bytes phase one already produced
// and executing the decorations now. That ordering is the whole trick: it is why
// `seg<style_of<kMagenta>, literal<" on ">, git_t>` renders ` on main` in a repo
// and NOTHING outside one, the literal and the style vanishing with the module,
// while still emitting the style before the bytes it colours.
//
// `pending` DOES NOT COUNT AS READY in v1, and that is a decision rather than an
// oversight: there is no completion path to resolve a pending element (§6.10 -
// v1's loop-integration surface is timers only), so treating it as ready would
// show a group whose module has nothing to say and no way to say it later. When
// #156 brings the completion event, this predicate is the one line that changes.
//
// A GROUP THAT CONTAINED A STYLE ENDS WITH A RESET. A seg is a styled span; the
// alternative - leaving the pen set for whatever follows - would make the next
// element's appearance depend on whether an unrelated group happened to render.
template <class... Children>
struct seg {
	static constexpr element_kind k = element_kind::group;

	static constexpr int call(const state& facts, sink& out) {
		std::array<sink, sizeof...(Children)> scratch{};
		std::array<int, sizeof...(Children)> answered{};
		bool any_ready = false;

		std::size_t at = 0;
		const auto vote = [&](auto tag) {
			using Child = typename decltype(tag)::type;
			if constexpr (Child::k == element_kind::module) {
				answered[at] = Child::call(facts, scratch[at]);
				if (status_of(answered[at]) == element_status::ready)
					any_ready = true;
			}
			++at;
		};
		(vote(std::type_identity<Children>{}), ...);

		if (!any_ready)
			return code(element_status::omitted);

		bool styled = false;
		at = 0;
		const auto emit = [&](auto tag) {
			using Child = typename decltype(tag)::type;
			if constexpr (Child::k == element_kind::module) {
				// An omitted sibling contributes no bytes, but a wake it asked
				// for still travels: a module may be silent and still want to be
				// asked again.
				if (status_of(answered[at]) != element_status::omitted)
					out.splice(scratch[at]);
				else if (scratch[at].wake() != 0)
					out.wake_in(scratch[at].wake());
			} else {
				if constexpr (is_styling<Child>::value)
					styled = true;
				Child::call(facts, out);
			}
			++at;
		};
		(emit(std::type_identity<Children>{}), ...);

		if (styled)
			out.write_style(style{});
		return code(element_status::ready);
	}
};

// A gate: the children run, all of them and in order, exactly when the predicate
// says so, and NOTHING runs otherwise.
//
// Not a `seg` with a condition bolted on. The two compose differently and
// deliberately: `seg` asks its modules and lets them decide, `when` asks the
// facts and does not ask its children at all. `when<in_a_repo, seg<...>>` is the
// spelling for both at once.
//
// Its answer is `ready` when a module child was ready and `neutral` otherwise,
// so a `when` nested in a `seg` neither votes (it is a group, and groups do not)
// nor lies to a composer that reads statuses.
template <bool (*Pred)(const state&), class... Children>
struct when {
	static constexpr element_kind k = element_kind::group;

	static constexpr int call(const state& facts, sink& out) {
		if (!Pred(facts))
			return code(element_status::omitted);

		bool any_ready = false;
		const auto run = [&](auto tag) {
			using Child = typename decltype(tag)::type;
			if (status_of(Child::call(facts, out)) == element_status::ready)
				any_ready = true;
		};
		(run(std::type_identity<Children>{}), ...);

		return code(any_ready ? element_status::ready : element_status::neutral);
	}
};

// A fallback: `A` if it is ready, otherwise `B`, whatever `B` has to say.
//
// `A` RENDERS INTO SCRATCH AND IS DISCARDED WHOLE when it is not ready, which is
// why this is a combinator rather than something a caller could write with two
// placements: half of `A`'s bytes reaching the prompt before it decided it had
// nothing is exactly the failure the two-phase group exists to prevent, and it
// would happen here too without the scratch.
template <class A, class B>
struct either {
	static constexpr element_kind k = element_kind::group;

	static constexpr int call(const state& facts, sink& out) {
		sink first;
		if (status_of(A::call(facts, first)) == element_status::ready) {
			out.splice(first);
			return code(element_status::ready);
		}

		sink second;
		const int answered = B::call(facts, second);
		if (status_of(answered) != element_status::omitted)
			out.splice(second);
		else if (second.wake() != 0)
			out.wake_in(second.wake());
		return answered;
	}
};

// ---------------------------------------------------------------------------
// The composer
// ---------------------------------------------------------------------------

// A flat walk: every element runs, and one that answers `omitted` contributes
// no bytes.
//
// TOP-LEVEL LITERALS ARE UNCONDITIONAL, BY DESIGN. `$ ` at the end of the
// default prompt is there whatever the modules did, because binding is explicit
// grouping and never inferred from adjacency (§6.10). A literal that should
// vanish with a module goes INSIDE that module's `seg`, and there is no other
// way to say it - which is the point.
constexpr void render_table(std::span<const element> elements, const state& facts, sink& out) {
	// One scratch for the whole walk, rewound per element: it keeps its capacity
	// at runtime, so a warm render allocates nothing here.
	sink scratch;
	for (const element& one : elements) {
		scratch.reset();
		const element_status answered = status_of(one.fn(facts, scratch, one.data));
		if (answered != element_status::omitted)
			out.splice(scratch);
		else if (scratch.wake() != 0)
			out.wake_in(scratch.wake());
	}
}

// ---------------------------------------------------------------------------
// The default prompt
// ---------------------------------------------------------------------------

inline constexpr style kCyan = style{.fg = color::of_index(6)};
inline constexpr style kMagenta = style{.fg = color::of_index(5)};
inline constexpr style kRed = style{.fg = color::of_index(1)};

// `~/src on main [2]$ `, with each half present only when it has something to
// say. A `constexpr std::array` of records: no initializer runs at startup and
// no allocation happens for the default configuration, which is the
// configuration almost every session has.
inline constexpr auto kDefaultLeft = table<
	seg<style_of<kCyan>, path_t>,
	seg<style_of<kMagenta>, literal<" on ">, git_t>,
	seg<style_of<kRed>, literal<" [">, status_t, literal<"]">>,
	literal<"$ ">>();

inline constexpr auto kDefaultContinuation = table<literal<"> ">>();

// ---------------------------------------------------------------------------
// The engine
// ---------------------------------------------------------------------------

// §6.10's two v1 surfaces. The right prompt and the transient prompt are #156's
// and arrive as new enumerators, which is why this is an enum and not a bool.
enum class surface_id : std::uint8_t {
	left = 0,
	continuation = 1,
	count_,
};

// The registry, the configuration verbs, the output slots and the tick wheel.
//
// RECALCULATION BY CAUSE (§6.10) is the reason this object exists at all rather
// than a `render_table` call per prompt. Every TOP-LEVEL element owns an output
// slot; a render re-invokes only the elements with a reason and splices every
// other slot unchanged. v1 has two of the three causes - a new prompt, and an
// element's own wake tick - because v1's loop-integration surface is timers
// only; the third (an event an element declared interest in) arrives with #156's
// completion path and is a third entry point, not a change to these two.
//
// THE TICK PATH'S CONTRACT, and the caller owns it: `render_tick` may be handed
// a `state` that differs from the last `render_full`'s only in `tick`, `hours`,
// `minutes` and `seconds`. Nothing else may have moved, because nothing else is
// re-read - a slot that is not due is memcpy'd, not recomputed. A changed `$?`
// or a changed `$PWD` is a NEW PROMPT and goes through `render_full`.
//
// LOOP-THREAD ONLY, like every other registry here (#93). No locking anywhere,
// and the rule is really "one thread at a time": #157's wiring calls
// `render_full` from the SHELL thread, in the window ADR-0009 gives it while the
// loop is blocked in `wait_on_shell` - the same window `loop_options::prompt` has
// been written in since #129. `render_tick` is the loop's own. See
// `session::refresh_prompt` in read.cpp, where the argument is made in full.
class engine {
public:
	engine();
	~engine();

	engine(const engine&) = delete;
	engine& operator=(const engine&) = delete;

	// --- The module registry ---

	// Registers a module under `name`, REPLACING any existing registration -
	// #101's rule, so re-sourcing an rc file is idempotent rather than an error.
	// Names are snake_case; anything else is LESH_ERR_INVAL, and a null `fn` is
	// too. LESH_OK otherwise.
	//
	// A REGISTRATION IS NOT A PLACEMENT. Registering a module puts it in the
	// table; where it appears in the prompt, and how many times, is
	// `add_module`'s (§6.10: singletons with free placement).
	std::int32_t register_module(std::string_view name, element_fn fn, void* userdata);

	// The same, for a module that came across the C ABI: the trampoline and the
	// owned `{fn, userdata}` pair are this file's, so `abi.h` needs no C++ type.
	std::int32_t register_abi_module(std::string_view name, lesh_prompt_module_fn fn,
	                                 void* userdata);

	[[nodiscard]] bool module_exists(std::string_view name) const;

	// Sorted, because the table is a `std::map` and a caller listing modules
	// wants an order that does not depend on registration history.
	void module_names(std::vector<std::string>& out) const;

	// --- Configuration ---

	void clear(surface_id which);
	void use_default(surface_id which);

	// False when no module of that name is registered - the one failure a
	// configuration verb has, and it is a miss rather than a crash.
	//
	// `arg` is COPIED into engine-owned storage. The caller's bytes are its own
	// (the ABI's copy-in convention, and the only convention that survives a
	// binding whose strings are garbage-collected).
	bool add_module(surface_id which, std::string_view name, std::string_view arg);

	void add_literal(surface_id which, std::string_view bytes);

	// GROUPS DO NOT NEST IN v1, ACROSS THE ABI. A second open while one is open
	// is refused rather than silently flattened or silently nested: the verbs are
	// a linear stream and a caller that lost track of its own nesting should hear
	// so. Nesting arrives with the template language, whose parser has the
	// structure to express it and the set-time validation to check it (§6.10).
	bool open_group(surface_id which);

	// False when none is open.
	bool close_group(surface_id which);

	// Whether anything has configured this engine - false until the first of the
	// six verbs above has run, from C++ or across the ABI, and true from then on.
	//
	// THIS IS WHAT LETS `$PS1` STAY THE DEFAULT WITHOUT BEING THE FUTURE. §6.10
	// makes `PS1`/`PS2` a transitional stub, rendered as literal bytes "as it does
	// today", superseded by the native prompt rather than grown into a POSIX
	// expansion vocabulary. Supersession is a trajectory, and in v1 - with the
	// configuration builtin still a follow-up and the ABI the only way in - a
	// shell nobody has configured must still print the `PS1` its user set, or the
	// stub would have been removed rather than kept. So the wiring site asks this
	// one question: an untouched engine leaves the prompt to `PS1`, and the moment
	// anything configures one the native engine owns it, for the rest of the
	// session.
	//
	// NOT REGAINED. There is no un-configure: `clear` and `use_default` are
	// themselves configuration, and `use_default` in particular is how a user asks
	// for the shipped prompt back - which is a native prompt and emphatically not
	// a request to be handed `$PS1` again.
	[[nodiscard]] bool configured() const noexcept { return _configured; }

	// --- Rendering ---

	// The new-prompt cause: every element runs, every slot is rewritten, and the
	// per-render `(module, arg)` memo starts empty.
	void render_full(const state& facts);

	// The tick cause: only the elements whose deadline has come are re-invoked,
	// every other slot is spliced unchanged, and the answer is whether the
	// surface bytes actually moved. False means no terminal write is owed - which
	// is the point of asking (§6.10: unchanged output produces no write).
	bool render_tick(const state& facts);

	[[nodiscard]] std::string_view output(surface_id which) const;

	// The earliest armed deadline over both surfaces, as an absolute tick. Zero
	// is NO TIMER AT ALL, not "now": an empty deadline list means a static prompt
	// causes zero idle wakeups.
	[[nodiscard]] std::uint64_t next_wake() const;

private:
	// One placement. A tree rather than a flat list because a group owns its
	// children, and `std::unique_ptr` rather than a vector of values because
	// `binding::arg` is a view into `arg` on the same node - a vector that
	// reallocated would dangle it.
	struct node {
		// Null IFF this is a runtime group: the engine drives its two phases
		// itself. Everything else - a built-in module, an ABI module, a runtime
		// literal, and every element copied out of the default table, `seg`
		// included - is opaque and answers for itself.
		element_fn fn = nullptr;
		const void* data = nullptr;
		element_kind kind = element_kind::module;

		// Whether this child is a style decoration, and therefore whether the
		// group owes a reset. NOTHING SETS IT IN v1: the ABI has no style verb,
		// because v1 has no style-string parser to give it an argument. It is
		// here because the two-phase group is one implementation and the rule is
		// part of it; the verb arrives with the string grammar.
		bool styles = false;

		std::string arg;   // engine-owned bytes; `bound.arg` views these
		binding bound;

		// Rendering scratch, per node, kept warm. On the node rather than in a
		// pool because a group's phase one needs one per module child at once,
		// and a node's lifetime is exactly the configuration's.
		sink scratch;
		element_status answered = element_status::omitted;

		std::vector<std::unique_ptr<node>> children;
	};

	// What a top-level element last produced. `wake_at` is ABSOLUTE - the tick
	// the element asked to be woken at, zero for no deadline - because a relative
	// request is only meaningful at the instant it was made.
	struct slot {
		std::string bytes;
		element_status status = element_status::omitted;
		std::uint64_t wake_at = 0;
	};

	struct surface {
		std::vector<std::unique_ptr<node>> nodes;
		// May be LONGER than `nodes`: entries past the end are stale and never
		// read, and keeping them is what stops a reconfiguration from giving back
		// the strings' capacity.
		std::vector<slot> slots;
		std::string bytes;
		node* open = nullptr;
	};

	struct module_entry {
		element_fn fn = nullptr;
		void* userdata = nullptr;
	};

	// The C function and its registration context, for a module that came in
	// across the ABI. Owned by name so a re-registration replaces it rather than
	// stacking another one up.
	struct abi_module {
		lesh_prompt_module_fn fn = nullptr;
		void* userdata = nullptr;
	};

	// One entry of the per-render `(module, arg)` memo (§6.10: `{env:USER}` and
	// `{env:HOST}` are two elements computed once each, and one module placed
	// twice with the same argument is computed once).
	//
	// A LINEAR VECTOR, and a used-count instead of `clear()`: a prompt has a
	// handful of modules, so a scan beats a hash, and reusing the entries keeps
	// their strings' capacity across renders.
	struct memo_entry {
		element_fn fn = nullptr;
		void* userdata = nullptr;
		std::string arg;
		std::string bytes;
		element_status status = element_status::omitted;
		std::uint64_t wake = 0;
	};

	[[nodiscard]] surface& at(surface_id which);
	[[nodiscard]] const surface& at(surface_id which) const;

	// The one bridge from the C ABI into the composer: it builds the per-call
	// context, calls the registered function, and kills the handle on the way
	// out. A member so that `abi_module` can stay private - nothing outside this
	// class has any business knowing the pair exists.
	static int abi_trampoline(const state& facts, sink& out, const void* data);

	node& place(surface& into, std::unique_ptr<node> made);
	[[nodiscard]] sink& scratch_at(std::size_t depth);

	element_status invoke(node& one, const state& facts, sink& out, std::size_t depth);
	element_status render_group(node& group, const state& facts, sink& out, std::size_t depth);
	void render_surface(surface& target, const state& facts);
	void rebuild(surface& target);

	std::map<std::string, module_entry, std::less<>> _modules;
	std::map<std::string, std::unique_ptr<abi_module>, std::less<>> _abi_modules;
	std::array<surface, static_cast<std::size_t>(surface_id::count_)> _surfaces;

	std::vector<memo_entry> _memo;
	std::size_t _memo_used = 0;

	// Rewound, never freed. Depth 0 is a top-level element's, depth 1 a group
	// child's; v1 cannot go deeper, and the vector grows if a later nesting rule
	// makes it possible.
	std::vector<sink> _scratch;
	sink _top;

	// See `configured()`. The constructor's own seeding of the default table
	// deliberately does not set it - that is the engine arriving with something to
	// render, not a user asking for it.
	bool _configured = false;
};

// ---------------------------------------------------------------------------
// The tick timer's interval
// ---------------------------------------------------------------------------

// What to arm the loop's prompt timer with, given the engine's `next_wake()` and
// the tick the render was computed at. Milliseconds, because `lesh_timer_start`
// counts in them and the wheel counts in ticks; zero means NO TIMER, which is
// `next_wake()`'s own zero carried through - a static prompt causes zero idle
// wakeups (§6.10) and that has to survive this conversion.
//
// PURE, and in the header, because the wiring that arms the timer runs on the
// loop thread inside a session and the arithmetic is the part worth testing on
// its own. A wiring site is hard to reach from a test; a function is not.
//
// PAST-DUE CLAMPS TO ONE TICK rather than to zero: a deadline that has already
// gone by means the loop was busy, and the answer is "fire at the next
// opportunity", not "fire never" (which zero means here) and not "fire with a
// zero interval" (which `lesh_timer_start` refuses). The same floor catches a
// sub-tick request, so nothing this returns is ever below the 10 ms grid.
[[nodiscard]] constexpr std::uint64_t timer_interval_ms(std::uint64_t next_wake_tick,
                                                        std::uint64_t now_tick) noexcept {
	if (next_wake_tick == 0)
		return 0;
	if (next_wake_tick <= now_tick)
		return 10;
	return (next_wake_tick - now_tick) * 10;
}

static_assert(timer_interval_ms(0, 0) == 0);
static_assert(timer_interval_ms(0, 12345) == 0);
// One tick out is one tick of milliseconds; ten is a hundred.
static_assert(timer_interval_ms(101, 100) == 10);
static_assert(timer_interval_ms(110, 100) == 100);
// Due now, and overdue by a parked minute: the floor, not zero and not a burst.
static_assert(timer_interval_ms(100, 100) == 10);
static_assert(timer_interval_ms(100, 6100) == 10);

// ---------------------------------------------------------------------------
// Compile-time selftests
//
// The discipline `lesh::args`'s constexpr scan set: the properties that must
// hold are asserted by the compiler, against synthetic facts, on the REAL
// default table rather than on a copy of it. A failure here is a build failure,
// not a red test - which is what one wants for "the composer omits correctly",
// because a composer that has stopped omitting is not a behaviour worth shipping
// far enough to run.
//
// The runtime tests cover what a constant expression cannot see: invocation
// COUNTS (the memo, the tick wheel's recalculation-by-cause), and everything
// that goes through the ABI.
// ---------------------------------------------------------------------------

namespace selftest {

// A render, in a fixed buffer, comparable to a string literal.
template <std::size_t N>
struct rendered {
	char bytes[N]{};
	std::size_t length = 0;

	[[nodiscard]] constexpr std::string_view view() const noexcept {
		return std::string_view{bytes, length};
	}
};

template <std::size_t N = 128>
[[nodiscard]] constexpr rendered<N> run(std::span<const element> elements, const state& facts) {
	sink out;
	render_table(elements, facts, out);
	rendered<N> copied;
	for (const char one : out.bytes())
		copied.bytes[copied.length++] = one;
	return copied;
}

template <class... Es, std::size_t N = 128>
[[nodiscard]] constexpr rendered<N> run_of(const state& facts) {
	const std::array<element, sizeof...(Es)> made = table<Es...>();
	return run<N>(std::span<const element>{made}, facts);
}

// The facts of a session in `~/src`, in no repo, after a successful command.
[[nodiscard]] constexpr state quiet() noexcept {
	state facts;
	facts.pwd = "/home/u/src";
	facts.home = "/home/u";
	facts.status = 0;
	facts.fs_allowed = false;
	return facts;
}

// 1. Omission propagates through the group: the style and the literal vanish
//    with the module, and what is left is not "almost nothing" but nothing.
static_assert(run_of<seg<style_of<kMagenta>, literal<" on ">, git_t>>(quiet()).view().empty());

// 5. The same assertion read the other way, which is how a constant expression
//    can see that phase two never ran: no style byte and no literal byte is
//    present anywhere in the output, so neither decoration executed.
static_assert(run_of<seg<style_of<kMagenta>, literal<" on ">, git_t>>(quiet()).length == 0);

// 2. The whole default table, both ways round. The `git` seg is gone entirely;
//    the `status` seg appears only when there is a status to show, bringing its
//    brackets with it; `$ ` is unconditional because it is top-level.
static_assert(run(std::span<const element>{kDefaultLeft}, quiet()).view()
              == "\x1b[36m~/src\x1b[0m$ ");

[[nodiscard]] constexpr state failed() noexcept {
	state facts = quiet();
	facts.status = 2;
	return facts;
}

static_assert(run(std::span<const element>{kDefaultLeft}, failed()).view()
              == "\x1b[36m~/src\x1b[0m\x1b[31m [2]\x1b[0m$ ");

static_assert(run(std::span<const element>{kDefaultContinuation}, quiet()).view() == "> ");

// 3a. `status` omits on zero and speaks otherwise - the module the whole
//     omission machinery exists for.
static_assert(run_of<status_t>(quiet()).view().empty());
static_assert(run_of<status_t>(failed()).view() == "2");

// The home contraction respects components: a sibling directory whose name
// merely starts with the home directory's is not under it.
[[nodiscard]] constexpr state elsewhere() noexcept {
	state facts = quiet();
	facts.pwd = "/home/username/x";
	return facts;
}
static_assert(run_of<path_t>(elsewhere()).view() == "/home/username/x");
static_assert(run_of<path_t>(quiet()).view() == "~/src");

[[nodiscard]] constexpr state at_home() noexcept {
	state facts = quiet();
	facts.pwd = "/home/u";
	return facts;
}
static_assert(run_of<path_t>(at_home()).view() == "~");

// 3b. `either` picks the first ready alternative and discards the other whole.
//     A fake variable table, constexpr so the function pointer is callable
//     inside a constant expression.
constexpr bool fake_getvar(const void*, std::string_view name, std::string_view& out) {
	if (name == "USER") {
		out = "u";
		return true;
	}
	if (name == "EMPTY") {
		out = std::string_view{};
		return true;
	}
	return false;
}

[[nodiscard]] constexpr state with_variables() noexcept {
	state facts = quiet();
	facts.getvar = &fake_getvar;
	return facts;
}

static_assert(run_of<env<"USER">>(with_variables()).view() == "u");
static_assert(run_of<env<"EMPTY">>(with_variables()).view().empty());
static_assert(run_of<env<"NOPE">>(with_variables()).view().empty());
static_assert(run_of<env<"USER">>(quiet()).view().empty());

static_assert(run_of<either<env<"NOPE">, env<"USER">>>(with_variables()).view() == "u");
static_assert(run_of<either<env<"USER">, env<"NOPE">>>(with_variables()).view() == "u");
static_assert(run_of<either<git_t, literal<"(no repo)">>>(quiet()).view() == "(no repo)");

// 3c. `when` gates on the facts and runs nothing at all when it is closed.
constexpr bool has_jobs(const state& facts) { return facts.jobs != 0; }

[[nodiscard]] constexpr state busy() noexcept {
	state facts = quiet();
	facts.jobs = 3;
	return facts;
}

static_assert(run_of<when<has_jobs, literal<"jobs:">, jobs_t>>(quiet()).view().empty());
static_assert(run_of<when<has_jobs, literal<"jobs:">, jobs_t>>(busy()).view() == "jobs:3");

// `duration`'s floor and its three formats.
[[nodiscard]] constexpr state took(std::uint64_t milliseconds) noexcept {
	state facts = quiet();
	facts.duration_ms = milliseconds;
	return facts;
}
static_assert(run_of<duration_t>(took(1999)).view().empty());
static_assert(run_of<duration_t>(took(2000)).view() == "2s");
static_assert(run_of<duration_t>(took(59'999)).view() == "59s");
static_assert(run_of<duration_t>(took(60'000)).view() == "1m0s");
static_assert(run_of<duration_t>(took(3'599'000)).view() == "59m59s");
static_assert(run_of<duration_t>(took(3'600'000)).view() == "1h0m0s");
static_assert(run_of<duration_t>(took(7'384'000)).view() == "2h3m4s");

// `time` is zero-padded and always ready.
[[nodiscard]] constexpr state clock_at(unsigned h, unsigned m, unsigned s) noexcept {
	state facts = quiet();
	facts.hours = static_cast<std::uint8_t>(h);
	facts.minutes = static_cast<std::uint8_t>(m);
	facts.seconds = static_cast<std::uint8_t>(s);
	return facts;
}
static_assert(run_of<time_t_>(clock_at(9, 5, 3)).view() == "09:05:03");

// 4. The SGR round trip. `emit_sgr` is the inverse of `sgr.h`'s `apply_sgr`, and
//    these are what say so - each style emitted from reset semantics and read
//    back into the style it was.
constexpr bool round_trips(const style& pen) {
	std::string bytes;
	emit_sgr(pen, bytes);
	return apply_sgr(std::string_view{bytes}, style{}) == pen;
}

static_assert(round_trips(style{}));
static_assert(round_trips(kCyan));
static_assert(round_trips(style{.fg = color::of_index(12)}));
static_assert(round_trips(style{.fg = color::of_index(200)}));
static_assert(round_trips(style{.bg = color::of_index(4)}));
static_assert(round_trips(style{.fg = color::of_rgb(1, 2, 3), .bg = color::of_rgb(250, 0, 128)}));
static_assert(round_trips(style{.attrs = attribute::bold | attribute::underline}));
static_assert(round_trips(style{.attrs = attribute::undercurl}));
static_assert(round_trips(style{.fg = color::of_index(6),
                                .attrs = attribute::bold | attribute::italic
                                       | attribute::reverse | attribute::strikethrough}));

// The exact bytes, so that a reader and a writer that agreed with each other
// while both being wrong would still be caught.
constexpr bool emits(const style& pen, std::string_view expected) {
	std::string bytes;
	emit_sgr(pen, bytes);
	return bytes == expected;
}

static_assert(emits(style{}, "\x1b[0m"));
static_assert(emits(kCyan, "\x1b[36m"));
static_assert(emits(style{.fg = color::of_index(12)}, "\x1b[94m"));
static_assert(emits(style{.fg = color::of_index(200)}, "\x1b[38;5;200m"));
static_assert(emits(style{.bg = color::of_index(9)}, "\x1b[101m"));
static_assert(emits(style{.attrs = attribute::undercurl}, "\x1b[4:3m"));

} // namespace selftest

} // namespace lesh::leshper::prompt
