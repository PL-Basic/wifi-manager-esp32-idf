idf_component_register(
    SRCS "app_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash app_command
    PRIV_REQUIRES freertos
)

if(DEFINED APP_STORAGE_NVS_PARTITION_OVERRIDE)
    target_compile_definitions(
        ${COMPONENT_LIB}
        PRIVATE
            APP_STORAGE_NVS_PARTITION=${APP_STORAGE_NVS_PARTITION_OVERRIDE}
    )
endif()
