#pragma once
#include <stddef.h>

#include "esp_err.h"

typedef struct 
{
    const char *device_code;
    int status;
    const char *wifi_status;
    const char *ip;
    int current_clients;
    const char *firmware_version;
} device_status_snapshot_t;

// device的状态信息集合
esp_err_t device_status_collect(device_status_snapshot_t *snapshot);
// device的状态信息转json，snapshot：输入数据，buffer：输出缓冲区，buffer_size：缓冲区大小
esp_err_t device_status_to_json(const device_status_snapshot_t *snapshot,char *buffer,size_t buffer_size);
