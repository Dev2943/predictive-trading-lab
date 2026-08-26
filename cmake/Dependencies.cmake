include(FetchContent)

# --------------------------------------------------------------------------
# toml++ -- configuration. Single-header, comment-preserving.
# --------------------------------------------------------------------------
FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE)

# --------------------------------------------------------------------------
# CLI11 -- command-line parsing, including the --set dotted overrides that let
# one config file drive a parameter sweep.
# --------------------------------------------------------------------------
set(CLI11_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.4.2
    GIT_SHALLOW    TRUE)

# --------------------------------------------------------------------------
# nlohmann/json -- vendor payload parsing (Phase 2) and machine-readable run
# artifacts (Phase 14). Header-only. Chosen over hand-rolling because a JSON
# parser written for one vendor payload is a parser that will be wrong for the
# next one, and over simdjson because nothing here is throughput-bound: ingest
# runs once and caches.
# --------------------------------------------------------------------------
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE)

FetchContent_MakeAvailable(tomlplusplus CLI11 nlohmann_json)

# --------------------------------------------------------------------------
# SQLite -- experiment registry (runs, trials, search budgets, holdout
# unlocks). NOT bulk time-series storage.
#
# Prefer the system library: macOS ships it in the SDK and every Linux distro
# packages it. Fall back to the vendored amalgamation so a fresh clone builds
# with no system prerequisites at all.
# --------------------------------------------------------------------------
find_package(SQLite3 QUIET)
if(SQLite3_FOUND)
    message(STATUS "SQLite3: system (${SQLite3_VERSION})")
    add_library(ptl_sqlite3 INTERFACE)
    # CMake 4.x renamed the imported target and deprecated the old spelling.
    # Probe rather than pick, so the build is quiet on both old and new CMake.
    if(TARGET SQLite3::SQLite3)
        target_link_libraries(ptl_sqlite3 INTERFACE SQLite3::SQLite3)
    else()
        target_link_libraries(ptl_sqlite3 INTERFACE SQLite::SQLite3)
    endif()
else()
    message(STATUS "SQLite3: system not found, vendoring amalgamation")
    enable_language(C)
    # SOURCE_SUBDIR pointing at a path that does not exist is the documented way
    # to download a dependency WITHOUT running its CMakeLists. We only want
    # sqlite3.c and sqlite3.h; the upstream build script targets an old CMake
    # and we compile the amalgamation ourselves anyway with our own flags.
    #
    # The obvious alternative, FetchContent_Populate(), is deprecated as of
    # CMake 3.30 and slated for removal (policy CMP0169).
    FetchContent_Declare(sqlite3_amalgamation
        GIT_REPOSITORY https://github.com/azadkuh/sqlite-amalgamation.git
        GIT_TAG        3.38.2
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  ptl-do-not-configure)
    FetchContent_MakeAvailable(sqlite3_amalgamation)
    add_library(ptl_sqlite3_impl STATIC "${sqlite3_amalgamation_SOURCE_DIR}/sqlite3.c")
    target_include_directories(ptl_sqlite3_impl SYSTEM PUBLIC "${sqlite3_amalgamation_SOURCE_DIR}")
    # Vendored C: our C++ warning set does not apply and would not compile clean.
    set_target_properties(ptl_sqlite3_impl PROPERTIES C_STANDARD 99)
    target_compile_definitions(ptl_sqlite3_impl PRIVATE
        SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_THREADSAFE=1 SQLITE_DQS=0)
    find_package(Threads REQUIRED)
    target_link_libraries(ptl_sqlite3_impl PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    add_library(ptl_sqlite3 INTERFACE)
    target_link_libraries(ptl_sqlite3 INTERFACE ptl_sqlite3_impl)
endif()
add_library(ptl::sqlite3 ALIAS ptl_sqlite3)

# --------------------------------------------------------------------------
# Catch2 v3 -- unit, property, leakage and golden tests.
# --------------------------------------------------------------------------
if(PTL_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.5.2
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    include(Catch)
endif()

# --------------------------------------------------------------------------
# Google Benchmark -- off by default; slow to build, only needed when actually
# measuring (Phase 13).
# --------------------------------------------------------------------------
if(PTL_BUILD_BENCHMARKS)
    set(BENCHMARK_ENABLE_TESTING     OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL     OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.8.3
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(benchmark)
endif()

# --------------------------------------------------------------------------
# Eigen -- Phase 6 (models). brew install eigen / apt install libeigen3-dev.
# --------------------------------------------------------------------------
find_package(Eigen3 3.4 QUIET NO_MODULE)
if(Eigen3_FOUND)
    message(STATUS "Eigen3: found ${EIGEN3_VERSION_STRING}")
else()
    message(STATUS "Eigen3: not found -- required from Phase 6 (brew install eigen)")
endif()
