#pragma once

#include "esp_err.h"

// 启动Portal DNS服务。
// 两个参数同时为NULL表示不启用外部Portal域名例外。
esp_err_t portal_dns_start(const char *external_portal_domain, const char *external_portal_ipv4);