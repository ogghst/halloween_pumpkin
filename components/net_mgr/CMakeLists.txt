idf_component_register(
    SRCS "net_mgr.c" "net_backoff.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event
    PRIV_REQUIRES esp_timer esp_netif esp_wifi mdns
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
