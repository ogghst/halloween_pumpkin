cmake_minimum_required(VERSION 3.16)
project(ha_mqtt_host_tests C)
set(IDF_PATH_C "$ENV{IDF_PATH}")
set(JSON_DIR "${IDF_PATH_C}/components/json/cJSON")
set(UNITY_DIR "${IDF_PATH_C}/components/unity/unity/src")

add_executable(test_payloads
    test_payloads.c
    ../ha_registry.c
    ../ha_payloads.c
    "${JSON_DIR}/cJSON.c"
    "${UNITY_DIR}/unity.c")
target_include_directories(test_payloads PRIVATE .. ../include
    "${JSON_DIR}" "${UNITY_DIR}")
target_compile_options(test_payloads PRIVATE -Wall -Wextra)

add_executable(test_commands
    test_commands.c
    ../ha_commands.c
    "${JSON_DIR}/cJSON.c"
    "${UNITY_DIR}/unity.c")
target_include_directories(test_commands PRIVATE .. ../include
    "${JSON_DIR}" "${UNITY_DIR}")
target_compile_options(test_commands PRIVATE -Wall -Wextra)

add_executable(test_state
    test_state.c
    ../ha_state.c
    "${JSON_DIR}/cJSON.c"
    "${UNITY_DIR}/unity.c")
target_include_directories(test_state PRIVATE .. ../include
    "${JSON_DIR}" "${UNITY_DIR}")
target_compile_options(test_state PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME test_payloads COMMAND test_payloads)
add_test(NAME test_commands COMMAND test_commands)
add_test(NAME test_state COMMAND test_state)
