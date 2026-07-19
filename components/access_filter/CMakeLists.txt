idf_component_register(
    SRCS "access_filter.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES client_access esp_netif lwip log
)