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
# Lua: built from the vendored submodule (include/lua, pinned to v5.4.8).
# sol2 only supports Lua 5.1-5.4, so we must not pick up a system Lua 5.5.
#
# NOTE: ADR-0006 makes LuaJIT the extension runtime, and Phase 0 removes both
# Lua and sol2 from the tree. This block goes with them.
# ---------------------------------------------------------------------------
set(LUA_DIR "${PROJECT_SOURCE_DIR}/include/lua")
if(NOT EXISTS "${LUA_DIR}/lua.h")
	message(FATAL_ERROR "include/lua is empty - run: git submodule update --init --recursive")
endif()
# Guard the version explicitly: sol2 fails deep inside its headers on Lua >= 5.5,
# which surfaces as a wall of template errors rather than a useful message.
file(READ "${LUA_DIR}/lua.h" LUA_H_CONTENTS)
string(REGEX MATCH "#define[ \t]+LUA_VERSION_NUM[ \t]+([0-9]+)" LUA_VER_MATCH "${LUA_H_CONTENTS}")
set(LUA_VER "${CMAKE_MATCH_1}")
if(NOT LUA_VER OR LUA_VER LESS 501 OR LUA_VER GREATER 504)
	message(FATAL_ERROR
		"include/lua is not Lua 5.1-5.4 (sol2 supports no newer).\n"
		"  Fix: git -C include/lua checkout v5.4.8 && git add include/lua\n"
		"  Note: `git submodule update` reverts this unless the bump is staged/committed.")
endif()

file(GLOB LUA_SOURCES "${LUA_DIR}/*.c")
list(REMOVE_ITEM LUA_SOURCES
	"${LUA_DIR}/lua.c"      # standalone interpreter main()
	"${LUA_DIR}/onelua.c"   # single-file amalgamation
	"${LUA_DIR}/ltests.c")  # internal test library
add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC "${LUA_DIR}")
if(APPLE)
	target_compile_definitions(lua PUBLIC LUA_USE_MACOSX)
elseif(UNIX)
	target_compile_definitions(lua PUBLIC LUA_USE_LINUX)
endif()

# ---------------------------------------------------------------------------
# Shared build options.
#
# Attached per target, never to global CMAKE_CXX_FLAGS, so vendored
# dependencies are neither instrumented nor held to our warning policy.
# Sanitizers were previously global, which meant every ASan build also rebuilt
# and instrumented Lua, replxx and googletest.
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

add_subdirectory("include/replxx")
add_subdirectory("include/sol2")

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
#   lesh_options ──► lesh_substrate ──► lesh_legacy ──► lesh
#                            │                │           ▲
#                            └──► lesh_ui ────┴───────────┘
#
# lesh_legacy is the strangler quarantine (ADR-0002): everything scheduled for
# replacement lives there and nothing new may be added to it. The modules that
# replace it - syntax/ and runtime/ - become targets of their own as they land,
# and legacy shrinks until it is empty and the target is deleted.
# ---------------------------------------------------------------------------

# substrate: arena, containers, assertions, character classification.
# Depends on nothing. Header-only, so an INTERFACE target.
add_library(lesh_substrate INTERFACE)
target_link_libraries(lesh_substrate INTERFACE lesh_options)
target_sources(lesh_substrate INTERFACE
	src/substrate/arena.h
	src/substrate/assert.h
	src/substrate/char_utils.h
	src/substrate/grapheme.h
	src/substrate/hybrid_vector.h
	src/substrate/inline_vector.h
	src/substrate/traits.h
	src/substrate/unicode_tables.h)

# ui: terminal presentation. Depends only on the substrate.
add_library(lesh_ui STATIC
	src/ui/lolcat.cpp
	src/ui/lolcat.h)
target_link_libraries(lesh_ui PUBLIC lesh_substrate)

# legacy: the single-pass parser, its AST, and shell state. Being replaced.
# Header-only today, hence INTERFACE. Third-party dependencies are attached here
# rather than globally, so what replaces legacy does not silently inherit them.
add_library(lesh_legacy INTERFACE)
target_link_libraries(lesh_legacy INTERFACE
	lesh_substrate
	replxx::replxx
	sol2::sol2
	lua)
target_sources(lesh_legacy INTERFACE
	src/legacy/ast.h
	src/legacy/auto_complete.h
	src/legacy/executor.h
	src/legacy/prompt.h
	src/legacy/shell_state.h
	src/legacy/substrate_bridge.h
	src/legacy/zsh_parser_plus.h)

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

add_subdirectory("tools")

add_executable(lesh src/main.cpp)
target_link_libraries(lesh PRIVATE lesh_legacy lesh_runtime lesh_syntax lesh_ui)

add_executable(lesh_tests
	tests/base_tests.cpp
	tests/lesh_state_tests.cpp
	tests/unit/allocation_tests.cpp
	tests/unit/arithmetic_tests.cpp
	tests/unit/builtin_registry_tests.cpp
	tests/unit/cd_tests.cpp
	tests/unit/arena_tests.cpp
	tests/unit/lexer_tests.cpp
	tests/unit/executor_tests.cpp
	tests/unit/getopts_tests.cpp
	tests/unit/grapheme_tests.cpp
	tests/unit/invocation_tests.cpp
	tests/unit/options_tests.cpp
	tests/unit/expander_tests.cpp
	tests/unit/pattern_tests.cpp
	tests/unit/parser_tests.cpp
	tests/unit/read_tests.cpp
	tests/unit/readonly_tests.cpp
	tests/unit/test_builtin_tests.cpp
	tests/unit/signal_tests.cpp
	tests/unit/substrate_contract_tests.cpp)
target_link_libraries(lesh_tests PRIVATE lesh_legacy lesh_runtime lesh_syntax lesh_ui gtest gtest_main)
# The grapheme tests read the vendored UCD conformance file rather than a
# generated fixture, so a stale table cannot be validated by an equally stale
# expectation (#108).
target_compile_definitions(lesh_tests PRIVATE
	LESH_UCD_DIR="${PROJECT_SOURCE_DIR}/third_party/ucd")

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
target_link_libraries(lesh_header_self_containment PRIVATE lesh_runtime lesh_substrate lesh_syntax)

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
	# Both front ends are measured, continuously. Parity is tracked as the new
	# front end grows rather than discovered at the end of the migration - see #8,
	# and note that legacy's defects masked each other until they were fixed one
	# at a time.
	add_test(NAME spec_dash_legacy
		COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/spec_run.py
			--shell $<TARGET_FILE:lesh>
			--ref ${DASH_EXECUTABLE}
			--frontend legacy
			${PROJECT_SOURCE_DIR}/tests/spec)
	add_test(NAME spec_dash_next
		COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/spec_run.py
			--shell $<TARGET_FILE:lesh>
			--ref ${DASH_EXECUTABLE}
			--frontend next
			${PROJECT_SOURCE_DIR}/tests/spec)
else()
	message(STATUS "spec_dash test skipped: need dash and python3")
endif()
