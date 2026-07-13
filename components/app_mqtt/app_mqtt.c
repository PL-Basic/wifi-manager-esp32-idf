#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "app_mqtt.h"

#include "esp_log.h"
#include "mqtt_client.h"

static const char *TAG = "app_mqtt";

// 创建 MQTT client
static esp_mqtt_client_handle_t s_client = NULL;
// MQTT 连接标识
static bool s_connected = false;
// 接收下发 topic
static char s_command_topic[128] = {0};

static app_mqtt_command_handler_t s_command_handler = NULL;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "MQTT connected");
        int msg_id = esp_mqtt_client_subscribe(s_client, s_command_topic, 1);
        if (msg_id < 0)
        {
            ESP_LOGE(TAG, "MQTT subscribe failed, topic: %s", s_command_topic);
        }
        else
        {
            ESP_LOGI(TAG, "MQTT subscribe requested, topic: %s, msg_id: %d", s_command_topic, msg_id);
        }
        break;
    
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT subscribed, msg_id: %d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT command received, topic: %.*s, payload: %.*s", event->topic_len, event->topic, event->data_len, event->data);
        if (s_command_handler != NULL)
        {
            s_command_handler(event->topic, event->topic_len, event->data, event->data_len);
        }
        
        break;

    default:
        break;
    }

}

esp_err_t app_mqtt_start(const app_mqtt_config_t *config)
{
    // MQTT 内容检验
    if (config == NULL || config->broker_uri == NULL || strlen(config->broker_uri) == 0 || config->command_topic == NULL || strlen(config->command_topic) == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_client != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = config->broker_uri,
    };

    // 保存 topic
    int written = snprintf(s_command_topic, sizeof(s_command_topic), "%s", config->command_topic);
    if (written < 0 || (size_t)written >= sizeof(s_command_topic))
    {
        return ESP_ERR_NO_MEM;
    }
    
    s_command_handler = config->command_handler;

    // MQTT 初始化
    s_client = esp_mqtt_client_init(&mqtt_config);
    if (s_client == NULL)
    {
        return ESP_FAIL;
    }
    
    // MQTT 事件注册回调
    esp_err_t err = esp_mqtt_client_register_event(s_client,ESP_EVENT_ANY_ID,mqtt_event_handler,NULL);

    if (err != ESP_OK)
    {
        return err;
    }

    // MQTT client启动
    err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK)
    {
        return err;
    }

    ESP_LOGI(TAG, "MQTT client started, broker: %s", config->broker_uri);

    return ESP_OK;
}


esp_err_t app_mqtt_publish(const char *topic, const char *payload)
{
    // 依旧先对参数进行校验
    if (topic == NULL || payload == NULL || strlen(topic) == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // MQTT 没启动 或者 没连接
    if (s_client == NULL || !s_connected)
    {
        return ESP_ERR_INVALID_STATE;
    }
    
    // 执行发布
    int msg_id = esp_mqtt_client_publish(s_client, topic , payload , 0, 1, 0);
    if (msg_id < 0)
    {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "MQTT published, topic: %s, msg_id: %d", topic, msg_id);

    return ESP_OK;

}