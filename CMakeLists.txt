cmake_minimum_required(VERSION 3.21)

project(lesh CXX C)

# ---------------------------------------------------------------------------
# Toolchain policy
#
# Homebrew LLVM is required; Apple clang is not supported. Apple's toolchain
# ships no LeakSanitizer ("detect_leaks is not supported on this platform"),
# and proving the substrate does not leak is a gate we cannot reach without it.
#
# Configure through a preset, which pins the compiler:
#   cmake --preset debug && cmake --build --preset debug
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND NOT LESH_ALLOW_APPLECLANG)
	message(FATAL_ERROR
		"Apple clang is not supported: it ships no LeakSanitizer, so the substrate\n"
		"  leak gate cannot run, and a green build would be meaningless.\n"
		"  Fix:  brew install llvm  &&  cmake --preset debug\n"
		"  To build anyway, without leak detection: -DLESH_ALLOW_APPLECLANG=ON")
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	message(FATAL_ERROR
		"lesh is clang-only by policy. Found: ${CMAKE_CXX_COMPILER_ID}")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -stdlib=libc++ -lc++abi")

# ---------------------------------------------------------------------------
# Shared build options.
#
# Attached per target, never to global CMAKE_CXX_FLAGS, so vendored
# dependencies are neither instrumented nor held to our warning policy.
# Sanitizers were previously global, which meant every ASan build also rebuilt
# and instrumented googletest.
# ---------------------------------------------------------------------------
add_library(lesh_options INTERFACE)
target_compile_features(lesh_options INTERFACE cxx_std_23)
target_include_directories(lesh_options INTERFACE "${PROJECT_SOURCE_DIR}/src")
target_compile_options(lesh_options INTERFACE
	-Wall -Wextra
	# Designated array initializers are a C99 extension, used deliberately for
	# the character lookup tables. We are clang-only by policy, so this is a
	# choice rather than an accident.
	-Wno-c99-designator
	# Partial designated initializers are the whole point of the ABI's OPTIONS
	# STRUCT convention (#157: required arguments positional, optionals in a
	# by-value struct - `{ .style = "magenta", .prefix = "on " }` leaves every
	# unnamed field at its zero default). Naming every field defeats it, so this
	# warning is off by policy rather than by omission.
	-Wno-missing-designated-field-initializers
	# Release treats warnings as errors. The hygiene categories below are
	# demoted to plain warnings for now: they are noisy in code that ADR-0002
	# and the module-layout ticket are about to delete or move wholesale. They
	# still print, so they cannot hide; they must reach zero before module
	# layout lands, at which point these lines come out.
	$<$<CONFIG:Release>:-Werror>
	$<$<CONFIG:Release>:-Wno-error=unused-parameter>
	$<$<CONFIG:Release>:-Wno-error=unused-private-field>
	$<$<CONFIG:Release>:-Wno-error=unused-variable>
	$<$<CONFIG:Release>:-Wno-error=reorder-ctor>
	$<$<CONFIG:Release>:-Wno-error=deprecated-copy-with-user-provided-copy>
	$<$<CONFIG:Debug>:-fno-omit-frame-pointer;-fsanitize=address,undefined;-fno-sanitize-recover=undefined>
	$<$<CONFIG:RelWithDebInfo>:-fno-omit-frame-pointer>)
target_link_options(lesh_options INTERFACE
	$<$<CONFIG:Debug>:-fsanitize=address,undefined>)
# Assertions live everywhere except Release: a profiling run should still catch
# a broken invariant rather than produce a fast wrong answer.
target_compile_definitions(lesh_options INTERFACE
	$<$<NOT:$<CONFIG:Release>>:LESH_ENABLE_ASSERTS>)

# ---------------------------------------------------------------------------
# trace-level logging.
#
# #109's cost rule: the other four levels are runtime-gated and their call sites
# ship, but `trace` is not compiled at all unless this is on - "N-1 never sees
# logging in release", literally. The debug and bench presets turn it on and the
# release preset turns it off, which is where the decision is meant to be read.
#
# OFF is the default rather than ON so that a build configured WITHOUT a preset
# also ships no trace call sites. Presets are this project's supported way to
# configure (see the toolchain policy above), and a default that quietly puts
# keystroke logging into an unattended release build is the wrong way round.
# ---------------------------------------------------------------------------
option(LESH_TRACE_LOGGING "Compile trace-level log call sites into the binary" OFF)
target_compile_definitions(lesh_options INTERFACE
	$<$<BOOL:${LESH_TRACE_LOGGING}>:LESH_ENABLE_TRACE_LOGGING>)

# testing
add_subdirectory("include/googletest")
enable_testing()

# ---------------------------------------------------------------------------
# Modules.
#
# One target per module, with dependencies expressed through
# target_link_libraries. This is what makes the dependency rules in the scope
# spec enforceable rather than merely documented: a module can only use what its
# target links, so a violation is a build failure instead of a review comment.
#
# Sources are listed explicitly. file(GLOB) does not re-run when a file is added,
# so new sources are silently missed until CMake is re-run by hand.
#
# Current graph:
#
#   lesh_options ──► lesh_substrate ──► lesh_syntax ──► lesh_runtime ──┐
#                            │                                          ▼
#                            └───────────► lesh_leshper ──────────► lesh_ui ─────────────────► lesh
#                                                                      │                       ▲
#                                                                      └─► lesh_leshnici ──────┘
#
# `lesh_leshnici` links `lesh_ui` (prompt modules) and `lesh_runtime`
# (extension builtins, #165); it is still named only by `src/main.cpp`.
#
# `lesh_leshper` is the EDITOR and `lesh_ui` is the HOST that drives it (#168).
#
# `lesh_fiber` (#198) hangs off `lesh_substrate` BESIDE `lesh_leshper` and is
# named by nothing above it yet - `lesh_tests` and `lesh_bench` are its only
# consumers until step 1 of #145 puts the scheduler inside `event_loop`.
#
# THE EDITOR HANGS OFF THE SUBSTRATE AND NOTHING ELSE, since #168 Phase B. It
# used to link `lesh_syntax` for the highlighter's parse and the completer's lex;
# both moved to `lesh_ui` with the rest of the knowledge, and a `syntax/` include
# under `src/leshper/` would now fail to compile. What the editor depends on is
# text, arenas, grapheme widths and logging.
#
# One directory, one namespace, one target: `src/X/Y` is `lesh::X::Y`, with
# `substrate/` the one flat exception. `lesh_ui` is the ONE library that links
# both `lesh_runtime` and `lesh_leshper`; `lesh_leshnici` is the layer above it
# and is named only by `src/main.cpp`. See CONTEXT.md's "Layering".
#
# The strangler quarantine (ADR-0002) is GONE: src/legacy/ and the lesh_legacy
# target were deleted by #28, along with the third-party trees only they linked -
# replxx, sol2 and Lua. What replaced legacy is syntax/ and runtime/, which never
# linked any of them, so nothing here inherits a dependency by accident.
# ---------------------------------------------------------------------------

# substrate: arena, containers, assertions, character classification, the logger.
# Depends on nothing.
#
# STATIC rather than INTERFACE since #120: the logger owns two descriptors and a
# gate word, and one translation unit has to own them or every linked target
# would get its own copy of the state and the gate a keystroke checks would not
# be the gate `configure` armed. Everything else here is still header-only.
add_library(lesh_substrate STATIC
	src/substrate/arena.h
	src/substrate/args.cpp
	src/substrate/args.h
	src/substrate/assert.h
	src/substrate/char_utils.h
	src/substrate/fork_guard.h
	src/substrate/grapheme.h
	src/substrate/inline_vector.h
	src/substrate/log.cpp
	src/substrate/log.h
	src/substrate/measure.h
	src/substrate/traits.h
	src/substrate/unicode_tables.h)
target_link_libraries(lesh_substrate PUBLIC lesh_options)

# syntax: lexer, tokens, and (later) the parser. The replacement front end.
# Depends only on the substrate - never on legacy, never on the executor. That is
# the dependency rule from the scope spec, made enforceable: this target does not
# link them, so reaching for them will not compile.
add_library(lesh_syntax STATIC
	src/syntax/ast.h
	src/syntax/lexer.cpp
	src/syntax/lexer.h
	src/syntax/parser.cpp
	src/syntax/parser.h
	src/syntax/token.h)
target_link_libraries(lesh_syntax PUBLIC lesh_substrate)

# runtime: expansion, and later shell state and execution. Depends on syntax and
# the substrate; never on legacy, never on the editor.
add_library(lesh_runtime STATIC
	src/runtime/arithmetic.cpp
	src/runtime/arithmetic.h
	src/runtime/builtins.cpp
	src/runtime/builtins.h
	src/runtime/executor.cpp
	src/runtime/executor.h
	src/runtime/expander.cpp
	src/runtime/glob.cpp
	src/runtime/glob.h
	src/runtime/history_store.cpp
	src/runtime/history_store.h
	src/runtime/pattern.cpp
	src/runtime/pattern.h
	src/runtime/expander.h
	src/runtime/invocation.cpp
	src/runtime/invocation.h
	src/runtime/shell_state.cpp
	src/runtime/signals.cpp
	src/runtime/signals.h
	src/runtime/shell_state.h)
target_link_libraries(lesh_runtime PUBLIC lesh_syntax)

# leshper: the line editor's state machine, its events and its effects.
#
# THE EDITOR, AND NOTHING THAT DRIVES IT (#168). `loop`, `tty`, `workers` and
# `shell_actor` were here and are `lesh_ui`'s now: leshper is
# `step(state, event, now) -> effects` with no thread, fd, poll, timer or
# mailbox in it, and a host sends it events and performs its effects. What is
# left here has no `<poll.h>`, no `<termios.h>`, and no thread, fd or clock of
# its own - `<thread>` appears once, in `registry.cpp`, to hash a thread id for
# handle ownership.
#
# Links THE SUBSTRATE AND NOTHING ELSE (#168 Phase B). Not lesh_runtime, which
# is spec §4.4's rule made enforceable the same way every other one here is - a
# call into execution does not compile rather than being caught in review - and
# no longer lesh_syntax either. Running user code goes through the port #92
# shaped, whose lane-2 half is a spawn_request effect the host carries out;
# nothing in this target executes, parses or lexes anything.
#
# AND NOT THE PROMPT, since #170. `src/leshper/prompt/` was here; it is
# `src/ui/prompt/` now, because a prompt renders SHELL FACTS - cwd, exit status,
# jobs, time, the git branch - and that is host knowledge. The editor takes
# prompt BYTES (`loop_options::prompt` and a pen) and nothing else, and
# `abi.h` split at the same seam: the `lesh_prompt_*` verbs are
# `ui/prompt/abi.h`'s, and the registry carries the engine as an opaque `void*`
# so that no header here names `ui::prompt::engine`.
#
# AND NOT lesh_syntax EITHER, since #168 Phase B. It was linked here for one
# client: the highlighter called `parse()` and `complete.cpp` lexed to find the
# token under the cursor. Both were KNOWLEDGE - what a line MEANS and what a name
# COULD be - and both are `lesh_ui`'s now, so no file under `src/leshper/`
# includes `syntax/` at all. The editor's dependency is the substrate and nothing
# else: text, arena, grapheme widths, logging. C-1 to C-6 are still leshper's
# public API in the sense §4 means - the layer above may call them - but the
# editor itself does not, and the link line is what says so.
add_library(lesh_leshper STATIC
	src/leshper/decode.cpp
	src/leshper/decode.h
	src/leshper/blit.cpp
	src/leshper/blit.h
	src/leshper/abi.h
	src/leshper/builtin_actions.cpp
	src/leshper/decoration.cpp
	src/leshper/decoration.h
	src/leshper/editor.cpp
	src/leshper/editor.h
	src/leshper/effect.h
	src/leshper/host.h
	src/leshper/layout.cpp
	src/leshper/layout.h
	src/leshper/pager.cpp
	src/leshper/pager.h
	src/leshper/pager_actions.cpp
	src/leshper/proposal.cpp
	src/leshper/proposal.h
	src/leshper/sgr.h
	src/leshper/style_grammar.h
	src/leshper/theme.h
	src/leshper/registry.cpp
	src/leshper/registry.h
	src/leshper/event.h
	src/leshper/keymap.cpp
	src/leshper/keymap.h
	src/leshper/kill_store.cpp
	src/leshper/kill_store.h
	src/leshper/vi.cpp
	src/leshper/vi.h
	src/leshper/state.h
	src/leshper/surface.cpp
	src/leshper/surface.h
	src/leshper/text.h
	src/leshper/undo.h)
target_link_libraries(lesh_leshper PUBLIC lesh_substrate)

# fiber: stackful cooperative fibers - the switcher, guard-page stacks, an
# instantiable scheduler and the one channel type v1 needs (#198, part of #82).
#
# BESIDE lesh_leshper, NOT ABOVE IT, and for the same reason: it links
# lesh_substrate AND NOTHING ELSE. That is the whole sweep-exemption argument for
# `Fiber*`, made enforceable the way every other rule here is - this target does
# not link lesh_syntax, lesh_runtime or lesh_ui, so a fiber that reached for a
# parse tree, a shell variable or a file descriptor would fail to link rather
# than fail review. What it depends on is assertions, the logger and libc.
#
# NOTHING LINKS IT YET. Step 0 is the probe and the scaffold: `lesh_tests` runs
# `Fiber*` and `lesh_bench` times a switch, and the shell binary does not name
# this target at all. Step 1 (#145) is what puts the scheduler inside
# `event_loop` and makes the reactors fibers; until then no shipped behaviour
# depends on a single line here.
#
# The vendored switcher is PRIVATE: `src/fiber/minicoro.cpp` is the one
# translation unit that compiles minicoro, and no header above it names an
# `mco_*` type - `stack.h` and `scheduler.h` forward-declare `struct mco_coro`
# and stop there. SYSTEM plus `-w` on that one source keeps the vendoring promise
# at the top of this file: somebody else's single-header library is neither held
# to our warning policy nor allowed to fail a `-Werror` release build. It IS
# instrumented, deliberately and unlike the other vendored trees - minicoro's
# `__sanitizer_*_switch_fiber` annotations are compiled in from
# `__has_feature(address_sanitizer)`, so a TU built without `-fsanitize=address`
# would hand ASan no fiber annotations at all and the gate would be measuring
# nothing.
add_library(lesh_fiber STATIC
	src/fiber/minicoro.cpp
	src/fiber/scheduler.cpp
	src/fiber/scheduler.h
	src/fiber/slot.h
	src/fiber/stack.cpp
	src/fiber/stack.h)
target_link_libraries(lesh_fiber PUBLIC lesh_substrate)
target_include_directories(lesh_fiber SYSTEM PRIVATE
	"${PROJECT_SOURCE_DIR}/third_party/minicoro")
set_source_files_properties(src/fiber/minicoro.cpp PROPERTIES COMPILE_OPTIONS "-w")


# ui: THE HOST. The session that runs an interactive shell, the loop that drives
# the editor, the terminal, the workers, the timers, and the adapters that carry
# shell facts across (#164, #168).
#
# It owns everything with a thread, a descriptor or a clock in it, and drives
# leshper by sending events and performing the effects that come back - and by
# nothing else. `src/ui/loop.cpp` is the only caller of `step`.
#
# AND IT OWNS THE PROMPT ENGINE (#170). `src/ui/prompt/` is the engine, the
# seven built-in modules, the template language and the `lesh_prompt_*` ABI
# (`ui/prompt/abi.h`) - host state, because what a prompt renders is shell
# facts. `session.cpp` holds the one engine and hands the editor bytes.
#
# THE ONE LIBRARY THAT LINKS BOTH HALVES. `lesh_leshper` does not link
# `lesh_runtime` - spec §4.4 - so the shapes leshper declares have to be filled
# in somewhere that sees both, and this is that somewhere. Nothing in
# `lesh_leshper` may include `ui/`; the arrow points one way, and a file that
# drifted the other way would fail to link rather than fail review.
#
# NOT leshnici, which sits ABOVE this rather than beside it: the shipped
# extension set arrives through `run_interactive_shell`'s installer hook, which
# `src/main.cpp` fills in with `install_prompt_modules`. A
# `#include "leshnici/..."` from here would not link.
add_library(lesh_ui STATIC
	src/ui/completion.cpp
	src/ui/completion.h
	src/ui/editor_host.cpp
	src/ui/editor_host.h
	src/ui/history_search.cpp
	src/ui/history_search.h
	src/ui/history/blob.cpp
	src/ui/history/blob.h
	src/ui/history/store.cpp
	src/ui/history/store.h
	src/ui/history/history_generated.h
	src/ui/history/locking.cpp
	src/ui/history/locking.h
	src/ui/history/log.cpp
	src/ui/history/log.h
	src/ui/history/vacuum.cpp
	src/ui/history/vacuum.h
	src/ui/history/watch.cpp
	src/ui/history/watch.h
	src/ui/loop.cpp
	src/ui/loop.h
	src/ui/prompt/abi.h
	src/ui/prompt/prompt.cpp
	src/ui/prompt/prompt.h
	src/ui/prompt/module.h
	src/ui/prompt/module_duration.h
	src/ui/prompt/module_env.h
	src/ui/prompt/module_jobs.h
	src/ui/prompt/module_mode.h
	src/ui/prompt/module_path.h
	src/ui/prompt/module_status.h
	src/ui/prompt/module_time.h
	src/ui/prompt/modules.h
	src/ui/reactor_call.cpp
	src/ui/reactor_call.h
	src/ui/reactors.cpp
	src/ui/reactors.h
	src/ui/session.cpp
	src/ui/session.h
	src/ui/shell_knowledge.h
	src/ui/shell_actor.cpp
	src/ui/shell_actor.h
	src/ui/shell_side.h
	src/ui/shell_state_knowledge.h
	src/ui/tty.cpp
	src/ui/tty.h
	src/ui/workers.cpp
	src/ui/workers.h)
target_link_libraries(lesh_ui PUBLIC lesh_leshper lesh_runtime)
# The vendored FlatBuffers runtime (#191, ADR-0010): Tier 1 of the history is
# one mmap'd FlatBuffer, and `src/ui/history/blob.cpp` plus the committed
# `history_generated.h` are the only files in the tree that include it.
#
# HEADER-ONLY, SO ADR-0005 IS UNTOUCHED - there is no library to link and no new
# runtime shared object; the templates compile into `lesh_ui` and vanish. There
# is also no build-time codegen: `flatc` is not a build dependency, because the
# generated header is committed and a test (not the build) is what checks it is
# current. See `third_party/flatbuffers/README.lesh.md`.
#
# SYSTEM, because the block at the top of this file promises that vendored code
# is not held to our warning policy, and `-Werror` in Release would otherwise
# make somebody else's header our problem.
#
# PUBLIC and not PRIVATE for one reason only: the header self-containment target
# below compiles every `src/ui/**.h` on its own, `history_generated.h` among
# them, and it needs the path to do it. No lesh header above `blob.cpp` includes
# flatbuffers - `blob.h` deliberately hides the builder behind a pimpl - so the
# dependency is invisible to `lesh_leshnici` and to the tests despite this.
target_include_directories(lesh_ui SYSTEM PUBLIC
	"${PROJECT_SOURCE_DIR}/third_party/flatbuffers/include")

# leshnici: the shipped extension set (#163).
#
# A layer ABOVE leshper, not inside it. What lives here is everything lesh ships
# on top of the editor's own machinery but does not owe it - prompt modules now,
# builtins later - and the direction of the arrow is the whole point: leshnici
# links lesh_ui, and neither ui nor leshper knows leshnici exists, so a module
# that reached back into the engine's internals would fail to link rather than
# fail review. Nothing in leshper's own targets may name a symbol from here.
#
# `src/main.cpp` installs it, since #164: it passes
# `leshnici::install_prompt_modules` to `run_interactive_shell` as the session's
# extension hook. `lesh_ui` does not link this target - leshnici is ABOVE ui -
# so an engine built anywhere else, a unit test or a tool, is the bare
# seven-module one.
add_library(lesh_leshnici STATIC
	src/leshnici/coreutils/builtins.cpp
	src/leshnici/coreutils/builtins.h
	src/leshnici/coreutils/byte_stream.h
	src/leshnici/coreutils/cat.cpp
	src/leshnici/coreutils/head.cpp
	src/leshnici/coreutils/ls.cpp
	src/leshnici/coreutils/tail.cpp
	src/leshnici/leshnici.h
	src/leshnici/prompt/git.h
	src/leshnici/prompt/git_head.cpp
	src/leshnici/prompt/git_head.h
	src/leshnici/prompt/modules.cpp
	src/leshnici/prompt/modules.h)
# BOTH HALVES OF THE TREE, and #165 is what added the second. The prompt modules
# need `lesh_ui` (they register against `ui::prompt::engine`, #170); the extension builtins
# need `lesh_runtime` (they are `builtin_result (*)(shell_state&, char**)` and
# they install onto a `shell_state`). The direction of both arrows is unchanged
# and is the whole point: leshnici names them, neither of them names leshnici, so
# a builtin that reached back into the shell's own tables would fail to link
# rather than fail review.
target_link_libraries(lesh_leshnici PUBLIC lesh_ui lesh_runtime)

add_subdirectory("tools")

# The `lesh` binary. The session lives in lesh_ui now (#164); what is left here
# is the process - argument parsing, the floor check, the rc lookup, logging -
# and the ONE call that names the shipped extension set.
#
# THE ONLY PLACE IN THE TREE THAT SAYS `leshnici`. `lesh_ui` links
# `lesh_leshper` and `lesh_runtime` and not this, because leshnici is the layer
# ABOVE ui - the arrow runs leshnici -> ui, never back; main.cpp passes
# `&leshnici::install_prompt_modules` into
# `run_interactive_shell` and the session calls it on the engine it built.
#
# The graph is stated ONCE, on the libraries' PUBLIC link lines above; an
# executable names only the top of it. Listing the whole stack here as well
# handed every archive to the linker twice ("ld: warning: ignoring duplicate
# libraries"), and said nothing the libraries had not already said.
add_executable(lesh src/main.cpp)
target_link_libraries(lesh PRIVATE lesh_leshnici)

add_executable(lesh_tests
	tests/base_tests.cpp
	tests/unit/allocation_tests.cpp
	tests/unit/arithmetic_tests.cpp
	tests/unit/builtin_registry_tests.cpp
	tests/unit/cd_tests.cpp
	tests/unit/arena_tests.cpp
	tests/unit/lexer_tests.cpp
	tests/unit/leshper_selection_tests.cpp
	tests/unit/ui_highlight_tests.cpp
	tests/unit/ui_autosuggest_tests.cpp
	tests/unit/leshper_tests.cpp
	tests/unit/leshper_paste_tests.cpp
	tests/unit/ui_pty_tests.cpp
	tests/unit/ui_session_tests.cpp
	tests/unit/leshper_replay_tests.cpp
	tests/unit/log_tests.cpp
	tests/unit/leshper_decoration_tests.cpp
	tests/unit/leshper_layout_tests.cpp
	tests/unit/leshper_pager_tests.cpp
	tests/unit/leshnici_builtins_tests.cpp
	tests/unit/leshnici_git_tests.cpp
	tests/unit/ui_prompt_tests.cpp
	tests/unit/leshper_style_grammar_tests.cpp
	tests/unit/ui_style_grammar_tests.cpp
	tests/unit/leshper_proposal_tests.cpp
	tests/unit/ui_proposal_tests.cpp
	tests/unit/ui_loop_tests.cpp
	tests/unit/ui_workers_tests.cpp
	tests/unit/args_tests.cpp
	tests/unit/executor_tests.cpp
	tests/unit/getopts_tests.cpp
	tests/unit/grapheme_tests.cpp
	tests/unit/history_store_tests.cpp
	tests/unit/ui_history_blob_tests.cpp
	tests/unit/ui_history_log_tests.cpp
	tests/unit/ui_history_search_tests.cpp
	tests/unit/ui_history_property_tests.cpp
	tests/unit/ui_history_stale_tests.cpp
	tests/unit/ui_history_tests.cpp
	tests/unit/ui_history_vacuum_tests.cpp
	tests/unit/leshper_keymap_tests.cpp
	tests/unit/ui_bind_tests.cpp
	tests/unit/leshper_vi_tests.cpp
	tests/unit/ui_command_kind_tests.cpp
	tests/unit/ui_knowledge_tests.cpp
	tests/unit/ui_completion_tests.cpp
	tests/unit/invocation_tests.cpp
	tests/unit/measure_tests.cpp
	tests/unit/options_tests.cpp
	tests/unit/expander_tests.cpp
	tests/unit/fiber_tests.cpp
	tests/unit/pattern_tests.cpp
	tests/unit/parser_tests.cpp
	tests/unit/prompt_builtin_tests.cpp
	tests/unit/read_tests.cpp
	tests/unit/readonly_tests.cpp
	tests/unit/test_builtin_tests.cpp
	tests/unit/signal_tests.cpp
	tests/unit/substrate_contract_tests.cpp)
# `lesh_fiber` is named here explicitly because nothing else in the tree links
# it yet (#198): the tests and the compass are its only consumers until step 1
# puts the scheduler inside `event_loop`.
target_link_libraries(lesh_tests PRIVATE lesh_leshnici lesh_fiber gtest_main)
# The grapheme tests read the vendored UCD conformance file rather than a
# generated fixture, so a stale table cannot be validated by an equally stale
# expectation (#108).
target_compile_definitions(lesh_tests PRIVATE
	LESH_UCD_DIR="${PROJECT_SOURCE_DIR}/third_party/ucd")
# Same argument one directory over (#191): `history_generated.h` is committed
# rather than generated by the build, so something has to prove it is still what
# `flatc` emits from `history.fbs`. `UiHistoryBlobSchema.GeneratedHeaderIsCurrent`
# regenerates and diffs when `flatc` is on PATH and skips when it is not - so a
# machine without the schema compiler does not report a false regression, and a
# machine with one catches a hand-edited generated file.
target_compile_definitions(lesh_tests PRIVATE
	LESH_HISTORY_SCHEMA_DIR="${PROJECT_SOURCE_DIR}/src/ui/history")
# The pty test drives the REAL binary rather than a session forked out of the
# test process (#134). It has to: `loop.cpp` captures the process's pid at static
# initialization to tell a forked child from the shell, so a session forked out
# of an already-running gtest binary sees every signal as "delivered to a child"
# and re-raises it. Exec'ing the shell is also the honest test - main.cpp's floor
# check, its rc lookup and its logging setup are all on the path.
target_compile_definitions(lesh_tests PRIVATE LESH_BINARY="$<TARGET_FILE:lesh>")
add_dependencies(lesh_tests lesh)

add_test(NAME lesh_tests COMMAND lesh_tests)

# THE LSan POSITIVE CONTROL (#198). `FiberLsan.ABlockHeldOnlyByAParkedFiberStackIsNotReported`
# asserts that LeakSanitizer does NOT report a block reachable only from a parked
# fiber's mmap'd stack - and a negative assertion about a detector is worthless
# unless something proves the detector was awake. This is that something: the
# same allocation, on the same kind of fiber frame, with the stack unmapped
# underneath it, run in a process of its own.
#
# WILL_FAIL inverts the verdict, so the case passes when the process exits
# non-zero (LSan reported the leak) and FAILS when the process exits clean. The
# day LSan stops seeing this shape - a sanitizer upgrade, a platform change, a
# stray `detect_leaks=0` - this case goes red on an otherwise green gate, which
# is the only way the negative control's silence keeps meaning anything.
#
# Debug only: there is no LeakSanitizer in the other configurations, so the
# process would exit 0 and WILL_FAIL would report a failure that is really just
# "you are not running the gate".
#
# The test itself is DISABLED_ in the binary, so the ordinary `lesh_tests` run
# does not deliberately leak 500 KB; only this case runs it, by name.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
	add_test(NAME fiber_lsan_positive_control
		COMMAND lesh_tests
			--gtest_also_run_disabled_tests
			--gtest_filter=FiberLsanPositiveControl.DISABLED_DroppedBlockIsReported)
	set_tests_properties(fiber_lsan_positive_control PROPERTIES
		WILL_FAIL TRUE
		ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1")
endif()

# Differential conformance: every snippet in tests/spec runs through lesh and
# through a reference shell, and stdout plus exit status must match. dash is
# authoritative for the POSIX floor (ADR-0001); zsh only for the curated layer.
# Skipped rather than failed when the reference shell is absent, so a machine
# without dash does not report a false regression.
# Header self-containment. Every header must compile on its own, with nothing
# included before it. Splitting the monolith turned up two headers that only
# compiled because something else had already pulled in <algorithm> and
# <functional> - the kind of defect that stays invisible until an unrelated
# include is removed, and then breaks a file nobody touched.
file(GLOB_RECURSE LESH_PUBLIC_HEADERS
	RELATIVE ${PROJECT_SOURCE_DIR}/src
	${PROJECT_SOURCE_DIR}/src/substrate/*.h
	${PROJECT_SOURCE_DIR}/src/syntax/*.h
	${PROJECT_SOURCE_DIR}/src/fiber/*.h
	${PROJECT_SOURCE_DIR}/src/leshper/*.h
	${PROJECT_SOURCE_DIR}/src/leshnici/*.h
	${PROJECT_SOURCE_DIR}/src/runtime/*.h
	${PROJECT_SOURCE_DIR}/src/ui/*.h)
set(SELF_CONTAINMENT_DIR ${CMAKE_CURRENT_BINARY_DIR}/self_containment)
file(MAKE_DIRECTORY ${SELF_CONTAINMENT_DIR})
set(SELF_CONTAINMENT_SOURCES "")
foreach(header ${LESH_PUBLIC_HEADERS})
	string(REPLACE "/" "_" stem "${header}")
	string(REPLACE ".h" "" stem "${stem}")
	set(generated ${SELF_CONTAINMENT_DIR}/selftest_${stem}.cpp)
	file(WRITE ${generated} "#include \"${header}\"\n")
	list(APPEND SELF_CONTAINMENT_SOURCES ${generated})
endforeach()
add_library(lesh_header_self_containment OBJECT ${SELF_CONTAINMENT_SOURCES})
target_link_libraries(lesh_header_self_containment PRIVATE lesh_leshnici lesh_fiber)

# The committed src/substrate/unicode_tables.h must be exactly what the
# generator emits from the vendored UCD (#108). The 766 conformance cases prove
# the rules agree with the table; only this proves the table agrees with Unicode.
# A hand-edited or half-regenerated table is a wrong answer that still compiles.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
	add_test(NAME unicode_tables_current
		COMMAND ${Python3_EXECUTABLE}
			${PROJECT_SOURCE_DIR}/tools/gen_unicode_tables.py --check)
endif()

find_program(DASH_EXECUTABLE dash)
if(DASH_EXECUTABLE AND Python3_Interpreter_FOUND)
	# ONE axis, because there is one shell. The corpus ran twice - once per front
	# end - for as long as both were live (#8); #28 deleted legacy, and with it the
	# axis that measured a quarantine against dash.
	add_test(NAME spec_dash
		COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/spec_run.py
			--shell $<TARGET_FILE:lesh>
			--ref ${DASH_EXECUTABLE}
			${PROJECT_SOURCE_DIR}/tests/spec)
else()
	message(STATUS "spec_dash test skipped: need dash and python3")
endif()
