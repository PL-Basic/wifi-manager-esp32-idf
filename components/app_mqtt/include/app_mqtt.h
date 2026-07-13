#pragma once

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



// 启动mqtt
esp_err_t app_mqtt_start(const app_mqtt_config_t *config);
// mqtt 发布函数
esp_err_t app_mqtt_publish(const char *topic, const char *payload);
