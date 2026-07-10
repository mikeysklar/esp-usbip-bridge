# Script to generate version_autogen.h at CMake configure time.
# Usage: cmake -P scripts/generate_version.cmake
#        -D OUTPUT_DIR=<path-to-output-dir>
#
# The generated header defines HARNESS_FW_VERSION as a string combining
# git describe output and the build date.

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "OUTPUT_DIR is required")
endif()

# Try git describe; fall back to "unknown" if git fails or not in a repo.
set(GIT_RESULT "unknown")
set(GIT_RETURN_CODE 0)

find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --dirty --always
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
        OUTPUT_VARIABLE GIT_DESC
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_RETURN_CODE
    )
    if(GIT_RETURN_CODE EQUAL 0 AND GIT_DESC)
        set(GIT_RESULT "${GIT_DESC}")
    endif()
endif()

# Strip a leading "v" if present (e.g. "v0.1.0" -> "0.1.0")
string(REGEX REPLACE "^v" "" GIT_RESULT "${GIT_RESULT}")

# Get build date and time in YYYY-MM-DD HH:MM:SS format
string(TIMESTAMP BUILD_DATE "%Y-%m-%d %H:%M:%S")

# Combine: "<git-describe> <build-date>"
set(FW_VERSION "${GIT_RESULT} ${BUILD_DATE}")

# Generate the header file
set(HEADER_PATH "${OUTPUT_DIR}/version_autogen.h")
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/version_autogen.h.in"
    "${HEADER_PATH}"
    @ONLY
)

message(STATUS "Generated firmware version: ${FW_VERSION} → ${HEADER_PATH}")
