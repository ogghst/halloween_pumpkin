cmake_minimum_required(VERSION 3.16)
project(radar_host_tests C)
set(IDF_PATH_C "$ENV{IDF_PATH}")
add_executable(radar_host_tests
    test_radar_parser.c test_radar_command.c test_radar_trigger.c
    ../radar_parser.c ../radar_command.c ../radar_trigger.c
    "${IDF_PATH_C}/components/unity/unity/src/unity.c")
target_include_directories(radar_host_tests PRIVATE .. ../include stub
    "${IDF_PATH_C}/components/unity/unity/src")
target_compile_options(radar_host_tests PRIVATE -Wall -Wextra)
target_link_libraries(radar_host_tests PRIVATE m)
enable_testing()
add_test(NAME radar_host_tests COMMAND radar_host_tests)
