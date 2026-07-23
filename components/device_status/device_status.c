#include <stdio.h>

#include "device_status.h"
#include "wifi_gateway.h"
#include "esp_wifi.h"

static const char DEVICE_CODE[] = "esp32-gateway-001";
static const char FIRMWARE_VERSION[] = "0.1.2";

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

    int written = snprintf(buffer, buffer_size,
        "{\"deviceCode\":\"%s\",\"status\":%d,\"rssi\":%d,\"wifiStatus\":\"%s\",\"ip\":\"%s\",\"currentClients\":%d,\"firmwareVersion\":\"%s\"}",
        snapshot->device_code, snapshot->status, snapshot->rssi,
        snapshot->wifi_status, snapshot->ip,
        snapshot->current_clients, snapshot->firmware_version);

    if (written < 0)
    {
        return ESP_FAIL;
    }

    if ((size_t)written >= buffer_size)
    {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}