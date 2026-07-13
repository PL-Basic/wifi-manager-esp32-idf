idf_component_register(
    SRCS "app_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_event log
)