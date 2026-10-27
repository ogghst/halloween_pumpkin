cmake_minimum_required(VERSION 3.16)
project(config_host_tests C)
set(IDF_PATH_C "$ENV{IDF_PATH}")
add_executable(config_host_tests
    test_config_core.c
    ../config_store_core.c
    ../config_store.c
    stub/stubs.c
    "${IDF_PATH_C}/components/unity/unity/src/unity.c")
target_include_directories(config_host_tests PRIVATE .. ../include stub
    "${IDF_PATH_C}/components/unity/unity/src")
# I simboli Kconfig non esistono sull'host: valore di test per il
# debounce del commit NVS (stesso default del Kconfig).
target_compile_definitions(config_host_tests PRIVATE
    CONFIG_STORE_DEBOUNCE_MS=2000)
target_compile_options(config_host_tests PRIVATE -Wall -Wextra)
enable_testing()
add_test(NAME config_host_tests COMMAND config_host_tests)
