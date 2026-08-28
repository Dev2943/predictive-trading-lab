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
# --------------------------------------------------------------------------
# Eigen -- dense linear algebra for the model layer (Phase 6).
#
# Header-only, and confined to the private implementation of ptl::models: no
# other module includes an Eigen header, so the backend stays replaceable and
# compile times elsewhere are unaffected.
#
# 3.3 is accepted as well as 3.4. Debian and Ubuntu still ship 3.4 as
# libeigen3-dev while some distributions lag; nothing used here needs 3.4.
# --------------------------------------------------------------------------
# --------------------------------------------------------------------------
# Locating Eigen is genuinely fiddly, and getting it wrong is expensive: the
# build succeeds, prints a notice, and silently omits an entire subsystem.
# Four independent things can go wrong, so each is handled explicitly.
#
#  1. HOMEBREW PREFIX. On Apple Silicon `brew install eigen` lands in
#     /opt/homebrew, which CMake does not reliably search. We ask brew for its
#     own prefix rather than hardcoding a path, which also covers Intel Macs
#     and custom prefixes.
#
#  2. PACKAGE NAME. Eigen 3.x installs Eigen3Config.cmake and exports
#     Eigen3::Eigen. Eigen 5.x -- what Homebrew now ships -- installs
#     EigenConfig.cmake and exports Eigen::Eigen. Searching only for "Eigen3"
#     misses a perfectly good Eigen 5 installation entirely.
#
#  3. VERSION REQUEST. Eigen's config uses SameMajorVersion compatibility, so
#     `find_package(Eigen3 3.3)` REFUSES to match 5.0. We therefore request no
#     version and check what we got afterwards, rather than asking a question
#     that cannot be answered yes for a newer major release.
#
#  4. NO CMAKE CONFIG AT ALL. Some distributions ship only headers. A direct
#     header probe is the last resort before giving up.
#
# Eigen stays PRIVATE to ptl::models: no other module includes an Eigen header.
# --------------------------------------------------------------------------

# NOT Eigen3_DIR, not NOT DEFINED Eigen3_DIR. A failed find_package(NO_MODULE)
# CACHES the variable as "Eigen3_DIR-NOTFOUND", which IS defined -- so a
# "NOT DEFINED" guard skips this block on every configure after the first
# failure, precisely the case it exists to rescue. CMake evaluates a value
# ending in -NOTFOUND as false, so "NOT Eigen3_DIR" is correct and still yields
# to an explicit -DEigen3_DIR override.
if(APPLE AND NOT Eigen3_DIR AND NOT Eigen_DIR)
    find_program(PTL_BREW_EXECUTABLE brew)
    if(PTL_BREW_EXECUTABLE)
        execute_process(COMMAND "${PTL_BREW_EXECUTABLE}" --prefix
                        OUTPUT_VARIABLE PTL_BREW_PREFIX
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET)
        if(PTL_BREW_PREFIX)
            list(APPEND CMAKE_PREFIX_PATH "${PTL_BREW_PREFIX}")
            message(STATUS "Homebrew prefix added to search path: ${PTL_BREW_PREFIX}")
        endif()
    endif()
endif()

set(PTL_EIGEN_TARGET "")
set(PTL_EIGEN_VERSION "unknown")

# Attempt 1: the Eigen 3.x package name, unversioned.
find_package(Eigen3 QUIET NO_MODULE)
if(TARGET Eigen3::Eigen)
    set(PTL_EIGEN_TARGET Eigen3::Eigen)
    if(DEFINED Eigen3_VERSION)
        set(PTL_EIGEN_VERSION "${Eigen3_VERSION}")
    elseif(DEFINED EIGEN3_VERSION_STRING)
        set(PTL_EIGEN_VERSION "${EIGEN3_VERSION_STRING}")
    endif()
elseif(Eigen3_FOUND AND DEFINED EIGEN3_INCLUDE_DIR)
    # A very old config that exports only an include directory.
    add_library(ptl_eigen_imported INTERFACE IMPORTED)
    set_target_properties(ptl_eigen_imported PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${EIGEN3_INCLUDE_DIR}")
    set(PTL_EIGEN_TARGET ptl_eigen_imported)
    set(PTL_EIGEN_VERSION "${EIGEN3_VERSION_STRING}")
endif()

# Attempt 2: the Eigen 4.x/5.x package name.
if(NOT PTL_EIGEN_TARGET)
    find_package(Eigen QUIET NO_MODULE)
    if(TARGET Eigen::Eigen)
        set(PTL_EIGEN_TARGET Eigen::Eigen)
        if(DEFINED Eigen_VERSION)
            set(PTL_EIGEN_VERSION "${Eigen_VERSION}")
        endif()
    elseif(TARGET Eigen3::Eigen)
        # Some Eigen 5 packages still export the legacy target name.
        set(PTL_EIGEN_TARGET Eigen3::Eigen)
        if(DEFINED Eigen_VERSION)
            set(PTL_EIGEN_VERSION "${Eigen_VERSION}")
        endif()
    endif()
endif()

# Attempt 3: headers only, no CMake config anywhere.
if(NOT PTL_EIGEN_TARGET)
    find_path(PTL_EIGEN_INCLUDE_DIR
        NAMES Eigen/Dense
        PATH_SUFFIXES eigen3 eigen
        DOC "Directory containing Eigen/Dense")
    # Verify the header is actually there. find_path honours a pre-set cache
    # value without checking it, so an explicit -DPTL_EIGEN_INCLUDE_DIR
    # pointing somewhere wrong would otherwise be accepted and fail much later
    # with a confusing compile error instead of a clear configure one.
    if(PTL_EIGEN_INCLUDE_DIR AND NOT EXISTS "${PTL_EIGEN_INCLUDE_DIR}/Eigen/Dense")
        message(WARNING
            "PTL_EIGEN_INCLUDE_DIR is set to '${PTL_EIGEN_INCLUDE_DIR}' but "
            "Eigen/Dense is not there; ignoring it.")
        set(PTL_EIGEN_INCLUDE_DIR "PTL_EIGEN_INCLUDE_DIR-NOTFOUND" CACHE PATH
            "Directory containing Eigen/Dense" FORCE)
    endif()
    if(PTL_EIGEN_INCLUDE_DIR)
        add_library(ptl_eigen_headers INTERFACE)
        target_include_directories(ptl_eigen_headers SYSTEM INTERFACE
            "${PTL_EIGEN_INCLUDE_DIR}")
        set(PTL_EIGEN_TARGET ptl_eigen_headers)
        set(PTL_EIGEN_VERSION "headers at ${PTL_EIGEN_INCLUDE_DIR}")
    endif()
endif()

if(PTL_EIGEN_TARGET)
    set(PTL_HAVE_EIGEN TRUE)
    message(STATUS "Eigen: found ${PTL_EIGEN_VERSION} (target ${PTL_EIGEN_TARGET})")
else()
    set(PTL_HAVE_EIGEN FALSE)
endif()

# PTL_REQUIRE_EIGEN defaults ON, so a missing Eigen is a HARD CONFIGURE ERROR
# rather than a two-line notice followed by a build that quietly omits the
# model layer and still reports a clean test run. A silently smaller test count
# is exactly the failure mode this project exists to eliminate; turn the option
# off deliberately if you want to build without models.
option(PTL_REQUIRE_EIGEN "Fail configuration when Eigen is unavailable" ON)

if(NOT PTL_HAVE_EIGEN)
    set(PTL_EIGEN_HELP
        "Eigen was not found, so ptl::models (Phase 6) cannot be built.\n"
        "  macOS:         brew install eigen\n"
        "  Debian/Ubuntu: sudo apt install libeigen3-dev\n"
        "  Fedora:        sudo dnf install eigen3-devel\n"
        "  Explicit path: -DPTL_EIGEN_INCLUDE_DIR=/path/containing/Eigen/Dense\n"
        "  To build without the model layer: -DPTL_REQUIRE_EIGEN=OFF")
    if(PTL_REQUIRE_EIGEN)
        message(FATAL_ERROR ${PTL_EIGEN_HELP})
    else()
        message(WARNING ${PTL_EIGEN_HELP})
    endif()
endif()
