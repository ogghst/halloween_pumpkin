cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# PROJECT_VER dalla descrizione git (es. "v1.0.0", "v1.0.0-2-g1234567-dirty");
# fallback se git non e' disponibile (archivi sorgente senza .git).
execute_process(
    COMMAND git -C ${CMAKE_CURRENT_LIST_DIR} describe --tags --dirty --always
    OUTPUT_VARIABLE PROJECT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _git_describe_result
)
if(NOT _git_describe_result EQUAL 0 OR "${PROJECT_VER}" STREQUAL "")
    set(PROJECT_VER "0.0.0-dev")
endif()

project(pumpkin)
