#pragma once

// WHAT THE SHELL KNOWS, as an interface THE HOST owns (#130, #135; spec §6.7,
// narrowed by ADR-0009; moved out of `src/leshper/` by #168 Phase B).
//
// The highlighter has to say whether `ll` is an alias, `cd` a builtin, `deploy`
// a function and `ls` a thing on `$PATH` (F-21). All four answers live in
// `shell_state`. This header used to be leshper's, declaring the SHAPE of the
// question so the editor could ask it without linking `lesh_runtime` - the CMake
// rule spec §4.4 made enforceable rather than reviewable. That was the right
// arrangement while the highlighter was leshper's too. It is not: what a command
// name IS is shell knowledge, and the editor only colours regions. So the whole
// question lives on this side now, `src/ui/shell_state_knowledge.h` implements it
// over the real state, and a test fakes it with a map. What crosses to the editor
// is `leshper::host::classify_command`, one `std::uint32_t` in abi.h's own
// LESH_COMMAND_* space, with everything below it - tables, `$PATH`, the sweep -
// on this side of the door.
//
// ONE OWNER, NO VERSION. #130 resolved this over a copy-on-write definitions
// version held by the request token, because the highlighter then ran on a
// worker while the loop mutated the tables. ADR-0009 dissolved that: the shell
// is the main thread, it owns `shell_state`, and a highlight, a port call and an
// execution are serialized on it. So the implementation may hand back views into
// the state's own storage - there is no second thread that could invalidate one
// mid-call - and this interface is a plain const reference, not a refcounted
// snapshot. The only version left is the editor's generation, already on the
// token.
//
// WHY TWO METHODS AND NOT ONE. #130 wrote "one method: command_kind(name)". The
// `$PATH` walk is a stat per directory and the request MEMOIZES it (F-22 keeps
// the filesystem off the keystroke path), so the walk has to run where the memo
// can see it. The shell's contribution to it is the VALUE of `$PATH` and nothing
// else. Folding the walk behind `classify` would put a filesystem sweep where
// the memo cannot see it and re-stat every candidate for every repeat of a name
// on the line. Both methods below are pure lookups; neither touches disk.
//
// The two are joined by `ui::classify_command_name` (`editor_host.h`), which is
// where the walk itself lives; the memo stayed on the request token, in leshper,
// because a cost cache that can never change an answer belongs with the caller
// (#168 Phase B).

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace lesh::ui {

// What a command name IS, in the shell's own resolution order.
//
// The numbers are the ABI's `LESH_COMMAND_*` and registry.cpp static_asserts
// that they still agree. `unknown` is zero so that a caller who ignored a
// failed status reads the harmless answer rather than a confident wrong one.
enum class command_kind : std::uint8_t {
	unknown = 0,   // no table holds it and no $PATH directory has it
	external = 1,  // a regular executable file, found by the $PATH walk or named directly
	builtin = 2,   // the static builtin registry
	function = 3,  // a shell function
	alias = 4,     // an alias
};

// WHICH NAMES the completer is asking for (#139, spec 6.9).
//
// FIVE DOMAINS AND NOT ONE MERGED LIST, and the argument is F-21's. The first
// three are the same three tables the highlighter paints distinctly - a builtin
// is not a function is not an alias - and the completion pager's only v1
// description is a kind marker (spec 6.9), so a merged list would arrive with
// the one fact the marker needs already thrown away. Re-deriving it would mean
// `classify` per candidate, which is a second crossing per NAME instead of a
// second crossing per TABLE.
//
// `path_directory` is here rather than as a `$PATH` walk on the shell side for
// exactly #135's reason: the walk is a readdir per directory and it belongs
// where the memo and the other readdir already are, which is the completer, on
// the loop. What the shell contributes is the SPLIT of its own `$PATH` - the
// value it already owns, cut on `:` with POSIX's empty-element-means-`.` rule
// applied once, in the one place that knows whether the variable was unset or
// merely empty.
enum class name_domain : std::uint8_t {
	builtin = 0,         // the static builtin registry
	function = 1,        // shell functions
	alias = 2,           // aliases
	// Variable names, for a token that leads with `$`. The names only - a value
	// is not a completion, and copying every value out would copy the
	// environment on every Tab.
	variable = 3,
	// The elements of `$PATH`, in order, already split. Directories, not
	// commands: what is IN them is the completer's readdir.
	path_directory = 4,
};

// ADR-0009's rule, made checkable (#151).
//
// THE RULE IS "THE SHELL WRITES ITS OWN STATE, AND NOBODY READS IT WHILE IT
// DOES". Everything above depends on it - the bare pointers on the request
// token, the borrowed `string_view` out of `path`, the deleted copy-on-write
// version of #130 - and until this type existed it was a paragraph in an ADR
// that a future reader could only obey by remembering. What makes the rule
// checkable is that there are exactly TWO writers, `shell_side::execute` and
// `shell_side::port_call`, and both are entered from one place: the loop
// calling them (`event_loop::accept_current_line`, `finish_cancelled_line`,
// `call_port`; it was `shell_actor` serving a slot until #201). So the loop
// raises this flag around them and every read through an adapter that holds one
// asserts it is down.
//
// WHY AN ATOMIC WHEN THE CLAIM IS THAT THERE IS NO CONCURRENCY. Because the
// claim was exactly what was being checked: the flag was written on the shell
// thread and read wherever a reader happened to be - the shell thread for the
// highlighter, the LOOP thread for the completer (see `enumerate` below) - and
// a plain `bool` read across those would be the data race the assertion exists
// to catch, which is a poor way to catch it. Relaxed on both sides: this is a
// tripwire, not a handshake, and it orders nothing.
//
// DEBUG-ONLY COST. The load lives inside `LESH_ASSERT` and compiles out in
// release; what remains is two relaxed stores per execution or port call, which
// is per COMMAND and not per keystroke.
class shell_writing_flag {
public:
	[[nodiscard]] bool writing() const noexcept {
		return _writing.load(std::memory_order_relaxed);
	}

	// Raised for the length of one write, ON THE SHELL THREAD. RAII rather than
	// a pair of calls, because the write it brackets is a `virtual` the shell
	// implements and may leave by throwing.
	class scope {
	public:
		explicit scope(shell_writing_flag* flag) noexcept : _flag(flag) {
			if (_flag != nullptr)
				_flag->_writing.store(true, std::memory_order_relaxed);
		}
		~scope() {
			if (_flag != nullptr)
				_flag->_writing.store(false, std::memory_order_relaxed);
		}

		scope(const scope&) = delete;
		scope& operator=(const scope&) = delete;

	private:
		shell_writing_flag* _flag;
	};

private:
	std::atomic<bool> _writing{false};
};

// The shell's tables, asked one name at a time.
//
// Const throughout: leshper never mutates shell state, which is the whole of
// what makes ADR-0009's single owner work.
class shell_knowledge {
public:
	shell_knowledge() = default;
	virtual ~shell_knowledge() = default;

	shell_knowledge(const shell_knowledge&) = delete;
	shell_knowledge& operator=(const shell_knowledge&) = delete;

	// Alias, then function, then builtin - POSIX's order (2.3.1 for the alias,
	// 2.9.1.1 for the rest), and the order the executor searches in.
	//
	// `unknown` means "none of the three tables", NOT "no such command": the
	// caller walks `$PATH` next. An implementation that already knows a name is
	// external may answer `external` and skip the walk; the `shell_state` adapter
	// does not, because knowing costs a stat and the token is the side that
	// memoizes those.
	//
	// ONE LEVEL OF RESOLUTION FOR AN ALIAS, and never an expansion. `ll` in
	// `alias ll='ls -l'` is an alias and the answer stops there - the body is not
	// re-resolved to see what `ls` is. #95 is why: the highlighter's spans are
	// over the bytes the user typed, and an expanded alias's tokens live in a
	// text region that no position in the line can name.
	[[nodiscard]] virtual command_kind classify(std::string_view name) const = 0;

	// The shell's `$PATH`, borrowed. False when the variable is unset, which is
	// not the same as empty: POSIX gives an empty PATH one empty element, and an
	// empty element means the current directory.
	[[nodiscard]] virtual bool path(std::string_view& out) const = 0;

	// --- The enumeration read (#130's third verb, #139, spec 6.9) -------------

	// Every name of one domain, APPENDED to `into`. The caller clears.
	//
	// COPY-OUT, WHERE THE OTHER TWO BORROW, and that is the whole point of the
	// method existing rather than an iterator or a view. `classify` and `path`
	// answer ONE question and are asked at keystroke frequency, so borrowing is
	// what keeps them free. This one hands over a WHOLE TABLE to a caller that
	// then sorts, filters and de-duplicates it, and it is asked once per Tab per
	// domain - human frequency. A view would be a view over storage that the
	// next `unset`, `alias` or function definition invalidates, held by a
	// completer that has already gone back to the loop's poll; the copy is what
	// makes the lifetime the caller's. #137 records a cached command list as the
	// v2 that makes it free.
	//
	// ASKED FROM INSIDE AN ACTION (#151), and that is legal by ADR-0009 rather
	// than in spite of it. The loop reads shell state while nothing EXECUTES,
	// and nothing can: `execute` and `port_call` are the only writers, and the
	// loop is what CALLS them - so a dispatch is by construction not inside
	// either (it was "blocked in `wait_on_shell` for the whole of each" until
	// #201, which is the same argument with a thread in it). #139 routed this
	// through a round trip on the actor's `enumerate` slot; #151 deleted the
	// slot, because a copy the loop makes for itself and a copy the shell makes
	// for it are the same copy with one less protocol. `shell_writing_flag`
	// above is the tripwire that keeps the argument true.
	//
	// NO `$PATH` WALK HERE, for the same reason #135 gave for splitting `path`
	// out of `classify`: the walk is a readdir per directory and it belongs on
	// the side that memoizes it - which is the completer, on the loop, where the
	// directory walk it is already doing lives. `path_directory` hands over the
	// SPLIT of the value and stops there. Folding the sweep in would put a
	// filesystem walk on the shell thread, serialized ahead of the next
	// execution, to answer a question the loop was about to walk anyway.
	//
	// A DEFAULTED BODY, not a pure virtual: this arrived after two
	// implementations and several fakes, and #130's growth rule for this door is
	// additive. A knowledge with no tables to walk answers nothing, which reads
	// as an empty shell rather than as an error.
	virtual void enumerate(name_domain which, std::vector<std::string>& into) const {
		(void)which;
		(void)into;
	}
};

// The answer when no shell has been attached to the token.
//
// Tables empty, `$PATH` from the process environment - which is precisely what
// the highlighter did before this door existed, and what a leshper embedded in
// something that is not this shell would want. Named rather than left as a
// silent `getenv` inside the ABI, so that "where did that PATH come from" has an
// object to point at.
//
// The environment belongs to the shell thread, which is the only thread that
// writes it; a reader on any other thread is reading a table that could be
// changing under it. That is a pre-existing property of `getenv`, unchanged
// here, and it is one more reason the wired-up path passes a real
// `shell_knowledge` instead.
class environment_knowledge final : public shell_knowledge {
public:
	[[nodiscard]] command_kind classify(std::string_view) const override {
		return command_kind::unknown;
	}

	[[nodiscard]] bool path(std::string_view& out) const override {
		const char* value = std::getenv("PATH");
		if (value == nullptr)
			return false;
		out = std::string_view{value};
		return true;
	}
};

} // namespace lesh::ui
