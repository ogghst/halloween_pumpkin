idf_component_register(SRCS "main.c"
    REQUIRES pumpkin_fsm config_store led_effects radar_ld2420 console
             esp_timer esp_event)
