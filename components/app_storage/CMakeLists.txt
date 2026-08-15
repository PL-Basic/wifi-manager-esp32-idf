idf_component_register(
    SRCS "app_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash app_command
    PRIV_REQUIRES freertos
)
