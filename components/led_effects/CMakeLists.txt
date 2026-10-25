idf_component_register(
    SRCS "led_fx.c" "fx_color.c" "fx_current.c" "fx_effects.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES espressif__led_strip esp_timer esp_driver_gpio
)
