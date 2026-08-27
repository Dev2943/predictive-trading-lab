add_library(ptl_sanitizers INTERFACE)
add_library(ptl::sanitizers ALIAS ptl_sanitizers)

if(PTL_SANITIZER STREQUAL "none")
    return()
endif()

if(MSVC)
    message(WARNING "PTL_SANITIZER is not wired up for MSVC; ignoring")
    return()
endif()

set(_ptl_san "")
if(PTL_SANITIZER MATCHES "address")
    list(APPEND _ptl_san address)
endif()
if(PTL_SANITIZER MATCHES "undefined")
    list(APPEND _ptl_san undefined)
endif()
if(PTL_SANITIZER MATCHES "thread")
    list(APPEND _ptl_san thread)
endif()

list(JOIN _ptl_san "," _ptl_san_joined)
target_compile_options(ptl_sanitizers INTERFACE
    -fsanitize=${_ptl_san_joined} -fno-omit-frame-pointer -g)
target_link_options(ptl_sanitizers INTERFACE -fsanitize=${_ptl_san_joined})

# LeakSanitizer is not available as a standalone runtime on macOS; ASan on
# Darwin also does not enable leak detection by default. Say so rather than
# letting someone believe leaks are being checked when they are not.
if(APPLE AND PTL_SANITIZER MATCHES "address")
    message(STATUS "ASan enabled. Note: LeakSanitizer is unavailable on macOS -- "
                   "run the leak-checking preset on Linux CI for allocation audits.")
endif()
