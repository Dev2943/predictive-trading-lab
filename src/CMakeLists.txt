# One target per module. Dependencies are declared explicitly and point only
# downward, so a cyclic include becomes a CMake configure error rather than a
# design problem discovered later.

add_library(ptl_core
    core/types.cpp
    core/time.cpp
    core/clock.cpp
    core/rng.cpp
    core/result.cpp
    core/instrument_table.cpp)
add_library(ptl::core ALIAS ptl_core)
target_include_directories(ptl_core PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_BINARY_DIR}/generated")
target_link_libraries(ptl_core PUBLIC ptl::warnings PRIVATE ptl::sanitizers ptl::coverage)
if(PTL_ENABLE_TRACE)
    target_compile_definitions(ptl_core PUBLIC PTL_ENABLE_TRACE=1)
endif()
if(PTL_FORCE_RESULT_FALLBACK)
    target_compile_definitions(ptl_core PUBLIC PTL_FORCE_RESULT_FALLBACK=1)
endif()

add_library(ptl_log log/logger.cpp)
add_library(ptl::log ALIAS ptl_log)
target_link_libraries(ptl_log PUBLIC ptl::core PRIVATE ptl::sanitizers)

add_library(ptl_config config/config.cpp)
add_library(ptl::config ALIAS ptl_config)
target_link_libraries(ptl_config
    PUBLIC ptl::core ptl::log
    PRIVATE tomlplusplus::tomlplusplus ptl::sanitizers)

add_library(ptl_experiments experiments/registry.cpp)
add_library(ptl::experiments ALIAS ptl_experiments)
target_link_libraries(ptl_experiments
    PUBLIC ptl::core
    PRIVATE ptl::sqlite3 ptl::sanitizers)
