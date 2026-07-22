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

// 提供给其他模块读取的客户端信息快照。
// 外部模块只能拿到复制后的数据，不能直接修改内部客户端状态表。
typedef struct
{
    // 客户端的六字节MAC地址
    uint8_t mac[6];

    // DHCP分配给客户端的IPv4地址
    uint32_t ipv4;

    // 客户端当前的认证状态
    client_access_state_t state;

    // 客户端到本设备SoftAP的信号强度，单位dBm，用于蹭网检测和三角定位
    int8_t rssi;
} client_access_snapshot_t;

// 启动客户端状态管理，并监听SoftAP客户端事件
esp_err_t client_access_start(void);

// 修改指定在线客户端的访问状态
esp_err_t client_access_set_state(const uint8_t mac[6], client_access_state_t state);

// 查询指定在线客户端的访问状态
esp_err_t client_access_get_state(const uint8_t mac[6], client_access_state_t *state);

// 根据客户端IPv4地址读取一份状态快照
esp_err_t client_access_get_snapshot_by_ipv4(uint32_t source_ip, client_access_snapshot_t *snapshot);

// 这里使用 uint32_t，是因为 IPv4 地址底层就是一个 32 位值。这样公共头文件不需要暴露 lwIP 的内部类型。
// 根据数据包的源IPv4地址判断对应客户端是否已经获得外网转发权限,只有在线并且状态为AUTHORIZED的客户端才返回true
bool client_access_can_forward_ipv4(uint32_t source_ip);

// 将内部状态枚举转换为日志使用的字符串
const char *client_access_state_to_string(client_access_state_t state);
// 将指定在线客户端与后端会话绑定，并标记为已认证
esp_err_t client_access_authorize(const char *mac_text, int64_t session_id, uint32_t ttl_seconds);
// 撤销指定客户端当前会话的认证权限。
esp_err_t client_access_revoke_authorization(const char *mac_text, int64_t session_id);
// 周期性扫描所有在线客户端，将已过期的授权状态降为 UNAUTHORIZED。
void client_access_expire_check(void);
// 通过esp_wifi_ap_get_sta_list()刷新所有在线客户端的RSSI，为后端蹭网检测和三角定位提供实时信号数据
void client_access_update_rssi_all(void);