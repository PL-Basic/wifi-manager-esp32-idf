cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wifi-manager-esp32-idf)


# 获取ESP-IDF内部的lwIP组件目标
idf_component_get_property(lwip_component lwip COMPONENT_LIB)

# 让lwIP编译时能够找到我们的自定义钩子头文件
target_compile_options(
    ${lwip_component}
    PRIVATE
    "-I${PROJECT_DIR}/components/access_filter/include"
)

# 指定ESP-IDF额外包含的lwIP钩子文件
target_compile_definitions(
    ${lwip_component}
    PRIVATE
    "-DESP_IDF_LWIP_HOOK_FILENAME=\"access_filter_lwip_hook.h\""
) 