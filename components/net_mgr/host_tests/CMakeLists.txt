cmake_minimum_required(VERSION 3.16)
project(net_host_tests C)
set(IDF_PATH_C "$ENV{IDF_PATH}")
add_executable(net_host_tests
    test_net_backoff.c
    ../net_backoff.c
    "${IDF_PATH_C}/components/unity/unity/src/unity.c")
target_include_directories(net_host_tests PRIVATE .. ../include stub
    "${IDF_PATH_C}/components/unity/unity/src")
target_compile_options(net_host_tests PRIVATE -Wall -Wextra)
enable_testing()
add_test(NAME net_host_tests COMMAND net_host_tests)
