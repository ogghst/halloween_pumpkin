idf_component_register(
    SRCS "config_store_core.c" "config_store.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_event
    PRIV_REQUIRES esp_timer
)
