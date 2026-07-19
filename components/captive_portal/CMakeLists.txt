idf_component_register(
    SRCS "captive_portal.c" "portal_dns.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_http_server log esp_netif client_access freertos lwip esp_system
)