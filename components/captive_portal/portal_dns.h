#pragma once

#include "esp_err.h"

// 启动Captive Portal内部使用的DNS服务
// 该函数只在captive_portal组件内部调用，因此头文件不放入公共include
esp_err_t portal_dns_start(void);