#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// 类型定义，支持注册命令处理函数,函数指针类型
typedef void (*app_mqtt_command_handler_t)(
    const char *topic,
    int topic_len,
    const char *payload,
    int payload_len
);

typedef struct 
{
    const char *broker_uri;
    const char *device_code;
    const char *command_topic;
    app_mqtt_command_handler_t command_handler;
}app_mqtt_config_t;

typedef struct
{
    bool connected;
    // 每次收到新的 MQTT_CONNECTED 事件递增，用于证明候选链路已真正重连。
    uint32_t generation;
} app_mqtt_connection_state_t;



// 启动mqtt
esp_err_t app_mqtt_start(const app_mqtt_config_t *config);
// mqtt 发布函数
esp_err_t app_mqtt_publish(const char *topic, const char *payload);
void app_mqtt_get_connection_state(app_mqtt_connection_state_t *state);
