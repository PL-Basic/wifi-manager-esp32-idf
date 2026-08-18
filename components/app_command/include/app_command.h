#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define APP_COMMAND_REQUEST_ID_SIZE 64
#define APP_COMMAND_TYPE_SIZE 32
#define APP_COMMAND_MESSAGE_SIZE 96
// 标准 MAC 地址有 17 个可见字符，最后还需要一个 '\0'
#define APP_COMMAND_MAC_SIZE 18
// KICK 原因最多 255 个 UTF-8 字节，最后还需要一个 '\0'
#define APP_COMMAND_REASON_SIZE 256
// ALLOW 命令允许的最小授权时长，单位：秒
#define APP_COMMAND_TTL_SECONDS_MIN 1
// ALLOW 命令允许的最大授权时长，单位：秒
#define APP_COMMAND_TTL_SECONDS_MAX 86400
#define APP_COMMAND_DESTINATION_IP_SIZE 46
#define APP_COMMAND_SNI_SIZE 256
#define APP_COMMAND_DEVICE_CODE_SIZE 64
#define APP_COMMAND_WIFI_SSID_SIZE 33
#define APP_COMMAND_WIFI_PASSWORD_SIZE 64

// app_command 将MQTT传来的字符串转换成这个枚举
typedef enum
{
    // 未识别或尚未初始化的命令
    APP_COMMAND_TYPE_UNKNOWN = 0,
    
    // 后端正是业务命令
    APP_COMMAND_TYPE_ALLOW,
    APP_COMMAND_TYPE_REVOKE_ACCESS,
    APP_COMMAND_TYPE_KICK,
    APP_COMMAND_TYPE_DISCONNECT_MAC,
    APP_COMMAND_TYPE_BLOCK_TRAFFIC,
    APP_COMMAND_TYPE_STAGE_WIFI_CONFIG,
    
    // 用于固件链路的测试命令
    // 测试ESP32与后端之间的命令链路
    APP_COMMAND_TYPE_PING,
    // 要求ESP32立即上报一次设备状态
    APP_COMMAND_TYPE_GET_STATUS
} app_command_type_t;

// 生产命令在业务字段校验前即可识别并持久化的不可变身份。
typedef struct
{
    char request_id[APP_COMMAND_REQUEST_ID_SIZE];
    app_command_type_t type;
    char device_code[APP_COMMAND_DEVICE_CODE_SIZE];
} app_command_envelope_t;

// 保存从 MQTT topic 和 payload 解析出的命令输入
typedef struct
{
    // 后端命令编号
    char request_id[APP_COMMAND_REQUEST_ID_SIZE];
    // 从 topic 识别出的命令类型
    app_command_type_t type;
    // MAC地址
    char mac[APP_COMMAND_MAC_SIZE];
    // KICK 的可选原因，不再复用 MAC 缓冲区
    char reason[APP_COMMAND_REASON_SIZE];
    // BLOCK_TRAFFIC 的目标 IP，容量兼容后端最长 45 字符输入。
    char dst_ip[APP_COMMAND_DESTINATION_IP_SIZE];
    // BLOCK_TRAFFIC 的可选规范化域名阻断目标。
    char sni[APP_COMMAND_SNI_SIZE];
    // topic 中的目标设备；KICK/STAGE 还要求 payload 中的设备与其一致。
    char device_code[APP_COMMAND_DEVICE_CODE_SIZE];
    // STAGE_WIFI_CONFIG 的双槽候选配置。
    char wifi_ssid[APP_COMMAND_WIFI_SSID_SIZE];
    char wifi_password[APP_COMMAND_WIFI_PASSWORD_SIZE];
    uint32_t wifi_config_version;
    // 告警编号
    int64_t alert_id;
    // 后端为本次认证创建的会话编号
    int64_t session_id;
    // 本次授权有效时长，单位：秒
    int64_t ttl_seconds;
}app_command_request_t;

typedef struct
{
    // 后端传来的命令编号，用来匹配请求与返回结果
    char request_id[APP_COMMAND_REQUEST_ID_SIZE];
    // 内部使用枚举保存命令类型，避免不同模块反复比较字符串
    // ESP32 内部识别出的命令类型
    app_command_type_t type;
    // 命令是否执行成功
    bool success;
    // 命令执行结果说明
    char message[APP_COMMAND_MESSAGE_SIZE];
} app_command_result_t;


// mqtt分发层

// 把内部命令枚举转换成 MQTT JSON 需要的字符串
const char *app_command_type_to_string(app_command_type_t type);
// 校验命令是否确实发往当前固件身份。
bool app_command_targets_device(
    const app_command_request_t *request,
    const char *local_device_code);
// 只提取生产 topic 和 requestId 身份，不校验业务 payload 字段。
esp_err_t app_command_parse_production_envelope(
    const char *topic,
    int topic_len,
    const char *payload,
    int payload_len,
    app_command_envelope_t *envelope);
// 在已冻结 envelope 上解析并校验生产命令业务字段。
esp_err_t app_command_parse_production_payload(
    const char *payload,
    int payload_len,
    const app_command_envelope_t *envelope,
    app_command_request_t *request);
// 下派任务处理函数
esp_err_t app_command_parse(const char *topic,int topic_len,const char *payload,int payload_len, app_command_request_t *request);

