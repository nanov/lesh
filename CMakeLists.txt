cmake_minimum_required(VERSION 3.13)

project(lesh)

# Lua: built from the vendored submodule (include/lua, pinned to v5.4.8).
# sol2 only supports Lua 5.1-5.4, so we must not pick up a system Lua 5.5.
set(LUA_DIR "${PROJECT_SOURCE_DIR}/include/lua")
if(NOT EXISTS "${LUA_DIR}/lua.h")
	message(FATAL_ERROR "include/lua is empty - run: git submodule update --init --recursive")
endif()
# Guard the version explicitly: sol2 fails deep inside its headers on Lua >= 5.5,
# which surfaces as a wall of template errors rather than a useful message.
# This also catches `git submodule update` silently resetting the pin.
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

file(GLOB_RECURSE HEADER_FILES src/*.hpp)
file(GLOB_RECURSE SOURCE_FILES src/*.cpp)
file(GLOB_RECURSE TEST_FILES tests/*.cpp)
list(REMOVE_ITEM SOURCE_FILES ${PROJECT_SOURCE_DIR}/src/main.cpp)

set(CMAKE_CXX_STANDARD 23) # Enable the C++23 standard
# set(CMAKE_CXX_COMPILER "/opt/homebrew/opt/llvm/bin/clang++")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-omit-frame-pointer -fsanitize=address")
# set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fno-omit-frame-pointer -fsanitize=address")
# set(CMAKE_CXX_FLAGS "-O3 -Wall -Wextra")
# set(CMAKE_CXX_FLAGS "cmake-build-debug")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -stdlib=libc++ -lc++abi")
set(CMAKE_BUILD_TYPE Debug)

# add_compile_options(-fsanitize=address)
# add_link_options(-fsanitize=address)
# add_subdirectory("include/abseil")
# include_directories("include/abseil")
add_subdirectory("include/replxx")
add_subdirectory("include/sol2")
add_subdirectory("tools")


# testing
add_subdirectory("include/googletest")
enable_testing()
include_directories(${gtest_SOURCE_DIR}/include ${gtest_SOURCE_DIR})

include_directories("include/*")
include_directories("src")

add_library(lesh_lib ${SOURCE_FILES} ${HEADER_FILES})
target_link_libraries(lesh_lib replxx::replxx sol2::sol2 lua)

add_executable(lesh ${PROJECT_SOURCE_DIR}/src/main.cpp
        src/auto_complete.h
        src/executor.h
        src/prompt.h
        src/lesh_string_utils.h)
target_link_libraries(lesh lesh_lib)

add_executable(lesh_tests ${TEST_FILES} ${HEADER_FILES})

target_link_libraries(lesh_tests gtest gtest_main lesh_lib)
