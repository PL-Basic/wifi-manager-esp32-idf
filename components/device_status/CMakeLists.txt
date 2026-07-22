idf_component_register(
    SRCS "device_status.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES wifi_gateway esp_wifi
)