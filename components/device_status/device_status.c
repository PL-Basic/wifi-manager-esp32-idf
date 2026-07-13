#include <stdio.h>

#include "device_status.h"
#include "wifi_gateway.h"

static const char DEVICE_CODE[] = "esp32-gateway-001";
static const char FIRMWARE_VERSION[] = "0.1.0";

esp_err_t device_status_collect(device_status_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_gateway_status_t wifi_status = wifi_gateway_get_status();

    snapshot->device_code = DEVICE_CODE;
    snapshot->wifi_status = wifi_gateway_status_to_string(wifi_status);
    snapshot->ip = wifi_gateway_get_sta_ip();
    snapshot->current_clients = wifi_gateway_get_current_clients();
    snapshot->firmware_version = FIRMWARE_VERSION;
    
    return ESP_OK;
}

esp_err_t device_status_to_json(const device_status_snapshot_t *snapshot,char *buffer,size_t buffer_size)
{
    if (snapshot == NULL || buffer == NULL || buffer_size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(buffer, buffer_size, "{\"deviceCode\":\"%s\",\"wifiStatus\":\"%s\",\"ip\":\"%s\",\"currentClients\":%d,\"firmwareVersion\":\"%s\"}",snapshot->device_code, snapshot->wifi_status, snapshot->ip, snapshot->current_clients, snapshot->firmware_version);

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