#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

#include "app_command.h"

#include "esp_log.h"


static const char *TAG = "app_command";

#define COMMAND_PAYLOAD_BUFFER_SIZE 256
// MQTT topic 的本地字符串缓冲区大小
#define COMMAND_TOPIC_BUFFER_SIZE 128

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

    // topic 格式正常，但最后一段不是当前支持的正式命令
    return ESP_ERR_NOT_FOUND;
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

    // 后端数据库生成的告警ID应当大于0
    if (value <= 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out = (int64_t)value;
    
    
    return ESP_OK;
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

    case APP_COMMAND_TYPE_KICK:
        return "KICK";
    
    case APP_COMMAND_TYPE_DISCONNECT_MAC:
        return "DISCONNECT_MAC";
    
    case APP_COMMAND_TYPE_BLOCK_TRAFFIC:
        return "BLOCK_TRAFFIC";

    case APP_COMMAND_TYPE_GET_STATUS:
        return "GET_STATUS";

    case APP_COMMAND_TYPE_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}



esp_err_t app_command_parse(const char *topic,int topic_len,const char *payload,int payload_len,app_command_request_t *request)
{
    // 参数校验
    if (topic == NULL || topic_len <= 0 || payload == NULL || payload_len <= 0 || request == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // 清空调用方传入的整个请求结构体，防止遗留旧命令的数据
    memset(request, 0, sizeof(*request));
    request->type = APP_COMMAND_TYPE_UNKNOWN;

    if (payload_len >= COMMAND_PAYLOAD_BUFFER_SIZE)
    {
        return ESP_ERR_NO_MEM;
    }

    char payload_buffer[COMMAND_PAYLOAD_BUFFER_SIZE] = {0};
    memcpy(payload_buffer, payload, payload_len);
    // 避免没有结束符
    payload_buffer[payload_len] = '\0';

    // 先尝试读取 requestId。
    // requestId 是后端用来匹配“哪一条命令对应哪一个结果”的。
    // 它不是执行命令的必要字段，所以没有也可以继续。
    esp_err_t err = read_json_string_field(payload_buffer, "requestId", request->request_id, sizeof(request->request_id));

    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND)
    {
        return err;
    }

    // 从 MQTT topic 识别后端正式命令
    app_command_type_t topic_command_type = APP_COMMAND_TYPE_UNKNOWN;

    err = read_command_type_from_topic(topic, topic_len, &topic_command_type);
    if (err == ESP_OK)
    {
        // 先保存从topic 识别出的命令类型
        // 即使后续payload 解析失败，调用方仍然知道是哪种命令失败
        request->type = topic_command_type;
        
        // allow 要求放行指定MAC对应的客户端会话
        if (topic_command_type == APP_COMMAND_TYPE_ALLOW)
        {
            err = read_json_string_field(payload_buffer,"mac",request->mac,sizeof(request->mac));
            
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Read allow mac failed: %s", esp_err_to_name(err));

                return err;
            }

            err = read_json_int64_field(payload_buffer,"sessionId",&request->session_id);

            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Read allow sessionId failed: %s",esp_err_to_name(err));
             
                return err;
            }
            
            ESP_LOGI(TAG, "ALLOW parsed mac=%s, sessionId=%lld", request->mac, (long long)request->session_id);

        } // disconnect-mac 命令必须从payload中读取mac和alertId
        else if (topic_command_type == APP_COMMAND_TYPE_DISCONNECT_MAC)
        {
            // 读取要断开的客户端MAC地址
            err = read_json_string_field(payload_buffer,"mac",request->mac,sizeof(request->mac));

            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Read disconnect-mac mac failed: %s", esp_err_to_name(err));
            
                return err;
            }

            // 读取触发本次端口操作的告警编号
            err = read_json_int64_field(payload_buffer,"alertId",&request->alert_id);

            if (err != ESP_OK)
            {
                ESP_LOGE(TAG,"Read disconnect-mac alertId failed: %s", esp_err_to_name(err));
                
                return err;
            }

            // 当前只证明参数解析成功，还没有真正断开客户端
            ESP_LOGI(TAG, "DISCONNECT_MAC parsed, mac=%s, alertId=%lld", request->mac, (long long)request->alert_id);
        }

        ESP_LOGI(TAG,"Formal command recognized:%s",app_command_type_to_string(topic_command_type));
        
        return ESP_OK;
    }

    

    // ESP_ERR_NOT_FOUND 表示这不是当前四种正式 topic。
    if (err != ESP_ERR_NOT_FOUND)
    {
        return err;
    }

    // 再读取type
    // type 是必须字段，因为没有type，就不知道后端要ESP32做什么
    char command_type[APP_COMMAND_TYPE_SIZE] = {0};
    err = read_json_string_field(payload_buffer, "type", command_type, sizeof(command_type));

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Command type not found: %s", payload_buffer);
        return err;
    }

    // 匹配type为PING的json
    if (strcmp(command_type, "PING") == 0)
    {
        // 这里只说明解析出 PING，不代表已经执行成功
        request->type = APP_COMMAND_TYPE_PING;
        return ESP_OK;
    }

    // 匹配识别GET_STATUS
    if (strcmp(command_type, "GET_STATUS") == 0)
    {
        // 这里只说明解析出 GET_STATUS
        request->type = APP_COMMAND_TYPE_GET_STATUS;

        ESP_LOGI(TAG, "Command GET_STATUS received");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Unknown command payload: %s", payload_buffer);
    return ESP_ERR_NOT_FOUND;
}

