#pragma once

// The prompt engine (#157, architecture spec §6.10): the element is the
// PLACEMENT, `(…)` is the only combinator, and a module is a typed singleton
// object that owns its own parameter grammar.
//
// THE ELEMENT IS THE PLACEMENT, AND THAT IS THE WHOLE DESIGN. The template's
// five parts - module, style, type, prefix, postfix - are ONE record here, not a
// module element with decorations arranged around it. Rendering one is a single
// two-phase span: ask the module, and if it said something, paint the pen, the
// prefix, its bytes, the postfix and the reset. A placement's decorations are
// FIELDS of the placement, so there is nothing about a styled placement that a
// composer could mistake for something else.
//
// WHAT THAT MAKES IMPOSSIBLE. The first cut of this engine desugared
// `{git:magenta}` into a GROUP over `[style, module, affixes]` and then had to
// stamp that group with a module kind so it would still vote in its parent -
// because `( on {git})` worked and `( on {git:magenta})` rendered nothing, for
// ever, and nothing about the spelling said so. That whole class of bug is gone
// by construction: a placement is one record with one status, and putting a
// colour on it sets a `style` field. There is no node to mis-stamp, because the
// decoration is not a node. `PuttingAColourOnAPlacementDoesNotChangeHowItVotes`
// is still in the suite, and it is now a statement about a struct rather than
// about a rule somebody has to remember.
//
// MODULES ARE TYPED SINGLETON OBJECTS. `module` is an interface with two verbs:
// `parse`, which turns the template's type slot into a `params_blob` ONCE at set
// time, and `render`, which is a function of the facts and those params. A
// module author writes `typed_module<my_params>` and never sees the blob; the
// blob exists so that the (module, params) pair is a memcmp - which is what the
// per-render memo keys on - and so that a compiled template is a trivially
// copyable value with no pointers into itself.
//
// SET-TIME REFUSAL IS THE MODULE'S OWN. `{path:cyan:medum}` is refused when it
// is written, by `path`, with the byte to look at - not silently rendered as the
// default and not discovered three weeks later. The engine has no table of which
// module takes what; the module has the grammar, because the module is the only
// thing that can have it once a binding may register one.
//
// ONE AUTHORING SURFACE, TWO EVALUATION TIMES. `compile<"{path}> ">()` runs the
// SAME scanner and the SAME `parse` methods a `prompt` builtin runs, at compile
// time, into a `constexpr` value - so the shipped default is not a hand-built
// table that has to be kept in step with its own spelling, it IS that spelling
// compiled. `kDefaultLeft = compile<"{path}> ">()`; the equivalence with
// `set_template("{path}> ")` is asserted rather than claimed.
//
// WHAT MAKES A CONSTEXPR DEFAULT LEGAL. Two things. `state::fs_allowed` - the
// memory-only modules are pure functions of the struct, and a budgeted one
// (leshnici's `git`) answers `omitted` on a false `fs_allowed` BEFORE reaching
// the filesystem, and C++23 lets a `constexpr` function contain a call it never
// evaluates. And C++20's
// constexpr virtual functions (P1064): the built-in modules are literal types
// with `constexpr` bodies and `inline constexpr` instances, so a virtual call
// through a pointer whose target the compiler can see is a constant expression.
// The static_asserts at the bottom render the REAL default through the REAL
// module objects rather than a paper copy of either.
//
// AFFIX BYTES ARE OFFSETS, NOT POINTERS. A placement's prefix and postfix are
// `{at, length}` into an arena the program carries beside its steps. That is
// what lets one representation serve both worlds: the compile-time program is a
// self-contained value with nothing pointing into itself (so it can be returned
// out of a `consteval` function), and the runtime one is a `std::vector<step>`
// beside a `std::string` that may grow without dangling a single view.
//
// WHAT THIS FILE DOES NOT DO. It does not know about the loop, the layout, the
// blitter or `shell_state`. `state` is a plain struct of facts somebody else
// gathers, so a test - and a constant expression - can hand it a synthetic one.
//
// WHERE THE MODULES ARE (#163). Not here. `prompt/module.h` is the vocabulary a
// module is written in, `prompt/module_<name>.h` is one module each, and
// `prompt/modules.h` is the table of the seven built-ins; this file includes
// that table and nothing includes this file back. The eighth module the ticket
// shipped, `git`, is not a built-in at all - it is `src/leshnici/`'s, installed
// on an engine by the wiring site, because it reads a filesystem and everything
// named in the table is a pure function of `state`.

#include "leshper/sgr.h"
#include "leshper/style_grammar.h"
#include "leshper/surface.h"
#include "ui/prompt/abi.h"
#include "ui/prompt/modules.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace lesh::ui::prompt {

// The style GRAMMAR is the editor's too, beside the pen types `module.h` names:
// `style_grammar.h` stayed in `leshper/` because the highlighter's theme parses
// the same spellings (#170).
using leshper::parse_style;
using leshper::style_parse;

// ---------------------------------------------------------------------------
// Status, as the engine reads it
// ---------------------------------------------------------------------------
//
// The enum itself, and every module written against it, is `prompt/module.h`'s.

// A module's `int` answer, read as a status. Anything outside the four is
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
// A template, as a template argument
// ---------------------------------------------------------------------------

// A string literal as a non-type template argument. Nothing in the repo had one;
// this is the minimum that works - a char array, its size, and a view.
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

// THE STANDALONE STYLED LITERAL, spelled like a placement because everything
// with a style is spelled like a placement - and it is NOT a module.
//
// A `{literal:blue::hi}` builds a placement with a NULL CORE: its affixes are its
// text, its pen paints them, and it reports `neutral`, so it never votes. That is
// the same record every other placement is, with the one field that would have
// made it vote left empty - which is why there is no pseudo-module here and no
// second kind of thing for a group to have an opinion about.
//
// Its text rides the AFFIX slots and never the type slot: the slot order is
// uniform across the grammar (slot 3 is always the type), so `{literal:blue::hi}`
// is simply the placement with no value in the middle - `{literal:blue:x:hi}` is
// refused for the same reason `{git::x}` is. The name shadows any module
// registered under it, which is the one thing the spelling costs and is worth
// saying out loud.
inline constexpr std::string_view kLiteralPlacement = "literal";

// ---------------------------------------------------------------------------
// The placement, the group, and the program
// ---------------------------------------------------------------------------

// A run of bytes in the program's arena.
//
// AN OFFSET, NOT A POINTER, and that single choice is what lets one
// representation serve compile time and run time. The compiled default is a
// `constexpr` value returned out of a `consteval` function, which nothing
// pointing into itself could survive; the runtime program is a vector beside a
// `std::string` that grows as verbs arrive, which no view into it could survive
// either. Offsets survive both.
struct bytes {
	std::uint32_t at = 0;
	std::uint32_t length = 0;

	[[nodiscard]] constexpr bool empty() const noexcept { return length == 0; }
};

[[nodiscard]] constexpr std::string_view view_of(const bytes& span, const char* arena) noexcept {
	return span.length == 0 ? std::string_view{} : std::string_view{arena + span.at, span.length};
}

// §6.10'S ELEMENT, AND THERE IS ONLY ONE. The template's five parts as one
// record: which module (null for a literal), what it was configured with, the
// pen, and the two affixes.
//
// THE PEN'S DEFAULT IS UNSTYLED, and `pen == style{}` is how that is asked. No
// separate flag, because no non-empty style spec parses to `style{}` - every
// item of the grammar sets a colour or an attribute - so "the default pen" and
// "no style slot" are the same value and cannot disagree.
struct placement {
	const module* core = nullptr;   // null for `{literal:…}` and for a free literal run
	params_blob params{};           // the parsed type slot, opaque here, typed inside the module
	style pen{};                    // default = unstyled: no SGR emitted, no reset
	bytes prefix{};
	bytes postfix{};
};

// One entry of the flat program: a placement, or a group over the entries that
// follow it.
//
// FLAT AND PRE-ORDER RATHER THAN A TREE OF NODES. A group's children are the
// `span` entries immediately after it, so the whole program is one contiguous
// array - trivially copyable, `constexpr`-constructible, and indexable, which is
// what lets a scratch buffer be found by step index instead of hung off a node.
// The old node tree needed a `unique_ptr` per element to keep a `string_view`
// from dangling; nothing here has a pointer to keep alive.
struct step {
	placement place{};
	std::uint32_t span = 0;   // a group: how many following steps are its subtree
	bool group = false;
};

// How many steps one item occupies - itself, plus a group's whole subtree.
[[nodiscard]] constexpr std::size_t step_extent(std::span<const step> steps,
                                                std::size_t at) noexcept {
	return steps[at].group ? 1 + steps[at].span : 1;
}

// A program and the arena its affixes live in.
struct program_view {
	std::span<const step> steps{};
	const char* arena = nullptr;
};

// Two sinks per step, and what the step last answered.
//
// TWO AND NOT ONE: a group's phase one has to keep a child's whole contribution
// while that child's own module buffer is being reused, so the ITEM's bytes and
// the module's bytes cannot share a buffer. Indexed by step, because the program
// is flat - which is what replaced the old `sink` hung off every heap node.
struct step_scratch {
	sink item;
	sink core;
	element_status answered = element_status::omitted;

	constexpr void reset() noexcept {
		item.reset();
		core.reset();
		answered = element_status::omitted;
	}
};

// ---------------------------------------------------------------------------
// The composer
// ---------------------------------------------------------------------------

// Running a placement's module, as a policy.
//
// TWO OF THESE EXIST AND THE COMPOSER IS WRITTEN ONCE. The compile-time one just
// calls `render`; the engine's consults §6.10's per-prompt memo first. Templating
// over them is what keeps "a group built across the ABI" and "a group compiled
// from a string" from being two behaviours that drift - the drift the first cut
// of this engine had, with a `seg<>` composer beside a node-tree one.
struct direct_core {
	constexpr element_status operator()(const placement& one, const state& facts,
	                                    sink& into) const {
		return status_of(one.core->render(facts, one.params, into));
	}
};

// ONE PLACEMENT, RENDERED - the two-phase span, inlined.
//
// A NULL CORE EMITS ITS AFFIXES AND REPORTS NEUTRAL. It is decoration: it always
// paints, and it never votes in an enclosing group. That is `{literal:dim::on}`
// and it is also every free literal run between placements.
//
// A CORE THAT SAID NOTHING TAKES ITS AFFIXES WITH IT. The module is asked FIRST,
// into `scratch`, and the pen and the affixes are written only once it has
// answered `ready` - which is why `{status:red::[:]}` is a pair of red brackets
// after a failure and nothing whatsoever after a success. A wake it asked for
// still travels even when it said nothing: a module may be silent and still want
// to be asked again.
template <class RunCore>
constexpr element_status render_placement(const placement& one, const char* arena,
                                          const state& facts, sink& out, sink& scratch,
                                          RunCore& run_core) {
	const bool styled = !(one.pen == style{});

	if (one.core == nullptr) {
		if (styled)
			out.write_style(one.pen);
		out.append(view_of(one.prefix, arena));
		out.append(view_of(one.postfix, arena));
		if (styled)
			out.write_style(style{});
		return element_status::neutral;
	}

	scratch.reset();
	const element_status answered = run_core(one, facts, scratch);
	if (answered != element_status::ready) {
		// `pending` is not-ready in v1 and paints nothing; see `element_status`.
		if (scratch.wake() != 0)
			out.wake_in(scratch.wake());
		return answered;
	}

	if (styled)
		out.write_style(one.pen);
	out.append(view_of(one.prefix, arena));
	out.splice(scratch);
	out.append(view_of(one.postfix, arena));
	if (styled)
		out.write_style(style{});
	return element_status::ready;
}

template <class RunCore>
constexpr element_status render_item(const program_view& program, std::size_t at,
                                     std::span<step_scratch> scratch, const state& facts,
                                     sink& out, RunCore& run_core);

// §6.10's group, and `(…)` IS THE ONLY COMBINATOR THERE IS. Shown iff at least
// one child reported `ready`; decorations do not vote; evaluation is two-phase so
// a child that will not be shown never has its bytes spliced anywhere.
//
// THE VOTE RECURSES THROUGH WHAT A CHILD REPORTS, and that is the whole rule. A
// placement reports its module's status; a nested group reports `ready` or
// `omitted` exactly as a placement does; a null-core placement reports `neutral`.
// So `({git} ({path}))` lives or dies on `git` OR on the inner group - and the
// inner group is itself alive only if `path` said something. There is no separate
// question of "what kind of thing is this child", because there is only one kind
// of thing.
//
// PHASE ONE SKIPS NULL-CORE PLACEMENTS BECAUSE THERE IS NOTHING TO COMPUTE. A
// literal's bytes are already in the placement; running it early would mean
// writing them into a scratch buffer only to throw them away when the vote fails.
// So a group whose module says nothing costs its decorations neither bytes nor a
// call, which is the property the old design needed a `kind` tag to get and this
// one gets from the shape.
//
// A WAKE ASKED FOR BY A CHILD OF A VANISHED GROUP IS DROPPED, deliberately: the
// group is not on screen, so there is nothing for a wake to redraw. When #156's
// completion path arrives and `pending` starts counting, this is the line that
// changes with it.
//
// A STYLED CHILD PUTS THE PEN BACK ITSELF, so a group owes no reset of its own:
// every pen this engine emits is closed by the placement that opened it.
template <class RunCore>
constexpr element_status render_group(const program_view& program, std::size_t at,
                                      std::span<step_scratch> scratch, const state& facts,
                                      sink& out, RunCore& run_core) {
	const std::size_t first = at + 1;
	const std::size_t last = first + program.steps[at].span;

	bool any_ready = false;
	for (std::size_t c = first; c < last; c += step_extent(program.steps, c)) {
		const step& child = program.steps[c];
		if (!child.group && child.place.core == nullptr)
			continue;   // decoration: nothing to compute, and it does not vote

		scratch[c].item.reset();
		const element_status answered =
			render_item(program, c, scratch, facts, scratch[c].item, run_core);
		scratch[c].answered = answered;
		if (answered == element_status::ready)
			any_ready = true;
	}

	if (!any_ready)
		return element_status::omitted;

	for (std::size_t c = first; c < last; c += step_extent(program.steps, c)) {
		const step& child = program.steps[c];
		if (!child.group && child.place.core == nullptr) {
			render_placement(child.place, program.arena, facts, out, scratch[c].core, run_core);
			continue;
		}
		if (scratch[c].answered != element_status::omitted)
			out.splice(scratch[c].item);
		else if (scratch[c].item.wake() != 0)
			out.wake_in(scratch[c].item.wake());
	}

	return element_status::ready;
}

template <class RunCore>
constexpr element_status render_item(const program_view& program, std::size_t at,
                                     std::span<step_scratch> scratch, const state& facts,
                                     sink& out, RunCore& run_core) {
	if (program.steps[at].group)
		return render_group(program, at, scratch, facts, out, run_core);
	return render_placement(program.steps[at].place, program.arena, facts, out,
	                        scratch[at].core, run_core);
}

// A flat walk of the top level: every item runs, and one that answers `omitted`
// contributes no bytes.
//
// TOP-LEVEL LITERALS ARE UNCONDITIONAL, BY DESIGN. `> ` at the end of the default
// prompt is there whatever the modules did, because binding is explicit grouping
// and never inferred from adjacency (§6.10). A literal that should vanish with a
// module goes in that placement's affix slot or inside its group, and there is no
// other way to say it - which is the point.
template <class RunCore>
constexpr void render_program(const program_view& program, std::span<step_scratch> scratch,
                              const state& facts, sink& out, RunCore& run_core) {
	for (std::size_t at = 0; at < program.steps.size(); at += step_extent(program.steps, at)) {
		sink& into = scratch[at].item;
		into.reset();
		const element_status answered = render_item(program, at, scratch, facts, into, run_core);
		if (answered != element_status::omitted)
			out.splice(into);
		else if (into.wake() != 0)
			out.wake_in(into.wake());
	}
}

// ---------------------------------------------------------------------------
// The template language
// ---------------------------------------------------------------------------

// prmt's placement grammar, adapted (§6.10, owner's ruling on #157):
//
//   template  := ( literal-run | group | placement )*
//   group     := '(' template ')'                    - nestable
//   placement := '{' name [':' style [':' type [':' prefix [':' postfix]]]] '}'
//   escapes   := \{  \}  \(  \)  \:  \n  \t  \\
//
// AN EMPTY SLOT IS THE DEFAULT, which is prmt's omission table kept verbatim
// because it is what makes five slots bearable to type: `{git}`, `{git:magenta}`,
// `{path:cyan:short}`, `{git:magenta::on :}` (default type, prefix only),
// `{git::::!}` (postfix only), `{env::USER}` (type only). An empty style is no
// styling, an empty type is the module's own default, an empty affix is no affix,
// and a trailing colon is legal - `{git:}` is `{git}`.
//
// FIVE SLOTS AND NOT SIX. A sixth unescaped `:` inside a placement is refused at
// set time, and that refusal is what makes `\:` unambiguous inside a type or an
// affix: colons in bytes are escaped, colons between slots are not, and there is
// no counting rule to remember.
//
// THE TYPE SLOT IS THE MODULE'S, AND THE SCANNER DOES NOT READ IT. It unescapes
// the bytes and hands them to `module::parse`, which answers yes or no with the
// byte to look at. That is why `{path:cyan:s}` and `{path:cyan:medum}` are told
// apart here at all, and why a module a binding registered gets the same
// treatment as a built-in without this file knowing anything about its grammar.
//
// FREE LITERAL RUNS ARE UNCONDITIONAL; binding a literal to a module is always
// explicit - an affix slot or a group, never adjacency (§6.10). `{path}( on
// {git})> ` is the whole of that rule: the arrow always paints, ` on ` paints
// only in a repository.
//
// WHAT WAS TAKEN FROM prmt's `src/parser.rs`, AND WHAT WAS DELIBERATELY INVERTED.
// Taken: the single byte-level pass with no backtracking, the structural jump
// (`find_first_of` over `{ ( ) \`) rather than per-byte inspection, and the lazy
// unescape - a slice with no backslash is copied whole and the transform loop
// runs only where one appears. Inverted, all three for the same underlying
// reason, that prmt re-parses per prompt draw and we parse once:
//
//   1. prmt DEGRADES a malformed placement to literal text, because an error on
//      a path that runs every draw would corrupt every prompt. We refuse at set
//      time with a message and leave the old prompt standing, which is the only
//      answer that can tell a user their typo at the moment they made it.
//   2. prmt BORROWS its bytes (`Cow`) because the template outlives the render.
//      Ours outlive the builtin's argv by the whole session, so every byte is
//      copied into the program's own arena as it is parsed.
//   3. prmt emits a flat token vector for a later pass to interpret. We build
//      placements directly, through the same hook the ABI's `place` verb uses,
//      so there is no intermediate AST to keep in step with the vocabulary.

// What went wrong, as a value. The runtime path words these into a sentence
// (`describe_template_error` in prompt.cpp); the compile-time path only needs to
// know THAT one happened and where, so the wording is not in the header and a
// `static_assert` compares codes and offsets instead of strings.
enum class template_error : std::uint8_t {
	none = 0,
	unclosed_placement,
	unbalanced_close,
	unclosed_group,
	too_many_fields,
	empty_name,
	unknown_module,
	bad_style,
	bad_escape,
	bad_type,
	literal_needs_text,
	literal_takes_no_type,
};

// The scan's answer. `error_at` is a byte offset INTO THE TEMPLATE and points at
// the byte a user has to look at - the offending brace, colon, backslash, name,
// style item or type byte, never at the start of the line. `what` names the
// offending token where there is one to name (`gti`, `blod`, `\q`, `medum`), as a
// view into the caller's own bytes.
//
// `subject` AND `detail` ARE `bad_type`'S, and they are how a module's refusal
// travels without a `std::string` in a `constexpr` walk: the module's name and
// the module's own words, both static, assembled into one sentence on the side
// that has a user to talk to.
struct template_check {
	bool ok = true;
	std::size_t error_at = 0;
	template_error error = template_error::none;
	std::string_view what{};
	std::string_view subject{};
	std::string_view detail{};
};

// One slot or one literal run, as the scanner found it: the RAW bytes, still
// escaped, plus where they start and whether the transform is needed at all.
// Carrying the offset rather than deriving it by pointer subtraction keeps the
// whole walk usable inside a constant expression without arithmetic on pointers
// into a `string_view` nobody owns.
struct template_slice {
	std::string_view raw{};
	std::size_t at = 0;
	bool escaped = false;

	[[nodiscard]] constexpr bool empty() const noexcept { return raw.empty(); }
};

// The eight escapes, and nothing else is one.
[[nodiscard]] constexpr bool escape_byte(char spelled, char& out) noexcept {
	switch (spelled) {
		case '{': case '}': case '(': case ')': case ':': case '\\':
			out = spelled;
			return true;
		case 'n': out = '\n'; return true;
		case 't': out = '\t'; return true;
		default: return false;
	}
}

// AN UNKNOWN ESCAPE IS AN ERROR, not two literal bytes. The alternative - keep
// `\q` as a backslash and a `q` - makes a mistyped `\n` a prompt that silently
// says `\n` forever, and this parser's whole posture is that a mistake is told to
// its author at the moment it is written.
constexpr void unescape_into(const template_slice& piece, std::string& out) {
	if (!piece.escaped) {
		// The common case, and the reason the flag exists: no inspection, no loop.
		out.append(piece.raw);
		return;
	}
	for (std::size_t i = 0; i < piece.raw.size(); ++i) {
		char decoded = 0;
		if (piece.raw[i] == '\\' && i + 1 < piece.raw.size()
		    && escape_byte(piece.raw[i + 1], decoded)) {
			out.push_back(decoded);
			++i;
			continue;
		}
		out.push_back(piece.raw[i]);
	}
}

// The one grammar walk. `build` is a POLICY, not an interface: three of them
// exist - one that measures, one that builds, one that does nothing at all - and
// templating over them is what keeps the compile-time validator, the compile-time
// builder and the set-time builder from being three walks that drift (#156's rule
// that a second walk is a second grammar).
//
// A policy provides:
//   const module* resolve(std::string_view name) const;   // null: nobody has that name
//   void on_placement(const module* core, const params_blob& params, const style& pen,
//                     std::string_view prefix, std::string_view postfix);
//   void on_open_group();
//   void on_close_group();
//
// ONE HOOK FOR EVERY ELEMENT, because there is one element. A literal run, a
// `{literal:…}` and a `{git:magenta:: on :}` all arrive through `on_placement`;
// the first two with a null core. That is the model showing through the parser:
// there is no `on_literal` to forget to keep in step, because a literal is not a
// separate thing.
//
// THE VIEWS HANDED TO A HOOK ARE THE SCANNER'S OWN BUFFERS and live only for the
// call. A policy that keeps bytes copies them.
//
// NOTHING IS EMITTED PAST THE FIRST ERROR: the scan returns at the byte that
// failed, so a policy that built half a prompt has built only that half - which
// is why the builder builds into its own storage and the engine swaps at the end
// rather than mutating a surface as it goes.
template <class Builder>
[[nodiscard]] constexpr template_check scan_template(std::string_view text, Builder& build) {
	template_check answer;
	const auto fail = [&answer](template_error which, std::size_t at,
	                            std::string_view what) -> template_check {
		answer.ok = false;
		answer.error = which;
		answer.error_at = at;
		answer.what = what;
		return answer;
	};

	// Two counters instead of a stack of open offsets: the group that was left
	// unclosed is always the OUTERMOST unmatched one, and that is the last `(`
	// seen at depth zero. No allocation, and the same code in both worlds.
	std::size_t depth = 0;
	std::size_t outermost_open_at = 0;
	std::size_t i = 0;

	// Reused across placements. Declared out here so a template with twenty
	// placements does not allocate twenty times over at set time.
	std::string type_bytes;
	std::string prefix_bytes;
	std::string postfix_bytes;

	while (i < text.size()) {
		// --- a literal run, by structural jumps rather than byte by byte ---
		const std::size_t run_start = i;
		std::size_t run_end = text.size();
		bool run_escaped = false;
		for (;;) {
			const std::size_t at = text.find_first_of("{()\\", i);
			if (at == std::string_view::npos) {
				i = text.size();
				break;
			}
			if (text[at] != '\\') {
				run_end = at;
				i = at;
				break;
			}
			char decoded = 0;
			if (at + 1 >= text.size() || !escape_byte(text[at + 1], decoded))
				return fail(template_error::bad_escape, at,
				            text.substr(at, text.size() - at < 2 ? text.size() - at : 2));
			run_escaped = true;
			i = at + 2;
		}
		if (run_end > run_start) {
			// A DECORATION-ONLY PLACEMENT, which is all a literal run is: no module,
			// no style, its bytes in the prefix.
			prefix_bytes.clear();
			unescape_into(template_slice{text.substr(run_start, run_end - run_start), run_start,
			                             run_escaped},
			              prefix_bytes);
			build.on_placement(nullptr, params_blob{}, style{}, prefix_bytes, std::string_view{});
		}
		if (i >= text.size())
			break;

		// --- a group ---
		if (text[i] == '(') {
			if (depth == 0)
				outermost_open_at = i;
			++depth;
			build.on_open_group();
			++i;
			continue;
		}
		if (text[i] == ')') {
			// `)` is structural everywhere, so a literal one is `\)`. `}` is not - it
			// means nothing outside a placement - which is why a bare `}` needs no
			// escape even though `\}` is accepted for symmetry.
			if (depth == 0)
				return fail(template_error::unbalanced_close, i, text.substr(i, 1));
			--depth;
			build.on_close_group();
			++i;
			continue;
		}

		// --- a placement ---
		const std::size_t open_at = i;
		template_slice field[5];
		std::size_t filled = 0;
		std::size_t colons = 0;
		std::size_t sixth_at = 0;
		std::size_t start = open_at + 1;
		bool escaped = false;
		bool closed = false;
		std::size_t k = start;
		while (k < text.size()) {
			const char one = text[k];
			if (one == '\\') {
				char decoded = 0;
				if (k + 1 >= text.size() || !escape_byte(text[k + 1], decoded))
					return fail(template_error::bad_escape, k,
					            text.substr(k, text.size() - k < 2 ? text.size() - k : 2));
				escaped = true;
				k += 2;
				continue;
			}
			if (one != ':' && one != '}') {
				++k;
				continue;
			}

			const template_slice piece{text.substr(start, k - start), start, escaped};
			if (filled < 5)
				field[filled++] = piece;
			else if (!piece.empty())
				// The trailing colon is legal and an empty sixth field is what it
				// leaves behind; bytes in that field are a slot this grammar does not
				// have.
				return fail(template_error::too_many_fields, sixth_at, piece.raw);
			escaped = false;
			start = k + 1;

			if (one == '}') {
				closed = true;
				++k;
				break;
			}
			++colons;
			if (colons > 5)
				return fail(template_error::too_many_fields, k, text.substr(k, 1));
			if (colons == 5)
				sixth_at = k;
			++k;
		}
		if (!closed)
			return fail(template_error::unclosed_placement, open_at, text.substr(open_at, 1));

		// A module name is snake_case, so nothing in one ever needs an escape and
		// the raw bytes are the name. An escape there simply makes it a name nobody
		// registered.
		const std::string_view name = field[0].raw;
		if (name.empty())
			return fail(template_error::empty_name, field[0].at, name);

		const template_slice& style_slot = field[1];
		const template_slice& type_slot = field[2];
		const template_slice& prefix = field[3];
		const template_slice& postfix = field[4];

		style pen{};
		if (!style_slot.empty()) {
			// NOT UNESCAPED FIRST: a style spec is names, digits, `#`, `+`, `.` and
			// `-`, and none of the eight escapes can appear in a valid one, so the raw
			// bytes are the spec and an escaped byte simply fails to parse.
			const style_parse parsed = parse_style(style_slot.raw);
			if (!parsed.ok) {
				// The failing ITEM, not the whole spec: `bad style 'blod'` is what the
				// author has to fix, and the offset is absolute so they can find it in
				// a long line.
				std::string_view item = style_slot.raw.substr(parsed.error_at);
				const std::size_t dot = item.find('.');
				if (dot != std::string_view::npos)
					item = item.substr(0, dot);
				return fail(template_error::bad_style, style_slot.at + parsed.error_at, item);
			}
			pen = parsed.value;
		}

		prefix_bytes.clear();
		unescape_into(prefix, prefix_bytes);
		postfix_bytes.clear();
		unescape_into(postfix, postfix_bytes);

		if (name == kLiteralPlacement) {
			if (!type_slot.empty())
				return fail(template_error::literal_takes_no_type, type_slot.at, name);
			if (prefix.empty() && postfix.empty())
				return fail(template_error::literal_needs_text, field[0].at, name);
			build.on_placement(nullptr, params_blob{}, pen, prefix_bytes, postfix_bytes);
			i = k;
			continue;
		}

		const module* core = build.resolve(name);
		if (core == nullptr)
			return fail(template_error::unknown_module, field[0].at, name);

		type_bytes.clear();
		unescape_into(type_slot, type_bytes);

		params_blob params;
		parse_error why;
		if (!core->parse(type_bytes, params, why)) {
			// THE MODULE'S REFUSAL, POINTED AT A BYTE. An empty slot has no byte of
			// its own to point at, so the name is what a user has to look at - `{env}`
			// is wrong at `env`, not at the brace after it.
			template_check refused;
			refused.ok = false;
			refused.error = template_error::bad_type;
			refused.error_at = type_slot.empty() ? field[0].at : type_slot.at + why.at;
			refused.subject = name;
			refused.detail = why.what;
			if (why.length != 0 && why.at < type_slot.raw.size()) {
				const std::size_t room = type_slot.raw.size() - why.at;
				refused.what = type_slot.raw.substr(why.at, why.length < room ? why.length : room);
			}
			return refused;
		}

		build.on_placement(core, params, pen, prefix_bytes, postfix_bytes);
		i = k;
	}

	if (depth != 0)
		return fail(template_error::unclosed_group, outermost_open_at,
		            text.substr(outermost_open_at, 1));
	return answer;
}

// The do-nothing policy: the same walk, resolving against the built-in table and
// building nothing at all.
struct template_validator {
	[[nodiscard]] constexpr const module* resolve(std::string_view name) const noexcept {
		return builtin_module_named(name);
	}

	constexpr void on_placement(const module*, const params_blob&, const style&, std::string_view,
	                            std::string_view) const noexcept {}
	constexpr void on_open_group() const noexcept {}
	constexpr void on_close_group() const noexcept {}
};

// A template's structure, its styles and its built-in modules' type grammars,
// checked wherever the bytes are known at compile time. It cannot see a module
// the ABI registered at run time, which is the one thing the set-time walk has
// that this does not; everything else is the same code, the same `parse` methods
// and the same errors.
[[nodiscard]] constexpr template_check validate_template(std::string_view text) {
	template_validator only_checking;
	return scan_template(text, only_checking);
}

// ---------------------------------------------------------------------------
// Compiling a template at compile time
// ---------------------------------------------------------------------------

// A compiled template: the steps, and the arena their affix bytes live in.
//
// A PURE VALUE WITH NOTHING POINTING INTO ITSELF, which is what lets it come out
// of a `consteval` function and be a `constexpr` variable. That is the whole
// reason `bytes` is an offset.
template <std::size_t Steps, std::size_t Bytes>
struct compiled_template {
	// `Bytes` is zero for a template with no literal bytes at all; a zero-length
	// array is not a thing, and one spare byte is cheaper than a special case.
	std::array<step, Steps> steps{};
	std::array<char, (Bytes == 0 ? 1 : Bytes)> arena{};

	[[nodiscard]] constexpr program_view view() const noexcept {
		return program_view{std::span<const step>{steps}, arena.data()};
	}
};

namespace compile_detail {

struct sizes {
	std::size_t steps = 0;
	std::size_t bytes = 0;
};

// Pass one: how many steps, and how many affix bytes.
struct measuring_policy {
	sizes counted;

	[[nodiscard]] constexpr const module* resolve(std::string_view name) const noexcept {
		return builtin_module_named(name);
	}

	constexpr void on_placement(const module*, const params_blob&, const style&,
	                            std::string_view prefix, std::string_view postfix) noexcept {
		++counted.steps;
		counted.bytes += prefix.size() + postfix.size();
	}

	constexpr void on_open_group() noexcept { ++counted.steps; }
	constexpr void on_close_group() const noexcept {}
};

// Pass two: the steps themselves, into storage pass one sized.
//
// THE SAME POLICY SHAPE THE ENGINE'S BUILDER HAS, and the two are deliberately
// near-identical - a `std::array` and a `std::vector` of the same `step`, an
// arena that is an array here and a `std::string` there. What differs is where
// the storage came from, which is the only thing that can differ between compile
// time and run time once the element is a value.
template <std::size_t Steps, std::size_t Bytes>
struct building_policy {
	compiled_template<Steps, Bytes> made{};
	std::size_t used_steps = 0;
	std::size_t used_bytes = 0;
	std::array<std::size_t, (Steps == 0 ? 1 : Steps)> open{};
	std::size_t depth = 0;
	bool overflowed = false;

	[[nodiscard]] constexpr const module* resolve(std::string_view name) const noexcept {
		return builtin_module_named(name);
	}

	constexpr bytes intern(std::string_view text) {
		const bytes span{static_cast<std::uint32_t>(used_bytes),
		                 static_cast<std::uint32_t>(text.size())};
		for (const char one : text) {
			if (used_bytes >= made.arena.size()) {
				overflowed = true;
				return bytes{};
			}
			made.arena[used_bytes++] = one;
		}
		return span;
	}

	constexpr void on_placement(const module* core, const params_blob& params, const style& pen,
	                            std::string_view prefix, std::string_view postfix) {
		if (used_steps >= Steps) {
			overflowed = true;
			return;
		}
		step& one = made.steps[used_steps++];
		one.place.core = core;
		one.place.params = params;
		one.place.pen = pen;
		one.place.prefix = intern(prefix);
		one.place.postfix = intern(postfix);
	}

	constexpr void on_open_group() {
		if (used_steps >= Steps) {
			overflowed = true;
			return;
		}
		open[depth++] = used_steps;
		made.steps[used_steps++].group = true;
	}

	constexpr void on_close_group() {
		const std::size_t at = open[--depth];
		made.steps[at].span = static_cast<std::uint32_t>(used_steps - at - 1);
	}
};

template <fixed_string Template>
[[nodiscard]] consteval sizes measure() {
	measuring_policy counting;
	const template_check checked = scan_template(Template.view(), counting);
	if (!checked.ok)
		// A CONSTEVAL FUNCTION THAT CANNOT ANSWER IS A BUILD FAILURE, which is the
		// entire point: `compile<"{gti}">()` does not compile, and a shipped default
		// that stopped parsing is caught by the compiler rather than by a user whose
		// prompt went blank.
		throw "lesh: the prompt template does not parse";
	return counting.counted;
}

} // namespace compile_detail

// A TEMPLATE, COMPILED - the same scanner and the same `module::parse` methods a
// `prompt` builtin runs, at compile time, into a value.
//
// This is §6.10's "one grammar, two evaluation times" arriving at its end state:
// the shipped default is not a table that has to be kept in step with the string
// `prompt` prints for it, it IS that string compiled.
// `TheDefaultTableAndItsTemplateAgree` still runs, because an assertion is
// cheaper than trusting the sentence above.
template <fixed_string Template>
[[nodiscard]] consteval auto compile() {
	constexpr compile_detail::sizes counted = compile_detail::measure<Template>();
	compile_detail::building_policy<counted.steps, counted.bytes> building;
	const template_check checked = scan_template(Template.view(), building);
	if (!checked.ok || building.overflowed)
		throw "lesh: the prompt template does not parse";
	return building.made;
}

// ---------------------------------------------------------------------------
// The default prompt
// ---------------------------------------------------------------------------

inline constexpr style kCyan = style{.fg = color::of_index(6)};
inline constexpr style kMagenta = style{.fg = color::of_index(5)};
inline constexpr style kRed = style{.fg = color::of_index(1)};

// `~/src> `: the working directory, `$HOME` contracted to `~`, and an arrow.
// Nothing else - no colour, no branch, no status (owner's ruling on #157).
//
// THE QUIET ONE, DELIBERATELY. A shipped default is the prompt of every user who
// has not decided yet, and the one thing it must not do is decide for them: a
// shell that arrives already wearing three coloured segments has spent their
// terminal's width and their attention on choices they never made, and every
// starship segment they DO want is one `prompt` away. The machinery is not being
// hedged on - the group vote, the omission rule, the affixes that vanish with
// their module are all still here and still proved, in the standalone asserts in
// `selftest` below rather than in this prompt. What is small here is the default,
// not the engine.
//
// COMPILED FROM ITS OWN SPELLING, so there is no second statement of it to drift:
// no initializer runs at startup, no allocation happens for the configuration
// almost every session has, and `prompt` on a fresh shell prints the string this
// line is written in.
inline constexpr std::string_view kDefaultLeftTemplate = "{path}> ";
inline constexpr std::string_view kDefaultContinuationTemplate = "> ";

inline constexpr auto kDefaultLeft = compile<"{path}> ">();
inline constexpr auto kDefaultContinuation = compile<"> ">();

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

// What a configuration verb answered. Three failures, and they are different
// questions: nobody has that name, that style spec does not parse, that module
// will not take that type.
enum class place_result : std::uint8_t {
	ok,
	unknown_module,
	bad_style,
	bad_type,
};

// The registry, the configuration verbs, the output slots and the tick wheel.
//
// RECALCULATION BY CAUSE (§6.10) is the reason this object exists at all rather
// than a `render_program` call per prompt. Every TOP-LEVEL item owns an output
// slot; a render re-invokes only the items with a reason and splices every other
// slot unchanged. v1 has two of the three causes - a new prompt, and an item's
// own wake tick - because v1's loop-integration surface is timers only; the third
// (an event an element declared interest in) arrives with #156's completion path
// and is a third entry point, not a change to these two.
//
// THE TICK PATH'S CONTRACT, and the caller owns it: `render_tick` may be handed a
// `state` that differs from the last `render_full`'s only in `tick`, `hours`,
// `minutes` and `seconds`. Nothing else may have moved, because nothing else is
// re-read - a slot that is not due is copied, not recomputed. A changed `$?` or a
// changed `$PWD` is a NEW PROMPT and goes through `render_full`.
//
// LOOP-THREAD ONLY, like every other registry here (#93). No locking anywhere,
// and the rule is really "one thread at a time": #157's wiring calls
// `render_full` from the SHELL thread, in the window ADR-0009 gives it while the
// loop is blocked in `wait_on_shell` - the same window `loop_options::prompt` has
// been written in since #129. `render_tick` is the loop's own. See
// `session::refresh_prompt` in `ui/session.cpp`, where the argument is made in
// full.
class engine {
public:
	engine();
	~engine();

	engine(const engine&) = delete;
	engine& operator=(const engine&) = delete;

	// --- The module registry ---

	// Registers a module under `name`, REPLACING any existing registration -
	// #101's rule, so re-sourcing an rc file is idempotent rather than an error.
	// Names are snake_case; anything else is LESH_ERR_INVAL, and a null `which` is
	// too. LESH_OK otherwise.
	//
	// THE MODULE IS BORROWED, NOT OWNED. A `module` is a singleton (§6.10) and the
	// built-ins are `constexpr` objects with static storage; a caller registering
	// its own keeps it alive for the engine's life, which is the same contract
	// every other registry here has.
	//
	// A REGISTRATION IS NOT A PLACEMENT. Registering a module puts it in the
	// table; where it appears in the prompt, and how many times, is `place`'s
	// (§6.10: singletons with free placement).
	std::int32_t register_module(std::string_view name, const module* which);

	// The same, for a module that came across the C ABI: the trampoline object and
	// its `{fn, validate, userdata}` triple are this file's, so `abi.h` needs no
	// C++ type. A null `validate` means "accepts any type", which is what the
	// unchecked registration verb promises.
	std::int32_t register_abi_module(std::string_view name, lesh_prompt_module_fn fn,
	                                 lesh_prompt_validate_fn validate, void* userdata);

	[[nodiscard]] bool module_exists(std::string_view name) const;

	// Sorted, because the table is a `std::map` and a caller listing modules wants
	// an order that does not depend on registration history.
	void module_names(std::vector<std::string>& out) const;

	// --- Configuration ---

	void clear(surface_id which);
	void use_default(surface_id which);

	// ONE PLACEMENT, the internal primitive `add_module` and `add_literal` place
	// through: one call that places exactly what `{name:style:type:prefix:postfix}`
	// spells, and nothing else.
	//
	// An empty `name` places a LITERAL: a placement with no module, its bytes in
	// the affixes, its pen painting them. That is the same record and not a
	// special case.
	//
	// NOT THE ABI'S OWN DOOR. `lesh_prompt_set_placements` resolves `module`
	// itself (including the "literal" keyword and its two refusals) against
	// `builder` directly, rather than through this method - see
	// `engine::build_placements` in prompt.cpp for why: the ABI walks a tree, not
	// a flat verb stream, and needs the group hooks `place` has no way to reach.
	//
	// The bytes of every argument are COPIED into the surface's arena (the ABI's
	// copy-in convention, and the only convention that survives a binding whose
	// strings are garbage-collected).
	place_result place(surface_id which, std::string_view name, std::string_view style_spec,
	                   std::string_view type, std::string_view prefix, std::string_view postfix);

	// `place` with only a module and its type - what the runtime console's
	// assembly verb maps onto. False for a name nobody registered OR a type the
	// module refused, which is the one bool a console with no message channel can
	// carry.
	bool add_module(surface_id which, std::string_view name, std::string_view type);

	// `place` with only bytes: a decoration-only placement.
	void add_literal(surface_id which, std::string_view text);

	// GROUPS DO NOT NEST IN v1, ACROSS THE ABI. A second open while one is open is
	// refused rather than silently flattened or silently nested: the verbs are a
	// linear stream and a caller that lost track of its own nesting should hear
	// so. Nesting is the template language's, whose parser has the structure to
	// express it and the set-time validation to check it (§6.10).
	bool open_group(surface_id which);

	// False when none is open.
	bool close_group(surface_id which);

	// THE C ABI's WHOLE-SURFACE VERB (#157, owner's ruling): a `lesh_prompt_placement`
	// tree, validated recursively and swapped atomically, exactly the promise
	// `set_template` below makes for a string. Returns the ABI's own status:
	// LESH_OK, LESH_ERR_INVAL (a structural error in the tree), LESH_ERR_NOTFOUND
	// (the first unresolved module) or 1 (the first style/type/literal refusal) -
	// see abi.h for the rule each answers.
	//
	// ONE BUILDER, TWO FRONT DOORS: this walks the tree calling the identical
	// `builder` `set_template` drives from scanned bytes, so a tree that says what
	// a template says builds the identical program. `count == 0` clears the
	// surface, `items` unread.
	std::int32_t set_placements(surface_id which, const lesh_prompt_placement* items,
	                            std::size_t count);

	// The template language, parsed ONCE and swapped ATOMICALLY.
	//
	// On success the surface holds the placements the template describes and
	// remembers its source; on failure `error_out` holds one human sentence with a
	// byte offset and THE SURFACE IS UNTOUCHED - its steps, its slots and its
	// remembered text are all exactly what they were, because the parse builds
	// into its own storage and only a complete parse reaches the surface. That is
	// the promise `prompt_console::set` documents on the runtime side, and it is
	// kept here rather than there because only a parser can keep it.
	//
	// A FAILED SET CONFIGURED NOTHING: `configured()` does not move, so a shell
	// whose only prompt verb was a typo still has `$PS1`.
	bool set_template(surface_id which, std::string_view text, std::string& error_out);

	// The source string the surface was last set from, or empty.
	//
	// EMPTY IS AN HONEST ANSWER, NOT A MISSING ONE. `use_default` remembers the
	// shipped template (`kDefaultLeftTemplate`), and `set_template` remembers what
	// it was handed - but the assembly verbs cannot: a prompt built out of `place`
	// calls has no template string, and inventing one by walking the placements
	// back into a spelling would put the element vocabulary on the far side of a
	// boundary §6.10 closed. So every one of those verbs, and `clear`, empties it.
	[[nodiscard]] std::string_view template_text(surface_id which) const;

	// Whether anything has configured this engine - false until the first of the
	// verbs above has run, from C++ or across the ABI, and true from then on.
	//
	// ONE HALF OF THE PRECEDENCE RULE, and since #157's ruling it is no longer the
	// whole of it. §6.10 makes `PS1`/`PS2` a transitional stub, rendered as literal
	// bytes "as it does today", superseded by the native prompt rather than grown
	// into a POSIX expansion vocabulary; the owner's ruling is that the
	// supersession has arrived, so the native prompt is what a fresh shell shows
	// and `$PS1` is the opt-out. What this flag still decides is the case where the
	// two disagree: a user who set `$PS1` gets the stub UNTIL something configures
	// the engine, and from that moment the native composer owns both surfaces for
	// the rest of the session. The rest of the rule - "an untouched `$PS1` is not a
	// preference" - is a question about the shell's variables and is asked in the
	// ui layer, which is the only side that can see them. See
	// `session::refresh_prompt` in `ui/session.cpp`.
	//
	// NOT REGAINED. There is no un-configure: `clear` and `use_default` are
	// themselves configuration, and `use_default` in particular is how a user asks
	// for the shipped prompt back - which is a native prompt and emphatically not a
	// request to be handed `$PS1` again.
	[[nodiscard]] bool configured() const noexcept { return _configured; }

	// --- Rendering ---

	// The new-prompt cause: every item runs, every slot is rewritten, and the
	// per-render (module, params) memo starts empty.
	void render_full(const state& facts);

	// The tick cause: only the items whose deadline has come are re-invoked, every
	// other slot is spliced unchanged, and the answer is whether the surface bytes
	// actually moved. False means no terminal write is owed - which is the point
	// of asking (§6.10: unchanged output produces no write).
	bool render_tick(const state& facts);

	[[nodiscard]] std::string_view output(surface_id which) const;

	// The earliest armed deadline over both surfaces, as an absolute tick. Zero is
	// NO TIMER AT ALL, not "now": an empty deadline list means a static prompt
	// causes zero idle wakeups.
	[[nodiscard]] std::uint64_t next_wake() const;

private:
	// No group open on the verb stream. v1 refuses a second open, so one index is
	// a stack of one.
	static constexpr std::size_t kNoGroup = static_cast<std::size_t>(-1);

	// What a top-level item last produced. `wake_at` is ABSOLUTE - the tick the
	// item asked to be woken at, zero for no deadline - because a relative request
	// is only meaningful at the instant it was made.
	struct slot {
		std::string bytes;
		element_status status = element_status::omitted;
		std::uint64_t wake_at = 0;
	};

	// One configured prompt.
	//
	// THE PROGRAM AND ITS ARENA TRAVEL TOGETHER, and the arena is the reason a
	// reconfiguration is a swap of two containers rather than a walk of a tree
	// freeing nodes. Nothing in `program` points at anything; `arena` is the only
	// storage, and `view()` is what turns the pair into something the composer can
	// walk.
	struct surface {
		std::vector<step> program;
		std::string arena;

		// One per step, kept warm across renders. The capacity is what makes a warm
		// render allocate nothing.
		std::vector<step_scratch> scratch;

		// One per TOP-LEVEL item. `slots` may be LONGER than `tops`: entries past
		// the end are stale and never read, and keeping them is what stops a
		// reconfiguration from giving back the strings' capacity.
		std::vector<slot> slots;
		std::vector<std::size_t> tops;   // step index of each top-level item

		std::string bytes;

		// The group the verb stream has open, as a step index.
		std::size_t open = kNoGroup;

		// What `template_text` answers: the source `set_template` was handed, or
		// the shipped default's own spelling, or empty. See `template_text`.
		std::string text;

		[[nodiscard]] program_view view() const noexcept {
			return program_view{std::span<const step>{program}, arena.data()};
		}
	};

	// The BUILDING policy for `scan_template`, defined in prompt.cpp. It builds
	// into its own `std::vector` and `std::string` and hands both over only at the
	// end, which is the whole of the atomicity promise: there is no partial
	// application to undo because nothing was applied.
	struct builder;

	// The four-way answer `set_placements` collapses the template parser's finer
	// refusals into (#157): the array verb has one status for a style refusal, a
	// type refusal and `literal`'s two refusals, because it has no message
	// channel to tell them apart with.
	enum class placements_error : std::uint8_t { none, invalid, unknown_module, refused };

	// `set_placements`'s recursive walk over one `lesh_prompt_placement` array,
	// calling `builder`'s four hooks exactly as `scan_template` does from scanned
	// bytes - "one builder, two front doors". A `struct builder` member is what
	// lets this reach the private type; defined in prompt.cpp beside `builder`.
	static placements_error build_placements(builder& build, const lesh_prompt_placement* items,
	                                         std::size_t count);

	// The C function, its optional validator and its registration context, for a
	// module that came in across the ABI. A `module` like any other - it parses
	// its type slot through the validator and renders through the trampoline - so
	// the composer has one kind of thing in it and not two.
	struct abi_module;

	// One entry of the per-render (module, params) memo (§6.10: `{env::USER}` and
	// `{env::HOST}` are two placements computed once each, and one module placed
	// twice with the same params is computed once).
	//
	// A LINEAR VECTOR, and a used-count instead of `clear()`: a prompt has a
	// handful of modules, so a scan beats a hash, and reusing the entries keeps
	// their strings' capacity across renders.
	struct memo_entry {
		const module* which = nullptr;
		params_blob params{};
		std::string bytes;
		element_status status = element_status::omitted;
		std::uint64_t wake = 0;
	};

	[[nodiscard]] surface& at(surface_id which);
	[[nodiscard]] const surface& at(surface_id which) const;

	// The memoizing core runner - the engine's half of the composer's one template
	// parameter. A struct rather than a lambda so both render paths name the same
	// type.
	struct memoizing_core {
		engine* owner = nullptr;
		element_status operator()(const placement& one, const state& facts, sink& into);
	};

	void adopt(surface& target, std::vector<step>&& program, std::string&& arena);
	void refit(surface& target);
	[[nodiscard]] bytes intern(surface& target, std::string_view text);
	[[nodiscard]] step& append_step(surface& target);
	void touched(surface& target);

	void render_surface(surface& target, const state& facts);
	void rebuild(surface& target);

	std::map<std::string, const module*, std::less<>> _modules;
	std::map<std::string, std::unique_ptr<abi_module>, std::less<>> _abi_modules;
	std::array<surface, static_cast<std::size_t>(surface_id::count_)> _surfaces;

	std::vector<memo_entry> _memo;
	std::size_t _memo_used = 0;

	sink _top;

	// See `configured()`. The constructor's own seeding of the default
	// deliberately does not set it - that is the engine arriving with something to
	// render, not a user asking for it.
	bool _configured = false;
};

// ---------------------------------------------------------------------------
// C++ sugar over `lesh_prompt_set_placements` - NOT part of the ABI (#157)
// ---------------------------------------------------------------------------

// Deduces the count a C caller has to pass by hand, so an in-tree caller writes
//
//   set_placements(registry, LESH_PROMPT_LEFT, {
//       { "path", { .style = "cyan", .type = "s" } },
//       { "literal", { .prefix = "> " } },
//   });
//
// This is exactly `lesh_prompt_set_placements(registry, surface, items, N)` -
// abi.h stays purely C11, where the same call is a compound literal plus its
// own count, and that form is what this forwards to.
template <std::size_t N>
inline std::int32_t set_placements(lesh_registry* registry, std::uint32_t surface,
                                   const lesh_prompt_placement (&items)[N]) {
	return lesh_prompt_set_placements(registry, surface, items, N);
}

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
// The discipline `lesh::args`'s constexpr scan set: the properties that must hold
// are asserted by the compiler, against synthetic facts, on the REAL compiled
// default rather than on a copy of it. A failure here is a build failure, not a
// red test - which is what one wants for "the composer omits correctly", because
// a composer that has stopped omitting is not a behaviour worth shipping far
// enough to run.
//
// The runtime tests cover what a constant expression cannot see: invocation
// COUNTS (the memo, the tick wheel's recalculation-by-cause), allocation counts,
// and everything that goes through the ABI.
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

template <std::size_t N = 160, std::size_t Steps, std::size_t Bytes>
[[nodiscard]] constexpr rendered<N> run(const compiled_template<Steps, Bytes>& program,
                                        const state& facts) {
	std::array<step_scratch, (Steps == 0 ? 1 : Steps)> scratch{};
	sink out;
	direct_core plainly;
	render_program(program.view(), std::span<step_scratch>{scratch}, facts, out, plainly);

	rendered<N> copied;
	for (const char one : out.bytes())
		copied.bytes[copied.length++] = one;
	return copied;
}

// The wake the whole program asked for, which is the other half of what a render
// answers and the half a byte comparison cannot see.
template <std::size_t Steps, std::size_t Bytes>
[[nodiscard]] constexpr std::uint64_t wake_of(const compiled_template<Steps, Bytes>& program,
                                              const state& facts) {
	std::array<step_scratch, (Steps == 0 ? 1 : Steps)> scratch{};
	sink out;
	direct_core plainly;
	render_program(program.view(), std::span<step_scratch>{scratch}, facts, out, plainly);
	return out.wake();
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

[[nodiscard]] constexpr state failed() noexcept {
	state facts = quiet();
	facts.status = 2;
	return facts;
}

// 1. THE WHOLE SHIPPED DEFAULT, compiled from the string it prints for itself:
//    the contracted path and the arrow, and nothing else. `> ` is unconditional
//    because it is top-level.
static_assert(run(kDefaultLeft, quiet()).view() == "~/src> ");
static_assert(run(kDefaultContinuation, quiet()).view() == "> ");

// 1b. THE SAME BYTES AFTER A FAILURE, and that identity is the assertion. Since
//     #157's ruling the default carries no `status` placement, so a non-zero `$?`
//     changes nothing about what it paints; the proof that an affix vanishes with
//     the module it belongs to is 3 and 4 below.
static_assert(run(kDefaultLeft, failed()).view() == "~/src> ");

// 2. THE OWNER'S EXAMPLE, which is the one the ticket is written around: a short
//    cyan path, and a magenta something inside a group that vanishes with it.
//
//    SPELLED WITH `jobs` RATHER THAN `git` (#163), which is a fact about where
//    the modules live and not about the shape being asserted. `git` is
//    leshnici's now and a constant expression here cannot see it; `jobs` omits
//    on the same facts for the same kind of reason - it has nothing to say - so
//    the group vanishes exactly as the branch did, and every byte below is the
//    byte the branch form produced.
inline constexpr auto kExample = compile<"{path:cyan:s}( on {jobs:magenta})">();
static_assert(run(kExample, quiet()).view() == "\x1b[36msrc\x1b[0m");

// 3. OMISSION PROPAGATES THROUGH THE GROUP: the literal and the style vanish with
//    the module, and what is left is not "almost nothing" but nothing. Read the
//    other way, this is how a constant expression sees that phase two never ran -
//    no style byte and no literal byte is present anywhere.
inline constexpr auto kBranch = compile<"({literal:magenta:: on }{jobs:magenta})">();
static_assert(run(kBranch, quiet()).view().empty());
static_assert(run(kBranch, quiet()).length == 0);

// 4. THE `status` PLACEMENT, both ways round: it brings its colour and its
//    brackets with it when there is something to say, and takes both away when
//    there is not. One placement, five parts, one vote.
inline constexpr auto kStatus = compile<"{status:red:: [:]}">();
static_assert(run(kStatus, quiet()).view().empty());
static_assert(run(kStatus, failed()).view() == "\x1b[31m [2]\x1b[0m");

// 4b. AND A COLOUR ON A PLACEMENT DOES NOT CHANGE HOW IT VOTES, which is the
//     regression this whole model change exists to make unsayable. The two
//     spellings differ by one word and must differ by one SGR pair.
inline constexpr auto kVotePlain = compile<"({literal:::[}{status}{literal:::]})">();
inline constexpr auto kVoteColoured = compile<"({literal:::[}{status:red}{literal:::]})">();
static_assert(run(kVotePlain, failed()).view() == "[2]");
static_assert(run(kVoteColoured, failed()).view() == "[\x1b[31m2\x1b[0m]");
static_assert(run(kVotePlain, quiet()).view().empty());
static_assert(run(kVoteColoured, quiet()).view().empty());

// 5. `status` omits on zero and speaks otherwise - the module the whole omission
//    machinery exists for - and its symbol form says a mark instead of a number.
static_assert(run(compile<"{status}">(), quiet()).view().empty());
static_assert(run(compile<"{status}">(), failed()).view() == "2");
static_assert(run(compile<"{status::x}">(), quiet()).view().empty());
static_assert(run(compile<"{status::x}">(), failed()).view() == "x");

// 6. THE FIVE `path` VARIANTS, on one set of facts, so the differences are the
//    assertion rather than five separate claims.
[[nodiscard]] constexpr state deep() noexcept {
	state facts = quiet();
	facts.pwd = "/home/u/private/github/lesh";
	return facts;
}
static_assert(run(compile<"{path}">(), deep()).view() == "~/private/github/lesh");
static_assert(run(compile<"{path::relative}">(), deep()).view() == "~/private/github/lesh");
static_assert(run(compile<"{path::r}">(), deep()).view() == "~/private/github/lesh");
static_assert(run(compile<"{path::absolute}">(), deep()).view() == "/home/u/private/github/lesh");
static_assert(run(compile<"{path::a}">(), deep()).view() == "/home/u/private/github/lesh");
static_assert(run(compile<"{path::f}">(), deep()).view() == "/home/u/private/github/lesh");
static_assert(run(compile<"{path::short}">(), deep()).view() == "lesh");
static_assert(run(compile<"{path::s}">(), deep()).view() == "lesh");
static_assert(run(compile<"{path::initials}">(), deep()).view() == "~/p/g/lesh");
static_assert(run(compile<"{path::i}">(), deep()).view() == "~/p/g/lesh");
static_assert(run(compile<"{path::unvowel}">(), deep()).view() == "~/prvt/gthb/lesh");
static_assert(run(compile<"{path::u}">(), deep()).view() == "~/prvt/gthb/lesh");

// The home contraction respects components: a sibling directory whose name merely
// starts with the home directory's is not under it.
[[nodiscard]] constexpr state elsewhere() noexcept {
	state facts = quiet();
	facts.pwd = "/home/username/x";
	return facts;
}
static_assert(run(compile<"{path}">(), elsewhere()).view() == "/home/username/x");
static_assert(run(compile<"{path}">(), quiet()).view() == "~/src");

[[nodiscard]] constexpr state at_home() noexcept {
	state facts = quiet();
	facts.pwd = "/home/u";
	return facts;
}
// AT HOME, THE SHORT FORM IS `~` AND NOT THE NAME OF THE DIRECTORY. Contracting
// first and then taking the last component is what makes that fall out rather
// than needing a rule of its own.
static_assert(run(compile<"{path::s}">(), at_home()).view() == "~");
static_assert(run(compile<"{path::i}">(), at_home()).view() == "~");
static_assert(run(compile<"{path::u}">(), at_home()).view() == "~");

// A component that is all vowels keeps its first byte rather than vanishing.
[[nodiscard]] constexpr state vowelly() noexcept {
	state facts = quiet();
	facts.pwd = "/home/u/aeiou/x";
	return facts;
}
static_assert(run(compile<"{path::u}">(), vowelly()).view() == "~/a/x");

// 7. `env` reads the variable its type slot names, and an empty value omits.
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

static_assert(run(compile<"{env::USER}">(), with_variables()).view() == "u");
static_assert(run(compile<"{env::EMPTY}">(), with_variables()).view().empty());
static_assert(run(compile<"{env::NOPE}">(), with_variables()).view().empty());
static_assert(run(compile<"{env::USER}">(), quiet()).view().empty());

// 8. `duration`'s floor and its three formats.
[[nodiscard]] constexpr state took(std::uint64_t milliseconds) noexcept {
	state facts = quiet();
	facts.duration_ms = milliseconds;
	return facts;
}
inline constexpr auto kDuration = compile<"{duration}">();
static_assert(run(kDuration, took(1999)).view().empty());
static_assert(run(kDuration, took(2000)).view() == "2s");
static_assert(run(kDuration, took(59'999)).view() == "59s");
static_assert(run(kDuration, took(60'000)).view() == "1m0s");
static_assert(run(kDuration, took(3'599'000)).view() == "59m59s");
static_assert(run(kDuration, took(3'600'000)).view() == "1h0m0s");
static_assert(run(kDuration, took(7'384'000)).view() == "2h3m4s");

// 9. `time`'s four forms, and the cadence each asks for. The 24-hour minute form
//    is the default and wakes once a MINUTE; only the forms that show seconds
//    wake once a second.
[[nodiscard]] constexpr state clock_at(unsigned h, unsigned m, unsigned s) noexcept {
	state facts = quiet();
	facts.hours = static_cast<std::uint8_t>(h);
	facts.minutes = static_cast<std::uint8_t>(m);
	facts.seconds = static_cast<std::uint8_t>(s);
	return facts;
}
static_assert(run(compile<"{time}">(), clock_at(9, 5, 3)).view() == "09:05");
static_assert(run(compile<"{time::24h}">(), clock_at(9, 5, 3)).view() == "09:05");
static_assert(run(compile<"{time::24hs}">(), clock_at(9, 5, 3)).view() == "09:05:03");
static_assert(run(compile<"{time::12h}">(), clock_at(13, 5, 3)).view() == "01:05");
static_assert(run(compile<"{time::12hs}">(), clock_at(0, 5, 3)).view() == "12:05:03");
static_assert(run(compile<"{time::12h}">(), clock_at(12, 5, 3)).view() == "12:05");
// Three seconds into a minute, thirty-seven ticks into a second: the seconds form
// wants the next second, the minute form the next minute, both derived and
// neither stored.
static_assert(wake_of(compile<"{time::24hs}">(), clock_at(9, 5, 3)) == 100);
static_assert(wake_of(compile<"{time::24h}">(), clock_at(9, 5, 3)) == 56 * 100 + 100);

// 10. THE SGR ROUND TRIP. `emit_sgr` is the inverse of `sgr.h`'s `apply_sgr`, and
//     these are what say so - each style emitted from reset semantics and read
//     back into the style it was.
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

// 11. THE PARAMS BLOB, which the memo's correctness rests on: the same module
//     with different params is a different question, and the round trip through
//     the bytes is lossless.
constexpr params_blob params_for(const module& which, std::string_view type) {
	params_blob made;
	parse_error why;
	if (!which.parse(type, made, why))
		throw "the selftest handed a module a type it refuses";
	return made;
}
static_assert(params_for(kModulePath, "s") == params_for(kModulePath, "short"));
static_assert(!(params_for(kModulePath, "s") == params_for(kModulePath, "i")));
static_assert(params_for(kModuleEnv, "USER").as<env_params>().name.view() == "USER");
static_assert(!(params_for(kModuleEnv, "USER") == params_for(kModuleEnv, "HOST")));
static_assert(params_for(kModuleJobs, "").size == 0);

// 12. THE TEMPLATE GRAMMAR, AT THE OTHER OF ITS TWO EVALUATION TIMES. The same
//     `scan_template` the engine builds with, walked with the do-nothing policy:
//     what these prove is the grammar itself - the omission table, the affix
//     slots, the escapes and every refusal - and that it is one walk, because
//     there is only one to fail.
//
//     THE SHIPPED DEFAULTS PARSE, and that is the assertion that would catch a
//     grammar change breaking the string `use_default` hands back. (That they
//     COMPILE is proved harder, one screen up: `kDefaultLeft` is that string put
//     through `consteval`, so a grammar change that broke it would not build.)
static_assert(validate_template(kDefaultLeftTemplate).ok);
static_assert(validate_template(kDefaultContinuationTemplate).ok);
static_assert(validate_template("{path}> ").ok);
static_assert(validate_template("").ok);

// prmt's omission table, row by row: every legal spelling of an empty slot.
static_assert(validate_template("{jobs}").ok);
static_assert(validate_template("{jobs:magenta}").ok);
static_assert(validate_template("{jobs:}").ok);               // trailing colon
static_assert(validate_template("{jobs:magenta::on :}").ok);  // default type, prefix only
static_assert(validate_template("{jobs::::!}").ok);           // postfix only
static_assert(validate_template("{status:red::[:]}").ok);     // both affixes
static_assert(validate_template("{env::USER}").ok);           // type only
static_assert(validate_template("{env::A\\:B}").ok);          // a colon IN the type

// Groups, nested, and the literal runs between them.
static_assert(validate_template("{path}( on {jobs})> ").ok);
static_assert(validate_template("(({jobs}))").ok);
static_assert(validate_template("\\{not a placement\\}").ok);
static_assert(validate_template("a\\nb\\tc\\\\d").ok);

// The standalone styled literal: its text is in the AFFIX slots, and the type
// slot it does not have is refused rather than quietly read as text.
static_assert(validate_template("{literal:blue::hi}").ok);
static_assert(validate_template("{literal:blue::hi :there}").ok);
static_assert(validate_template("{literal:::plain}").ok);
static_assert(validate_template("{literal:blue:x:hi}").error == template_error::literal_takes_no_type);
static_assert(validate_template("{literal::x}").error == template_error::literal_takes_no_type);
static_assert(validate_template("{literal}").error == template_error::literal_needs_text);
static_assert(validate_template("{literal:blue}").error == template_error::literal_needs_text);

// Every refusal, with the byte it points at - the offset is the contract, not a
// detail, because it is what the message tells the user to look at.
static_assert(!validate_template("{gti}").ok);
static_assert(validate_template("{gti}").error == template_error::unknown_module);
static_assert(validate_template("{gti}").error_at == 1);
static_assert(validate_template("{gti}").what == "gti");
static_assert(validate_template("{path}{gti}").error_at == 7);

static_assert(validate_template("{path").error == template_error::unclosed_placement);
static_assert(validate_template("{path").error_at == 0);

static_assert(validate_template("( x").error == template_error::unclosed_group);
static_assert(validate_template("( x").error_at == 0);
static_assert(validate_template("(a)(b").error_at == 3);
static_assert(validate_template("x)").error == template_error::unbalanced_close);
static_assert(validate_template("x)").error_at == 1);

// THE TYPE SLOT'S REFUSALS ARE THE MODULES' OWN, and each one carries the words
// that module chose. `env` has no default variable to mean, `jobs` has no type
// at all, and `path` has five variants of which `medum` is not one.
static_assert(validate_template("{env}").error == template_error::bad_type);
static_assert(validate_template("{env}").error_at == 1);
static_assert(validate_template("{env}").subject == "env");
static_assert(validate_template("{env}").detail == " needs a variable name");
static_assert(validate_template("{env:cyan}").error == template_error::bad_type);

static_assert(validate_template("{jobs::x}").error == template_error::bad_type);
static_assert(validate_template("{jobs::x}").error_at == 7);
static_assert(validate_template("{jobs::x}").detail == " takes no argument");
static_assert(validate_template("{jobs::x}").what.empty());

static_assert(validate_template("{path::medum}").error == template_error::bad_type);
static_assert(validate_template("{path::medum}").error_at == 7);
static_assert(validate_template("{path::medum}").subject == "path");
static_assert(validate_template("{path::medum}").detail == ": unknown variant");
static_assert(validate_template("{path::medum}").what == "medum");
// AND THE FIVE THAT ARE. What used to be `path takes no argument` is now a
// grammar `path` owns, which is the model change visible from the outside.
static_assert(validate_template("{path::short}").ok);
static_assert(validate_template("{path:cyan:s}").ok);
static_assert(validate_template("{time::24hs}").ok);
static_assert(validate_template("{time::13h}").error == template_error::bad_type);
// `status` has no vocabulary to fall outside of: the slot IS the symbol.
static_assert(validate_template("{status::anything}").ok);

static_assert(validate_template("{path:blod}").error == template_error::bad_style);
static_assert(validate_template("{path:blod}").what == "blod");
static_assert(validate_template("{path:cyan.blod}").error_at == 11);
static_assert(validate_template("{path:cyan.blod}").what == "blod");

static_assert(validate_template("{a:b:c:d:e:f}").error == template_error::too_many_fields);
static_assert(validate_template("{a:b:c:d:e:f}").error_at == 10);
static_assert(validate_template("{a:b:c:d:e:f:g}").error == template_error::too_many_fields);
// Five colons with nothing after the last is the trailing colon, not a sixth
// slot, and stays legal.
static_assert(validate_template("{jobs::::!:}").ok);

static_assert(validate_template("{}").error == template_error::empty_name);
static_assert(validate_template("a\\qb").error == template_error::bad_escape);
static_assert(validate_template("a\\qb").error_at == 1);
static_assert(validate_template("a\\").error == template_error::bad_escape);
static_assert(validate_template("{env::a\\qb}").error == template_error::bad_escape);

// A BAD DEFAULT IS A BUILD FAILURE, and there is no assertion for it because the
// assertion would BE the failure. `compile<"{gti}">()` throws out of a `consteval`
// function, which is ill-formed at the point of the call - so a shipped default
// that stopped parsing, or a `path` variant renamed out from under one, stops the
// build here rather than blanking somebody's prompt. Try it by hand if you doubt
// it; it cannot be left in the file.

// The unescape, which is the other half of the escape rule: the flag is what says
// whether the loop runs at all, and the bytes are the same either way.
constexpr bool unescapes(std::string_view raw, bool escaped, std::string_view expected) {
	std::string out;
	unescape_into(template_slice{raw, 0, escaped}, out);
	return out == expected;
}
static_assert(unescapes("plain", false, "plain"));
static_assert(unescapes("a\\:b", true, "a:b"));
static_assert(unescapes("\\{\\}\\(\\)", true, "{}()"));
static_assert(unescapes("a\\nb\\tc\\\\d", true, "a\nb\tc\\d"));

} // namespace selftest

} // namespace lesh::ui::prompt
