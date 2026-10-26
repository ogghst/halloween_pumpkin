cmake_minimum_required(VERSION 3.16)
project(prov_host_tests C)
set(IDF_PATH_C "$ENV{IDF_PATH}")
add_executable(prov_host_tests
    test_prov_codec.c
    ../prov_mqtt_codec.c
    ../prov_pop.c
    "${IDF_PATH_C}/components/unity/unity/src/unity.c")
target_include_directories(prov_host_tests PRIVATE .. ../include stub
    ../../config_store/include
    "${IDF_PATH_C}/components/unity/unity/src")
target_compile_options(prov_host_tests PRIVATE -Wall -Wextra)
enable_testing()
add_test(NAME prov_host_tests COMMAND prov_host_tests)
