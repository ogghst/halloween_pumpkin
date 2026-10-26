idf_component_register(
    SRCS "wifi_prov.c" "prov_button.c" "prov_mqtt_codec.c" "prov_pop.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event config_store
    PRIV_REQUIRES wifi_provisioning bt esp_wifi esp_timer driver net_mgr nvs_flash
)
