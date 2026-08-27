# Embeds the git description into the binary. Reproducibility depends on being
# able to say exactly which commit produced a result set, so this is not
# cosmetic -- PTL_GIT_DESCRIBE feeds into the RunId hash.
find_package(Git QUIET)
set(PTL_GIT_DESCRIBE "unknown")
set(PTL_GIT_SHA      "unknown")
set(PTL_GIT_DIRTY    "unknown")

if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
    execute_process(COMMAND ${GIT_EXECUTABLE} describe --always --tags --dirty
                    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                    OUTPUT_VARIABLE PTL_GIT_DESCRIBE
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
                    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                    OUTPUT_VARIABLE PTL_GIT_SHA
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain
                    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                    OUTPUT_VARIABLE _ptl_status OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_ptl_status STREQUAL "")
        set(PTL_GIT_DIRTY "clean")
    else()
        set(PTL_GIT_DIRTY "dirty")
    endif()
endif()

configure_file("${PROJECT_SOURCE_DIR}/cmake/version.hpp.in"
               "${PROJECT_BINARY_DIR}/generated/ptl/core/version.hpp" @ONLY)
