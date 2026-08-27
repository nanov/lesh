// The prompt engine's runtime half (#157, spec §6.10): the module registry, the
// configuration verbs, the output slots, the tick wheel, and the C ABI.
//
// The COMPOSER is in prompt.h and is pure - it is literally the same code the
// compiled default renders through, instantiated with a different core runner.
// What is here is everything that has to remember something between renders,
// which is exactly what §6.10's recalculation-by-cause needs: a slot per
// top-level item holding its last bytes and its deadline, so a tick that animates
// one item copies the rest.
//
// ONE PROGRAM SHAPE, TWO PROVENANCES. A surface is a `std::vector<step>` beside a
// `std::string` arena, and the compiled default is a `std::array<step, N>` beside
// a `std::array<char, M>`. `use_default` is therefore a two-container copy and
// not a walk that rebuilds anything: the offsets in the steps mean the same thing
// on both sides, which is the whole reason a placement's affixes are offsets.
//
// THE ABI HALF IS THE SAME SHAPE THE ACTION ABI HAS, deliberately. A module call
// gets an opaque `lesh_prompt_context` valid only for that call; everything
// copies in or copies out; a registered C module reaches the composer as an
// ordinary `module` object, which is the entire cost of NG-4's promise that the
// Lua binding reuses these verbs unchanged.

#include "ui/prompt/prompt.h"

#include "leshper/registry.h"
#include "substrate/assert.h"
#include "substrate/char_utils.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

// ---------------------------------------------------------------------------
// The handle abi.h declares, defined.
//
// At global scope for the same reason `lesh_editor` is - that is where the C
// typedef put it - and with ordinary C++ members, because nothing outside this
// translation unit may see the definition.
// ---------------------------------------------------------------------------

struct lesh_prompt_context {
	const lesh::ui::prompt::state* facts = nullptr;
	lesh::ui::prompt::sink* out = nullptr;
	std::string_view arg;

	// Validity, debug-asserted, exactly as the editor handle's is: zero between
	// calls, so a module that stashed the context and used it later aborts at the
	// call that did it rather than reading a dead sink.
	std::uint64_t call_token = 0;

	[[nodiscard]] bool live() const noexcept {
		return call_token != 0 && facts != nullptr && out != nullptr;
	}
};

namespace {

using lesh::ui::prompt::element_status;
using lesh::ui::prompt::engine;
using lesh::ui::prompt::place_result;
using lesh::ui::prompt::surface_id;

// The C numbers and the C++ enumerators are one space, and these keep them one.
// A reordered enum would otherwise make every module's `ready` read as `omitted`,
// and compile silently.
static_assert(static_cast<int>(element_status::omitted) == LESH_PROMPT_OMITTED);
static_assert(static_cast<int>(element_status::ready) == LESH_PROMPT_READY);
static_assert(static_cast<int>(element_status::pending) == LESH_PROMPT_PENDING);
static_assert(static_cast<int>(element_status::neutral) == LESH_PROMPT_NEUTRAL);
static_assert(static_cast<std::uint32_t>(surface_id::left) == LESH_PROMPT_LEFT);
static_assert(static_cast<std::uint32_t>(surface_id::continuation) == LESH_PROMPT_CONTINUATION);

// `lesh_buffer_get`'s convention, to the letter: the length is always reported, a
// short buffer is LESH_ERR_TOOSMALL rather than a truncation, and a null `out`
// with zero capacity is how a caller asks the length first.
std::int32_t copy_out(std::string_view source, char* out, std::size_t capacity,
                      std::size_t* length_out) noexcept {
	if (length_out == nullptr)
		return LESH_ERR_INVAL;
	*length_out = source.size();
	if (source.size() > capacity)
		return LESH_ERR_TOOSMALL;
	if (source.empty())
		return LESH_OK;
	if (out == nullptr)
		return LESH_ERR_INVAL;
	for (std::size_t i = 0; i < source.size(); ++i)
		out[i] = source[i];
	return LESH_OK;
}

bool context_ok(const lesh_prompt_context* context) noexcept {
	return context != nullptr && context->live();
}

// Every context accessor opens with this. In release it costs nothing; in debug
// it turns "stashed the context" into an abort at the call that did it.
#define LESH_PROMPT_HANDLE(context)                                            \
	do {                                                                       \
		LESH_ASSERT(context_ok(context));                                      \
		if (!context_ok(context))                                              \
			return LESH_ERR_INVAL;                                             \
	} while (0)

// The scan's error as the sentence the builtin prints after "prompt: ".
//
// THE WORDING IS HERE AND NOT IN THE HEADER, and the split is the point: the
// grammar walk is `constexpr` and carries a code and an offset, which is all a
// `static_assert` can compare anyway; turning one into English needs a
// `std::string` and belongs on the side that has a user to talk to. One sentence,
// one byte offset, no punctuation to strip - `report_prompt_outcome` prints it
// verbatim.
//
// `bad_type` IS ASSEMBLED, NOT LOOKED UP, and that is the model change reaching
// even here. The module's name and the module's own words are both carried on the
// check; there is no table of which module says what, because a module registered
// by a binding could never be in it.
std::string describe_template_error(const lesh::ui::prompt::template_check& why) {
	using lesh::ui::prompt::template_error;

	const std::string what{why.what};
	std::string said;
	switch (why.error) {
		case template_error::none:
			return {};
		case template_error::unclosed_placement:
			said = "unclosed '{'";
			break;
		case template_error::unbalanced_close:
			said = "unbalanced ')'";
			break;
		case template_error::unclosed_group:
			said = "unclosed '('";
			break;
		case template_error::too_many_fields:
			said = "too many fields";
			break;
		case template_error::empty_name:
			said = "a placement needs a module name";
			break;
		case template_error::unknown_module:
			said = "unknown module '" + what + "'";
			break;
		case template_error::bad_style:
			said = "bad style '" + what + "'";
			break;
		case template_error::bad_escape:
			said = "unknown escape '" + what + "'";
			break;
		case template_error::bad_type:
			// The module owns the whole predicate, separator included, so this line
			// joins two strings and quotes a token rather than choosing a shape.
			said = std::string{why.subject} + std::string{why.detail};
			if (!what.empty())
				said += " '" + what + "'";
			break;
		case template_error::literal_needs_text:
			said = "literal needs text";
			break;
		case template_error::literal_takes_no_type:
			said = "literal takes no type";
			break;
	}
	said += " at byte ";
	said += std::to_string(why.error_at);
	return said;
}

// THE OPAQUE SLOT, CAST BACK (#170). `registry::host_prompt` is a `void*` on the
// editor's side - leshper carries the pointer and does not name what it points
// at - and this file, which is the host's, is the only code that knows. The verb
// signatures are unchanged: a binding still hands the registry it already holds.
engine* engine_of(lesh_registry* registry) noexcept {
	return registry == nullptr ? nullptr : static_cast<engine*>(registry->host_prompt);
}

bool surface_of(std::uint32_t raw, surface_id& out) noexcept {
	if (raw == LESH_PROMPT_LEFT) {
		out = surface_id::left;
		return true;
	}
	if (raw == LESH_PROMPT_CONTINUATION) {
		out = surface_id::continuation;
		return true;
	}
	return false;
}

// A NUL-terminated C string as a view, NULL meaning empty. Every `place`
// argument arrives this way.
std::string_view text_of(const char* raw) noexcept {
	return raw == nullptr ? std::string_view{} : std::string_view{raw};
}

} // namespace

namespace lesh::ui::prompt {

// ---------------------------------------------------------------------------
// A module that came in across the C ABI
// ---------------------------------------------------------------------------

// What a C module's type slot travels as: the raw bytes, as written, for
// `lesh_prompt_arg` to hand back.
//
// A CEILING RATHER THAN A HEAP POINTER, because these bytes live inside every
// `step` and every memo entry, and a placement has to stay a trivially copyable
// value (see `params_blob`). Eighty-eight bytes is what fits beside the blob's
// length; a longer type slot is refused at set time with a message, which is a
// better answer than a silent truncation and a better one than an allocation on
// a path that has to be copyable.
struct abi_params {
	fixed_text<88> type{};
};

// The C function, its optional validator and its registration context - as an
// ordinary `module`.
//
// ONE KIND OF THING IN THE COMPOSER. The old design needed a trampoline function,
// plus a `binding` struct, plus a `{fn, userdata}` pair the element's `data`
// pointed at; here the object IS the module, `parse` is the validator and
// `render` is the call. Nothing in the render path knows this module came from C.
struct engine::abi_module final : typed_module<abi_params> {
	std::string named;
	lesh_prompt_module_fn fn = nullptr;
	lesh_prompt_validate_fn validate = nullptr;
	void* userdata = nullptr;

	// Where a refusal's words live between `parse` returning false and the
	// sentence being built. `parse_error::what` is a view and a validator's phrase
	// is dynamic, so it needs storage with a lifetime; the set-time path reads it
	// immediately, before anything else can parse.
	mutable std::string refusal;

	[[nodiscard]] std::string_view name() const noexcept override { return named; }

protected:
	bool parse(std::string_view type, abi_params& out, parse_error& err) const override {
		if (!out.type.assign(type)) {
			err.at = 0;
			err.length = 0;
			err.what = ": type is too long";
			return false;
		}
		if (validate == nullptr)
			// NO VALIDATOR IS NOT NO GRAMMAR, it is "accepts anything" - which is
			// what the unchecked registration verb promises, and what every module
			// registered before the checked verb existed keeps getting.
			return true;

		char buffer[256];
		std::size_t length = 0;
		const std::int32_t answered =
			validate(type.empty() ? nullptr : type.data(), type.size(), userdata, buffer,
			         sizeof buffer, &length);
		if (answered == 0)
			return true;

		refusal = ": ";
		if (length != 0 && length <= sizeof buffer)
			refusal.append(buffer, length);
		else
			refusal += "refused this type";
		err.at = 0;
		err.length = 0;
		err.what = refusal;
		return false;
	}

	int render(const state& facts, const abi_params& params, sink& out) const override {
		if (fn == nullptr)
			return code(element_status::omitted);

		lesh_prompt_context context;
		context.facts = &facts;
		context.out = &out;
		context.arg = params.type.view();
		context.call_token = 1;

		const std::int32_t answered = fn(&context, userdata);

		// The handle dies with the call, so a module that kept it finds it dead.
		context.call_token = 0;
		context.facts = nullptr;
		context.out = nullptr;

		// A negative status is an ABI error and reads as `omitted`; so does anything
		// outside the four constants. See abi.h - there is nowhere for an error out
		// of a render to go that is not the prompt itself.
		return code(status_of(static_cast<int>(answered)));
	}
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

// The seven built-ins go in through `register_module` and by no other route -
// A-11's no-side-door rule, one layer up from the ABI. A user module replacing
// `path` replaces the one a template resolves, because there is only ever one
// table and one lookup.
//
// SEVEN AND NOT EIGHT (#163): `git` is leshnici's, not a built-in, and arrives
// through this same door when the wiring site calls `install_prompt_modules`.
// An engine built by anybody else does not have it, and a template naming it is
// refused as an unknown module rather than special-cased anywhere.
engine::engine() {
	for (const builtin_module& one : kBuiltinModules)
		register_module(one.name, one.which);

	use_default(surface_id::left);
	use_default(surface_id::continuation);

	// AND UNSET AGAIN, last thing. The two calls above are the engine arriving
	// with something to render - `render_full` on a fresh engine has to answer
	// bytes - and not a user asking for the native prompt. `configured()` is the
	// wiring site's question about INTENT (see its comment), so the constructor
	// must not answer it on the user's behalf; the same `use_default` typed by a
	// user does.
	_configured = false;
}

engine::~engine() = default;

engine::surface& engine::at(surface_id which) {
	const std::size_t index = static_cast<std::size_t>(which);
	LESH_ASSERT(index < _surfaces.size());
	return _surfaces[index];
}

const engine::surface& engine::at(surface_id which) const {
	const std::size_t index = static_cast<std::size_t>(which);
	LESH_ASSERT(index < _surfaces.size());
	return _surfaces[index];
}

// ---------------------------------------------------------------------------
// The module registry
// ---------------------------------------------------------------------------

std::int32_t engine::register_module(std::string_view name, const module* which) {
	if (which == nullptr || !is_snake_case(name))
		return LESH_ERR_INVAL;

	// REPLACES (#101). An rc file re-sourced twice leaves one registration, and
	// the placements already made keep pointing at the module they resolved to -
	// which is the old one until the surface is configured again. That is the same
	// rule the action registry has: registration is not re-binding.
	_modules[std::string{name}] = which;
	return LESH_OK;
}

std::int32_t engine::register_abi_module(std::string_view name, lesh_prompt_module_fn fn,
                                         lesh_prompt_validate_fn validate, void* userdata) {
	if (fn == nullptr || !is_snake_case(name))
		return LESH_ERR_INVAL;

	auto owned = std::make_unique<abi_module>();
	owned->named.assign(name);
	owned->fn = fn;
	owned->validate = validate;
	owned->userdata = userdata;
	abi_module* borrowed = owned.get();

	// Owned BY NAME, so a re-registration replaces the object rather than stacking
	// another one behind it - the same idempotence #101 asks of the registration
	// itself, applied to the storage the registration needs.
	_abi_modules[std::string{name}] = std::move(owned);
	return register_module(name, borrowed);
}

bool engine::module_exists(std::string_view name) const {
	return _modules.find(name) != _modules.end();
}

void engine::module_names(std::vector<std::string>& out) const {
	out.clear();
	out.reserve(_modules.size());
	for (const auto& [name, which] : _modules) {
		(void)which;
		out.push_back(name);
	}
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

// THE FLAG IS SET BY THE VERBS AND BY NOTHING ELSE, and here rather than in six
// places. A verb that changed nothing configured nothing, so a `place` for a name
// nobody registered leaves `$PS1` in charge - which is why every failure returns
// before reaching this.
void engine::touched(surface& target) {
	_configured = true;
	// An assembled surface was set from no template, which is the truth and not a
	// gap: see `template_text`.
	target.text.clear();
}

// The scratch, the slots and the top-level index, brought back into step with the
// program. Called after every verb and after every swap - it is O(steps) and runs
// at CONFIGURATION time only, which is what buys the render path a scratch buffer
// it can find by index without asking anybody for one.
void engine::refit(surface& target) {
	// A GROUP THAT IS STILL OPEN SPANS EVERYTHING PLACED SO FAR. `close_group`
	// writes the final number, but the top-level walk has to be correct in
	// between, or a render taken mid-assembly would count a group's children as
	// top-level items and give them slots of their own.
	if (target.open != kNoGroup && target.open < target.program.size())
		target.program[target.open].span =
			static_cast<std::uint32_t>(target.program.size() - target.open - 1);

	if (target.scratch.size() < target.program.size())
		target.scratch.resize(target.program.size());

	target.tops.clear();
	const std::span<const step> steps{target.program};
	for (std::size_t at = 0; at < steps.size(); at += step_extent(steps, at))
		target.tops.push_back(at);

	if (target.slots.size() < target.tops.size())
		target.slots.resize(target.tops.size());
}

bytes engine::intern(surface& target, std::string_view text) {
	const bytes span{static_cast<std::uint32_t>(target.arena.size()),
	                 static_cast<std::uint32_t>(text.size())};
	target.arena.append(text);
	return span;
}

step& engine::append_step(surface& target) {
	target.program.emplace_back();
	return target.program.back();
}

void engine::clear(surface_id which) {
	surface& target = at(which);
	target.program.clear();
	target.arena.clear();
	target.tops.clear();
	target.bytes.clear();
	target.open = kNoGroup;

	// The slots and the scratch are NOT freed, only orphaned: `render_surface`
	// writes every slot it reads and nothing reads past `tops.size()`, so keeping
	// them keeps their strings' capacity across a reconfiguration.
	for (slot& one : target.slots) {
		one.status = element_status::omitted;
		one.wake_at = 0;
	}

	touched(target);
}

namespace {

// The compiled default, into a surface's two containers. A copy and not a walk:
// the steps mean the same thing on both sides, and the offsets in them index the
// same bytes once those bytes are copied in the same order.
template <std::size_t Steps, std::size_t Bytes>
void copy_compiled(std::vector<step>& program, std::string& arena,
                   const compiled_template<Steps, Bytes>& from) {
	program.assign(from.steps.begin(), from.steps.end());
	arena.assign(from.arena.data(), Bytes);
}

} // namespace

void engine::use_default(surface_id which) {
	clear(which);
	surface& target = at(which);

	if (which == surface_id::continuation)
		copy_compiled(target.program, target.arena, kDefaultContinuation);
	else
		copy_compiled(target.program, target.arena, kDefaultLeft);

	refit(target);

	// AND THE DEFAULT'S OWN SPELLING, so `prompt` prints the prompt the shell is
	// showing rather than the empty line an untemplated surface would answer. The
	// two are the same prompt and the suite says so - see
	// `TheDefaultTableAndItsTemplateAgree`, which renders both against two states
	// and compares bytes.
	target.text.assign(which == surface_id::continuation ? kDefaultContinuationTemplate
	                                                     : kDefaultLeftTemplate);
}

place_result engine::place(surface_id which, std::string_view name, std::string_view style_spec,
                           std::string_view type, std::string_view prefix,
                           std::string_view postfix) {
	// EVERY REFUSAL HAPPENS BEFORE ANYTHING IS PLACED, which is the same atomicity
	// `set_template` promises, scaled down to one call: a `place` that answered a
	// failure changed nothing at all, so `configured()` does not move either.
	const module* core = nullptr;
	if (!name.empty()) {
		const auto found = _modules.find(name);
		if (found == _modules.end())
			return place_result::unknown_module;
		core = found->second;
	}

	style pen{};
	if (!style_spec.empty()) {
		const style_parse parsed = parse_style(style_spec);
		if (!parsed.ok)
			return place_result::bad_style;
		pen = parsed.value;
	}

	params_blob params;
	if (core != nullptr) {
		parse_error why;
		if (!core->parse(type, params, why))
			return place_result::bad_type;
	} else if (!type.empty()) {
		// A LITERAL HAS NO TYPE SLOT, the same refusal `{literal::x}` gets. Two
		// spellings of one configuration answer alike, or they are two languages.
		return place_result::bad_type;
	}

	surface& target = at(which);
	step& one = append_step(target);
	one.place.core = core;
	one.place.params = params;
	one.place.pen = pen;
	one.place.prefix = intern(target, prefix);
	one.place.postfix = intern(target, postfix);

	touched(target);
	refit(target);
	return place_result::ok;
}

bool engine::add_module(surface_id which, std::string_view name, std::string_view type) {
	return place(which, name, std::string_view{}, type, std::string_view{}, std::string_view{})
	       == place_result::ok;
}

void engine::add_literal(surface_id which, std::string_view text) {
	(void)place(which, std::string_view{}, std::string_view{}, std::string_view{}, text,
	            std::string_view{});
}

bool engine::open_group(surface_id which) {
	surface& target = at(which);
	if (target.open != kNoGroup)
		return false;

	target.open = target.program.size();
	append_step(target).group = true;

	touched(target);
	refit(target);
	return true;
}

bool engine::close_group(surface_id which) {
	surface& target = at(which);
	if (target.open == kNoGroup)
		return false;

	target.program[target.open].span =
		static_cast<std::uint32_t>(target.program.size() - target.open - 1);
	target.open = kNoGroup;

	touched(target);
	refit(target);
	return true;
}

// ---------------------------------------------------------------------------
// The template language, built
// ---------------------------------------------------------------------------

// `scan_template`'s building policy: the same walk the `constexpr` validator runs
// and the same walk `compile<>()` runs, with a vector and a string underneath
// instead of two arrays.
//
// IT BUILDS INTO ITS OWN STORAGE AND HANDS OVER ONLY AT THE END. That is the
// whole of the atomicity promise - there is no partial application to undo,
// because nothing was applied. The engine is borrowed only to resolve names.
struct engine::builder {
	engine* owner = nullptr;
	std::vector<step> program;
	std::string arena;
	std::vector<std::size_t> open;   // the group stack; the scanner guarantees it balances

	// KEYED ON WHAT THE NAME RESOLVES TO, and then the module decides. A user who
	// registered their own `path` owns its type grammar as surely as they own its
	// bytes, which is the only answer that does not make the engine the arbiter of
	// a vocabulary it did not define.
	[[nodiscard]] const module* resolve(std::string_view name) const {
		const auto found = owner->_modules.find(name);
		return found == owner->_modules.end() ? nullptr : found->second;
	}

	[[nodiscard]] bytes intern(std::string_view text) {
		const bytes span{static_cast<std::uint32_t>(arena.size()),
		                 static_cast<std::uint32_t>(text.size())};
		arena.append(text);
		return span;
	}

	void on_placement(const module* core, const params_blob& params, const style& pen,
	                  std::string_view prefix, std::string_view postfix) {
		program.emplace_back();
		step& one = program.back();
		one.place.core = core;
		one.place.params = params;
		one.place.pen = pen;
		one.place.prefix = intern(prefix);
		one.place.postfix = intern(postfix);
	}

	void on_open_group() {
		open.push_back(program.size());
		program.emplace_back();
		program.back().group = true;
	}

	void on_close_group() {
		const std::size_t at = open.back();
		open.pop_back();
		program[at].span = static_cast<std::uint32_t>(program.size() - at - 1);
	}
};

void engine::adopt(surface& target, std::vector<step>&& program, std::string&& arena) {
	target.program = std::move(program);
	target.arena = std::move(arena);
	target.open = kNoGroup;
	target.bytes.clear();
	for (slot& one : target.slots) {
		one.status = element_status::omitted;
		one.wake_at = 0;
	}
	refit(target);
}

bool engine::set_template(surface_id which, std::string_view text, std::string& error_out) {
	builder build;
	build.owner = this;
	// A structural count, not an exact one: every step is a placement or a group,
	// so the braces and parens bound it from above with one to spare for a trailing
	// literal run.
	std::size_t structural = 1;
	for (const char one : text)
		if (one == '{' || one == '(')
			++structural;
	build.program.reserve(structural);
	build.arena.reserve(text.size());

	const template_check checked = scan_template(text, build);
	if (!checked.ok) {
		error_out = describe_template_error(checked);
		// AND NOTHING ELSE HAPPENS. Not the swap, not the slots, not `_configured` -
		// a verb that changed nothing configured nothing, and the prompt that was
		// standing is still standing.
		return false;
	}

	surface& target = at(which);
	adopt(target, std::move(build.program), std::move(build.arena));
	target.text.assign(text);
	_configured = true;
	return true;
}

std::string_view engine::template_text(surface_id which) const {
	return at(which).text;
}

// THE ARRAY VERB'S FRONT DOOR onto `set_template`'s builder (#157, owner's
// ruling: "one builder, two front doors"). Where `scan_template` walks bytes
// and calls `build.resolve`/`on_placement`/`on_open_group`/`on_close_group` as
// it finds `{…}` and `(…)`, this walks a `lesh_prompt_placement` tree and calls
// the SAME four hooks on the SAME builder - so a tree that says what a template
// says builds the identical program, and the module/style/type/`literal` rules
// are one rule read twice rather than two rules that happen to agree today.
//
// RECURSIVE, DEPTH FIRST, AND IT STOPS AT THE FIRST FAILURE - exactly
// `scan_template`'s own promise: nothing built past the failing item survives,
// because the caller (`engine::set_placements`) never adopts `build`'s storage
// unless this returns `none`.
engine::placements_error engine::build_placements(builder& build,
                                                   const lesh_prompt_placement* items,
                                                   std::size_t count) {
	for (std::size_t i = 0; i < count; ++i) {
		const lesh_prompt_placement& item = items[i];
		const bool has_children = item.children != nullptr && item.child_count != 0;
		// NULL OR "" ANSWER ALIKE, the convention every optional string field in
		// this ABI already keeps - `module` is the one field on this struct that
		// is not optional, but "unset" is still one spelling, not two.
		const bool has_module = item.module != nullptr && item.module[0] != '\0';

		if (!has_module) {
			// NO MODULE, NO CHILDREN: neither a placement nor a group.
			if (!has_children)
				return placements_error::invalid;
			build.on_open_group();
			const placements_error nested = build_placements(build, item.children, item.child_count);
			if (nested != placements_error::none)
				return nested;
			build.on_close_group();
			continue;
		}
		// BOTH SET: a caller that meant one and wrote both.
		if (item.children != nullptr)
			return placements_error::invalid;

		const std::string_view name{item.module};
		const bool literal_keyword = name == kLiteralPlacement;
		const module* core = nullptr;
		if (!literal_keyword) {
			core = build.resolve(name);
			if (core == nullptr)
				return placements_error::unknown_module;
		}

		style pen{};
		const std::string_view style_spec = text_of(item.options.style);
		if (!style_spec.empty()) {
			const style_parse parsed = parse_style(style_spec);
			if (!parsed.ok)
				return placements_error::refused;
			pen = parsed.value;
		}

		const std::string_view type = text_of(item.options.type);
		params_blob params;
		if (core != nullptr) {
			parse_error why;
			if (!core->parse(type, params, why))
				return placements_error::refused;
		} else if (!type.empty()) {
			// `literal` HAS NO TYPE SLOT, the same refusal `{literal::x}` gets.
			return placements_error::refused;
		}

		const std::string_view prefix = text_of(item.options.prefix);
		const std::string_view postfix = text_of(item.options.postfix);
		if (literal_keyword && prefix.empty() && postfix.empty())
			// `literal` NEEDS BYTES TO PAINT, the same refusal `{literal}` gets.
			return placements_error::refused;

		build.on_placement(core, params, pen, prefix, postfix);
	}
	return placements_error::none;
}

std::int32_t engine::set_placements(surface_id which, const lesh_prompt_placement* items,
                                    std::size_t count) {
	builder build;
	build.owner = this;
	build.program.reserve(count);

	const placements_error failed = build_placements(build, items, count);
	switch (failed) {
		case placements_error::none:           break;
		case placements_error::invalid:        return LESH_ERR_INVAL;
		case placements_error::unknown_module: return LESH_ERR_NOTFOUND;
		case placements_error::refused:        return 1;
	}

	surface& target = at(which);
	adopt(target, std::move(build.program), std::move(build.arena));
	// AN ARRAY-BUILT SURFACE HAS NO TEMPLATE STRING, the same rule `place` and
	// `add_literal`/`add_module` always followed - see `template_text`.
	target.text.clear();
	_configured = true;
	return LESH_OK;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// §6.10's per-prompt (module, params) memo, as the composer's core runner.
//
// `{env::USER}@{env::HOST}` is two entries and computes twice; the same module
// placed twice with the same params computes once, which is what makes free
// placement free. The key is A POINTER AND A MEMCMP - the module is a singleton
// and the params are bytes - where the old design compared a function pointer, a
// userdata pointer and a `std::string` argument.
element_status engine::memoizing_core::operator()(const placement& one, const state& facts,
                                                  sink& into) {
	engine& me = *owner;

	for (std::size_t i = 0; i < me._memo_used; ++i) {
		const memo_entry& seen = me._memo[i];
		if (seen.which != one.core || !(seen.params == one.params))
			continue;
		if (seen.status != element_status::omitted)
			into.append(seen.bytes);
		if (seen.wake != 0)
			into.wake_in(seen.wake);
		return seen.status;
	}

	const element_status answered = status_of(one.core->render(facts, one.params, into));

	if (me._memo_used == me._memo.size())
		me._memo.emplace_back();
	memo_entry& remembered = me._memo[me._memo_used++];
	remembered.which = one.core;
	remembered.params = one.params;
	remembered.bytes.assign(into.bytes());
	remembered.status = answered;
	remembered.wake = into.wake();
	return answered;
}

void engine::rebuild(surface& target) {
	target.bytes.clear();
	for (std::size_t i = 0; i < target.tops.size(); ++i)
		target.bytes.append(target.slots[i].bytes);
}

void engine::render_surface(surface& target, const state& facts) {
	if (target.slots.size() < target.tops.size())
		target.slots.resize(target.tops.size());

	memoizing_core memo{this};
	const program_view program = target.view();
	const std::span<step_scratch> scratch{target.scratch};

	for (std::size_t i = 0; i < target.tops.size(); ++i) {
		slot& into = target.slots[i];
		_top.reset();
		into.status = render_item(program, target.tops[i], scratch, facts, _top, memo);
		if (into.status == element_status::omitted)
			into.bytes.clear();
		else
			into.bytes.assign(_top.bytes());
		// Relative in, absolute out. A request only means anything at the instant it
		// was made, so this is the one place it is turned into a deadline.
		into.wake_at = _top.wake() != 0 ? facts.tick + _top.wake() : 0;
	}

	rebuild(target);
}

void engine::render_full(const state& facts) {
	_memo_used = 0;
	for (surface& target : _surfaces)
		render_surface(target, facts);
}

bool engine::render_tick(const state& facts) {
	_memo_used = 0;
	bool anything_moved = false;

	memoizing_core memo{this};
	for (surface& target : _surfaces) {
		bool moved = false;
		const program_view program = target.view();
		const std::span<step_scratch> scratch{target.scratch};
		const std::size_t count = std::min(target.tops.size(), target.slots.size());

		for (std::size_t i = 0; i < count; ++i) {
			slot& one = target.slots[i];

			// THE GATE, and it is the whole of recalculation-by-cause: an item with
			// no deadline is not re-invoked on a tick, ever. Its bytes are spliced
			// from the slot untouched - which is why `git` costs nothing while a
			// clock ticks beside it.
			if (one.wake_at == 0 || one.wake_at > facts.tick)
				continue;

			_top.reset();
			const element_status answered =
				render_item(program, target.tops[i], scratch, facts, _top, memo);
			const std::string_view produced =
				answered == element_status::omitted ? std::string_view{} : _top.bytes();

			if (answered != one.status || produced != std::string_view{one.bytes}) {
				one.status = answered;
				one.bytes.assign(produced);
				moved = true;
			}
			one.wake_at = _top.wake() != 0 ? facts.tick + _top.wake() : 0;
		}

		// Rebuilt only when something actually changed. An unchanged prompt produces
		// no terminal write at all (§6.10), and the cheapest way to guarantee that
		// is to answer honestly here.
		if (moved) {
			rebuild(target);
			anything_moved = true;
		}
	}

	return anything_moved;
}

std::string_view engine::output(surface_id which) const {
	return at(which).bytes;
}

std::uint64_t engine::next_wake() const {
	std::uint64_t earliest = 0;
	for (const surface& target : _surfaces) {
		const std::size_t count = std::min(target.tops.size(), target.slots.size());
		for (std::size_t i = 0; i < count; ++i) {
			const std::uint64_t when = target.slots[i].wake_at;
			if (when != 0 && (earliest == 0 || when < earliest))
				earliest = when;
		}
	}
	return earliest;
}

} // namespace lesh::ui::prompt

// ---------------------------------------------------------------------------
// The C ABI
// ---------------------------------------------------------------------------

extern "C" {

std::int32_t lesh_prompt_module_register(lesh_registry* registry, const char* name,
                                         lesh_prompt_module_fn fn, void* userdata) {
	// THE PLAIN FORM IS THE OPTIONS FORM WITH A ZERO-INIT OPTIONS VALUE, which is
	// what makes the addition additive rather than a second path that could
	// answer differently.
	return lesh_prompt_module_register_with(registry, name, fn, userdata,
	                                        lesh_prompt_module_options{});
}

std::int32_t lesh_prompt_module_register_with(lesh_registry* registry, const char* name,
                                              lesh_prompt_module_fn fn, void* userdata,
                                              lesh_prompt_module_options options) {
	if (registry == nullptr)
		return LESH_ERR_INVAL;
	engine* which = engine_of(registry);
	if (which == nullptr)
		return LESH_ERR_NOTFOUND;
	if (name == nullptr || fn == nullptr)
		return LESH_ERR_INVAL;
	return which->register_abi_module(std::string_view{name}, fn, options.validate, userdata);
}

std::int32_t lesh_prompt_module_exists(lesh_registry* registry, const char* name,
                                       std::int32_t* out) {
	if (registry == nullptr)
		return LESH_ERR_INVAL;
	engine* which = engine_of(registry);
	if (which == nullptr)
		return LESH_ERR_NOTFOUND;
	if (name == nullptr || out == nullptr)
		return LESH_ERR_INVAL;
	*out = which->module_exists(std::string_view{name}) ? 1 : 0;
	return LESH_OK;
}

std::int32_t lesh_prompt_write(lesh_prompt_context* context, const char* bytes,
                               std::size_t length) {
	LESH_PROMPT_HANDLE(context);
	if (bytes == nullptr && length != 0)
		return LESH_ERR_INVAL;
	if (length != 0)
		context->out->append(std::string_view{bytes, length});
	return LESH_OK;
}

std::int32_t lesh_prompt_arg(const lesh_prompt_context* context, char* out,
                             std::size_t capacity, std::size_t* length_out) {
	LESH_PROMPT_HANDLE(context);
	return copy_out(context->arg, out, capacity, length_out);
}

std::int32_t lesh_prompt_tick(const lesh_prompt_context* context, std::uint64_t* out) {
	LESH_PROMPT_HANDLE(context);
	if (out == nullptr)
		return LESH_ERR_INVAL;
	*out = context->facts->tick;
	return LESH_OK;
}

std::int32_t lesh_prompt_wake_in(lesh_prompt_context* context, std::uint64_t ticks) {
	LESH_PROMPT_HANDLE(context);
	context->out->wake_in(ticks);
	return LESH_OK;
}

std::int32_t lesh_prompt_variable(const lesh_prompt_context* context, const char* name,
                                  char* out, std::size_t capacity, std::size_t* length_out) {
	LESH_PROMPT_HANDLE(context);
	if (name == nullptr)
		return LESH_ERR_INVAL;

	const lesh::ui::prompt::state& facts = *context->facts;
	std::string_view value;
	if (facts.getvar == nullptr || !facts.getvar(facts.getvar_ctx, std::string_view{name}, value))
		return LESH_ERR_NOTFOUND;
	return copy_out(value, out, capacity, length_out);
}

std::int32_t lesh_prompt_last_status(const lesh_prompt_context* context, std::int32_t* out) {
	LESH_PROMPT_HANDLE(context);
	if (out == nullptr)
		return LESH_ERR_INVAL;
	*out = static_cast<std::int32_t>(context->facts->status);
	return LESH_OK;
}

std::int32_t lesh_prompt_clear(lesh_registry* registry, std::uint32_t surface) {
	if (registry == nullptr)
		return LESH_ERR_INVAL;
	engine* which = engine_of(registry);
	if (which == nullptr)
		return LESH_ERR_NOTFOUND;
	surface_id target = surface_id::left;
	if (!surface_of(surface, target))
		return LESH_ERR_INVAL;
	which->clear(target);
	return LESH_OK;
}

std::int32_t lesh_prompt_use_default(lesh_registry* registry, std::uint32_t surface) {
	if (registry == nullptr)
		return LESH_ERR_INVAL;
	engine* which = engine_of(registry);
	if (which == nullptr)
		return LESH_ERR_NOTFOUND;
	surface_id target = surface_id::left;
	if (!surface_of(surface, target))
		return LESH_ERR_INVAL;
	which->use_default(target);
	return LESH_OK;
}

std::int32_t lesh_prompt_set_placements(lesh_registry* registry, std::uint32_t surface,
                                        const lesh_prompt_placement* items, std::size_t count) {
	if (registry == nullptr)
		return LESH_ERR_INVAL;
	engine* which = engine_of(registry);
	if (which == nullptr)
		return LESH_ERR_NOTFOUND;
	surface_id target = surface_id::left;
	if (!surface_of(surface, target))
		return LESH_ERR_INVAL;

	// `count == 0` IS THE EMPTY SURFACE, `items` unread - the same rule
	// `lesh_prompt_clear` gives directly, and cheaper: nothing to validate.
	if (count == 0) {
		which->clear(target);
		return LESH_OK;
	}
	if (items == nullptr)
		return LESH_ERR_INVAL;

	return which->set_placements(target, items, count);
}

std::int32_t lesh_prompt_set(lesh_registry* registry, std::uint32_t surface, const char* text,
                             std::size_t length, char* error_out, std::size_t error_capacity,
                             std::size_t* error_length_out) {
	if (registry == nullptr)
		return LESH_ERR_INVAL;
	engine* which = engine_of(registry);
	if (which == nullptr)
		return LESH_ERR_NOTFOUND;
	surface_id target = surface_id::left;
	if (!surface_of(surface, target))
		return LESH_ERR_INVAL;
	if ((text == nullptr && length != 0) || error_length_out == nullptr)
		return LESH_ERR_INVAL;

	std::string message;
	const std::string_view source =
		length == 0 ? std::string_view{} : std::string_view{text, length};
	if (which->set_template(target, source, message)) {
		// The length is always written, and on success it is zero - so a caller that
		// asked with a null buffer to size the message first learns from the return
		// value that there is none.
		*error_length_out = 0;
		return LESH_OK;
	}

	// REFUSED, and the message travels under `lesh_buffer_get`'s convention: the
	// length is reported whether or not it fit, a short buffer is TOOSMALL rather
	// than a truncation, and NULL with zero capacity asks the length. The domain
	// status 1 is what a caller sees once the message has actually been handed over
	// - TOOSMALL can only happen on this path, so the two together are "refused,
	// ask again with room".
	const std::int32_t copied = copy_out(message, error_out, error_capacity, error_length_out);
	return copied == LESH_OK ? 1 : copied;
}

std::int32_t lesh_prompt_text(lesh_registry* registry, std::uint32_t surface, char* out,
                              std::size_t capacity, std::size_t* length_out) {
	if (registry == nullptr)
		return LESH_ERR_INVAL;
	engine* which = engine_of(registry);
	if (which == nullptr)
		return LESH_ERR_NOTFOUND;
	surface_id target = surface_id::left;
	if (!surface_of(surface, target))
		return LESH_ERR_INVAL;
	return copy_out(which->template_text(target), out, capacity, length_out);
}

} // extern "C"
