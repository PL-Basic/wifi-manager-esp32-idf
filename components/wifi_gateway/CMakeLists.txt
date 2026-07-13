idf_component_register(
    SRCS "wifi_gateway.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES log esp_netif esp_event esp_wifi
)