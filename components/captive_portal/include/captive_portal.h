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

    // 外部Portal的基础地址
    const char *external_portal_url;

    // 设备编号，会作为参数传给外部Portal
    const char *device_code;

    // 外部Portal域名，例如portal.test。
    // DNS服务会对该域名返回外部服务器真实IP。
    const char *external_portal_domain;

    // 外部Portal服务器的IPv4文本地址
    const char *external_portal_ipv4;

    // 配网表单提交成功后调用。
    captive_portal_provision_handler_t provision_handler;
} captive_portal_config_t;

// 启动本地Captive Portal HTTP服务
esp_err_t captive_portal_start(const captive_portal_config_t *config);
