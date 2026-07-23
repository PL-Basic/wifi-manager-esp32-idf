#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"

#include "app_storage.h"
#include "wifi_gateway.h"
#include "device_status.h"
#include "app_command.h"
#include "app_mqtt.h"
#include "client_access.h"
#include "access_filter.h"
#include "captive_portal.h"

#include "env/secrets.h"

static void handle_mqtt_command(const char *topic, int topic_len, const char *payload, int payload_len);
static esp_err_t publish_command_result(const app_command_result_t *result);
static esp_err_t handle_wifi_provisioning(const char *ssid, const char *password);

// 主题事件状态上发
#define DEVICE_STATUS_TOPIC "wifi/device/" DEVICE_CODE "/event/status"
// 主题任务下发
#define DEVICE_COMMAND_TOPIC "wifi/device/" DEVICE_CODE "/cmd/#"
// 主题结果返回
#define DEVICE_COMMAND_RESULT_TOPIC "wifi/device/" DEVICE_CODE "/event/command-result"
// 主题客户端信号上报
#define DEVICE_CLIENT_SIGNAL_TOPIC "wifi/device/" DEVICE_CODE "/event/client-signal"
#define CLIENT_SIGNAL_MAX_CLIENTS 4
#define CLIENT_SIGNAL_JSON_SIZE 768

static const char *TAG = "app_main";

// 保存从NVS读取的上游WiFi凭据。
// 必须使用静态存储，因为wifi_config中的指针会指向这两个数组。
static app_storage_wifi_credentials_t s_wifi_credentials = {0};

static wifi_gateway_config_t wifi_config = {
    .sta_enabled = false,
    .sta_ssid = NULL,
    .sta_password = NULL,
    .ap_ssid = AP_SSID,
    .ap_password = AP_PASSWORD,
    .ap_max_connection = CLIENT_SIGNAL_MAX_CLIENTS,
};

static captive_portal_config_t portal_config = {
    .provisioning_mode = false,
    .external_portal_url = PORTAL_EXTERNAL_URL,
    .device_code = DEVICE_CODE,
    // 这两个字段交给Portal内部DNS服务使用
    .external_portal_domain = PORTAL_EXTERNAL_DOMAIN,
    .external_portal_ipv4 = PORTAL_SERVER_IPV4,
    
    .provision_handler = handle_wifi_provisioning,
};

static access_filter_config_t access_filter_config = {
    .portal_server_ipv4 = PORTAL_SERVER_IPV4,
};

app_mqtt_config_t mqtt_config = {
    .broker_uri = MQTT_BROKER_URI,
    .device_code = DEVICE_CODE,
    .command_topic = DEVICE_COMMAND_TOPIC,
    .command_handler = handle_mqtt_command,
};

static esp_err_t append_json(char *buffer, size_t size, size_t *used, const char *format, ...)
{
    if (!buffer || !used || !format || *used >= size)
    {
        return ESP_ERR_INVALID_ARG;
    }

    va_list args;
    va_start(args, format);
    int written = vsnprintf(buffer + *used, size - *used, format, args);
    va_end(args);

    if (written < 0)
    {
        return ESP_FAIL;
    }
    if ((size_t)written >= size - *used)
    {
        return ESP_ERR_NO_MEM;
    }
    *used += (size_t)written;
    return ESP_OK;
}

static esp_err_t publish_client_signals(void)
{
    client_access_snapshot_t clients[CLIENT_SIGNAL_MAX_CLIENTS] = {0};
    size_t count = 0, used = 0, emitted = 0;
    static char json[CLIENT_SIGNAL_JSON_SIZE];

    esp_err_t err = client_access_copy_snapshots(clients, CLIENT_SIGNAL_MAX_CLIENTS, &count);
    if (err != ESP_OK || count == 0)
    {
        return err;
    }

    err = append_json(json, sizeof(json), &used, "{\"deviceCode\":\"%s\",\"clients\":[", DEVICE_CODE);
    if (err != ESP_OK)
    {
        return err;
    }

    for (size_t i = 0; i < count; i++)
    {
        if (clients[i].rssi >= 0)
        {
            continue;
        }

        err = append_json(json, sizeof(json), &used,
                          "%s{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
                          "\"sessionId\":%lld,\"rssi\":%d,\"state\":\"%s\"}",
                          emitted++ == 0 ? "" : ",",
                          clients[i].mac[0], clients[i].mac[1], clients[i].mac[2],
                          clients[i].mac[3], clients[i].mac[4], clients[i].mac[5],
                          (long long)clients[i].session_id, (int)clients[i].rssi,
                          client_access_state_to_string(clients[i].state)
                        );

        if (err != ESP_OK)
        {
            return err;
        }
    }

    if (emitted == 0)
    {
        return ESP_OK;
    }

    err = append_json(json, sizeof(json), &used, "]}");
    return err == ESP_OK ? app_mqtt_publish(DEVICE_CLIENT_SIGNAL_TOPIC, json) : err;
}

// 选择正常网关模式使用的开放热点。
static void select_normal_gateway_mode(void)
{
    wifi_config.ap_ssid = AP_SSID;
    wifi_config.ap_password = AP_PASSWORD;

    portal_config.provisioning_mode = false;
}

// 选择管理员本地配网模式。
static void select_provisioning_mode(void)
{
    wifi_config.sta_enabled = false;
    wifi_config.sta_ssid = NULL;
    wifi_config.sta_password = NULL;

    wifi_config.ap_ssid = PROVISION_AP_SSID;
    wifi_config.ap_password = PROVISION_AP_PASSWORD;

    portal_config.provisioning_mode = true;
}

static esp_err_t handle_wifi_provisioning(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    app_storage_wifi_credentials_t credentials = {0};

    int ssid_written = snprintf(credentials.ssid, sizeof(credentials.ssid),"%s",ssid);
    if (ssid_written < 0 || (size_t)ssid_written >= sizeof(credentials.ssid))
    {
        return ESP_ERR_INVALID_ARG;
    }
    
    int password_written = snprintf(credentials.password, sizeof(credentials.password), "%s", password);

    if (password_written < 0 || (size_t)password_written >= sizeof(credentials.password))
    {
        return ESP_ERR_INVALID_ARG;
    }

    // main只负责调度，真正的NVS写入交给app_storage。
    return app_storage_save_wifi_credentials(&credentials);
}


// 根据NVS中的上游WiFi凭据，决定网络启动模式
static esp_err_t prepare_wifi_gateway_config(void)
{
    esp_err_t err = app_storage_load_wifi_credentials(&s_wifi_credentials);

    if (err == ESP_OK)
    {
        if (app_storage_is_recovery_triggered())
        {
            // 还有重试次数：清除标记，尝试用旧凭据连接一次
            // 不清除计数器，这样如果还是连不上，下次计数器会继续增加
            ESP_LOGW(TAG, "Recovery flag set, retry , clearing flag and retrying old credentials");

            esp_err_t clear_err = app_storage_clear_recovery_triggered();
            if (clear_err != ESP_OK)
            {
                // 清除失败也继续尝试连接，不能让标记卡住设备
                ESP_LOGE(TAG, "Clear recovery triggered flag failed: %s", esp_err_to_name(clear_err));
            }

            // 走正常网关模式，尝试连接旧凭据
            select_normal_gateway_mode();
            wifi_config.sta_enabled = true;
            wifi_config.sta_ssid = s_wifi_credentials.ssid;
            wifi_config.sta_password = s_wifi_credentials.password;

            ESP_LOGI(TAG, "Auto-retry with stored upstream WiFi");
            return ESP_OK;


        }
        else
        {
            // 凭据有效，且没有恢复标记：正常网关模式
            select_normal_gateway_mode();
            wifi_config.sta_enabled = true;
            wifi_config.sta_ssid = s_wifi_credentials.ssid;
            wifi_config.sta_password = s_wifi_credentials.password;
            ESP_LOGI(TAG, "Stored upstream WiFi found, normal gateway mode selected");
            return ESP_OK;
        }

    }

    if (err == ESP_ERR_NOT_FOUND)
    {
        // 没有上游凭据，启动受保护的管理员配网热点。
        select_provisioning_mode();
        ESP_LOGI(TAG, "No stored upstream WiFi, provisioning mode selected");

        return ESP_OK;
    }

    if (err == ESP_ERR_INVALID_ARG)
    {
        // NVS中有数据，但数据不符合当前SSID或密码规则。
        // 清除无效数据，避免设备每次启动都读取同一份错误配置。
        ESP_LOGW(TAG, "Stored upstream WiFi is invalid, clearing credentials");

        err = app_storage_clear_wifi_credentials();
        if (err != ESP_OK)
        {
            return err;
        }
        select_provisioning_mode();

        return ESP_OK;
    }

    // Flash读取失败等真正的存储异常不能伪装成“尚未配网”。
    return err;
}

// 执行上游WiFi凭据失效后的恢复调度。
static void handle_upstream_wifi_recovery(void)
{
    ESP_LOGW(TAG, "Upstream WiFi credentials appear invalid, clearing stored credentials");
    // 删除已经连续认证失败的上游WIFI凭据
    esp_err_t err = app_storage_clear_wifi_credentials();

    if (err != ESP_OK)
    {
        // 清除失败时不能重启，否则设备重启仍会读取同一份失效凭据，形成重启循环
        ESP_LOGE(TAG, "Clear upstream WiFi credentials failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGW(TAG, "Upstream WiFi credentials cleared, restarting into provisioning mode");
    // 给串口日志一点发送时间。
    vTaskDelay(pdMS_TO_TICKS(500));
    // 重启不会返回。
    esp_restart();
}

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
    {
        // 传入 ttl_seconds，让 client_access 记录过期时间用于后续自动撤销
        esp_err_t authorize_err = client_access_authorize(request.mac, request.session_id, request.ttl_seconds);

        if (authorize_err == ESP_OK)
        {
            result.success = true;

            // 当前只修改访问状态，数据包放行将在下一阶段接入
            snprintf(result.message, sizeof(result.message), "%s", "client access state authorized");
        }
        else
        {
            result.success = false;

            snprintf(result.message, sizeof(result.message), "authorize client failed: %s", esp_err_to_name(authorize_err));
        }

        break;
    }
    case APP_COMMAND_TYPE_REVOKE_ACCESS:
    {
        // app_command已经解析出MAC和sessionId。
        // client_access负责核对并撤销对应的真实认证会话。
        esp_err_t revoke_err = client_access_revoke_authorization(request.mac, request.session_id);

        if (revoke_err == ESP_OK)
        {
            result.success = true;

            // 这里只撤销外网访问权限，不断开客户端与SoftAP的连接。
            snprintf(result.message, sizeof(result.message), "%s", "client authorization revoked");
        }
        else
        {
            result.success = false;

            snprintf(result.message, sizeof(result.message), "revoke client authorization failed: %s", esp_err_to_name(revoke_err));
        }

        break;
    }

    case APP_COMMAND_TYPE_KICK:
    {
        // KICK 是节点级安全应急复位命令
        // 先发布 command-result 告知后端已收到，再执行设备重启
        ESP_LOGW(TAG, "KICK command received, reason=%s, restarting device", request.mac);

        result.success = true;
        snprintf(result.message, sizeof(result.message), "%s", "device restarting");

        esp_err_t kick_err = publish_command_result(&result);
        if (kick_err != ESP_OK)
        {
            ESP_LOGE(TAG, "Publish KICK result failed: %s", esp_err_to_name(kick_err));
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        return;
    }
    case APP_COMMAND_TYPE_BLOCK_TRAFFIC:
    {
        // BLOCK_TRAFFIC：后端根据告警规则下发目标IP阻断
        ESP_LOGW(TAG, "BLOCK_TRAFFIC command: alertId=%lld, dstIp=%s", (long long)request.alert_id, request.mac);

        result.success = true;
        snprintf(result.message, sizeof(result.message), "block traffic acknowledged, alertId=%lld", (long long)request.alert_id);
        break;
    }
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
    // 先读取NVS并确定正常网关模式或本地配网模式
    ESP_ERROR_CHECK(prepare_wifi_gateway_config());
    // wifi_gateway只负责执行main已经决定好的启动模式
    ESP_ERROR_CHECK(wifi_gateway_start(&wifi_config));
    ESP_ERROR_CHECK(client_access_start());
    // client_access状态表启动后，再启动读取该状态表的数据包过滤器
    ESP_ERROR_CHECK(access_filter_start(&access_filter_config));
    // 过滤器保留发往SoftAP本机的流量，因此未认证客户端仍能访问Portal
    ESP_ERROR_CHECK(captive_portal_start(&portal_config));

    bool mqtt_started = false;

    while (true)
    {
        wifi_gateway_status_t wifi_status = wifi_gateway_get_status();
        if (wifi_status == WIFI_GATEWAY_STATUS_STA_RECOVERY_REQUIRED)
        {
            handle_upstream_wifi_recovery();
            // 只有清除NVS失败时函数才会返回。
            // 此时避免继续执行MQTT和状态发布。
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // 新增：AP 不可达恢复——写标记后重启，不清凭据
        if (wifi_status == WIFI_GATEWAY_STATUS_STA_UNREACHABLE)
        {
            ESP_LOGW(TAG, "Upstream WiFi AP unreachable, setting recovery flag and restarting into provisioning mode");

            esp_err_t flag_err = app_storage_set_recovery_triggered();
            if (flag_err != ESP_OK)
            {
                // 写标记失败不应阻止重启，否则设备永久卡在重连循环
                ESP_LOGE(TAG, "Set recovery triggered flag failed: %s", esp_err_to_name(flag_err));
            }

            // 增加重试计数器，重启后 prepare_wifi_gateway_config 据此判断是否还允许自动重试
            esp_err_t retry_err = app_storage_increment_recovery_retry();
            if (retry_err != ESP_OK)
            {
                ESP_LOGE(TAG, "Increment recovery retry count failed: %s", esp_err_to_name(retry_err));
            }
            // 切 Portal 为配网页面，不重启设备
            captive_portal_set_provisioning_mode(true);
            wifi_gateway_set_status(WIFI_GATEWAY_STATUS_PROVISIONING); // 防止重复触发
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // 如果mqtt没启动并且sta获取到了ip，才启动mqtt
        if (!mqtt_started && wifi_status == WIFI_GATEWAY_STATUS_STA_GOT_IP)
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

       if (mqtt_started && wifi_status == WIFI_GATEWAY_STATUS_STA_GOT_IP)
       {
           esp_err_t publish_err = publish_device_status();
           if (publish_err != ESP_OK)
           {
               ESP_LOGE(TAG, "Publish device status failed: %s", esp_err_to_name(publish_err));
           }

           client_access_expire_check();
           esp_err_t rssi_err = client_access_update_rssi_all();
           if (rssi_err != ESP_OK)
           {
               ESP_LOGE(TAG, "Refresh client RSSI failed: %s", esp_err_to_name(rssi_err));
           }
           else
           {
               esp_err_t signal_err = publish_client_signals();
               if (signal_err != ESP_OK)
                   ESP_LOGE(TAG, "Publish client RSSI failed: %s", esp_err_to_name(signal_err));
           }
       }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
    
}
