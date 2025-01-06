cmake_minimum_required(VERSION 3.13)

project(lesh)

file(GLOB_RECURSE HEADER_FILES src/*.hpp)
file(GLOB_RECURSE SOURCE_FILES src/*.cpp)

set(CMAKE_CXX_STANDARD 23) # Enable the C++23 standard
# set(CMAKE_CXX_COMPILER "/opt/homebrew/opt/llvm/bin/clang++")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-omit-frame-pointer -fsanitize=address")
# set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fno-omit-frame-pointer -fsanitize=address")
# set(CMAKE_CXX_FLAGS "-O3 -Wall -Wextra")
# set(CMAKE_CXX_FLAGS "cmake-build-debug")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -stdlib=libc++ -lc++abi")
set(CMAKE_BUILD_TYPE Debug)

# add_compile_options(-fsanitize=address)
# add_link_options(-fsanitize=address)
# add_subdirectory("include/abseil")
# include_directories("include/abseil")
add_subdirectory("include/cpp-linenoise")
add_subdirectory("include/replxx")
include_directories("include/*")

add_executable(shell ${SOURCE_FILES} ${HEADER_FILES})
target_link_libraries(shell linenoise)
target_link_libraries(shell replxx::replxx)
