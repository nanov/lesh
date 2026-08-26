#include "leshper/read.h"

#include "leshper/abi.h"
#include "leshper/complete.h"
#include "leshper/keymap.h"
#include "leshper/loop.h"
#include "leshper/prompt.h"
#include "leshper/registry.h"
#include "leshper/shell_actor.h"
#include "leshper/shell_state_knowledge.h"
#include "leshper/tty.h"
#include "leshper/workers.h"
#include "runtime/builtins.h"
#include "runtime/executor.h"
#include "runtime/history_store.h"
#include "runtime/shell_state.h"
#include "substrate/arena.h"
#include "substrate/assert.h"
#include "substrate/log.h"
#include "syntax/parser.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lesh::leshper {

// ---------------------------------------------------------------------------
// The providers
// ---------------------------------------------------------------------------

bool shell_syntax_layer::line_is_complete(std::string_view line) const {
	// A pool of its OWN, rewound per call. The shell's pool holds the trees a
	// function body is a node in (#106) and this parse is a throwaway; the
	// worker arenas belong to #90. Nothing here outlives the call - the answer
	// is one bool - so the whole parse is bump-allocated and forgotten.
	//
	// NO ALIASES, deliberately. `parse` takes an `alias_source` and this passes
	// none: an alias could turn an incomplete line into a complete one, and
	// substituting one from HERE would be reading `shell_state` from the loop
	// thread, which ADR-0009 forbids. The cost is that Enter on `l` where
	// `alias l='while true; do'` accepts rather than continuing - a line the
	// executor will then report as incomplete, which is what a shell that
	// misjudged does anyway.
	static thread_local buffer_pool scratch{BUFFER_POOL_SIZE};
	static thread_local char* const base = scratch.at();
	scratch.reset(base);

	const syntax::tree parsed = syntax::parse(scratch, line, nullptr);
	// C-2's tristate, and only its first branch is asked here: `incomplete()`
	// wins over a defect while more input could still arrive, because the
	// continuation prompt is the right answer to an unterminated quote and a
	// syntax error is not. A line that is malformed and NOT incomplete is
	// COMPLETE for F-35's purposes - the shell should run it and report the
	// error, not hold the user in a continuation they cannot escape.
	return !parsed.incomplete();
}

void shell_prompt_source::left(std::string& into) const {
	std::string_view value;
	into.assign(_state->lookup(std::string_view{"PS1"}, value) ? value : kPosixPrompt);
}

void shell_prompt_source::continuation(std::string& into) const {
	std::string_view value;
	into.assign(_state->lookup(std::string_view{"PS2"}, value) ? value : kPosixContinuation);
}

void history_store_source::for_each_newest_first(
	const std::function<bool(std::string_view)>& fn) const {
	// The store's callback answers void, so "stop" is a flag rather than a
	// return: the rest of the walk still happens and every entry after the stop
	// is skipped without being shown. See the note on the class - the v1 store
	// has already read the whole file by the time it calls anybody, so this is a
	// loop over spans in memory and no further I/O.
	bool going = true;
	_store->for_each_newest_first([&](std::string_view entry) {
		if (going)
			going = fn(entry);
	});
}

bool terminal_meets_floor(const char* term) noexcept {
	return term != nullptr && term[0] != '\0' && std::strcmp(term, "dumb") != 0;
}

namespace {

// ---------------------------------------------------------------------------
// `bind`, from the other side of the link boundary.
// ---------------------------------------------------------------------------

// The runtime declares `binding_console` because `lesh_runtime` cannot call a
// keymap function - `lesh` is built on `lesh_runtime lesh_syntax lesh_ui` plus,
// since #134, `lesh_leshper`, and the editor must not be reachable from a
// builtin or every `lesh -c` would link it. This is the implementation, and it
// is the same twenty lines `leshper_keymap_tests.cpp` proved were enough.
class leshper_binding_console final : public runtime::binding_console {
public:
	explicit leshper_binding_console(editing_context& context) noexcept : _context(&context) {}

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
			std::int32_t exists = 0;
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

	outcome list_bindings(
		std::string_view name,
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

// ---------------------------------------------------------------------------
// The prompt, from the other side of the same boundary (#157, §6.10).
// ---------------------------------------------------------------------------

// `binding_console`'s twin, and for the identical reason: the prompt registry,
// the composer and the tick wheel are leshper's, and a builtin cannot reach
// them. The runtime declares what a configuration builtin needs to say and this
// is the implementation, verb for verb over `prompt::engine`.
//
// IT HAS NO CALLER IN v1 AND IS INSTALLED ANYWAY. The `bind`-shaped builtin is
// #157's recorded follow-up (its operand grammar is the template language's, and
// half of that grammar is the half that would have to be kept), so what exists
// today is the seam and the session that owns it. Installing it now is what
// makes writing that builtin a builtin-shaped job rather than a wiring one.
class leshper_prompt_console final : public runtime::prompt_console {
public:
	explicit leshper_prompt_console(prompt::engine& engine) noexcept : _engine(&engine) {}

	void module_names(std::vector<std::string>& into) const override {
		_engine->module_names(into);
	}

	outcome clear(surface which) override {
		_engine->clear(surface_of(which));
		return outcome::ok;
	}

	outcome use_default(surface which) override {
		_engine->use_default(surface_of(which));
		return outcome::ok;
	}

	outcome add_module(surface which, std::string_view name, std::string_view arg) override {
		// The engine's one failure, and it is a miss rather than a fault: nothing
		// is registered under that name.
		return _engine->add_module(surface_of(which), name, arg) ? outcome::ok
		                                                         : outcome::no_such_module;
	}

	outcome add_literal(surface which, std::string_view bytes) override {
		_engine->add_literal(surface_of(which), bytes);
		return outcome::ok;
	}

	outcome open_group(surface which) override {
		// False means one is already open, which in v1 is a caller that lost track
		// of its own nesting - groups do not nest until the template language.
		return _engine->open_group(surface_of(which)) ? outcome::ok
		                                             : outcome::unbalanced_group;
	}

	outcome close_group(surface which) override {
		return _engine->close_group(surface_of(which)) ? outcome::ok
		                                              : outcome::unbalanced_group;
	}

private:
	// TWO ENUMS, TRANSLATED HERE. The runtime's `surface` cannot BE leshper's
	// `surface_id` without the runtime including a leshper header, which is the
	// link this whole arrangement exists to prevent; so the mapping is three lines
	// at the one point that legitimately sees both.
	[[nodiscard]] static prompt::surface_id surface_of(surface which) noexcept {
		return which == surface::continuation ? prompt::surface_id::continuation
		                                      : prompt::surface_id::left;
	}

	prompt::engine* _engine;
};

// ---------------------------------------------------------------------------
// The facts a prompt is a function of
// ---------------------------------------------------------------------------

// `prompt::state` is a bundle of VIEWS, and the tick path re-reads it from the
// LOOP thread milliseconds or minutes after the shell thread filled it in - so
// the bytes behind those views cannot be a temporary, a `shell_state` lookup's
// return, or anything else with a lifetime shorter than the session. This struct
// is where they live. It is the session's own member, filled on the shell thread
// inside ADR-0009's window and read on the loop thread between prompts.
struct prompt_facts {
	std::string pwd;
	std::string home;

	int status = 0;
	std::size_t jobs = 0;
	std::uint64_t duration_ms = 0;
	std::uint64_t tick = 0;
	std::uint8_t hours = 0;
	std::uint8_t minutes = 0;
	std::uint8_t seconds = 0;

	// The variable door, filled with the shell's on the shell thread and NULLED on
	// the tick path - see `session::prompt_tick`, which is where the reasoning is.
	bool (*getvar)(const void*, std::string_view, std::string_view&) = nullptr;
	const void* getvar_ctx = nullptr;

	[[nodiscard]] prompt::state view() const noexcept {
		prompt::state facts;
		facts.pwd = pwd;
		facts.home = home;
		// EMPTY, and deliberately: the vi-mode indicator's context tweak is v2's
		// (§6.10, recorded on #156). A `mode` module placed today renders nothing
		// rather than rendering a guess.
		facts.mode = std::string_view{};
		facts.status = status;
		facts.jobs = jobs;
		facts.duration_ms = duration_ms;
		facts.tick = tick;
		facts.hours = hours;
		facts.minutes = minutes;
		facts.seconds = seconds;
		// A REAL PROMPT MAY TOUCH THE FILESYSTEM. §6.10's floor is kept by the
		// budget inside the module, not by refusing it here: `fs_allowed` false is
		// for the `constexpr` default table, whose whole evaluation has to be a
		// constant expression. This is a running shell.
		facts.fs_allowed = true;
		facts.getvar = getvar;
		facts.getvar_ctx = getvar_ctx;
		return facts;
	}
};

// §6.10's virtual clock: ticks of 10 ms off the monotonic clock. Not the wall
// clock, which steps when the machine's does - a spinner must not stall or
// double-step because ntp moved the hour.
[[nodiscard]] std::uint64_t tick_now() noexcept {
	const auto since = std::chrono::steady_clock::now().time_since_epoch();
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(since).count();
	return static_cast<std::uint64_t>(ms) / 10;
}

// And the wall clock, which IS what the `time` module shows. `localtime_r`
// rather than `localtime`: the loop thread calls this too, and the non-`_r` form
// hands back a pointer into storage the other thread would reuse.
void fill_wall_clock(prompt_facts& into) noexcept {
	const std::time_t now = std::time(nullptr);
	std::tm parts{};
	if (::localtime_r(&now, &parts) == nullptr)
		return;
	into.hours = static_cast<std::uint8_t>(parts.tm_hour);
	into.minutes = static_cast<std::uint8_t>(parts.tm_min);
	into.seconds = static_cast<std::uint8_t>(parts.tm_sec);
}

// ---------------------------------------------------------------------------
// The session
// ---------------------------------------------------------------------------

// The shell thread's half of ADR-0009, and the owner of everything an
// interactive shell has that a non-interactive one does not.
//
// ONE OBJECT, so that ADR-0007 is answered by its destructor: the helper pool,
// the loop, the actor, the two reactor contexts, the signal hub and the four
// adapters all die together and in the reverse order they were built. Nothing
// here is a global and nothing is leaked on any exit path, which is what lets
// the leak gate expect zero.
class session final : public shell_side {
public:
	session(runtime::shell_state& state, buffer_pool& pool, const provider_bundle& providers,
	        int in, int out);

	~session() override;

	session(const session&) = delete;
	session& operator=(const session&) = delete;

	[[nodiscard]] int run(std::string_view rc_path);

	// --- shell_side (A-5), both on THIS thread -------------------------------

	std::int32_t execute(std::string_view line) override;
	std::int32_t port_call(std::string_view code) override;

	// --- the loop thread's side ---------------------------------------------

	[[nodiscard]] const syntax_layer& syntax() const noexcept { return *_providers.syntax; }

	// Set by the `cancel_line` action, read and cleared by `execute`. The post
	// that follows it takes the actor's mutex and the shell thread reads under
	// the same one, so the ordering is the channel's and not this flag's; the
	// atomic is for the store itself.
	void note_cancel() noexcept { _cancelled.store(true, std::memory_order_relaxed); }

private:
	void register_line_actions();
	void bind_line_keys();
	void refresh_prompt();
	void fill_prompt_facts();
	void reconcile_prompt_timer();
	void source_rc(std::string_view path);

	// The composer's door to `shell_state`, for the `env` module. A trampoline
	// because `prompt::state` carries a C function pointer and a context - it is
	// the shape the ABI hands a module, and the C++ side uses the same one rather
	// than a second.
	//
	// SHELL THREAD ONLY. `fill_prompt_facts` installs it and the tick path takes
	// it away again; see `prompt_tick`.
	static bool prompt_variable(const void* ctx, std::string_view name,
	                            std::string_view& out);

	// The three line-reading actions, ON THE LOOP THREAD.
	//
	// REGISTERED AT THE WIRING SITE rather than in builtin_actions.cpp, and the
	// reason is their userdata: each needs the session - the syntax layer for
	// F-35, the cancel flag for #98 decision 3 - where the ten built-ins are
	// pure editor verbs registered with a null context because they have none to
	// need. They still cross through `lesh_action_register` and by no other
	// route (A-11), so a user rebinding `accept_line` replaces one of these
	// exactly as it replaces any other.
	static std::int32_t accept_line(lesh_editor* editor, const lesh_invocation* how, void* self);
	static std::int32_t cancel_line(lesh_editor* editor, const lesh_invocation* how, void* self);
	static std::int32_t end_of_file(lesh_editor* editor, const lesh_invocation* how, void* self);

	// The prompt's own wake, dispatched by the loop's timer topic (#129's
	// `lesh_timer_start`, §6.10's tick). A fourth action for the same reason the
	// three above are here: it needs the session.
	static std::int32_t prompt_tick(lesh_editor* editor, const lesh_invocation* how, void* self);

	runtime::shell_state& _state;
	const provider_bundle& _providers;
	runtime::tree_walking_executor _executor;
	// ADR-0009's tripwire (#151), DECLARED BEFORE THE ADAPTER AND THE ACTOR
	// because both borrow it: the actor raises it around `execute` and
	// `port_call` below, and the adapter asserts it is down on every read. One
	// flag, so the two cannot be talking about different windows.
	shell_writing_flag _writing;
	shell_state_knowledge _knowledge;
	owned_highlighter _highlighter;
	owned_autosuggester _autosuggester;
	worker_pool _helpers;
	signal_hub _signals;
	// DECLARED BEFORE THE LOOP, and for the actor's reason one line down: the
	// registry the loop owns holds a borrowed pointer to this engine (see the
	// constructor), and members die in reverse - so the loop, and the registry
	// with it, has to go first.
	prompt::engine _prompt_engine;
	prompt_facts _prompt_facts;
	// THE ACTOR IS DECLARED BEFORE THE LOOP, and it is not a style choice:
	// `~event_loop` recycles the messages `drain` handed it back into the
	// channel the ACTOR owns (ADR-0007), so an actor destroyed first would have
	// the loop locking a mutex whose storage had gone. Members die in reverse,
	// so declaring it first is how the loop dies first.
	shell_actor _actor;
	event_loop _loop;
	std::optional<leshper_binding_console> _console;
	std::optional<leshper_prompt_console> _prompt_console;

	std::atomic<bool> _cancelled{false};
	// Loop-thread scratch for the accept action; shell-thread scratch for the
	// prompt. Members so neither path allocates once the session is warm.
	//
	// TWO PROMPT SCRATCHES AND NOT ONE, since #157: the precedence rule compares
	// the stub's two surfaces against the POSIX defaults in the same breath, so
	// both sets of bytes have to be in hand at once.
	std::string _accept_scratch;
	std::string _prompt_left_scratch;
	std::string _prompt_continuation_scratch;

	// Whether the engine is the surface the loop is showing - the precedence rule's
	// answer, cached rather than re-asked. `refresh_prompt` is the only writer and
	// `prompt_tick` the only other reader, which is the SAME serialization every
	// other option write here has (ADR-0009): the shell thread writes it in the
	// window where the loop is blocked in `wait_on_shell`, and the loop thread
	// reads it afterwards, on the far side of the channel mutex that published the
	// reply. Cached rather than recomputed because recomputing it on the tick path
	// would mean reading `shell_state` from the loop thread, which is the one thing
	// ADR-0009 forbids outright.
	//
	// TRUE TO BEGIN WITH, and the constructor's `refresh_prompt` settles it before
	// the loop starts.
	bool _engine_owns_prompt = true;

	// How long the last command took, measured around `run_input` in `execute`
	// and read by the next `fill_prompt_facts`. The `duration` module's fact, and
	// the shell is the only side that can know it.
	std::uint64_t _last_duration_ms = 0;

	// What the prompt timer is armed with, or zero for "nothing armed". The
	// interval is remembered beside the id because reconciliation compares
	// intervals: an unchanged cadence must LEAVE the timer alone, or a spinner
	// would be disarmed and re-armed on every frame and the loop's rearm-from-fire
	// would never get to do its job.
	std::uint64_t _prompt_timer_id = 0;
	std::uint64_t _prompt_timer_interval = 0;

	// --- The completer's door to the shell (#139, direct since #151) ---------

	// THE SAME ADAPTER THE HIGHLIGHTER'S TOKENS READ THROUGH, called on the LOOP
	// thread. There is one statement anywhere in this tree of what leshper may
	// see of `shell_state`, and both readers go through it.
	//
	// #139 put a `name_source` and a round trip on the actor between these two
	// lines; the owner's reading of ADR-0009 removed the need. `complete` is
	// called from inside an action, dispatched by the loop; the only writers are
	// `execute` and `port_call`, both of which the loop itself requests and then
	// blocks on for their whole duration. So while this call runs, nothing is
	// writing - and `_writing` above is what says so out loud if that ever stops
	// being true.
	shell_completer _completer;
};


// The loop's options, built once. A function rather than an initializer list in
// the member init, because half of it is environment reading and the ctor is
// already long enough.
loop_options options_for(const provider_bundle& providers, bool manage_terminal) {
	loop_options options;
	if (providers.prompt != nullptr) {
		providers.prompt->left(options.prompt);
		providers.prompt->continuation(options.continuation);
	}
	// #97 decision 2, "assume first": the trivial environment reads and nothing
	// else. Never terminfo, and no startup query - a DA1 round trip needs a
	// timeout and that is a latency tax on everyone.
	options.capabilities = terminal_capabilities::from_env(
		std::getenv("TERM"), std::getenv("COLORTERM"), std::getenv("NO_COLOR"));
	options.manage_terminal = manage_terminal;
	return options;
}

session::session(runtime::shell_state& state, buffer_pool& pool,
                 const provider_bundle& providers, int in, int out)
	: _state(state),
	  _providers(providers),
	  _executor(pool, state),
	  _knowledge(state, &_writing),
	  _autosuggester(providers.history),
	  _actor(*this, &_knowledge, &_writing),
	  _loop(loop_fds{in, out}, options_for(providers, true)),
	  _completer(&_knowledge) {
	// The EXIT trap belongs to the session and not to the first line of it. See
	// tree_walking_executor::defer_exit_trap; `run` runs it on the way out.
	_executor.defer_exit_trap(true);

	// THE EDITING CONTEXT IS THE STATE'S. editor.cpp dispatches through
	// `context_of(state)`, so the registries the loop attaches and the registries
	// a keystroke reaches have to be the same object - and they are, because this
	// asks the loop's own editor state for its context rather than building a
	// second one beside it.
	editing_context& context = context_of(_loop.editor());
	// The ten built-in actions and the three default keymaps are the context's
	// constructor's; what is added here is everything that needs a shell.
	register_builtin_reactors(context.actions(), _highlighter.get());
	register_autosuggester(context.actions(), _autosuggester.get());
	register_line_actions();
	bind_line_keys();
	context.loop().set_shell_knowledge(&_knowledge);
	// #94's `Completer` override point, filled (#139). The bundle's field wins
	// when a caller supplied one - which is what makes it an override point and
	// not a hard-wired provider - and the session's own trio is the default.
	context.actions().completion =
		providers.completion != nullptr ? providers.completion : &_completer;
	// §6.10's engine, lent to the registry the same way and on the same terms:
	// BORROWED, null when there is no session, and the route every
	// `lesh_prompt_*` verb takes into the composer. A binding that registers a
	// prompt module reaches this object and no other.
	context.actions().prompt_engine = &_prompt_engine;

	_console.emplace(context);
	// `bind` reaches the keymaps through here and by no other route, and only for
	// as long as this session lives (#118, #134).
	_state.set_binding_console(&*_console);

	_prompt_console.emplace(_prompt_engine);
	// And the prompt's console, installed the day the session starts even though
	// v1 has no builtin to call it (#157) - see the class above.
	_state.set_prompt_console(&*_prompt_console);

	_loop.attach_registry(context.actions());
	_loop.attach_helpers(_helpers);
	_loop.attach_shell(_actor);
	_loop.attach_signals(_signals);
	// #135's door is NOT attached to the loop (#151). The actor was given it at
	// construction and stamps it on every token it serves, so the loop never
	// holds a pointer to state it does not own; see `event_loop`'s note where
	// `attach_shell_knowledge` used to be.
	LESH_ASSERT(_actor.knowledge() == &_knowledge);

	// THE FIRST PAINT, and it needs saying here or the first prompt of every
	// session is the wrong one. `options_for` seeded the loop's options from the
	// provider, which was the whole story while `$PS1` was the default; since the
	// flip it is the fallback, and a shell with no rc file would otherwise paint
	// `$ ` once and go native only after the first command. `source_rc` cannot
	// cover it - it returns immediately when there is no rc to read.
	//
	// AFTER THE CONSOLES AND THE ENGINE, BEFORE `run`. After, because this renders
	// the engine and writes `_loop.options()`, and both have to exist and be wired
	// to the registry the loop attached; before, because `run` sources the rc and
	// then starts the loop, and the loop's first render must find the answer
	// already there. The rc's own `refresh_prompt` still runs after the rc, so a
	// `PS1=` set in `~/.leshrc` takes the surface back before anything is painted.
	//
	// ADR-0009 IS TRIVIALLY SATISFIED HERE: this is the shell thread, and the loop
	// thread does not exist yet - `_loop.start()` is two calls away, in `run`.
	refresh_prompt();
}

session::~session() {
	// Before the console dies, and before the context it points into does. A
	// `bind` from a shell whose editor has gone is "no line editor", which is the
	// truth (ADR-0007: the owner takes the view away as it takes the object).
	_state.set_binding_console(nullptr);
	// And the prompt's, before the engine it points at goes. Same rule, same
	// sentence: the owner takes the view away as it takes the object.
	_state.set_prompt_console(nullptr);
}

void session::register_line_actions() {
	registry& actions = context_of(_loop.editor()).actions();
	lesh_action_register(&actions, "accept_line", &session::accept_line, this);
	lesh_action_register(&actions, "cancel_line", &session::cancel_line, this);
	lesh_action_register(&actions, "end_of_file", &session::end_of_file, this);
	// BY NAME, resolved at expiry (see `lesh_timer_start`) - so this registration
	// and the arming below it are independent, and a user who rebinds
	// `prompt_tick` to their own action gets their own action ticked. That is the
	// same rule a key follows, which is the point of routing the prompt's wake
	// through the action table at all rather than through a private callback.
	lesh_action_register(&actions, "prompt_tick", &session::prompt_tick, this);
}

void session::bind_line_keys() {
	keymap_registry& maps = context_of(_loop.editor()).keymaps();
	const auto bind = [&](std::string_view map_name, const char* notation,
	                      std::string_view action) {
		keymap* map = maps.find(map_name);
		std::string encoded;
		if (map == nullptr || !parse_key_notation(notation, encoded)) {
			LESH_ASSERT(false && "a default binding does not parse");
			return;
		}
		map->bind(encoded, action);
	};

	// ENTER IS BOTH `<C-m>` AND `<C-j>`, and both are required. The key sends
	// U+000D (Ctrl-M), but `enter_raw` forces only the handful of bits the editor needs
	// and leaves the rest of the line discipline as the last command left it -
	// so ICRNL is usually still on and what the reader actually gets is U+000A
	// (Ctrl-J). readline and zle bind both for exactly this reason. keymap.cpp
	// left Enter deliberately unbound - "F-35 makes it a decision the parser takes
	// part in, and binding it to self_insert here would answer that question
	// wrongly and quietly" - and this is that decision arriving, as a binding to
	// an action that asks the syntax layer.
	//
	// CTRL-C IS NOT BOUND, and cannot be. ISIG stays on (tty.h's first rule), so
	// the driver turns Ctrl-C into SIGINT and no byte ever reaches a keymap;
	// `loop_options::interrupt_action` is where that key is "bound", by name, and
	// it names the same `cancel_line` a user could rebind here.
	//
	// The autosuggestion accept keys are #140's and UNDECIDED, so nothing is bound
	// to `accept_autosuggestion` - a registered name with no key, which is exactly
	// what an rc file binds.
	for (const std::string_view map_name :
	     {keymap_registry::emacs, keymap_registry::vi_insert, keymap_registry::vi_command}) {
		bind(map_name, "<C-m>", "accept_line");
		bind(map_name, "<C-j>", "accept_line");
	}
	bind(keymap_registry::emacs, "<C-d>", "end_of_file");
	bind(keymap_registry::vi_insert, "<C-d>", "end_of_file");

	// TAB COMPLETES, and it is bound HERE rather than in keymap.cpp's default
	// tables for the same reason `accept_line` is: a completion needs a
	// completer, and a completer needs a shell. keymap.cpp builds the tables an
	// editor has with no session at all - `vared` (#102) will pass a bundle whose
	// completer is a different one, and a Tab hard-wired into the default emacs
	// map would have decided that for it.
	//
	// NOT IN `vi_command`: Tab there is not an insertion, and vi's own repertoire
	// has no completion verb. Insert mode has it, which is where text is being
	// entered.
	bind(keymap_registry::emacs, "<Tab>", "complete_word");
	bind(keymap_registry::vi_insert, "<Tab>", "complete_word");
}

bool session::prompt_variable(const void* ctx, std::string_view name, std::string_view& out) {
	return static_cast<const runtime::shell_state*>(ctx)->lookup(name, out);
}

void session::fill_prompt_facts() {
	// ON THE SHELL THREAD, every one of them. This is the only place `shell_state`
	// is read for the prompt, and it runs inside ADR-0009's window - see
	// `refresh_prompt`. The composer never reaches across the boundary itself
	// (§6.10: "shell facts reach modules through the provider seam"); it is handed
	// a snapshot that has already been taken.
	_prompt_facts.pwd = _state.logical_working_directory();

	std::string_view home;
	_prompt_facts.home.assign(_state.lookup(std::string_view{"HOME"}, home)
	                              ? home
	                              : std::string_view{});

	_prompt_facts.status = _state.last_status();
	_prompt_facts.duration_ms = _last_duration_ms;

	// ZERO, AND IT IS RECORDED FOG rather than a value. The count lives in the
	// executor's `_background`, which is private, and #157 does not open it: the
	// accessor arrives when job-control UI needs one anyway (#98's seam), and
	// widening `src/runtime/` for a number nothing yet displays would be paying
	// for that ticket's design decision early and in the wrong place. Until then
	// the `jobs` module omits, which is exactly what it does when there are none.
	_prompt_facts.jobs = 0;

	_prompt_facts.tick = tick_now();
	fill_wall_clock(_prompt_facts);

	// LEGAL HERE AND NOWHERE ELSE. `lookup` reads `shell_state`, so the door is
	// open only while this thread owns it; the tick path shuts it again.
	_prompt_facts.getvar = &session::prompt_variable;
	_prompt_facts.getvar_ctx = &_state;
}

void session::refresh_prompt() {
	// WRITTEN FROM THE SHELL THREAD, and safe because of WHEN. This runs inside
	// `execute`, and the loop is blocked in `wait_on_shell`'s poll for the reply
	// this execution is about to post - it renders nothing and reads no option
	// until then. The happens-before edge is the channel's mutex: the shell posts
	// under it and the loop drains under it, so the write below is visible to
	// every read that follows the reply. ADR-0009 in one line: the shell owns its
	// state, and leshper reads it at a moment the shell chose.
	//
	// THE SAME SENTENCE COVERS THE ENGINE, whose header says loop-thread only.
	// "Loop-thread only" there means "one thread at a time and no locking", and
	// this is the other moment the loop is provably not that thread - the same
	// window `_loop.options()` is written in, one line down, and has been since
	// #129.
	fill_prompt_facts();
	_prompt_engine.render_full(_prompt_facts.view());

	// The stub's bytes, read whether or not they are used: the rule below is a
	// question ABOUT them, so they have to be in hand before it can be asked.
	if (_providers.prompt != nullptr) {
		_providers.prompt->left(_prompt_left_scratch);
		_providers.prompt->continuation(_prompt_continuation_scratch);
	}

	// THE PRECEDENCE RULE, TURNED ROUND BY THE OWNER'S RULING ON #157. This is
	// §6.10's supersession arriving: `PS1`/`PS2` were always "a transitional stub
	// ... the native prompt supersedes them", and the ruling is that it does so
	// now. The ENGINE renders unless the user expressed a `$PS1` preference.
	//
	// AN UNTOUCHED `$PS1` IS NOT A PREFERENCE. `shell_state`'s constructor seeds
	// `PS1='$ '` and `PS2='> '` because POSIX says those are the defaults, so
	// every session starts holding bytes nobody typed. Reading them back as a
	// choice would mean no shell ever gets the native prompt, and the flip would
	// have shipped as a no-op. So the stub wins only when what it produces DIFFERS
	// from those defaults - which is exactly the set of users who set the variable
	// - and a null provider (no shell behind the editor at all) is the engine's
	// too.
	//
	// BOTH SURFACES TOGETHER, never one each. A shell whose `$PS1` was set but
	// whose `$PS2` was not would otherwise paint a user prompt and a native
	// continuation: two prompts arguing, from one line to the next, about which
	// shell this is. One question, one answer, both surfaces.
	//
	// `PS1='$ '` TYPED DELIBERATELY READS AS "NO PREFERENCE", and that is accepted
	// imprecision rather than an oversight. Telling it from the seeded value needs
	// `shell_state` to remember whether an assignment ever happened - a bit on
	// every variable, carried forever, to be more precise about a mechanism that
	// is documented to be dropped. The user who wanted a bare `$ ` and got the
	// native prompt has one `PS1='$ '`-shaped complaint and several ways to say it
	// (`PS1='$ '` with any other byte, or the prompt console); the design debt of
	// the alternative would outlive the feature it serves.
	_engine_owns_prompt = _providers.prompt == nullptr || _prompt_engine.configured()
	                      || (_prompt_left_scratch == kPosixPrompt
	                          && _prompt_continuation_scratch == kPosixContinuation);

	if (_engine_owns_prompt) {
		_loop.options().prompt = _prompt_engine.output(prompt::surface_id::left);
		_loop.options().continuation = _prompt_engine.output(prompt::surface_id::continuation);
	} else {
		_loop.options().prompt = _prompt_left_scratch;
		_loop.options().continuation = _prompt_continuation_scratch;
	}

	reconcile_prompt_timer();
}

void session::reconcile_prompt_timer() {
	// TWO CALLERS, TWO THREADS, AND NEITHER NEEDS A LOCK (ADR-0009). The shell
	// thread calls this from `refresh_prompt`, in the window where the loop is
	// blocked in `wait_on_shell` waiting for the reply; the loop thread calls it
	// from `prompt_tick`, where it IS the loop. There is no third caller, and the
	// two windows cannot overlap - which is what makes the registry's
	// "loop-thread only" rule (ADR-0008) hold here rather than be bent.
	registry& actions = context_of(_loop.editor()).actions();

	const std::uint64_t desired =
		prompt::timer_interval_ms(_prompt_engine.next_wake(), _prompt_facts.tick);

	// UNCHANGED CADENCE LEAVES THE TIMER ALONE, and this is the load-bearing
	// branch rather than an optimisation: the loop rearms a timer from the moment
	// it fires (#129), so a spinner that keeps asking for the same cadence keeps
	// one steady timer. Stopping and starting it every frame would restart the
	// interval from inside the dispatch each time, and the cadence would drift by
	// however long a render took.
	if (desired == _prompt_timer_interval)
		return;

	if (_prompt_timer_interval != 0) {
		(void)lesh_timer_stop(&actions, _prompt_timer_id);
		_prompt_timer_id = 0;
		_prompt_timer_interval = 0;
	}
	// ZERO IS NO TIMER AT ALL - `next_wake()`'s own zero, carried through. A
	// static prompt costs no idle wakeups (§6.10), and the way that is guaranteed
	// is that there is nothing armed to wake for.
	if (desired == 0)
		return;

	if (lesh_timer_start(&actions, desired, "prompt_tick", &_prompt_timer_id) == LESH_OK)
		_prompt_timer_interval = desired;
}

std::int32_t session::execute(std::string_view line) {
	// A CANCEL ARRIVES AS AN EMPTY LINE (see event_loop::finish_cancelled_line):
	// nothing to run, at a command boundary. `$?` = 130 and the INT trap are what
	// it is for.
	if (_cancelled.exchange(false, std::memory_order_relaxed))
		_executor.interrupt_at_prompt();

	// ZEROED FIRST, so that a cancel and an Enter on an empty line both report
	// nothing rather than re-reporting the last command's time. A prompt redrawn
	// after a bare Enter that said "took 4s" would be claiming that the Enter
	// took four seconds; the truth is that nothing ran, and the `duration`
	// module's floor already omits a zero.
	_last_duration_ms = 0;

	if (!line.empty()) {
		// F-34 and #113: the entry goes in with its newlines, before it runs, so
		// a command that ends the session is still in the history. A blank line
		// is not history in any shell.
		if (_providers.store != nullptr
		    && line.find_first_not_of(" \t\n") != std::string_view::npos)
			(void)_providers.store->append(line);

		// AROUND `run_input` AND NOTHING ELSE, on the monotonic clock. Wall time is
		// what a user means by "how long did that take", and it is measured here
		// because this thread is the only side that knows when the command began
		// and when it ended - the loop was parked for the whole of it.
		const auto began = std::chrono::steady_clock::now();
		(void)_executor.run_input(line);
		const auto elapsed = std::chrono::steady_clock::now() - began;
		_last_duration_ms = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
	}

	// An `exit` typed at the prompt ends the session, and the shell thread is the
	// only side that can know. NOT `stop()`: the loop is waiting for the reply
	// this call is about to post, so joining it from here would be waiting on a
	// thread that is waiting on us.
	if (_executor.exit_requested())
		_loop.request_stop();

	refresh_prompt();
	// THE DISPOSITIONS, RE-ASSERTED HERE AND NOT ON THE LOOP THREAD (#142). The
	// line that just ran may have been a `trap`, which `sigaction`s from THIS
	// thread; the hub takes them back from this thread too. The loop used to do
	// it in `enter_read` and on the unpark, which made two threads writers of one
	// piece of process-wide state; now the loop only reads the hub.
	//
	// BEFORE THE REPLY IS POSTED, which is what "before the loop resumes" means:
	// this function's return value IS the reply, so everything above it is
	// ordered ahead of the loop waking up.
	_signals.reassert();
	return _state.last_status();
}

std::int32_t session::port_call(std::string_view code) {
	// #92's port, lane discipline and all, is the executor's - and in v1 the
	// editor has no ABI door that reaches this, so nothing calls it yet. It is
	// implemented rather than left abstract because A-5 is an interface with two
	// methods and a session that answered only one of them would be a session
	// that cannot be handed to `shell_actor`.
	//
	// The three lanes (#92 decision 1) are not enforced here: `run_input` runs
	// what it is given. The refusal of the forking forms belongs with the door
	// that opens this, and there is none.
	LESH_LOG(log::level::debug, log::category::exec, "port: %zu bytes", code.size());
	(void)_executor.run_input(code);
	refresh_prompt();
	// The other door onto `run_input` from this thread, so the other place a
	// `trap` can have run; see `execute`.
	_signals.reassert();
	return _state.last_status();
}

// --- The actions ------------------------------------------------------------

std::int32_t session::accept_line(lesh_editor* editor, const lesh_invocation*, void* self) {
	session& me = *static_cast<session*>(self);

	std::size_t length = 0;
	if (lesh_buffer_length(editor, &length) != LESH_OK)
		return LESH_ERR_INVAL;
	me._accept_scratch.resize(length);
	std::size_t copied = 0;
	if (lesh_buffer_get(editor, me._accept_scratch.data(), length, &copied) != LESH_OK)
		return LESH_ERR_INVAL;
	me._accept_scratch.resize(copied);

	// F-35, and the whole of what the syntax layer is asked at a prompt. An
	// incomplete line does not accept: it grows a newline and the continuation
	// prompt appears, which is what `> ` has always meant.
	if (!me.syntax().line_is_complete(me._accept_scratch)) {
		std::size_t cursor = 0;
		if (lesh_cursor_get(editor, &cursor) != LESH_OK)
			return LESH_ERR_INVAL;
		// STAGED like any other write (A-12): one undo entry, one generation
		// bump, one redraw, because the loop commits and the action does not.
		return lesh_buffer_replace(editor, cursor, cursor, "\n", 1);
	}
	return lesh_accept_line(editor);
}

std::int32_t session::cancel_line(lesh_editor* editor, const lesh_invocation*, void* self) {
	// The editor half is the loop's - discard, paint `^C`, fresh prompt. What
	// this adds is the shell half: `$?` = 130 and the user's INT trap, fired at
	// the prompt, the zsh way (#98 decision 3). The flag is read by `execute`,
	// which the loop posts as an empty line the moment this returns.
	static_cast<session*>(self)->note_cancel();
	return lesh_cancel_line(editor);
}

std::int32_t session::end_of_file(lesh_editor* editor, const lesh_invocation*, void* self) {
	session& me = *static_cast<session*>(self);
	std::size_t length = 0;
	if (lesh_buffer_length(editor, &length) != LESH_OK)
		return LESH_ERR_INVAL;
	// ONLY ON AN EMPTY LINE, which is every shell's rule: Ctrl-D with text typed
	// deletes forward in bash and zsh, and `delete_forward_char` is #119's, so
	// here it does nothing rather than doing something else.
	if (length != 0)
		return LESH_OK;
	// ZERO, and the session's own answer replaces it. This runs on the LOOP
	// thread, and `$?` is the shell thread's - reading it here would be reading
	// state ADR-0009 gives one owner. `run` returns the shell's last status, so
	// nothing is lost by not guessing at it from the wrong side.
	(void)me;
	return lesh_exit(editor, 0);
}

std::int32_t session::prompt_tick(lesh_editor*, const lesh_invocation*, void* self) {
	session& me = *static_cast<session*>(self);

	// ON THE LOOP THREAD, inside `fire_timers`. Everything written onto the
	// snapshot here is derived from a CLOCK and from nothing the shell owns, which
	// is what makes the write legal from this side: no `shell_state`, no executor,
	// no variable.
	me._prompt_facts.tick = tick_now();
	fill_wall_clock(me._prompt_facts);

	prompt::state facts = me._prompt_facts.view();

	// AND THE VARIABLE DOOR IS SHUT. A tick may only re-invoke clock-derived
	// elements (`engine::render_tick`'s contract says so), but the engine cannot
	// enforce that on a module it did not write: an `env` module that asked for a
	// wake it had no business asking for would be re-invoked here and would read
	// `shell_state` from the wrong thread. With `getvar` null it reads as UNSET
	// instead - a wrong prompt for one frame, which is a bug someone finds, rather
	// than a race with the shell, which is a bug nobody reproduces.
	facts.getvar = nullptr;
	facts.getvar_ctx = nullptr;

	const bool moved = me._prompt_engine.render_tick(facts);

	// ONLY IF THE BYTES ACTUALLY MOVED, and only if the engine is what the loop is
	// showing. A tick against a `$PS1` prompt is possible - a module can register
	// and arm a wake while the user's own `$PS1` owns the surface - and writing the
	// engine's output over it here would hand the surface over behind the rule's
	// back. THE SAME RULE, NOT A SECOND ONE: `_engine_owns_prompt` is what
	// `refresh_prompt` decided, and asking it again from this thread is not
	// available anyway - half the question is about `shell_state`.
	if (moved && me._engine_owns_prompt) {
		me._loop.options().prompt = me._prompt_engine.output(prompt::surface_id::left);
		me._loop.options().continuation =
			me._prompt_engine.output(prompt::surface_id::continuation);
		// DIRECTLY, and this is the one place an action calls it. The loop is the
		// calling thread and is mid-turn with nothing staged, and what changed is
		// an OPTION rather than editor state - so there is no generation to bump
		// and no batch to apply, and the `_needs_render` flag the loop checks at
		// the end of a turn would never be set by anything that happened here.
		// #112's diff is what keeps this cheap: the changed cells are the ones the
		// spinner moved, and an unchanged frame never got this far.
		me._loop.render();
	}

	me.reconcile_prompt_timer();
	return LESH_OK;
}

// --- Running ----------------------------------------------------------------

void session::source_rc(std::string_view path) {
	if (path.empty())
		return;
	// DOT-SCRIPT SEMANTICS, no new machinery (#101 decision 2). A missing file is
	// silence - a shell that complained about the config file you never wrote
	// would be wrong on every fresh machine. There is no watchdog: a hanging rc is
	// the user's own code, and Ctrl-C answers it.
	std::ifstream file{std::string{path}};
	if (!file)
		return;
	std::string source;
	for (std::string line; std::getline(file, line);) {
		source += line;
		source += '\n';
	}
	// ON THIS EXECUTOR, so a function or a trap the rc defines belongs to the
	// session rather than to a throwaway - and so the deferred EXIT trap stays
	// deferred rather than firing at the end of the config file.
	(void)_executor.run_input(source);
	// The prompt the rc set, before the first paint.
	refresh_prompt();
}

int session::run(std::string_view rc_path) {
	// #98 decision 5: SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT restore the
	// terminal and re-raise. From the WIRING SITE and not from the loop, because
	// process-wide dispositions belong to whoever owns the process - and only
	// where the disposition is still SIG_DFL, so ASan keeps its own.
	install_fatal_restore_handlers();

	// #101 decision 3: state, then rc, THEN the first read. The editing context
	// and the binding console already exist - this object's constructor built
	// them - so `bind` in an rc file reaches a real keymap registry, while
	// anything that needs a live read answers "the editor is not reading" (F-18).
	source_rc(rc_path);

	// AFTER THE RC AND BEFORE THE LOOP, and both halves matter (#142). `install`
	// IS the first take - the same disposition rules `reassert` applies, applied
	// once - so running it after the rc is what lets an rc's `trap 'cmd' CHLD`
	// be seen by the hub and chained to, rather than stomped by a hub that had
	// already decided. Before `_loop.start()`, because from there on this thread
	// is the only writer of dispositions and the loop must find them settled.
	if (!_signals.install())
		LESH_LOG(log::level::warn, log::category::loop,
		         "some signal dispositions could not be installed");

	// ONE THREAD SPAWNED, and this is it (ADR-0009, #136). Everything after this
	// line is the SHELL thread serving the loop's three slots; the loop thread
	// owns editor state and the terminal until it is finished, and releases this
	// one by calling `shell_actor::stop` on its way out.
	_loop.start();
	_actor.run();
	// Joins. The terminal is already restored: `event_loop::run` leaves the read
	// before it returns, which is the path every exit takes.
	_loop.stop();

	// The EXIT trap, deferred all session, runs exactly once and here.
	return _executor.finish(_state.last_status());
}

} // namespace

int run_interactive_shell(runtime::shell_state& state, buffer_pool& pool,
                          const provider_bundle& providers, int in, int out,
                          std::string_view rc_path) {
	LESH_ASSERT(providers.syntax != nullptr);
	LESH_ASSERT(providers.history != nullptr);
	LESH_ASSERT(providers.prompt != nullptr);
	session interactive{state, pool, providers, in, out};
	return interactive.run(rc_path);
}

} // namespace lesh::leshper
