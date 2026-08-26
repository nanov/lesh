// The prompt engine's runtime half (#157, spec §6.10): the module registry, the
// configuration verbs, the output slots, the tick wheel, and the C ABI.
//
// The COMPOSER is in prompt.h and is pure - it is the same code the compile-time
// default table renders through. What is here is everything that has to remember
// something between renders, which is exactly what §6.10's recalculation-by-cause
// needs: a slot per top-level element holding its last bytes and its deadline,
// so a tick that animates one element memcpy's the rest.
//
// THE ABI HALF IS THE SAME SHAPE THE ACTION ABI HAS, deliberately. A module call
// gets an opaque `lesh_prompt_context` valid only for that call; everything
// copies in or copies out; a registered C module reaches the composer through
// one trampoline, which is the entire cost of NG-4's promise that the Lua
// binding reuses these verbs unchanged.

#include "leshper/prompt.h"

#include "leshper/registry.h"
#include "substrate/assert.h"

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
	const lesh::leshper::prompt::state* facts = nullptr;
	lesh::leshper::prompt::sink* out = nullptr;
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

using lesh::leshper::prompt::element_kind;
using lesh::leshper::prompt::element_status;
using lesh::leshper::prompt::engine;
using lesh::leshper::prompt::surface_id;

// The C numbers and the C++ enumerators are one space, and these keep them one.
// A reordered enum would otherwise make every module's `ready` read as
// `omitted`, and compile silently.
static_assert(static_cast<int>(element_status::omitted) == LESH_PROMPT_OMITTED);
static_assert(static_cast<int>(element_status::ready) == LESH_PROMPT_READY);
static_assert(static_cast<int>(element_status::pending) == LESH_PROMPT_PENDING);
static_assert(static_cast<int>(element_status::neutral) == LESH_PROMPT_NEUTRAL);
static_assert(static_cast<std::uint32_t>(surface_id::left) == LESH_PROMPT_LEFT);
static_assert(static_cast<std::uint32_t>(surface_id::continuation) == LESH_PROMPT_CONTINUATION);

// snake_case, and it is registry.cpp's rule restated rather than shared: that
// one is in an anonymous namespace, TU-private on purpose, and exporting it to
// get one copy would widen a surface to save six lines. The rule itself must not
// drift - a module named `git-branch` and one named `git_branch` looking like
// one name is the failure both spellings exist to design out.
bool is_snake_case(std::string_view name) noexcept {
	if (name.empty() || name[0] < 'a' || name[0] > 'z')
		return false;
	for (const char one : name) {
		const bool ok = (one >= 'a' && one <= 'z') || (one >= '0' && one <= '9') || one == '_';
		if (!ok)
			return false;
	}
	return true;
}

// `lesh_buffer_get`'s convention, to the letter: the length is always reported,
// a short buffer is LESH_ERR_TOOSMALL rather than a truncation, and a null `out`
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
std::string describe_template_error(const lesh::leshper::prompt::template_check& why) {
	using lesh::leshper::prompt::template_error;

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
		case template_error::needs_argument:
			// v1's only `needs_argument` built-in is `env`, so this can afford to
			// say what the argument IS rather than that there must be one. A second
			// module of this rule would arrive with its own noun and this line would
			// have to grow a table; one module does not need one.
			said = what + " needs a variable name";
			break;
		case template_error::takes_no_argument:
			said = what + " takes no argument";
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

engine* engine_of(lesh_registry* registry) noexcept {
	return registry == nullptr ? nullptr : registry->prompt_engine;
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

} // namespace

namespace lesh::leshper::prompt {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

// The eight built-ins go in through `register_module` and by no other route -
// A-11's no-side-door rule, one layer up from the ABI. A user module replacing
// `git` replaces the one the default table places, because there is only ever
// one table and one lookup.
engine::engine() {
	// Deep enough for v1's one level of grouping, with room to spare, so that
	// `scratch_at` never resizes on the render path. It CAN resize - a later
	// nesting rule would need it to - and the only frame that holds a reference
	// across a call is the module path, which does not recurse.
	_scratch.resize(4);

	register_module("path", &module_path, nullptr);
	register_module("status", &module_status, nullptr);
	register_module("jobs", &module_jobs, nullptr);
	register_module("mode", &module_mode, nullptr);
	register_module("time", &module_time, nullptr);
	register_module("duration", &module_duration, nullptr);
	register_module("env", &module_env, nullptr);
	register_module("git", &module_git, nullptr);

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

std::int32_t engine::register_module(std::string_view name, element_fn fn, void* userdata) {
	if (fn == nullptr || !is_snake_case(name))
		return LESH_ERR_INVAL;

	// REPLACES (#101). An rc file re-sourced twice leaves one registration, and
	// the placements already made keep pointing at the node's stored function -
	// which is the old one until the surface is configured again. That is the
	// same rule the action registry has: registration is not re-binding.
	module_entry& entry = _modules[std::string{name}];
	entry.fn = fn;
	entry.userdata = userdata;
	return LESH_OK;
}

int engine::abi_trampoline(const state& facts, sink& out, const void* data) {
	const abi_module* which = static_cast<const abi_module*>(userdata_of(data));
	if (which == nullptr || which->fn == nullptr)
		return code(element_status::omitted);

	lesh_prompt_context context;
	context.facts = &facts;
	context.out = &out;
	context.arg = arg_of(data);
	context.call_token = 1;

	const std::int32_t answered = which->fn(&context, which->userdata);

	// The handle dies with the call, so a module that kept it finds it dead.
	context.call_token = 0;
	context.facts = nullptr;
	context.out = nullptr;

	// A negative status is an ABI error and reads as `omitted`; so does anything
	// outside the four constants. See abi.h - there is nowhere for an error out
	// of a render to go that is not the prompt itself.
	return code(status_of(static_cast<int>(answered)));
}

std::int32_t engine::register_abi_module(std::string_view name, lesh_prompt_module_fn fn,
                                         void* userdata) {
	if (fn == nullptr || !is_snake_case(name))
		return LESH_ERR_INVAL;

	auto owned = std::make_unique<abi_module>();
	owned->fn = fn;
	owned->userdata = userdata;
	abi_module* borrowed = owned.get();

	// Owned BY NAME, so a re-registration replaces the pair rather than stacking
	// another one behind it - the same idempotence #101 asks of the registration
	// itself, applied to the storage the registration needs.
	_abi_modules[std::string{name}] = std::move(owned);
	return register_module(name, &engine::abi_trampoline, borrowed);
}

bool engine::module_exists(std::string_view name) const {
	return _modules.find(name) != _modules.end();
}

void engine::module_names(std::vector<std::string>& out) const {
	out.clear();
	out.reserve(_modules.size());
	for (const auto& [name, entry] : _modules)
		out.push_back(name);
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void engine::clear(surface_id which) {
	// THE FLAG IS SET BY THE VERBS AND BY NOTHING ELSE, and here rather than in
	// six places because `use_default` goes through this one. The other four set
	// it where they succeed - a verb that changed nothing configured nothing, so
	// an `add_module` for a name nobody registered leaves `$PS1` in charge.
	_configured = true;

	surface& target = at(which);
	target.nodes.clear();
	target.bytes.clear();
	target.open = nullptr;

	// The slots are NOT cleared, only orphaned: `render_surface` writes every
	// slot it reads and nothing reads past `nodes.size()`, so keeping them keeps
	// their strings' capacity across a reconfiguration.
	for (slot& one : target.slots) {
		one.status = element_status::omitted;
		one.wake_at = 0;
	}

	// A cleared surface was set from no template, which is the truth and not a
	// gap: see `template_text`. Every assembly verb below empties it for the same
	// reason.
	target.text.clear();
}

void engine::use_default(surface_id which) {
	clear(which);
	surface& target = at(which);

	const std::span<const element> from = which == surface_id::continuation
		? std::span<const element>{kDefaultContinuation}
		: std::span<const element>{kDefaultLeft};

	// A default element is COPIED IN AS AN OPAQUE ONE, `seg` included: the
	// compile-time group answers for itself, two phases and style reset and all,
	// and the engine's own group machinery is for the ones the ABI builds. One
	// walk, two provenances, no second composer.
	for (const element& one : from) {
		auto made = std::make_unique<node>();
		made->fn = one.fn;
		made->data = one.data;
		made->kind = one.kind;
		target.nodes.push_back(std::move(made));
	}

	// AND THE TABLE'S OWN SPELLING, so `prompt` prints the prompt the shell is
	// showing rather than the empty line an untemplated surface would answer. The
	// two are the same prompt and the suite says so - see
	// `TheDefaultTableAndItsTemplateAgree`, which renders both against two states
	// and compares bytes.
	target.text.assign(which == surface_id::continuation ? kDefaultContinuationTemplate
	                                                     : kDefaultLeftTemplate);
}

engine::node& engine::place(surface& into, std::unique_ptr<node> made) {
	node& placed = *made;
	if (into.open != nullptr)
		into.open->children.push_back(std::move(made));
	else
		into.nodes.push_back(std::move(made));
	return placed;
}

bool engine::add_module(surface_id which, std::string_view name, std::string_view arg) {
	const auto found = _modules.find(name);
	if (found == _modules.end())
		return false;

	auto made = std::make_unique<node>();
	made->fn = found->second.fn;
	made->kind = element_kind::module;
	made->arg.assign(arg);
	made->bound.arg = made->arg;   // into the node's own bytes, which never move
	made->bound.userdata = found->second.userdata;
	made->data = &made->bound;

	surface& target = at(which);
	place(target, std::move(made));
	target.text.clear();
	_configured = true;
	return true;
}

void engine::add_literal(surface_id which, std::string_view bytes) {
	auto made = std::make_unique<node>();
	made->fn = &decoration_literal;
	made->kind = element_kind::decoration;
	made->arg.assign(bytes);
	made->bound.arg = made->arg;
	made->data = &made->bound;

	surface& target = at(which);
	place(target, std::move(made));
	target.text.clear();
	_configured = true;
}

bool engine::add_style(surface_id which, std::string_view spec) {
	const style_parse parsed = parse_style(spec);
	if (!parsed.ok)
		return false;

	auto made = std::make_unique<node>();
	made->fn = &decoration_style;
	made->kind = element_kind::decoration;
	made->styles = true;   // what makes an enclosing group owe a reset
	made->pen = parsed.value;
	made->data = &made->pen;

	surface& target = at(which);
	place(target, std::move(made));
	target.text.clear();
	_configured = true;
	return true;
}

bool engine::open_group(surface_id which) {
	surface& target = at(which);
	if (target.open != nullptr)
		return false;

	auto made = std::make_unique<node>();
	made->fn = nullptr;   // null IFF a runtime group; the engine drives its phases
	made->kind = element_kind::group;

	// Top level only: `place` would have put it inside the open group, and there
	// is no open group here by the check above.
	target.open = &place(target, std::move(made));
	target.text.clear();
	_configured = true;
	return true;
}

bool engine::close_group(surface_id which) {
	surface& target = at(which);
	if (target.open == nullptr)
		return false;
	target.open = nullptr;
	target.text.clear();
	_configured = true;
	return true;
}

// ---------------------------------------------------------------------------
// The template language, built
// ---------------------------------------------------------------------------

// `scan_template`'s building policy: the same walk the `constexpr` validator
// runs, with node construction hung off its hooks.
//
// IT BUILDS INTO ITS OWN STORAGE AND HANDS OVER ONLY AT THE END. That is the
// whole of the atomicity promise - there is no partial application to undo,
// because nothing was applied. The engine is borrowed only to resolve names.
struct engine::builder {
	engine* owner = nullptr;
	std::vector<std::unique_ptr<node>> nodes;
	std::vector<node*> open;   // the group stack; the scanner guarantees it balances
	std::string scratch;       // the unescape target, reused across slots

	node& place_here(std::unique_ptr<node> made) {
		node& placed = *made;
		if (open.empty())
			nodes.push_back(std::move(made));
		else
			open.back()->children.push_back(std::move(made));
		return placed;
	}

	// KEYED ON THE RESOLVED FUNCTION, NOT ON THE NAME. `{path::short}` is refused
	// because `path` is still the built-in that takes no argument; a user who
	// registered their own `path` gets `free_argument` and owns its argument
	// grammar, which is the only answer that does not make the engine the arbiter
	// of a vocabulary it did not define.
	[[nodiscard]] module_rule resolve(std::string_view name) const {
		const auto found = owner->_modules.find(name);
		if (found == owner->_modules.end())
			return module_rule::unknown;

		const element_fn fn = found->second.fn;
		if (fn == &module_env)
			return module_rule::needs_argument;
		if (fn == &module_path || fn == &module_status || fn == &module_jobs
		    || fn == &module_mode || fn == &module_time || fn == &module_duration
		    || fn == &module_git)
			return module_rule::no_argument;
		return module_rule::free_argument;
	}

	[[nodiscard]] std::unique_ptr<node> make_literal(const template_slice& bytes) {
		scratch.clear();
		unescape_into(bytes, scratch);
		return make_literal_bytes(scratch);
	}

	[[nodiscard]] std::unique_ptr<node> make_literal_bytes(std::string_view bytes) {
		auto made = std::make_unique<node>();
		made->fn = &decoration_literal;
		made->kind = element_kind::decoration;
		made->arg.assign(bytes);
		made->bound.arg = made->arg;
		made->data = &made->bound;
		return made;
	}

	[[nodiscard]] std::unique_ptr<node> make_style(const style& pen) {
		auto made = std::make_unique<node>();
		made->fn = &decoration_style;
		made->kind = element_kind::decoration;
		made->styles = true;
		made->pen = pen;
		made->data = &made->pen;
		return made;
	}

	[[nodiscard]] std::unique_ptr<node> make_module(std::string_view name,
	                                                const template_slice& arg) {
		// `resolve` already said this name is there; the second lookup is what
		// makes `add_module` and this one entry point rather than two.
		const auto found = owner->_modules.find(name);
		auto made = std::make_unique<node>();
		made->fn = found->second.fn;
		made->kind = element_kind::module;
		scratch.clear();
		unescape_into(arg, scratch);
		made->arg.assign(scratch);
		made->bound.arg = made->arg;
		made->bound.userdata = found->second.userdata;
		made->data = &made->bound;
		return made;
	}

	void on_literal(const template_slice& run) { place_here(make_literal(run)); }

	void on_open_group() {
		auto made = std::make_unique<node>();
		made->kind = element_kind::group;   // fn null: the engine drives its phases
		open.push_back(&place_here(std::move(made)));
	}

	void on_close_group() { open.pop_back(); }

	// THE DESUGARING, AND IT IS ONE NODE SHAPE (§6.10, #156). A placement with no
	// style and no affixes is a bare module element - the cheapest thing the
	// engine can hold. A placement with any of them is ONE group over
	// `[style?, prefix, module, postfix?]`: the style spans the affixes and the
	// value, the group's own vote makes all three vanish together when the module
	// says nothing, and the group's styles flag puts the pen back at the end. No
	// new machinery, and no second rule for how a styled segment behaves.
	void on_placement(std::string_view name, const style& pen, bool styled,
	                  const template_slice& type, const template_slice& prefix,
	                  const template_slice& postfix) {
		if (!styled && prefix.empty() && postfix.empty()) {
			place_here(make_module(name, type));
			return;
		}

		auto group = std::make_unique<node>();
		// STAMPED A MODULE, THOUGH IT IS BUILT LIKE A GROUP, and this line is what
		// makes the desugaring TRANSPARENT. `{git}` votes in an enclosing group;
		// `{git:magenta}` is the same placement wearing a colour and has to vote
		// identically, or `( on {git:magenta})` would be a group with no module
		// child of its own, could never win a vote, and would render nothing for
		// ever - which is what adding one word to a working prompt would silently
		// do. The node IS a placement; the group is how a placement carries a style
		// and two affixes, not what it is. "Groups do not vote" is untouched: an
		// explicit `(…)` written by a user still does not vote in its parent.
		group->kind = element_kind::module;
		node& seg = place_here(std::move(group));

		// Built in order and pushed directly: `place_here` would have put them at
		// this builder's current level, which is the group's PARENT.
		if (styled)
			seg.children.push_back(make_style(pen));
		if (!prefix.empty())
			seg.children.push_back(make_literal(prefix));
		seg.children.push_back(make_module(name, type));
		if (!postfix.empty())
			seg.children.push_back(make_literal(postfix));
	}

	// The same shape with no value in the middle - which is exactly what
	// `{literal:blue::hi}` is. The two affixes concatenate with nothing between
	// them, so an unstyled one collapses to a single literal node and is then
	// indistinguishable from the bare bytes it spells.
	void on_literal_placement(const style& pen, bool styled, const template_slice& prefix,
	                          const template_slice& postfix) {
		if (!styled) {
			scratch.clear();
			unescape_into(prefix, scratch);
			unescape_into(postfix, scratch);
			place_here(make_literal_bytes(scratch));
			return;
		}

		// A SPAN: a group that does not vote. A group with no module child could
		// never win a vote, so this one is stamped a DECORATION - it always
		// renders, it answers `neutral`, and inside a parent group it takes no part
		// in the parent's vote (§6.10: decorations do not vote). The style still
		// resets at the span's end, which is what makes the styling local to the
		// bytes it was written for.
		auto span = std::make_unique<node>();
		span->kind = element_kind::decoration;   // fn null: the engine drives it
		node& placed = place_here(std::move(span));
		placed.children.push_back(make_style(pen));
		if (!prefix.empty())
			placed.children.push_back(make_literal(prefix));
		if (!postfix.empty())
			placed.children.push_back(make_literal(postfix));
	}
};

bool engine::set_template(surface_id which, std::string_view text, std::string& error_out) {
	builder build;
	build.owner = this;
	// A structural count, not an exact one: every top-level node is a placement, a
	// group or a literal run, so the braces and parens bound it from above with
	// one to spare for a trailing run.
	std::size_t structural = 1;
	for (const char one : text)
		if (one == '{' || one == '(')
			++structural;
	build.nodes.reserve(structural);

	const template_check checked = scan_template(text, build);
	if (!checked.ok) {
		error_out = describe_template_error(checked);
		// AND NOTHING ELSE HAPPENS. Not the swap, not the slots, not
		// `_configured` - a verb that changed nothing configured nothing, and the
		// prompt that was standing is still standing.
		return false;
	}

	surface& target = at(which);
	target.nodes = std::move(build.nodes);
	target.open = nullptr;
	target.bytes.clear();
	for (slot& one : target.slots) {
		one.status = element_status::omitted;
		one.wake_at = 0;
	}
	target.text.assign(text);
	_configured = true;
	return true;
}

std::string_view engine::template_text(surface_id which) const {
	return at(which).text;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

sink& engine::scratch_at(std::size_t depth) {
	if (_scratch.size() <= depth)
		_scratch.resize(depth + 1);
	return _scratch[depth];
}

element_status engine::invoke(node& one, const state& facts, sink& out, std::size_t depth) {
	if (one.fn == nullptr)
		return render_group(one, facts, out, depth);

	// A decoration is never memoized: it has no shell fact to read, so a second
	// execution costs an append and a memo lookup would cost more.
	if (one.kind != element_kind::module)
		return status_of(one.fn(facts, out, one.data));

	// §6.10's per-prompt `(module, arg)` memo. `{env:USER}@{env:HOST}` is two
	// entries and computes twice; the same module placed twice with the same
	// argument computes once, which is what makes free placement free.
	for (std::size_t i = 0; i < _memo_used; ++i) {
		const memo_entry& seen = _memo[i];
		if (seen.fn != one.fn || seen.userdata != one.bound.userdata
		    || seen.arg != one.bound.arg)
			continue;
		if (seen.status != element_status::omitted)
			out.append(seen.bytes);
		if (seen.wake != 0)
			out.wake_in(seen.wake);
		return seen.status;
	}

	sink& scratch = scratch_at(depth);
	scratch.reset();
	const element_status answered = status_of(one.fn(facts, scratch, one.data));

	if (_memo_used == _memo.size())
		_memo.emplace_back();
	memo_entry& remembered = _memo[_memo_used++];
	remembered.fn = one.fn;
	remembered.userdata = one.bound.userdata;
	remembered.arg.assign(one.bound.arg);
	remembered.bytes.assign(scratch.bytes());
	remembered.status = answered;
	remembered.wake = scratch.wake();

	if (answered != element_status::omitted)
		out.splice(scratch);
	else if (scratch.wake() != 0)
		out.wake_in(scratch.wake());
	return answered;
}

// The two phases the compile-time `seg` runs, driven by the child kind tags
// instead of by `if constexpr`. Same rule, same order, same style reset - which
// is the point: a group built across the ABI and a group written in C++ must not
// be two behaviours.
element_status engine::render_group(node& group, const state& facts, sink& out,
                                    std::size_t depth) {
	// A SPAN - the same node shape with the vote skipped, which is what a
	// `{literal:blue::hi}` desugars to. It has no module child and could therefore
	// never win a vote; being stamped a decoration says it was never in the
	// running, so it always renders, it answers `neutral`, and it takes no part in
	// an enclosing group's vote either. The style reset at the end is the same one
	// a voting group owes.
	if (group.kind == element_kind::decoration) {
		bool painted = false;
		for (const std::unique_ptr<node>& child : group.children) {
			if (child->styles)
				painted = true;
			invoke(*child, facts, out, depth + 1);
		}
		if (painted)
			out.write_style(style{});
		return element_status::neutral;
	}

	bool any_ready = false;
	for (const std::unique_ptr<node>& child : group.children) {
		if (child->kind != element_kind::module)
			continue;
		child->scratch.reset();
		child->answered = invoke(*child, facts, child->scratch, depth + 1);
		if (child->answered == element_status::ready)
			any_ready = true;
	}

	// The vote failed: emit nothing, and note what did NOT happen - the
	// decorations never ran, so a literal inside a group whose module had nothing
	// to say costs neither bytes nor a call. `pending` does not count as ready in
	// v1; see `seg` in prompt.h for why.
	if (!any_ready)
		return element_status::omitted;

	bool styled = false;
	for (const std::unique_ptr<node>& child : group.children) {
		if (child->kind == element_kind::module) {
			if (child->answered != element_status::omitted)
				out.splice(child->scratch);
			else if (child->scratch.wake() != 0)
				out.wake_in(child->scratch.wake());
			continue;
		}
		if (child->styles)
			styled = true;
		// A CHILD GROUP RECURSES THROUGH HERE, and it is phase two that reaches it
		// because a group is not a module and does not vote - the same rule the
		// compile-time `seg` follows for a nested `seg` or `when`. So
		// `({git} ({path}))` shows or vanishes on `git` alone, and the inner group
		// renders only if the outer one survived. Nesting is the template
		// language's (the ABI's verb stream still refuses it - it is a linear
		// stream with no way to say which group a close belongs to).
		invoke(*child, facts, out, depth + 1);
	}

	if (styled)
		out.write_style(style{});
	return element_status::ready;
}

void engine::rebuild(surface& target) {
	target.bytes.clear();
	for (std::size_t i = 0; i < target.nodes.size(); ++i)
		target.bytes.append(target.slots[i].bytes);
}

void engine::render_surface(surface& target, const state& facts) {
	if (target.slots.size() < target.nodes.size())
		target.slots.resize(target.nodes.size());

	for (std::size_t i = 0; i < target.nodes.size(); ++i) {
		slot& into = target.slots[i];
		_top.reset();
		into.status = invoke(*target.nodes[i], facts, _top, 0);
		if (into.status == element_status::omitted)
			into.bytes.clear();
		else
			into.bytes.assign(_top.bytes());
		// Relative in, absolute out. A request only means anything at the instant
		// it was made, so this is the one place it is turned into a deadline.
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

	for (surface& target : _surfaces) {
		bool moved = false;
		const std::size_t count = std::min(target.nodes.size(), target.slots.size());
		for (std::size_t i = 0; i < count; ++i) {
			slot& one = target.slots[i];

			// THE GATE, and it is the whole of recalculation-by-cause: an element
			// with no deadline is not re-invoked on a tick, ever. Its bytes are
			// spliced from the slot untouched - which is why `git` costs nothing
			// while a clock ticks beside it.
			if (one.wake_at == 0 || one.wake_at > facts.tick)
				continue;

			_top.reset();
			const element_status answered = invoke(*target.nodes[i], facts, _top, 0);
			const std::string_view produced =
				answered == element_status::omitted ? std::string_view{} : _top.bytes();

			if (answered != one.status || produced != std::string_view{one.bytes}) {
				one.status = answered;
				one.bytes.assign(produced);
				moved = true;
			}
			one.wake_at = _top.wake() != 0 ? facts.tick + _top.wake() : 0;
		}

		// Rebuilt only when something actually changed. An unchanged prompt
		// produces no terminal write at all (§6.10), and the cheapest way to
		// guarantee that is to answer honestly here.
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
		const std::size_t count = std::min(target.nodes.size(), target.slots.size());
		for (std::size_t i = 0; i < count; ++i) {
			const std::uint64_t when = target.slots[i].wake_at;
			if (when != 0 && (earliest == 0 || when < earliest))
				earliest = when;
		}
	}
	return earliest;
}

} // namespace lesh::leshper::prompt

// ---------------------------------------------------------------------------
// The C ABI
// ---------------------------------------------------------------------------

extern "C" {

std::int32_t lesh_prompt_module_register(lesh_registry* registry, const char* name,
                                         lesh_prompt_module_fn fn, void* userdata) {
	if (registry == nullptr)
		return LESH_ERR_INVAL;
	engine* which = engine_of(registry);
	if (which == nullptr)
		return LESH_ERR_NOTFOUND;
	if (name == nullptr || fn == nullptr)
		return LESH_ERR_INVAL;
	return which->register_abi_module(std::string_view{name}, fn, userdata);
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

	const lesh::leshper::prompt::state& facts = *context->facts;
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

std::int32_t lesh_prompt_add_module(lesh_registry* registry, std::uint32_t surface,
                                    const char* name, const char* arg) {
	if (registry == nullptr)
		return LESH_ERR_INVAL;
	engine* which = engine_of(registry);
	if (which == nullptr)
		return LESH_ERR_NOTFOUND;
	surface_id target = surface_id::left;
	if (!surface_of(surface, target) || name == nullptr)
		return LESH_ERR_INVAL;

	const std::string_view argument = arg == nullptr ? std::string_view{} : std::string_view{arg};
	if (!which->add_module(target, std::string_view{name}, argument))
		return LESH_ERR_NOTFOUND;
	return LESH_OK;
}

std::int32_t lesh_prompt_add_literal(lesh_registry* registry, std::uint32_t surface,
                                     const char* bytes, std::size_t length) {
	if (registry == nullptr)
		return LESH_ERR_INVAL;
	engine* which = engine_of(registry);
	if (which == nullptr)
		return LESH_ERR_NOTFOUND;
	surface_id target = surface_id::left;
	if (!surface_of(surface, target))
		return LESH_ERR_INVAL;
	if (bytes == nullptr && length != 0)
		return LESH_ERR_INVAL;

	which->add_literal(target, length == 0 ? std::string_view{} : std::string_view{bytes, length});
	return LESH_OK;
}

std::int32_t lesh_prompt_add_style(lesh_registry* registry, std::uint32_t surface,
                                   const char* spec, std::size_t length) {
	if (registry == nullptr)
		return LESH_ERR_INVAL;
	engine* which = engine_of(registry);
	if (which == nullptr)
		return LESH_ERR_NOTFOUND;
	surface_id target = surface_id::left;
	if (!surface_of(surface, target))
		return LESH_ERR_INVAL;
	if (spec == nullptr && length != 0)
		return LESH_ERR_INVAL;

	// A SPEC THAT WILL NOT PARSE IS A POSITIVE DOMAIN STATUS, not LESH_ERR_INVAL.
	// The argument was well formed - a pointer and a length, both fine - and what
	// failed was its CONTENT, which is the caller's own text and not a misuse of
	// the verb. There is no message channel here because there is nothing to say
	// that `style_grammar.h`'s offset would not say better to a caller holding the
	// string; `lesh_prompt_set`, whose text a human typed, has one.
	const std::string_view text = length == 0 ? std::string_view{} : std::string_view{spec, length};
	return which->add_style(target, text) ? LESH_OK : 1;
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
	const std::string_view source = length == 0 ? std::string_view{} : std::string_view{text, length};
	if (which->set_template(target, source, message)) {
		// The length is always written, and on success it is zero - so a caller
		// that asked with a null buffer to size the message first learns from the
		// return value that there is none.
		*error_length_out = 0;
		return LESH_OK;
	}

	// REFUSED, and the message travels under `lesh_buffer_get`'s convention: the
	// length is reported whether or not it fit, a short buffer is TOOSMALL rather
	// than a truncation, and NULL with zero capacity asks the length. The domain
	// status 1 is what a caller sees once the message has actually been handed
	// over - TOOSMALL can only happen on this path, so the two together are
	// "refused, ask again with room".
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

std::int32_t lesh_prompt_group_open(lesh_registry* registry, std::uint32_t surface) {
	if (registry == nullptr)
		return LESH_ERR_INVAL;
	engine* which = engine_of(registry);
	if (which == nullptr)
		return LESH_ERR_NOTFOUND;
	surface_id target = surface_id::left;
	if (!surface_of(surface, target))
		return LESH_ERR_INVAL;
	return which->open_group(target) ? LESH_OK : LESH_ERR_REFUSED;
}

std::int32_t lesh_prompt_group_close(lesh_registry* registry, std::uint32_t surface) {
	if (registry == nullptr)
		return LESH_ERR_INVAL;
	engine* which = engine_of(registry);
	if (which == nullptr)
		return LESH_ERR_NOTFOUND;
	surface_id target = surface_id::left;
	if (!surface_of(surface, target))
		return LESH_ERR_INVAL;
	return which->close_group(target) ? LESH_OK : LESH_ERR_REFUSED;
}

} // extern "C"
