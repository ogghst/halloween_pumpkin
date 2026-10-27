idf_component_register(
    SRCS "eyes_ctrl.c" "eyes_ctrl_core.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES pumpkin_fsm config_store led_effects esp_event
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
