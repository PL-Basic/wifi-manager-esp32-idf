#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "app_storage.h"
#include "wifi_gateway.h"
#include "device_status.h"
#include "app_command.h"
#include "app_mqtt.h"

#include "env/secrets.h"

static void handle_mqtt_command(const char *topic, int topic_len, const char *payload, int payload_len);
static esp_err_t publish_command_result(const app_command_result_t *result);

// 主题事件状态上发
#define DEVICE_STATUS_TOPIC "wifi/device/" DEVICE_CODE "/event/status"
// 主题任务下发
#define DEVICE_COMMAND_TOPIC "wifi/device/" DEVICE_CODE "/cmd/#"
// 主题结果返回
#define DEVICE_COMMAND_RESULT_TOPIC "wifi/device/" DEVICE_CODE "/event/command-result"

static const char *TAG = "app_main";


wifi_gateway_config_t wifi_config = {
    .sta_ssid = WIFI_SSID,
    .sta_password = WIFI_PASSWORD,
    .ap_ssid = AP_SSID,
    .ap_password = AP_PASSWORD,
    .ap_max_connection = 4,
};

app_mqtt_config_t mqtt_config = {
    .broker_uri = MQTT_BROKER_URI,
    .device_code = DEVICE_CODE,
    .command_topic = DEVICE_COMMAND_TOPIC,
    .command_handler = handle_mqtt_command,
};


static esp_err_t publish_device_status(void)
{
    device_status_snapshot_t snapshot = {0};
    char json_buffer[256] = {0};

    esp_err_t err = device_status_collect(&snapshot);
    if (err != ESP_OK)
    {
        return err;
    }

    err = device_status_to_json(&snapshot, json_buffer, sizeof(json_buffer));
    if (err != ESP_OK)
    {
        return err;
    }

    ESP_LOGI(TAG, "DEVICE status JSON: %s", json_buffer);

    return app_mqtt_publish(DEVICE_STATUS_TOPIC, json_buffer);
}

// 发布ping结果
static esp_err_t publish_command_result(const app_command_result_t *result)
{
    char result_buffer[320] = {0};
    const char *type_text = app_command_type_to_string(result->type);

    int written = snprintf(result_buffer, sizeof(result_buffer), "{\"deviceCode\":\"%s\",\"requestId\":\"%s\",\"type\":\"%s\",\"success\":%s,\"message\":\"%s\"}", DEVICE_CODE, result->request_id, type_text, result->success ? "true" : "false", result->message);
    if (written < 0)
    {
        return ESP_FAIL;
    }

    if ((size_t)written >= sizeof(result_buffer))
    {
        return ESP_ERR_NO_MEM;
    }

    return app_mqtt_publish(DEVICE_COMMAND_RESULT_TOPIC, result_buffer);
}

static void handle_mqtt_command(const char *topic, int topic_len, const char *payload, int payload_len)
{ 
    ESP_LOGI(TAG, "Command handler called, topic: %.*s, payload: %.*s", topic_len, topic, payload_len, payload);
    // request 保存后端发来的命令
    app_command_request_t request = {0};
    // result 保存ESP32 执行后的返回结果
    app_command_result_t result = {0};

    // 解析MQTT topic 和 payload，生成命令请求
    esp_err_t err = app_command_parse(topic,topic_len,payload,payload_len,&request);

    // 无论解析是否成功，都先复制已经识别出的关联信息
    snprintf(result.request_id, sizeof(result.request_id),"%s", request.request_id);
    result.type = request.type;
    result.success = false;

    // 解析失败不代表可以直接结束
    // 仍然需要发布失败结果，让后端知道命令没有被执行
    if (err != ESP_OK)
    {
        snprintf(result.message,sizeof(result.message),"command parse failed: %s",esp_err_to_name(err));

        ESP_LOGE(TAG,"Parse command failed: %s",esp_err_to_name(err));

        esp_err_t publish_err = publish_command_result(&result);
        if (publish_err != ESP_OK)
        {
            ESP_LOGE(TAG,"Publish parse failure result failed: %s",esp_err_to_name(publish_err));
        }

        return;
    }

    // 解析成功，但此时还没有执行命令
    snprintf(result.message,sizeof(result.message),"%s","command not executed");


    switch (request.type)
    {
    case APP_COMMAND_TYPE_PING:
        // PING 不依赖其他模块，main 在这里直接完成执行
        result.success = true;
        
        snprintf(result.message,sizeof(result.message),"%s","pong");

        break;
    
    case APP_COMMAND_TYPE_GET_STATUS:
    {
        // 要求 device_status立即收集并发布一次真实设备状态
        esp_err_t status_err = publish_device_status();
        if (status_err == ESP_OK)
        {
            // 状态确实发布成功，此按钮把命令标记为成功
            result.success = true;
            snprintf(result.message, sizeof(result.message), "%s", "device status published");
        }
        else
        {
            // 状态发布失败，但仍然要发布command_result
            // 让后端知道这条命令执行失败，而不是一直等待。
            result.success = false;
            snprintf(result.message, sizeof(result.message), "status publish failed: %s", esp_err_to_name(status_err));
        
            ESP_LOGE(TAG, "GET_STATUS failed: %s",esp_err_to_name(status_err));
        }
        
        break;
    }

    case APP_COMMAND_TYPE_DISCONNECT_MAC:
        // app_command 已经解析出了MAC
        // 现在交给wifi_gateway执行真实网络操作
        esp_err_t disconnect_err = wifi_gateway_disconnect_client(request.mac);

        if (disconnect_err == ESP_OK)
        {
            result.success = true;
        
            // ESP_OK 只证明驱动接收了断开请求
            snprintf(result.message, sizeof(result.message), "%s", "client disconnect requested");
        }
        else
        {
            result.success = false;
            snprintf(result.message,sizeof(result.message),"disconnect client failed: %s", esp_err_to_name(disconnect_err));
        }
        
        break;

    case APP_COMMAND_TYPE_ALLOW:
    case APP_COMMAND_TYPE_KICK:
    case APP_COMMAND_TYPE_BLOCK_TRAFFIC:
        // 当前只识别了正式topic，还没有解析各自payload
        result.success = false;

        snprintf(result.message,sizeof(result.message),"%s","command payload parsing not implemented");

        break;

    case APP_COMMAND_TYPE_UNKNOWN:
    default:
        // 理论上未知命令已经被app_command_handle拦截了，这里是额外的安全保护。
        ESP_LOGE(TAG, "Unsupported command type");
        return;
    }

    err = publish_command_result(&result);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Publish command result failed: %s", esp_err_to_name(err));
        return;
    }
    
    
}





void app_main(void)
{
    ESP_LOGI(TAG,"WiFi gateway booting");

    ESP_ERROR_CHECK(app_storage_init_nvs());
    ESP_ERROR_CHECK(wifi_gateway_start(&wifi_config));

    bool mqtt_started = false;

    while (true)
    {
        
        // 如果mqtt没启动并且sta获取到了ip，才启动mqtt
        if (!mqtt_started && wifi_gateway_get_status() == WIFI_GATEWAY_STATUS_STA_GOT_IP)
        {
            esp_err_t mqtt_err = app_mqtt_start(&mqtt_config);
            if (mqtt_err == ESP_OK)
            {
                mqtt_started = true;
            }
            else
            {
                ESP_LOGE(TAG, "Start MQTT failed: %s", esp_err_to_name(mqtt_err));
            }
        }

       if (mqtt_started)
       {
            esp_err_t publish_err = publish_device_status();
            if (publish_err != ESP_OK)
            {
                ESP_LOGE(TAG, "Publish device status failed: %s", esp_err_to_name(publish_err));
            }
            
       }
       

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
    
}