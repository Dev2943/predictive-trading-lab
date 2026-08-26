# One INTERFACE target carrying the warning set. Every ptl_* target links it.
add_library(ptl_warnings INTERFACE)
add_library(ptl::warnings ALIAS ptl_warnings)

set(_ptl_gcc_clang_warnings
    -Wall -Wextra -Wpedantic
    -Wshadow                # shadowing is a classic source of rolling-window bugs
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wconversion            # silent double->int truncation in quantity maths
    -Wsign-conversion
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    -Wnull-dereference)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    list(APPEND _ptl_gcc_clang_warnings
        -Wmisleading-indentation -Wduplicated-cond -Wduplicated-branches
        -Wlogical-op -Wuseless-cast)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # AppleClang and upstream Clang both accept these.
    list(APPEND _ptl_gcc_clang_warnings -Wno-unknown-warning-option)
endif()

if(MSVC)
    target_compile_options(ptl_warnings INTERFACE /W4 /permissive- /Zc:__cplusplus)
    if(PTL_WERROR)
        target_compile_options(ptl_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(ptl_warnings INTERFACE ${_ptl_gcc_clang_warnings})
    if(PTL_WERROR)
        target_compile_options(ptl_warnings INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# Native architecture tuning.
#
# -march=native is an x86 spelling. On Apple Silicon AppleClang rejects it;
# arm64 wants -mcpu=. Probe rather than assume, so `--preset native` works on
# both an Intel Mac and an M-series Mac without editing anything.
# ---------------------------------------------------------------------------
add_library(ptl_native_arch INTERFACE)
add_library(ptl::native_arch ALIAS ptl_native_arch)

if(PTL_NATIVE_ARCH)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-march=native" PTL_HAS_MARCH_NATIVE)
    check_cxx_compiler_flag("-mcpu=native"  PTL_HAS_MCPU_NATIVE)
    if(PTL_HAS_MARCH_NATIVE)
        target_compile_options(ptl_native_arch INTERFACE -march=native)
    elseif(PTL_HAS_MCPU_NATIVE)
        target_compile_options(ptl_native_arch INTERFACE -mcpu=native)
    else()
        message(WARNING "PTL_NATIVE_ARCH requested but no native flag is supported")
    endif()
endif()
