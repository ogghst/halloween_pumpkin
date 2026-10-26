idf_component_register(SRCS "main.c"
    REQUIRES wifi_prov net_mgr config_store led_effects pumpkin_fsm
             radar_ld2420 console nvs_flash esp_timer esp_event esp_wifi
             esp_netif)
