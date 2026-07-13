//防止头文件被重复包含
#pragma once

#include "esp_err.h"

//初始化nvs
esp_err_t app_storage_init_nvs(void);