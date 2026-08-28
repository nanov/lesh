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
# `lesh_ui` since #206: the cooperative walk's price is a property of the HOST -
# a yield goes back to `event_loop::turn`, which polls the terminal before it
# comes back - so the number can only be measured with the real loop, the real
# autosuggester and the real fiber under it. `lesh_ui` links `lesh_runtime` and
# `lesh_leshper` itself, so this is a superset of what was already here.
target_link_libraries(lesh_bench PRIVATE lesh_ui lesh_runtime lesh_syntax lesh_fiber)
