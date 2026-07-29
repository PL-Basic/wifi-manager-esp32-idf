#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "app_mqtt.h"

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "mqtt_client.h"

#define APP_MQTT_COMMAND_TOPIC_SIZE 128
#define APP_MQTT_COMMAND_PAYLOAD_SIZE 2048

static const char *TAG = "app_mqtt";

// 创建 MQTT client
static esp_mqtt_client_handle_t s_client = NULL;
// MQTT 连接标识
static bool s_connected = false;
static uint32_t s_connection_generation = 0;
static portMUX_TYPE s_connection_lock = portMUX_INITIALIZER_UNLOCKED;
// 接收下发 topic
static char s_command_topic[APP_MQTT_COMMAND_TOPIC_SIZE] = {0};
static char s_incoming_topic[APP_MQTT_COMMAND_TOPIC_SIZE] = {0};
static char s_incoming_payload[APP_MQTT_COMMAND_PAYLOAD_SIZE] = {0};
static int s_incoming_expected = 0;
static int s_incoming_received = 0;

static app_mqtt_command_handler_t s_command_handler = NULL;

static void set_connection_state(bool connected)
{
    portENTER_CRITICAL(&s_connection_lock);
    s_connected = connected;
    if (connected)
    {
        s_connection_generation++;
    }
    portEXIT_CRITICAL(&s_connection_lock);
}

void app_mqtt_get_connection_state(app_mqtt_connection_state_t *state)
{
    if (state == NULL)
    {
        return;
    }

    portENTER_CRITICAL(&s_connection_lock);
    state->connected = s_connected;
    state->generation = s_connection_generation;
    portEXIT_CRITICAL(&s_connection_lock);
}

static void reset_incoming_command(void)
{
    s_incoming_topic[0] = '\0';
    s_incoming_payload[0] = '\0';
    s_incoming_expected = 0;
    s_incoming_received = 0;
}

static void handle_mqtt_data(esp_mqtt_event_handle_t event)
{
    if (event == NULL || event->data == NULL || event->data_len < 0 || event->total_data_len <= 0 ||
        event->current_data_offset < 0)
    {
        reset_incoming_command();
        return;
    }

    if (event->current_data_offset == 0)
    {
        reset_incoming_command();
        if (event->topic == NULL || event->topic_len <= 0 ||
            event->topic_len >= (int)sizeof(s_incoming_topic) ||
            event->total_data_len >= (int)sizeof(s_incoming_payload))
        {
            ESP_LOGW(TAG, "MQTT command dropped, topic_len=%d, payload_len=%d",
                     event->topic_len, event->total_data_len);
            return;
        }

        memcpy(s_incoming_topic, event->topic, event->topic_len);
        s_incoming_topic[event->topic_len] = '\0';
        s_incoming_expected = event->total_data_len;
    }

    if (s_incoming_expected == 0 ||
        event->current_data_offset != s_incoming_received ||
        event->current_data_offset + event->data_len > s_incoming_expected)
    {
        reset_incoming_command();
        return;
    }

    memcpy(
        s_incoming_payload + event->current_data_offset,
        event->data,
        event->data_len);
    s_incoming_received += event->data_len;

    if (s_incoming_received == s_incoming_expected)
    {
        s_incoming_payload[s_incoming_received] = '\0';
        ESP_LOGI(TAG, "MQTT command received, topic: %s, payload_len=%d",
                 s_incoming_topic, s_incoming_received);
        if (s_command_handler != NULL)
        {
            s_command_handler(
                s_incoming_topic,
                (int)strlen(s_incoming_topic),
                s_incoming_payload,
                s_incoming_received);
        }
        reset_incoming_command();
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        set_connection_state(true);
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
        set_connection_state(false);
        ESP_LOGW(TAG, "MQTT disconnected");
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT subscribed, msg_id: %d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        handle_mqtt_data(event);
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
    app_mqtt_connection_state_t connection = {0};
    app_mqtt_get_connection_state(&connection);
    if (s_client == NULL || !connection.connected)
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
