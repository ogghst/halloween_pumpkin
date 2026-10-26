cmake_minimum_required(VERSION 3.16)
project(fsm_host_tests C)
set(IDF_PATH_C "$ENV{IDF_PATH}")
add_executable(fsm_host_tests
    test_fsm_core.c
    ../fsm_core.c
    "${IDF_PATH_C}/components/unity/unity/src/unity.c")
target_include_directories(fsm_host_tests PRIVATE .. ../include stub
    "${IDF_PATH_C}/components/unity/unity/src")
target_compile_options(fsm_host_tests PRIVATE -Wall -Wextra)
enable_testing()
add_test(NAME fsm_host_tests COMMAND fsm_host_tests)
