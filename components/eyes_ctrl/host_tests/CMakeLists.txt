cmake_minimum_required(VERSION 3.16)
project(eyes_host_tests C)
set(IDF_PATH_C "$ENV{IDF_PATH}")
add_executable(eyes_host_tests
    test_eyes_ctrl_core.c
    ../eyes_ctrl_core.c
    "${IDF_PATH_C}/components/unity/unity/src/unity.c")
# stub/ fornisce esp_err.h/esp_event.h minimi per pumpkin_fsm.h e
# config_store.h (entrambi REALI del repo). led_fx.h e' quello REALE di
# led_effects (include dir dedicata, PRIMA di stub/): il contratto occhi
# e' atterrato con la Fase 8 e la doppia di test e' stata rimossa.
target_include_directories(eyes_host_tests PRIVATE
    .. ../../pumpkin_fsm/include ../../config_store/include
    ../../led_effects/include stub
    "${IDF_PATH_C}/components/unity/unity/src")
target_compile_options(eyes_host_tests PRIVATE -Wall -Wextra)
enable_testing()
add_test(NAME eyes_host_tests COMMAND eyes_host_tests)
