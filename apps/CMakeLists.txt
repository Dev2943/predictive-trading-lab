# ---------------------------------------------------------------------------
# GCC's -Wnull-dereference cannot prove reachability through CLI11's inlined
# string handling at -O2 and reports a false positive there.
#
# The suppression is an INTERFACE target rather than a target_compile_options()
# call because CMake emits a target's own options BEFORE those inherited from
# linked libraries, and GCC takes the last flag. Linking the suppression after
# ptl::core is therefore the only ordering that actually wins. Scoped to this
# executable, so our own code is still checked.
# ---------------------------------------------------------------------------
add_library(ptl_cli11_workaround INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(ptl_cli11_workaround INTERFACE -Wno-null-dereference)
endif()

add_executable(ptl_version ptl_version/main.cpp)
target_link_libraries(ptl_version PRIVATE
    ptl::core ptl::log ptl::config ptl::experiments
    CLI11::CLI11 ptl::sanitizers ptl_cli11_workaround)
