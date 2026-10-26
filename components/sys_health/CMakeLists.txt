idf_component_register(
    SRCS "sys_health.c" "sys_health_core.c"
    INCLUDE_DIRS "include"
    REQUIRES ha_mqtt net_mgr pumpkin_fsm radar_ld2420 app_update esp_event nvs_flash
    PRIV_REQUIRES esp_timer console)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
