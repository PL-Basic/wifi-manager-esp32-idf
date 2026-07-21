//防止头文件被重复包含
#pragma once

#include "esp_err.h"

// WiFi SSID 最大长度为32字节，额外存一个结束符
#define APP_STORAGE_WIFI_SSID_SIZE 33
// WPA2密码最多使用63个字符，额外存一个结束符
#define APP_STORAGE_WIFI_PASSWORD_SIZE 64

// 保存一组ESP32 STA上游网络凭据。
typedef struct
{
    char ssid[APP_STORAGE_WIFI_SSID_SIZE];
    char password[APP_STORAGE_WIFI_PASSWORD_SIZE];
} app_storage_wifi_credentials_t;

// 初始化nvs
esp_err_t app_storage_init_nvs(void);

// 将新的上游WIFI凭据保存到NVS
// const表示该函数只读取credentials，不允许修改调用放的数据
esp_err_t app_storage_save_wifi_credentials(const app_storage_wifi_credentials_t *credentials);

// 如果没有保存过配置，对外返回通用的ESP_ERR_NOT_FOUND。
esp_err_t app_storage_load_wifi_credentials(app_storage_wifi_credentials_t *credentials);

// 删除已经保存的上游WiFi凭据，用于恢复配网模式。
esp_err_t app_storage_clear_wifi_credentials(void);

// 写入 AP 不可达恢复标记，值为 1 表示凭据仍有效但需要进入配网模式
esp_err_t app_storage_set_recovery_triggered(void);

// 清除 AP 不可达恢复标记
esp_err_t app_storage_clear_recovery_triggered(void);

// 读取 AP 不可达恢复标记，返回 true 表示上次因 AP 不可达进入了恢复模式
bool app_storage_is_recovery_triggered(void);

// 将重试计数器加一，与 recovery_triggered 配合使用
esp_err_t app_storage_increment_recovery_retry(void);

// 读取当前重试计数值。NVS 中无此键时返回 0。
uint8_t app_storage_get_recovery_retry_count(void);