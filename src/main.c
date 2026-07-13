#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "app_storage.h"
#include "wifi_gateway.h"
#include "device_status.h"
#include "app_mqtt.h"

#include "env/secrets.h"

static void handle_mqtt_command(const char *topic, int topic_len, const char *payload, int payload_len);

// 主题事件状态上发
#define DEVICE_STATUS_TOPIC "wifi/device/" DEVICE_CODE "/event/status"
// 主题任务下发
#define DEVICE_COMMAND_TOPIC "wifi/device/" DEVICE_CODE "/command/#"

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


static void handle_mqtt_command(const char *topic, int topic_len, const char *payload, int payload_len)
{
    ESP_LOGI(TAG, "Command handler called, topic: %.*s, payload: %.*s", topic_len, topic, payload_len, payload);
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