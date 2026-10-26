cmake_minimum_required(VERSION 3.16)
project(pumpkin_ota_host_tests C)
set(IDF_PATH_C "$ENV{IDF_PATH}")
set(UNITY_DIR "${IDF_PATH_C}/components/unity/unity/src")

add_executable(test_ota_policy
    test_ota_policy.c
    ../ota_policy.c
    "${UNITY_DIR}/unity.c")
target_include_directories(test_ota_policy PRIVATE .. ../include
    "${UNITY_DIR}")
target_compile_options(test_ota_policy PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME test_ota_policy COMMAND test_ota_policy)
