idf_component_register(SRCS "ha_entities.c"
    INCLUDE_DIRS "include"
    REQUIRES ha_mqtt config_store pumpkin_fsm pumpkin_ota radar_ld2420 led_effects net_mgr
    PRIV_REQUIRES esp_timer esp_event log)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
