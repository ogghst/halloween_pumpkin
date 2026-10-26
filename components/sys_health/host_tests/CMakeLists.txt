cmake_minimum_required(VERSION 3.16)
project(sys_health_host_tests C)
set(UNITY_DIR "$ENV{IDF_PATH}/components/unity/unity/src")

add_executable(test_sys_health
    test_sys_health.c
    ../sys_health_core.c
    "${UNITY_DIR}/unity.c")
target_include_directories(test_sys_health PRIVATE ../include stub
    "${UNITY_DIR}")
target_compile_options(test_sys_health PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME test_sys_health COMMAND test_sys_health)
