# Examples are COMPILED BY THE BUILD, not pasted into documentation.
#
# An example that does not compile is worse than no example, because it teaches
# an API that no longer exists. CI builds this directory, so an interface change
# that breaks an example fails the build rather than rotting quietly.

add_executable(example_replay example_replay.cpp)
target_link_libraries(example_replay
    PRIVATE ptl::engine ptl::execution ptl::market ptl::portfolio ptl::oms
            ptl::risk ptl::accounting ptl::analytics ptl::sanitizers)

add_executable(example_optimization example_optimization.cpp)
target_link_libraries(example_optimization
    PRIVATE ptl::optimization ptl::sanitizers)

add_executable(example_execution example_execution.cpp)
target_link_libraries(example_execution
    PRIVATE ptl::algo ptl::oms ptl::sanitizers)

set_target_properties(
    example_replay example_optimization example_execution
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples")
