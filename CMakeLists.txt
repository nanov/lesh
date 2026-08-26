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
#   lesh_options ──► lesh_substrate ──► lesh_syntax ──► lesh_runtime ──► lesh
#                            │                                              ▲
#                            └──► lesh_ui ──────────────────────────────────┘
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
	src/substrate/assert.h
	src/substrate/char_utils.h
	src/substrate/fork_guard.h
	src/substrate/grapheme.h
	src/substrate/log.cpp
	src/substrate/log.h
	src/substrate/measure.h
	src/substrate/traits.h
	src/substrate/unicode_tables.h)
target_link_libraries(lesh_substrate PUBLIC lesh_options)

# ui: terminal presentation. Depends only on the substrate.
add_library(lesh_ui STATIC
	src/ui/lolcat.cpp
	src/ui/lolcat.h)
target_link_libraries(lesh_ui PUBLIC lesh_substrate)

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
# Links syntax and the substrate, and NOT lesh_runtime. That is spec §4.4's rule
# made enforceable the same way every other one here is: the editor may depend on
# the lexer, the parser and shell state, never on the executor - so a call into
# execution does not compile rather than being caught in review. Running user
# code goes through the port #92 shaped, whose lane-2 half is a spawn_request
# effect the loop carries out; nothing in this target executes anything.
#
# syntax is linked ahead of its first use: C-1 to C-6 froze as leshper's public
# API when #104 landed, and the highlighter reactor is the client that will call
# it. Nothing here calls it yet.
add_library(lesh_leshper STATIC
	src/leshper/decode.cpp
	src/leshper/decode.h
	src/leshper/blit.cpp
	src/leshper/blit.h
	src/leshper/abi.h
	src/leshper/builtin_actions.cpp
	src/leshper/complete.cpp
	src/leshper/complete.h
	src/leshper/builtin_reactors.cpp
	src/leshper/editor.cpp
	src/leshper/editor.h
	src/leshper/effect.h
	src/leshper/layout.cpp
	src/leshper/layout.h
	src/leshper/pager.cpp
	src/leshper/pager.h
	src/leshper/pager_actions.cpp
	src/leshper/sgr.h
	src/leshper/loop.cpp
	src/leshper/loop.h
	src/leshper/shell_actor.cpp
	src/leshper/shell_actor.h
	src/leshper/tty.cpp
	src/leshper/tty.h
	src/leshper/registry.cpp
	src/leshper/registry.h
	src/leshper/shell_knowledge.h
	src/leshper/event.h
	src/leshper/history_search.cpp
	src/leshper/history_search.h
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
	src/leshper/undo.h
	src/leshper/workers.cpp
	src/leshper/workers.h)
target_link_libraries(lesh_leshper PUBLIC lesh_syntax)

add_subdirectory("tools")

# The `lesh` binary, and the ONE place both halves meet.
#
# src/leshper/read.cpp is compiled HERE rather than into lesh_leshper, and that
# is the rule enforcing itself (spec §4.4): it includes runtime headers, and
# lesh_leshper does not link lesh_runtime, so a file that drifted into the
# editor's target would fail to link rather than fail review. The same goes for
# src/leshper/shell_state_knowledge.h, which is in no library target at all.
add_executable(lesh src/main.cpp src/leshper/read.cpp src/leshper/read.h)
target_link_libraries(lesh PRIVATE lesh_leshper lesh_runtime lesh_syntax lesh_ui)

add_executable(lesh_tests
	src/leshper/read.cpp
	tests/base_tests.cpp
	tests/unit/allocation_tests.cpp
	tests/unit/arithmetic_tests.cpp
	tests/unit/builtin_registry_tests.cpp
	tests/unit/cd_tests.cpp
	tests/unit/arena_tests.cpp
	tests/unit/lexer_tests.cpp
	tests/unit/leshper_selection_tests.cpp
	tests/unit/leshper_highlight_tests.cpp
	tests/unit/leshper_autosuggest_tests.cpp
	tests/unit/leshper_tests.cpp
	tests/unit/leshper_paste_tests.cpp
	tests/unit/leshper_pty_tests.cpp
	tests/unit/leshper_read_tests.cpp
	tests/unit/leshper_replay_tests.cpp
	tests/unit/log_tests.cpp
	tests/unit/leshper_layout_tests.cpp
	tests/unit/leshper_pager_tests.cpp
	tests/unit/leshper_loop_tests.cpp
	tests/unit/leshper_workers_tests.cpp
	tests/unit/executor_tests.cpp
	tests/unit/getopts_tests.cpp
	tests/unit/grapheme_tests.cpp
	tests/unit/history_store_tests.cpp
	tests/unit/leshper_history_search_tests.cpp
	tests/unit/leshper_keymap_tests.cpp
	tests/unit/leshper_vi_tests.cpp
	tests/unit/leshper_knowledge_tests.cpp
	tests/unit/invocation_tests.cpp
	tests/unit/measure_tests.cpp
	tests/unit/options_tests.cpp
	tests/unit/expander_tests.cpp
	tests/unit/pattern_tests.cpp
	tests/unit/parser_tests.cpp
	tests/unit/read_tests.cpp
	tests/unit/readonly_tests.cpp
	tests/unit/test_builtin_tests.cpp
	tests/unit/signal_tests.cpp
	tests/unit/substrate_contract_tests.cpp)
target_link_libraries(lesh_tests PRIVATE lesh_leshper lesh_runtime lesh_syntax lesh_ui gtest gtest_main)
# The grapheme tests read the vendored UCD conformance file rather than a
# generated fixture, so a stale table cannot be validated by an equally stale
# expectation (#108).
target_compile_definitions(lesh_tests PRIVATE
	LESH_UCD_DIR="${PROJECT_SOURCE_DIR}/third_party/ucd")
# The pty test drives the REAL binary rather than a session forked out of the
# test process (#134). It has to: `loop.cpp` captures the process's pid at static
# initialization to tell a forked child from the shell, so a session forked out
# of an already-running gtest binary sees every signal as "delivered to a child"
# and re-raises it. Exec'ing the shell is also the honest test - main.cpp's floor
# check, its rc lookup and its logging setup are all on the path.
target_compile_definitions(lesh_tests PRIVATE LESH_BINARY="$<TARGET_FILE:lesh>")
add_dependencies(lesh_tests lesh)

add_test(NAME lesh_tests COMMAND lesh_tests)

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
	${PROJECT_SOURCE_DIR}/src/leshper/*.h
	${PROJECT_SOURCE_DIR}/src/runtime/*.h)
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
target_link_libraries(lesh_header_self_containment PRIVATE lesh_leshper lesh_runtime lesh_substrate lesh_syntax)

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
