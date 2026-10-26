idf_component_register(
    SRCS "fsm_core.c" "fsm.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES config_store esp_event esp_timer led_effects radar_ld2420
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
