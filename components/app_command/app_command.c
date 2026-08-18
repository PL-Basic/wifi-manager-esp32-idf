#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

#include "app_command.h"

#include "cJSON.h"
#include "esp_log.h"


static const char *TAG = "app_command";

#define COMMAND_PAYLOAD_BUFFER_SIZE 2048
// MQTT topic 的本地字符串缓冲区大小
#define COMMAND_TOPIC_BUFFER_SIZE 128

static esp_err_t copy_payload_as_c_string(
    const char *payload,
    int payload_len,
    char **payload_copy)
{
    if (payload_copy == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *payload_copy = NULL;

    if (payload == NULL || payload_len <= 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_len >= COMMAND_PAYLOAD_BUFFER_SIZE)
    {
        return ESP_ERR_NO_MEM;
    }

    char *copy = malloc((size_t)payload_len + 1U);
    if (copy == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, payload, (size_t)payload_len);
    copy[payload_len] = '\0';
    *payload_copy = copy;
    return ESP_OK;
}

static bool is_production_command_type(app_command_type_t type)
{
    return type >= APP_COMMAND_TYPE_ALLOW &&
           type <= APP_COMMAND_TYPE_STAGE_WIFI_CONFIG;
}

// 从MQTT topic 最后一段识别命令类型
static esp_err_t read_command_type_from_topic(const char *topic, int topic_len, app_command_type_t *type)
{
    // 参数校验：topic、topic 长度和输出指针都必须有效
    if (topic == NULL || topic_len <= 0 || type == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    
    *type = APP_COMMAND_TYPE_UNKNOWN;

    // 必须留一个位置存放C字符串结束符'\0'
    if (topic_len >= COMMAND_TOPIC_BUFFER_SIZE)
    {
        return ESP_ERR_NO_MEM;
    }

    // MQTT 提供的 topic 是“指针 + 长度”，不能假设自带 '\0'。
    // 因此先复制到本地数组，再手动补结束符。
    char topic_buffer[COMMAND_TOPIC_BUFFER_SIZE] = {0};
    memcpy(topic_buffer, topic, topic_len);
    topic_buffer[topic_len] = '\0';

    // strrchr 查找字符串中最后一次出现的 '/'。
    const char *command_name = strrchr(topic_buffer,'/');
    // 没有找到 '/'，或者 '/' 后面没有内容，都不是合法命令 topic
    if (command_name == NULL || command_name[1] == '\0')
    {
        return ESP_ERR_NOT_FOUND;
    }
    
    // 指向真正的命令名称
    command_name++;

    if (strcmp(command_name, "allow") == 0)
    {
        *type = APP_COMMAND_TYPE_ALLOW;
        return ESP_OK;
    }

    if (strcmp(command_name, "revoke-access") == 0)
    {
        *type = APP_COMMAND_TYPE_REVOKE_ACCESS;
        return ESP_OK;
    }

    if (strcmp(command_name, "kick") == 0)
    {
        *type = APP_COMMAND_TYPE_KICK;
        return ESP_OK;
    }

    if (strcmp(command_name, "disconnect-mac") == 0)
    {
        *type = APP_COMMAND_TYPE_DISCONNECT_MAC;
        return ESP_OK;
    }

    if (strcmp(command_name, "block-traffic") == 0)
    {
        *type = APP_COMMAND_TYPE_BLOCK_TRAFFIC;
        return ESP_OK;
    }

    if (strcmp(command_name, "stage-wifi-config") == 0)
    {
        *type = APP_COMMAND_TYPE_STAGE_WIFI_CONFIG;
        return ESP_OK;
    }

    // topic 格式正常，但最后一段不是当前支持的正式命令
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t read_device_code_from_topic(
    const char *topic,
    int topic_len,
    char *device_code,
    size_t device_code_size)
{
    static const char topic_prefix[] = "wifi/device/";
    static const char command_marker[] = "/cmd/";

    if (topic == NULL || topic_len <= 0 ||
        device_code == NULL || device_code_size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    device_code[0] = '\0';
    if (topic_len >= COMMAND_TOPIC_BUFFER_SIZE)
    {
        return ESP_ERR_NO_MEM;
    }

    char topic_buffer[COMMAND_TOPIC_BUFFER_SIZE] = {0};
    memcpy(topic_buffer, topic, topic_len);
    topic_buffer[topic_len] = '\0';

    size_t prefix_length = strlen(topic_prefix);
    if (strncmp(topic_buffer, topic_prefix, prefix_length) != 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const char *device_start = topic_buffer + prefix_length;
    const char *marker = strstr(device_start, command_marker);
    if (marker == NULL || marker == device_start ||
        strchr(device_start, '/') != marker ||
        marker[strlen(command_marker)] == '\0' ||
        strchr(marker + strlen(command_marker), '/') != NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t device_length = (size_t)(marker - device_start);
    if (device_length >= device_code_size)
    {
        return ESP_ERR_NO_MEM;
    }

    memcpy(device_code, device_start, device_length);
    device_code[device_length] = '\0';
    return ESP_OK;
}

// 从 JSON 字符串里取指定字段（out = id）
static esp_err_t read_json_string_field(const char *json, const char *key, char *out, size_t out_size)
{
    if (json == NULL || key == NULL || out == NULL || out_size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // 清空输出，去除旧参数影响
    out[0] = '\0';
    
    // 拼出要查找的字段前缀
    // 如果key 是 requestId，那么pattern就是："requestId"
    char pattern[64] = {0};
    int written = snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    if (written < 0 || (size_t)written >= sizeof(pattern))
    {
        return ESP_ERR_NO_MEM;
    }
    
    // 在json中查找这个字段前缀的位置
    char *start = strstr(json, pattern);
    if (start == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    // start 原本指向字段前缀的开头：
    // "requestId":"test-001"
    //
    // 加上 strlen(pattern) 后，start 就跳到真正的值开头：
    // test-001"
    start += strlen(pattern);

    // 从值开头开始，找到下一个双引号，这个双引号就是字符串值的结束位置
    char *end = strchr(start, '"');
    if (end == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // pattern的字段长度
    size_t value_len = end - start;

    // 输出缓冲取必须六i一个位置放 '\0'
    if (value_len >= out_size)
    {
        return ESP_ERR_NO_MEM;
    }

    // 把字段值复制到 out
    memcpy(out, start, value_len);
    // c字符串必须手动补结束符
    out[value_len] = '\0';
    
    return ESP_OK;
}

static esp_err_t read_json_int64_field(const char *json, const char *key, int64_t *out)
{
    // 检查输入和输出参数是否有效
    if (json == NULL || key == NULL || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    //防止解析失败是保留旧值
    *out = 0;

    // 生成要查找的字段格式
    char pattern[64] = {0};
    int written = snprintf(pattern, sizeof(pattern),"\"%s\":",key);
    if (written < 0 || (size_t)written >= sizeof(pattern))
    {
        return ESP_ERR_NO_MEM;
    }
    
    // 查找alertId字段
    const char *start = strstr(json,pattern);
    if (start == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    // 将指针移动到数字开头
    start += strlen(pattern);

    // strtoll 会把字符串形式的证书转换成long long
    errno = 0;
    char *end = NULL;
    // 从start 到 end（从可以变成整数到不能变成整数的字符串的首个字符）
    long long value = strtoll(start,&end,10);

    // start == end 表示没有读取到任何数字
    // ERANGE 表示数字超出了 long long 的表示范围
    if (start == end || errno == ERANGE)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out = (int64_t)value;
    
    
    return ESP_OK;
}

static esp_err_t copy_required_json_string(
    const cJSON *root,
    const char *key,
    char *output,
    size_t output_size,
    bool allow_empty)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t length = strlen(item->valuestring);
    if ((!allow_empty && length == 0) || length >= output_size)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(output, item->valuestring, length + 1);
    return ESP_OK;
}

static void copy_envelope_to_request(
    const app_command_envelope_t *envelope,
    app_command_request_t *request)
{
    memset(request, 0, sizeof(*request));
    request->type = envelope->type;
    snprintf(
        request->request_id,
        sizeof(request->request_id),
        "%s",
        envelope->request_id);
    snprintf(
        request->device_code,
        sizeof(request->device_code),
        "%s",
        envelope->device_code);
}

esp_err_t app_command_parse_production_envelope(
    const char *topic,
    int topic_len,
    const char *payload,
    int payload_len,
    app_command_envelope_t *envelope)
{
    if (envelope == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(envelope, 0, sizeof(*envelope));
    envelope->type = APP_COMMAND_TYPE_UNKNOWN;

    if (topic == NULL || topic_len <= 0 ||
        payload == NULL || payload_len <= 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = read_command_type_from_topic(
        topic,
        topic_len,
        &envelope->type);
    if (err != ESP_OK)
    {
        return err;
    }
    if (!is_production_command_type(envelope->type))
    {
        return ESP_ERR_NOT_FOUND;
    }

    err = read_device_code_from_topic(
        topic,
        topic_len,
        envelope->device_code,
        sizeof(envelope->device_code));
    if (err != ESP_OK)
    {
        return err;
    }
    char *payload_buffer = NULL;
    err = copy_payload_as_c_string(payload, payload_len, &payload_buffer);
    if (err != ESP_OK)
    {
        return err;
    }

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithOpts(payload_buffer, &parse_end, true);
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        free(payload_buffer);
        return ESP_ERR_INVALID_ARG;
    }

    err = copy_required_json_string(
        root,
        "requestId",
        envelope->request_id,
        sizeof(envelope->request_id),
        false);
    cJSON_Delete(root);
    free(payload_buffer);
    return err;
}

static esp_err_t validate_payload_request_identity(
    const char *payload,
    const app_command_envelope_t *envelope)
{
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithOpts(payload, &parse_end, true);
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    char payload_request_id[APP_COMMAND_REQUEST_ID_SIZE] = {0};
    esp_err_t err = copy_required_json_string(
        root,
        "requestId",
        payload_request_id,
        sizeof(payload_request_id),
        false);
    if (err == ESP_OK &&
        strcmp(payload_request_id, envelope->request_id) != 0)
    {
        err = ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(root);
    return err;
}

static esp_err_t parse_stage_wifi_config_payload(
    const char *payload,
    const app_command_envelope_t *envelope,
    app_command_request_t *request)
{
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithOpts(payload, &parse_end, true);
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    char payload_request_id[APP_COMMAND_REQUEST_ID_SIZE] = {0};
    char payload_device_code[APP_COMMAND_DEVICE_CODE_SIZE] = {0};
    esp_err_t err = copy_required_json_string(
        root,
        "requestId",
        payload_request_id,
        sizeof(payload_request_id),
        false);
    if (err == ESP_OK)
    {
        err = copy_required_json_string(
            root,
            "deviceCode",
            payload_device_code,
            sizeof(payload_device_code),
            false);
    }
    if (err == ESP_OK &&
        (strcmp(payload_request_id, envelope->request_id) != 0 ||
         strcmp(payload_device_code, envelope->device_code) != 0))
    {
        err = ESP_ERR_INVALID_ARG;
    }
    if (err == ESP_OK)
    {
        err = copy_required_json_string(
            root, "ssid", request->wifi_ssid, sizeof(request->wifi_ssid), false);
    }
    if (err == ESP_OK)
    {
        err = copy_required_json_string(
            root, "password", request->wifi_password, sizeof(request->wifi_password), true);
    }

    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "configVersion");
    if (err == ESP_OK)
    {
        if (!cJSON_IsNumber(version) || version->valuedouble < 1.0 ||
            version->valuedouble > (double)UINT32_MAX)
        {
            err = ESP_ERR_INVALID_ARG;
        }
        else
        {
            uint32_t parsed_version = (uint32_t)version->valuedouble;
            if ((double)parsed_version != version->valuedouble)
            {
                err = ESP_ERR_INVALID_ARG;
            }
            else
            {
                request->wifi_config_version = parsed_version;
            }
        }
    }

    if (err == ESP_OK)
    {
        size_t ssid_length = strlen(request->wifi_ssid);
        size_t password_length = strlen(request->wifi_password);
        if (ssid_length < 1 || ssid_length > 32 ||
            (password_length > 0 && password_length < 8) ||
            password_length > 63)
        {
            err = ESP_ERR_INVALID_ARG;
        }
    }

    cJSON_Delete(root);
    return err;
}

static esp_err_t parse_kick_payload(
    const char *payload,
    const app_command_envelope_t *envelope,
    app_command_request_t *request)
{
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithOpts(payload, &parse_end, true);
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    char payload_request_id[APP_COMMAND_REQUEST_ID_SIZE] = {0};
    char payload_device_code[APP_COMMAND_DEVICE_CODE_SIZE] = {0};
    esp_err_t err = copy_required_json_string(
        root,
        "requestId",
        payload_request_id,
        sizeof(payload_request_id),
        false);
    if (err == ESP_OK)
    {
        err = copy_required_json_string(
            root,
            "deviceCode",
            payload_device_code,
            sizeof(payload_device_code),
            false);
    }
    if (err == ESP_OK &&
        (strcmp(payload_request_id, envelope->request_id) != 0 ||
         strcmp(payload_device_code, envelope->device_code) != 0))
    {
        err = ESP_ERR_INVALID_ARG;
    }

    const cJSON *reason = cJSON_GetObjectItemCaseSensitive(root, "reason");
    if (err == ESP_OK && reason != NULL)
    {
        if (!cJSON_IsString(reason) || reason->valuestring == NULL)
        {
            err = ESP_ERR_INVALID_ARG;
        }
        else
        {
            size_t length = strlen(reason->valuestring);
            if (length >= sizeof(request->reason))
            {
                err = ESP_ERR_INVALID_ARG;
            }
            else
            {
                memcpy(request->reason, reason->valuestring, length + 1);
            }
        }
    }

    cJSON_Delete(root);
    return err;
}

// 将命令枚举转换为字符串
// app_command 内部使用枚举，发布MQTT JSON时才会被调用转换字符串
const char *app_command_type_to_string(app_command_type_t type)
{
    switch (type)
    {
    case APP_COMMAND_TYPE_PING:
        return "PING";

    case APP_COMMAND_TYPE_ALLOW:
        return "ALLOW";

    case APP_COMMAND_TYPE_REVOKE_ACCESS:
        return "REVOKE_ACCESS";

    case APP_COMMAND_TYPE_KICK:
        return "KICK";
    
    case APP_COMMAND_TYPE_DISCONNECT_MAC:
        return "DISCONNECT_MAC";
    
    case APP_COMMAND_TYPE_BLOCK_TRAFFIC:
        return "BLOCK_TRAFFIC";

    case APP_COMMAND_TYPE_STAGE_WIFI_CONFIG:
        return "STAGE_WIFI_CONFIG";

    case APP_COMMAND_TYPE_GET_STATUS:
        return "GET_STATUS";

    case APP_COMMAND_TYPE_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

bool app_command_targets_device(
    const app_command_request_t *request,
    const char *local_device_code)
{
    return request != NULL &&
           local_device_code != NULL &&
           request->device_code[0] != '\0' &&
           local_device_code[0] != '\0' &&
           strcmp(request->device_code, local_device_code) == 0;
}



esp_err_t app_command_parse_production_payload(
    const char *payload,
    int payload_len,
    const app_command_envelope_t *envelope,
    app_command_request_t *request)
{
    if (payload == NULL || payload_len <= 0 ||
        envelope == NULL || request == NULL ||
        envelope->request_id[0] == '\0' ||
        envelope->device_code[0] == '\0' ||
        !is_production_command_type(envelope->type))
    {
        return ESP_ERR_INVALID_ARG;
    }
    char *payload_buffer = NULL;
    esp_err_t err =
        copy_payload_as_c_string(payload, payload_len, &payload_buffer);
    if (err != ESP_OK)
    {
        return err;
    }

    err = validate_payload_request_identity(
        payload_buffer,
        envelope);
    if (err != ESP_OK)
    {
        copy_envelope_to_request(envelope, request);
        goto cleanup;
    }

    copy_envelope_to_request(envelope, request);
    switch (envelope->type)
    {
    case APP_COMMAND_TYPE_ALLOW:
        err = read_json_string_field(
            payload_buffer,
            "mac",
            request->mac,
            sizeof(request->mac));
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Read ALLOW mac failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
        err = read_json_int64_field(
            payload_buffer,
            "sessionId",
            &request->session_id);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Read ALLOW sessionId failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
        if (request->session_id <= 0)
        {
            err = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }
        err = read_json_int64_field(
            payload_buffer,
            "ttlSeconds",
            &request->ttl_seconds);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Read ALLOW ttlSeconds failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
        if (request->ttl_seconds < APP_COMMAND_TTL_SECONDS_MIN ||
            request->ttl_seconds > APP_COMMAND_TTL_SECONDS_MAX)
        {
            err = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }
        break;

    case APP_COMMAND_TYPE_REVOKE_ACCESS:
        err = read_json_string_field(
            payload_buffer,
            "mac",
            request->mac,
            sizeof(request->mac));
        if (err != ESP_OK)
        {
            goto cleanup;
        }
        err = read_json_int64_field(
            payload_buffer,
            "sessionId",
            &request->session_id);
        if (err != ESP_OK || request->session_id <= 0)
        {
            err = err == ESP_OK ? ESP_ERR_INVALID_ARG : err;
            goto cleanup;
        }
        break;

    case APP_COMMAND_TYPE_DISCONNECT_MAC:
        err = read_json_string_field(
            payload_buffer,
            "mac",
            request->mac,
            sizeof(request->mac));
        if (err != ESP_OK)
        {
            goto cleanup;
        }
        err = read_json_int64_field(
            payload_buffer,
            "alertId",
            &request->alert_id);
        if (err != ESP_OK || request->alert_id < 0)
        {
            err = err == ESP_OK ? ESP_ERR_INVALID_ARG : err;
            goto cleanup;
        }
        break;

    case APP_COMMAND_TYPE_KICK:
        err = parse_kick_payload(payload_buffer, envelope, request);
        if (err != ESP_OK)
        {
            goto cleanup;
        }
        break;

    case APP_COMMAND_TYPE_BLOCK_TRAFFIC:
        err = read_json_int64_field(
            payload_buffer,
            "alertId",
            &request->alert_id);
        if (err != ESP_OK || request->alert_id < 0)
        {
            err = err == ESP_OK ? ESP_ERR_INVALID_ARG : err;
            goto cleanup;
        }
        err = read_json_string_field(
            payload_buffer,
            "dstIp",
            request->dst_ip,
            sizeof(request->dst_ip));
        if (err != ESP_OK)
        {
            goto cleanup;
        }
        err = read_json_string_field(
            payload_buffer,
            "sni",
            request->sni,
            sizeof(request->sni));
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND)
        {
            goto cleanup;
        }
        err = ESP_OK;
        break;

    case APP_COMMAND_TYPE_STAGE_WIFI_CONFIG:
        err = parse_stage_wifi_config_payload(
            payload_buffer,
            envelope,
            request);
        if (err != ESP_OK)
        {
            goto cleanup;
        }
        break;

    case APP_COMMAND_TYPE_UNKNOWN:
    case APP_COMMAND_TYPE_PING:
    case APP_COMMAND_TYPE_GET_STATUS:
    default:
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

cleanup:
    free(payload_buffer);
    if (err == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "Formal command payload valid: %s",
            app_command_type_to_string(envelope->type));
    }
    return err;
}

esp_err_t app_command_parse(
    const char *topic,
    int topic_len,
    const char *payload,
    int payload_len,
    app_command_request_t *request)
{
    if (request == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(request, 0, sizeof(*request));
    request->type = APP_COMMAND_TYPE_UNKNOWN;

    app_command_envelope_t envelope = {0};
    esp_err_t envelope_err = app_command_parse_production_envelope(
        topic,
        topic_len,
        payload,
        payload_len,
        &envelope);
    if (is_production_command_type(envelope.type))
    {
        copy_envelope_to_request(&envelope, request);
        if (envelope_err != ESP_OK)
        {
            return envelope_err;
        }
        return app_command_parse_production_payload(
            payload,
            payload_len,
            &envelope,
            request);
    }
    if (envelope_err != ESP_ERR_NOT_FOUND)
    {
        return envelope_err;
    }

    char *payload_buffer = NULL;
    esp_err_t err =
        copy_payload_as_c_string(payload, payload_len, &payload_buffer);
    if (err != ESP_OK)
    {
        return err;
    }

    err = read_json_string_field(
        payload_buffer,
        "requestId",
        request->request_id,
        sizeof(request->request_id));
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND)
    {
        free(payload_buffer);
        return err;
    }

    char command_type[APP_COMMAND_TYPE_SIZE] = {0};
    err = read_json_string_field(
        payload_buffer,
        "type",
        command_type,
        sizeof(command_type));
    if (err != ESP_OK)
    {
        free(payload_buffer);
        return err;
    }
    if (strcmp(command_type, "PING") == 0)
    {
        request->type = APP_COMMAND_TYPE_PING;
        free(payload_buffer);
        return ESP_OK;
    }
    if (strcmp(command_type, "GET_STATUS") == 0)
    {
        request->type = APP_COMMAND_TYPE_GET_STATUS;
        free(payload_buffer);
        return ESP_OK;
    }

    free(payload_buffer);
    return ESP_ERR_NOT_FOUND;
}

