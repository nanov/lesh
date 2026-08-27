# Tools are built from the parent project's targets, so they see the same
# configuration and the same flags. No project() call: this is a subdirectory,
# not a separate build.

add_executable(param_viewer param_viewer.cpp)

# The compass (issue #7). Allocation counts are the signal and are compiled in
# outside Release; timings are only meaningful in bench or release.
add_executable(lesh_bench bench.cpp)
# `lesh_fiber` since #198: the compass carries the fiber switch cost, because a
# cooperative scheduler on the host thread is only affordable if a switch is
# cheap next to the work between switches - and "cheap" has to be a number
# measured on this machine rather than one quoted from a research note.
target_link_libraries(lesh_bench PRIVATE lesh_runtime lesh_syntax lesh_fiber)
