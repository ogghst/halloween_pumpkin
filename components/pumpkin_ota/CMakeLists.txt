idf_component_register(SRCS "pumpkin_ota.c" "ota_policy.c"
    INCLUDE_DIRS "include"
    REQUIRES ha_mqtt led_effects esp_https_ota esp_http_client app_update esp_event
    PRIV_REQUIRES json esp_timer log)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
