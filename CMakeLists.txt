cmake_minimum_required(VERSION 3.13)

project(lesh)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LUA REQUIRED IMPORTED_TARGET lua>=5.2)

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


# testing
add_subdirectory("include/googletest")
enable_testing()
include_directories(${gtest_SOURCE_DIR}/include ${gtest_SOURCE_DIR})

include_directories("include/*")
include_directories("src")

add_library(lesh_lib ${SOURCE_FILES} ${HEADER_FILES})
target_link_libraries(lesh_lib replxx::replxx sol2::sol2 PkgConfig::LUA)

add_executable(lesh ${PROJECT_SOURCE_DIR}/src/main.cpp
        src/auto_complete.h
        src/executor.h)
target_link_libraries(lesh lesh_lib)

add_executable(lesh_tests ${TEST_FILES} ${HEADER_FILES})

target_link_libraries(lesh_tests gtest gtest_main lesh_lib)
