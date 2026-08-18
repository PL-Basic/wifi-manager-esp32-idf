idf_component_register(
    SRCS "app_command_coordinator.c"
    INCLUDE_DIRS "include"
    REQUIRES app_command app_storage
)
