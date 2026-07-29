#include <limits.h>
#include <stdio.h>

#include "device_status.h"
#include "app_storage.h"
#include "wifi_gateway.h"
#include "cJSON.h"
#include "esp_wifi.h"

static const char DEVICE_CODE[] = "esp32-gateway-001";
static const char FIRMWARE_VERSION[] = "0.1.4";

esp_err_t device_status_collect(device_status_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_gateway_status_t wifi_status = wifi_gateway_get_status();

    snapshot->device_code = DEVICE_CODE;
    snapshot->status = (wifi_status == WIFI_GATEWAY_STATUS_STA_GOT_IP) ? 1 : 0; 
    snapshot->wifi_status = wifi_gateway_status_to_string(wifi_status);
    snapshot->ip = wifi_gateway_get_sta_ip();
    snapshot->current_clients = wifi_gateway_get_current_clients();
    snapshot->firmware_version = FIRMWARE_VERSION;

    app_storage_wifi_config_status_t wifi_config_status = {0};
    esp_err_t storage_err = app_storage_load_wifi_config_status(&wifi_config_status);
    if (storage_err != ESP_OK)
    {
        return storage_err;
    }
    snprintf(snapshot->active_wifi_config_request_id,
             sizeof(snapshot->active_wifi_config_request_id),
             "%s", wifi_config_status.active_request_id);
    snapshot->active_wifi_config_version = wifi_config_status.active_version;
    snprintf(snapshot->pending_wifi_config_request_id,
             sizeof(snapshot->pending_wifi_config_request_id),
             "%s", wifi_config_status.pending_request_id);
    snapshot->pending_wifi_config_version = wifi_config_status.pending_version;
    
    // 采集STA到上游AP的信号强度，GIS定位和信号诊断使用
    wifi_ap_record_t ap_info = {0};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
    {
        snapshot->rssi = ap_info.rssi;
    }
    else
    {
        snapshot->rssi = 0;  // 未连接时RSSI无意义
    }
    
    return ESP_OK;
}

esp_err_t device_status_to_json(const device_status_snapshot_t *snapshot,char *buffer,size_t buffer_size)
{
    if (snapshot == NULL || buffer == NULL || buffer_size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    bool added =
        cJSON_AddStringToObject(root, "deviceCode", snapshot->device_code) != NULL &&
        cJSON_AddNumberToObject(root, "status", snapshot->status) != NULL &&
        cJSON_AddNumberToObject(root, "rssi", snapshot->rssi) != NULL &&
        cJSON_AddStringToObject(root, "wifiStatus", snapshot->wifi_status) != NULL &&
        cJSON_AddStringToObject(root, "ip", snapshot->ip) != NULL &&
        cJSON_AddNumberToObject(root, "currentClients", snapshot->current_clients) != NULL &&
        cJSON_AddStringToObject(root, "firmwareVersion", snapshot->firmware_version) != NULL &&
        cJSON_AddStringToObject(root, "activeWifiConfigRequestId",
                               snapshot->active_wifi_config_request_id) != NULL &&
        cJSON_AddNumberToObject(root, "activeWifiConfigVersion",
                               snapshot->active_wifi_config_version) != NULL &&
        cJSON_AddStringToObject(root, "pendingWifiConfigRequestId",
                               snapshot->pending_wifi_config_request_id) != NULL &&
        cJSON_AddNumberToObject(root, "pendingWifiConfigVersion",
                               snapshot->pending_wifi_config_version) != NULL;

    cJSON_bool printed = false;
    if (added && buffer_size <= INT_MAX)
    {
        printed = cJSON_PrintPreallocated(root, buffer, (int)buffer_size, false);
    }
    cJSON_Delete(root);
    return printed ? ESP_OK : ESP_ERR_NO_MEM;
}
