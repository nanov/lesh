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
add_subdirectory("tools")

# testing
add_subdirectory("include/googletest")
enable_testing()

# ---------------------------------------------------------------------------
# Targets. Sources are listed explicitly: file(GLOB) does not re-run when a
# file is added, so new sources are silently missed until CMake is re-run.
# ---------------------------------------------------------------------------
set(LESH_HEADERS
	src/auto_complete.h
	src/executor.h
	src/lesh_assert.h
	src/lesh_string_utils.h
	src/lolcat.h
	src/prompt.h
	src/utils.h
	src/zsh_parser_plus.h)

add_library(lesh_lib
	src/lolcat.cpp
	${LESH_HEADERS})
target_link_libraries(lesh_lib PUBLIC lesh_options replxx::replxx sol2::sol2 lua)

add_executable(lesh src/main.cpp)
target_link_libraries(lesh PRIVATE lesh_lib)

add_executable(lesh_tests
	tests/base_tests.cpp
	tests/lesh_state_tests.cpp)
target_link_libraries(lesh_tests PRIVATE lesh_lib gtest gtest_main)
add_test(NAME lesh_tests COMMAND lesh_tests)
