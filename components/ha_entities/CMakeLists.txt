idf_component_register(SRCS "ha_entities.c"
    INCLUDE_DIRS "include"
    REQUIRES ha_mqtt config_store pumpkin_fsm radar_ld2420 led_effects net_mgr
    PRIV_REQUIRES esp_timer esp_event log)
