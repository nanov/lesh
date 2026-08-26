#pragma once

// THE UI LAYER (#134, moved here by #164): the one place lesh-side and
// leshper-side types meet.
//
// Everything below this line in the dependency graph is arranged so that the
// editor and the shell never see each other. `lesh_leshper` does not link
// `lesh_runtime` - CMakeLists says so and spec §4.4 is why - so leshper declares
// SHAPES and the shell fills them in. #168 Phase B cut that list down: the
// editor's own doors are now `leshper::host` (one door, `ui::editor_host` behind
// it, answering command kinds and completion) and `shell_side` (ADR-0009, over
// the executor), plus `binding_console` in the other direction, declared by the
// runtime for `bind` and implemented here over the keymap registry. What used to
// be leshper's `history_source` (#125), `shell_knowledge` (#135) and `completer`
// (#94) are all `lesh::ui` types now - the shell's knowledge never was the
// editor's, and the editor only colours regions and inserts what the pager
// committed.
//
// THIS FILE IS NOT IN `lesh_leshper`. `lesh_ui` is the ONE library that links
// both `lesh_leshper` and `lesh_runtime`, and this is what it is for. If any of
// it ever ended up in the editor's own target the link would fail, which is the
// rule enforcing itself - exactly what `shell_state_knowledge.h` was built to
// be adopted by.
//
// WHAT HAPPENED TO `read(providers) -> line`. #94 fixed leshper's entry as
// `read(providers) -> line`: a call that takes a provider bundle and answers one
// accepted line. ADR-0009 arrived after it and inverted the ownership - the LOOP
// is a thread and drives the read; the shell is the main thread and serves the
// loop's slots - so the accepted line is DELIVERED to `shell_side::execute`
// rather than returned to a caller that is, by then, parked in
// `shell_actor::run`. What survives unchanged is the part #94 was actually
// about: the bundle. `run_interactive_shell` takes one, the four providers are
// named types, and #102's `vared` passes a different bundle to the nested read
// the second comment on #134 describes - which is where the literal call-shaped
// `read` arrives, because a nested read genuinely does return to its caller.
//
// NOT IN v1, and each is somebody else's ticket: the completion PAGER (#138 -
// the completer itself landed with #139 and is wired below), `$PS1` EXPANSION
// (see `shell_prompt_source`), job-control UI, `vared` (#102), and the
// autosuggestion accept keys (#140, undecided - so nothing is bound to them).

#include "ui/completion.h"
#include "ui/history_search.h"

#include <string>
#include <string_view>

namespace lesh {
class buffer_pool;
}

namespace lesh::runtime {
class history_store;
class shell_state;
}

namespace lesh::ui::prompt {
class engine;
}

namespace lesh::ui {

// ---------------------------------------------------------------------------
// The four providers (#94, A-5).
// ---------------------------------------------------------------------------

// The syntax layer, and it is SEALED (#94): not an override point, because a
// user-replaceable parser would repeal C-5's one-grammar rule, which is the
// whole bug class F-20 exists to prevent. It is here as a type only so that
// `vared` can be handed the trivially-complete one (F-17) without a second code
// path - which is #94's own acceptance test for A-5.
//
// SYNCHRONOUS, ON THE THREAD THAT ASKS. #94's recorded exception: 38.7 us
// against N-1's 1 ms budget, and F-35's accept-or-insert decision cannot wait
// for a worker, because the user has already pressed Enter.
class syntax_layer {
public:
	syntax_layer() = default;
	virtual ~syntax_layer() = default;

	syntax_layer(const syntax_layer&) = delete;
	syntax_layer& operator=(const syntax_layer&) = delete;

	// F-35, and the only question the editor asks the grammar: does Enter run
	// this, or add a line to it? False for an unterminated quote, an open `if`,
	// a trailing `|` - C-2's `incomplete()`, which wins over a defect while more
	// input can still come, because the continuation prompt is the right answer
	// to an unterminated string and a syntax error is not.
	[[nodiscard]] virtual bool line_is_complete(std::string_view line) const = 0;
};

// The real one: C-6's parser, asked whether it wants more input.
class shell_syntax_layer final : public syntax_layer {
public:
	[[nodiscard]] bool line_is_complete(std::string_view line) const override;
};

// F-17's, and the reason `syntax_layer` is a type at all: `vared` edits a
// VARIABLE, where a newline is a newline and Enter always accepts. F-35
// degenerates correctly rather than being special-cased away.
class trivially_complete_syntax final : public syntax_layer {
public:
	[[nodiscard]] bool line_is_complete(std::string_view) const override { return true; }
};

// The `Prompt` provider (#94): BYTES OUT, and leshper measures the width itself.
//
// One measurer in the binary - #108's tables plus SGR/CSI skipping - so a theme
// author never does width arithmetic and zsh's `%{ %}` folklore never has to
// exist here.
//
// ON THE SHELL THREAD, always: the answer comes out of `shell_state`, which has
// one owner (ADR-0009).
class prompt_source {
public:
	prompt_source() = default;
	virtual ~prompt_source() = default;

	prompt_source(const prompt_source&) = delete;
	prompt_source& operator=(const prompt_source&) = delete;

	// F-40's left prompt, and the continuation an incomplete line gets.
	virtual void left(std::string& into) const = 0;
	virtual void continuation(std::string& into) const = 0;
};

// The POSIX defaults `shell_state`'s constructor sets. NAMED, because since
// #157 something asks about them: `session::refresh_prompt` treats a `$PS1` that
// still holds these exact bytes as a variable nobody chose, and gives the
// surface to the native prompt. One statement of what the bytes are, read by the
// source below and by the rule that compares against it.
inline constexpr std::string_view kPosixPrompt = "$ ";
inline constexpr std::string_view kPosixContinuation = "> ";

// `$PS1` and `$PS2`, AS LITERAL BYTES - and, since #157, AS THE OPT-OUT.
//
// NOT WHAT AN INTERACTIVE SHELL SHOWS BY DEFAULT, not any more. §6.10 called
// `PS1`/`PS2` a transitional stub that the native prompt supersedes, and the
// owner's ruling on #157 is that the supersession has arrived: a fresh shell
// paints the native prompt, and this class is what a user who set `$PS1` to
// something of their own keeps getting. The precedence rule itself lives at the
// ui layer (`session::refresh_prompt` in session.cpp), which is the only side
// that can see both the engine and the variables.
//
// NO EXPANSION, and it is a decision rather than an omission. #94 put "PS1
// expansion, and the owner's caching and pre-compilation ambitions" lesh-side
// BEHIND this interface, which means it is a change to this class and to nothing
// else when it lands. Until then `PS1='$ '` prints `$ ` and `PS1='\w$ '` prints
// `\w$ ` - the bytes the variable holds, unaltered - so nobody can come to
// depend on a half-implemented expansion vocabulary that the real one would then
// have to keep. That decision is UNCHANGED by the flip above, and outlives it:
// an expansion vocabulary is exactly what one does not build for a mechanism
// that is now the fallback and is documented to be dropped.
class shell_prompt_source final : public prompt_source {
public:
	explicit shell_prompt_source(const runtime::shell_state& state) noexcept
		: _state(&state) {}

	void left(std::string& into) const override;
	void continuation(std::string& into) const override;

private:
	const runtime::shell_state* _state;
};

// A fixed pair, for `vared` and for the tests.
class fixed_prompt_source final : public prompt_source {
public:
	fixed_prompt_source(std::string left_prompt, std::string continuation_prompt) noexcept
		: _left(std::move(left_prompt)), _continuation(std::move(continuation_prompt)) {}

	void left(std::string& into) const override { into.assign(_left); }
	void continuation(std::string& into) const override { into.assign(_continuation); }

private:
	std::string _left;
	std::string _continuation;
};

// #113's store, adapted onto #125's shape.
//
// The one difference between the two is that the searcher's callback can STOP -
// it has to, for the supersede poll to be more than a formality - and the
// store's returns void. So a walk that stops early here drops the REST of the
// store's walk on the floor rather than truly stopping it: the v1 store reads
// the whole file before it calls anybody, so what that costs is a loop over
// spans already in memory and no further I/O at all (#125's note).
class history_store_source final : public history_source {
public:
	explicit history_store_source(const runtime::history_store& store) noexcept
		: _store(&store) {}

	void for_each_newest_first(
		const std::function<bool(std::string_view)>& fn) const override;

private:
	const runtime::history_store* _store;
};

// What a read is given. Four providers, three of them override points and the
// first sealed.
//
// Borrowed, every one: the bundle is a view, and whoever assembled it owns the
// providers for at least as long as the read.
struct provider_bundle {
	const syntax_layer* syntax = nullptr;
	const history_source* history = nullptr;
	const prompt_source* prompt = nullptr;
	// Null means the session builds the default `shell_completer` (#139): the
	// three sources of spec 6.9 over the shell's own tables. A caller that
	// supplies one replaces the whole trio, which is #94's override point, at one
	// indirect call per Tab.
	const completer* completion = nullptr;

	// The OTHER half of #94's `HistoryStore`, and the only non-const member here.
	// `history_source` above is the read side, which is all the searcher and the
	// autosuggester ever need; appending an accepted line is the shell's, on the
	// shell thread, and it is a different verb with a different owner. Null means
	// nothing is recorded - F-17's `vared`, and every test that must not write to
	// the developer's own `~/.lesh_history`.
	runtime::history_store* store = nullptr;
};

// ---------------------------------------------------------------------------
// #97 decision 3: the floor, and the refusal below it.
// ---------------------------------------------------------------------------

// Whether `$TERM` names a terminal leshper can drive.
//
// #97's floor is ANSI escapes plus 256 colours plus bracketed paste, and the
// detection is "assume first": the trivial environment read and nothing else,
// never terminfo, never a startup query. So the only terminal that fails is one
// that has said it is not a terminal - `dumb`, or no `$TERM` at all.
//
// NOT the same question as `terminal_capabilities::from_env`, which answers what
// the terminal can DO: `NO_COLOR=1` makes an ordinary xterm monochrome and is a
// user's explicit choice, not a terminal below the floor.
[[nodiscard]] bool terminal_meets_floor(const char* term) noexcept;

// ---------------------------------------------------------------------------
// The entry.
// ---------------------------------------------------------------------------

// Runs an interactive session to its end and answers the shell's exit status.
//
// WHAT IT BUILDS, and it builds all of it here because a non-interactive shell
// must build NONE of it (#101): the editing context (actions, reactors, the
// autosuggester, the default keymaps), the helper pool, the signal hub, the
// event loop, the shell actor, and the four adapters over `shell_state`.
//
// WHAT IT DOES, in order: install the fatal-signal terminal restore, take the
// dispositions, spawn the loop thread, and serve the loop's slots on THIS
// thread - which is the main thread and the owner of `shell_state` - until the
// editor is finished. Then join, restore the terminal, and return.
//
// `in` and `out` are the descriptors the loop is a function over; nothing here
// opens `/dev/tty`, which is what lets the pty test drive the whole session.
//
// `rc_path` is #101's startup file, run INSIDE the session and before the first
// read - which is the whole of decision 3's ordering: the state exists, the
// editing context exists (so `bind` in the rc reaches a real keymap registry and
// is not "no line editor"), and only then does the loop start. Dot-script
// semantics: a missing file is silence, a syntax error abandons the rest and the
// shell still comes up. Empty means no rc, which is what `vared` and the tests
// pass.
//
// `install_extensions` is the hook the SHIPPED EXTENSION SET arrives through
// (#163, moved out of the session by #164). `lesh_ui` links `lesh_leshper` and
// `lesh_runtime` and NOT `lesh_leshnici` - leshnici sits above this layer, not
// beside it - so the session cannot name `install_prompt_modules` itself. It
// calls this instead, on the engine it has just built and before anything can
// name a module: `main.cpp` passes `&leshnici::install_prompt_modules`, and null
// - what every unit test passes - means the bare seven-module engine, on which
// `{git}` is an unknown module refused at `set`.
using prompt_extension_installer = void (*)(ui::prompt::engine&);

[[nodiscard]] int run_interactive_shell(runtime::shell_state& state, buffer_pool& pool,
                                        const provider_bundle& providers, int in, int out,
                                        std::string_view rc_path = {},
                                        prompt_extension_installer install_extensions = nullptr);

} // namespace lesh::ui
