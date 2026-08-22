# The <min>...<max> range says: requires at least 3.24, and has been tested
# against the policies introduced up to 4.0. Without the upper bound, CMake 4.x
# applies OLD policy behaviour and emits deprecation noise.
cmake_minimum_required(VERSION 3.24...4.0)

project(predictive-trading-lab
        VERSION 0.1.0
        DESCRIPTION "End-to-end quantitative research and execution simulation platform"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# C++20 is the baseline; C++23 is used only where the toolchain proves it has
# it. AppleClang trails upstream LLVM, so hard-requiring 23 would fail at
# configure time on an Xcode that is otherwise perfectly capable of building
# this project -- nothing here NEEDS 23 except std::expected, and ptl::Result
# already falls back when that is absent.
# ---------------------------------------------------------------------------
if(PROJECT_IS_TOP_LEVEL)
    if("cxx_std_23" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
        set(CMAKE_CXX_STANDARD 23)
    else()
        set(CMAKE_CXX_STANDARD 20)
        message(STATUS "C++23 unavailable on this toolchain; building as C++20")
    endif()
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
    endif()
endif()

option(PTL_BUILD_TESTS       "Build the test suite"                        ON)
option(PTL_BUILD_BENCHMARKS  "Build Google Benchmark micro-benchmarks"     OFF)
option(PTL_BUILD_APPS        "Build the command-line applications"         ON)
option(PTL_ENABLE_TRACE      "Compile in trace-level logging (hot path)"   OFF)
option(PTL_WERROR            "Treat warnings as errors"                    ON)
option(PTL_NATIVE_ARCH       "Tune for the host CPU (see cmake/CompilerWarnings)" OFF)
option(PTL_COVERAGE          "Instrument for coverage"                     OFF)
option(PTL_FORCE_RESULT_FALLBACK
       "Compile ptl::Result's fallback instead of std::expected (older Xcode path)" OFF)
set(PTL_LOG_BACKEND "builtin" CACHE STRING "Logging backend: builtin | spdlog")
set(PTL_SANITIZER   "none"    CACHE STRING "none | address | undefined | address+undefined | thread")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)
include(GitVersion)
include(Dependencies)

add_library(ptl_coverage INTERFACE)
add_library(ptl::coverage ALIAS ptl_coverage)
if(PTL_COVERAGE AND NOT MSVC)
    target_compile_options(ptl_coverage INTERFACE --coverage -O0 -g)
    target_link_options(ptl_coverage INTERFACE --coverage)
endif()

add_subdirectory(src)

if(PTL_BUILD_APPS)
    add_subdirectory(apps)
endif()

if(PTL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(PTL_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

message(STATUS "")
message(STATUS "  predictive-trading-lab ${PROJECT_VERSION} (${PTL_GIT_DESCRIBE})")
message(STATUS "  compiler    : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  std         : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  build type  : ${CMAKE_BUILD_TYPE}")
message(STATUS "  log backend : ${PTL_LOG_BACKEND}")
message(STATUS "  sanitizer   : ${PTL_SANITIZER}")
message(STATUS "  trace log   : ${PTL_ENABLE_TRACE}")
message(STATUS "")
