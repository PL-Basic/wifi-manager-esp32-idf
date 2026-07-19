#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef esp_err_t (*captive_portal_provision_handler_t)(
    const char *ssid,
    const char *password
);

typedef struct 
{
    // true：显示本地上游WiFi配网页面。
    // false：显示普通客户端认证页面。
    bool provisioning_mode;

    // 配网表单提交成功后调用。
    captive_portal_provision_handler_t provision_handler;
} captive_portal_config_t;

// 启动本地Captive Portal HTTP服务
esp_err_t captive_portal_start(const captive_portal_config_t *config);
