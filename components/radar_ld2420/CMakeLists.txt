idf_component_register(
    SRCS "radar_parser.c" "radar_command.c" "radar_trigger.c" "radar.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES esp_timer esp_driver_uart esp_driver_gpio nvs_flash esp_event
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
