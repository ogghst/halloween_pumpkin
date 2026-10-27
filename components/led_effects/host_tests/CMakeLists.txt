cmake_minimum_required(VERSION 3.16)
project(fx_host_tests C)
set(IDF_PATH_C "$ENV{IDF_PATH}")
add_executable(fx_host_tests
    test_fx_color.c test_fx_current.c test_fx_matrix.c test_fx_overlay.c test_fx_eyes.c test_fx_sparkling.c
    ../fx_color.c ../fx_current.c ../fx_eyes.c ../fx_effects.c ../fx_matrix.c ../fx_overlay.c
    "${IDF_PATH_C}/components/unity/unity/src/unity.c")
target_include_directories(fx_host_tests PRIVATE .. ../include stub
    "${IDF_PATH_C}/components/unity/unity/src")
target_compile_options(fx_host_tests PRIVATE -Wall -Wextra)
target_link_libraries(fx_host_tests PRIVATE m)
enable_testing()
add_test(NAME fx_host_tests COMMAND fx_host_tests)
