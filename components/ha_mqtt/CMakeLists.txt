idf_component_register(SRCS "ha_mqtt.c" "ha_registry.c" "ha_payloads.c" "ha_commands.c" "ha_state.c"
    INCLUDE_DIRS "include"
    REQUIRES json config_store
    PRIV_REQUIRES mqtt esp_event esp_timer nvs_flash esp_partition esp_hw_support esp_app_format log)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
