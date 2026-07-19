idf_component_register(
    SRCS "client_access.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES log esp_event esp_wifi esp_netif freertos
)