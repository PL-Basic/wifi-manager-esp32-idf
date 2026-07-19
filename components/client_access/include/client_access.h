# pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

// 客户端访问状态
typedef enum
{
    // 客户端不存在，或当前没有状态
    CLIENT_ACCESS_STATE_UNKNOWN = 0,

    // 已连接热点，但尚未完成认证
    CLIENT_ACCESS_STATE_UNAUTHORIZED,

    // 已完成认证，允许访问外网
    CLIENT_ACCESS_STATE_AUTHORIZED,

    // 被规则或管理员禁止访问
    CLIENT_ACCESS_STATE_BLOCKED
} client_access_state_t;

// 启动客户端状态管理，并监听SoftAP客户端事件
esp_err_t client_access_start(void);

// 修改指定在线客户端的访问状态
esp_err_t client_access_set_state(const uint8_t mac[6], client_access_state_t state);

// 查询指定在线客户端的访问状态
esp_err_t client_access_get_state(const uint8_t mac[6], client_access_state_t *state);

// 这里使用 uint32_t，是因为 IPv4 地址底层就是一个 32 位值。这样公共头文件不需要暴露 lwIP 的内部类型。
// 根据数据包的源IPv4地址判断对应客户端是否已经获得外网转发权限,只有在线并且状态为AUTHORIZED的客户端才返回true
bool client_access_can_forward_ipv4(uint32_t source_ip);

// 将内部状态枚举转换为日志使用的字符串
const char *client_access_state_to_string(client_access_state_t state);
// 将指定在线客户端与后端会话绑定，并标记为已认证
esp_err_t client_access_authorize(const char *mac_text, int64_t session_id);
