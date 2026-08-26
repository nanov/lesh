#include "leshper/read.h"

#include "leshper/abi.h"
#include "leshper/keymap.h"
#include "leshper/loop.h"
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
#include <fstream>
#include <cstdlib>
#include <cstring>
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
	into.assign(_state->lookup(std::string_view{"PS1"}, value) ? value : std::string_view{"$ "});
}

void shell_prompt_source::continuation(std::string& into) const {
	std::string_view value;
	into.assign(_state->lookup(std::string_view{"PS2"}, value) ? value : std::string_view{"> "});
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
	void source_rc(std::string_view path);

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

	runtime::shell_state& _state;
	const provider_bundle& _providers;
	runtime::tree_walking_executor _executor;
	shell_state_knowledge _knowledge;
	owned_highlighter _highlighter;
	owned_autosuggester _autosuggester;
	worker_pool _helpers;
	signal_hub _signals;
	// THE ACTOR IS DECLARED BEFORE THE LOOP, and it is not a style choice:
	// `~event_loop` recycles the messages `drain` handed it back into the
	// channel the ACTOR owns (ADR-0007), so an actor destroyed first would have
	// the loop locking a mutex whose storage had gone. Members die in reverse,
	// so declaring it first is how the loop dies first.
	shell_actor _actor;
	event_loop _loop;
	std::optional<leshper_binding_console> _console;

	std::atomic<bool> _cancelled{false};
	// Loop-thread scratch for the accept action; shell-thread scratch for the
	// prompt. Members so neither path allocates once the session is warm.
	std::string _accept_scratch;
	std::string _prompt_scratch;
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
	  _knowledge(state),
	  _autosuggester(providers.history),
	  _actor(*this),
	  _loop(loop_fds{in, out}, options_for(providers, true)) {
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

	_console.emplace(context);
	// `bind` reaches the keymaps through here and by no other route, and only for
	// as long as this session lives (#118, #134).
	_state.set_binding_console(&*_console);

	_loop.attach_registry(context.actions());
	_loop.attach_helpers(_helpers);
	_loop.attach_shell(_actor);
	_loop.attach_signals(_signals);
	// #135's door. Only the shell-thread reactor's snapshot gets it; see
	// event_loop::attach_shell_knowledge for why no helper may.
	_loop.attach_shell_knowledge(&_knowledge);
}

session::~session() {
	// Before the console dies, and before the context it points into does. A
	// `bind` from a shell whose editor has gone is "no line editor", which is the
	// truth (ADR-0007: the owner takes the view away as it takes the object).
	_state.set_binding_console(nullptr);
}

void session::register_line_actions() {
	registry& actions = context_of(_loop.editor()).actions();
	lesh_action_register(&actions, "accept_line", &session::accept_line, this);
	lesh_action_register(&actions, "cancel_line", &session::cancel_line, this);
	lesh_action_register(&actions, "end_of_file", &session::end_of_file, this);
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
	// U+000D (Ctrl-M), but `enter_raw` forces only the four bits the editor needs
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
}

void session::refresh_prompt() {
	if (_providers.prompt == nullptr)
		return;
	// WRITTEN FROM THE SHELL THREAD, and safe because of WHEN. This runs inside
	// `execute`, and the loop is blocked in `wait_on_shell`'s poll for the reply
	// this execution is about to post - it renders nothing and reads no option
	// until then. The happens-before edge is the channel's mutex: the shell posts
	// under it and the loop drains under it, so the write below is visible to
	// every read that follows the reply. ADR-0009 in one line: the shell owns its
	// state, and leshper reads it at a moment the shell chose.
	_providers.prompt->left(_prompt_scratch);
	_loop.options().prompt = _prompt_scratch;
	_providers.prompt->continuation(_prompt_scratch);
	_loop.options().continuation = _prompt_scratch;
}

std::int32_t session::execute(std::string_view line) {
	// A CANCEL ARRIVES AS AN EMPTY LINE (see event_loop::finish_cancelled_line):
	// nothing to run, at a command boundary. `$?` = 130 and the INT trap are what
	// it is for.
	if (_cancelled.exchange(false, std::memory_order_relaxed))
		_executor.interrupt_at_prompt();

	if (!line.empty()) {
		// F-34 and #113: the entry goes in with its newlines, before it runs, so
		// a command that ends the session is still in the history. A blank line
		// is not history in any shell.
		if (_providers.store != nullptr
		    && line.find_first_not_of(" \t\n") != std::string_view::npos)
			(void)_providers.store->append(line);
		(void)_executor.run_input(line);
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
