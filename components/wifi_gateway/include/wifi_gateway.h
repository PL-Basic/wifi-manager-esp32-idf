#pragma once

#include <stdint.h>

#include "esp_err.h"

// 状态管理枚举
typedef enum
{
    WIFI_GATEWAY_STATUS_IDLE = 0,
    WIFI_GATEWAY_STATUS_STARTING,
    WIFI_GATEWAY_STATUS_STA_CONNECTING,
    WIFI_GATEWAY_STATUS_STA_CONNECTED,
    WIFI_GATEWAY_STATUS_STA_GOT_IP,
    WIFI_GATEWAY_STATUS_STA_DISCONNECTED,
} wifi_gateway_status_t;

//网关启动所需配置
typedef struct
{
    const char *sta_ssid;
    const char *sta_password;
    const char *ap_ssid;
    const char *ap_password;
    uint8_t ap_max_connection;
} wifi_gateway_config_t;


esp_err_t wifi_gateway_start(const wifi_gateway_config_t *config);

// getter 方法
wifi_gateway_status_t wifi_gateway_get_status(void);
// const 表示只读
const char *wifi_gateway_status_to_string(wifi_gateway_status_t status);
// 获取ip方法
const char *wifi_gateway_get_sta_ip(void);
// 获取客户端数量方法
int wifi_gateway_get_current_clients(void);
// 根据MAC地址断开当前连接到SoftAP的客户端
esp_err_t wifi_gateway_disconnect_client(const char *mac_text);