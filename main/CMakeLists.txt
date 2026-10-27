idf_component_register(SRCS "main.c"
    REQUIRES wifi_prov net_mgr config_store led_effects pumpkin_fsm
             eyes_ctrl radar_ld2420 console nvs_flash esp_timer esp_event
             esp_wifi esp_netif ha_entities ha_mqtt pumpkin_ota sys_health)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
