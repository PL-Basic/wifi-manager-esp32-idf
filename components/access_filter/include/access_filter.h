#pragma once

#include "esp_err.h"

// access_filter启动配置
typedef struct
{
    // 外部Portal服务器的IPv4文本地址。
    const char *portal_server_ipv4;
} access_filter_config_t;

// 启动SoftAP客户端IPv4访问过滤器
esp_err_t access_filter_start(const access_filter_config_t *config);
