# Python bindings — a STANDALONE CMake project.
#
# WHY STANDALONE, AND WHY THAT MATTERS.
#
# This is configured on its own, not through the engine's root CMakeLists. The
# consequence is that NOTHING AT THE REPOSITORY ROOT CHANGES: no new option, no
# new add_subdirectory, no conditional. A developer building the engine needs no
# Python toolchain and is not even aware this directory exists.
#
# The engine is pulled in with add_subdirectory(... EXCLUDE_FROM_ALL), so only
# the libraries these bindings actually link get built. The engine's own tests,
# apps, examples and benchmarks are switched off: this is a consumer of the
# engine, and a consumer does not build its dependency's test suite.
#
#   cmake -S frontend/backend/bindings -B build/bindings -G Ninja \
#         -DCMAKE_BUILD_TYPE=Release \
#         -Dpybind11_DIR=$(python3 -c "import pybind11; print(pybind11.get_cmake_dir())")
#   cmake --build build/bindings

cmake_minimum_required(VERSION 3.24)
project(ptl_bindings LANGUAGES CXX)

# The engine's headers are C++20-and-later. A standalone project does not
# inherit the engine's standard setting, so it is declared here -- without it,
# `requires` and `operator<=>` in the public headers fail to parse and the error
# points confusingly into the engine rather than at this file.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Locate the engine. Defaults to three directories up (frontend/backend/bindings
# -> repository root) and can be overridden to point at a checkout elsewhere,
# which is what a future out-of-tree build would need.
set(PTL_ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.."
    CACHE PATH "Path to the Predictive Trading Lab engine source root")

if(NOT EXISTS "${PTL_ENGINE_ROOT}/include/ptl/core/types.hpp")
    message(FATAL_ERROR
        "PTL_ENGINE_ROOT does not look like the engine source root:\n"
        "  ${PTL_ENGINE_ROOT}\n"
        "Point -DPTL_ENGINE_ROOT at the directory containing include/ptl.")
endif()

# The engine's own extras are off: a consumer builds the libraries it links and
# nothing else. Building the engine's 766 tests as a side effect of building a
# Python module would be a surprising and slow default.
# A Python extension is a SHARED module, so every static library linked into it
# must be position-independent. The engine builds its libraries without -fPIC by
# default, which is correct for the executables it ships; this sets PIC for THIS
# BUILD ONLY. It is a build setting on a consumer's copy, not a change to the
# engine, and the engine's own build is unaffected.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(PTL_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
set(PTL_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(PTL_BUILD_APPS       OFF CACHE BOOL "" FORCE)
set(PTL_BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)

add_subdirectory("${PTL_ENGINE_ROOT}" "${CMAKE_BINARY_DIR}/engine" EXCLUDE_FROM_ALL)

find_package(Python3 COMPONENTS Interpreter Development.Module REQUIRED)
find_package(pybind11 CONFIG REQUIRED)

pybind11_add_module(ptl MODULE ptl_bindings.cpp)

target_link_libraries(ptl
    PRIVATE ptl::core ptl::config ptl::optimization ptl::analytics
            ptl::attribution ptl::risk ptl::ops)

# The generated version header lives in the engine's build tree, so the module
# reports the same version and git provenance the engine binaries do.
target_include_directories(ptl PRIVATE "${CMAKE_BINARY_DIR}/engine/generated")

# The engine builds with -Werror and a strict warning set. pybind11's generated
# code does not, and forcing it to would mean patching a dependency to satisfy
# our policy. The boundary is the right place to relax it, and only here.
target_compile_options(ptl PRIVATE -Wno-shadow)
target_compile_features(ptl PRIVATE cxx_std_23)

set_target_properties(ptl PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

message(STATUS "ptl bindings: engine root ${PTL_ENGINE_ROOT}")
