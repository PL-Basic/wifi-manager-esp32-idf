#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define ACCESS_FILTER_BLOCKED_IPV4_CAPACITY 16
#define ACCESS_FILTER_BLOCKED_HOSTNAME_CAPACITY 16
#define ACCESS_FILTER_HOSTNAME_SIZE 256


// access_filter启动配置
typedef struct
{
    // 外部Portal服务器的IPv4文本地址。
    const char *portal_server_ipv4;
} access_filter_config_t;

// 启动SoftAP客户端IPv4访问过滤器
esp_err_t access_filter_start(const access_filter_config_t *config);

// 原子安装 BLOCK_TRAFFIC 的 IPv4 规则和可选 SNI 规则。
// 任一新规则容量不足或参数非法时，两条规则都不会安装。
esp_err_t access_filter_block_traffic(const char *destination_ipv4, const char *sni);
// 判断 DNS/SNI hostname 是否命中阻断规则。
// 没有 hostname 规则时返回 false；非法 hostname 在规则启用时 fail closed。
bool access_filter_is_hostname_blocked(const char *hostname);