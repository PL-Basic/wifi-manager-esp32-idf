#include <stdio.h>
#include <string.h>

#include "app_storage.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

// NVS使用namespace隔离不同模块的数据
#define APP_STORAGE_NAMESPACE "wifi_config"
// namespace内部的两个字段名
#define APP_STORAGE_WIFI_SSID_KEY "sta_ssid"
#define APP_STORAGE_WIFI_PASSWORD_KEY "sta_password"
#define APP_STORAGE_CANDIDATE_SSID_KEY "cand_ssid"
#define APP_STORAGE_CANDIDATE_PASSWORD_KEY "cand_pass"
#define APP_STORAGE_CANDIDATE_REQUEST_ID_KEY "cand_req"
#define APP_STORAGE_CANDIDATE_VERSION_KEY "cand_ver"
#define APP_STORAGE_ACTIVE_REQUEST_ID_KEY "active_req"
#define APP_STORAGE_ACTIVE_VERSION_KEY "active_ver"
// AP不可达恢复标记键名，uint8_t类型，1=恢复模式（凭据保留），0或无此键=正常
#define APP_STORAGE_RECOVERY_TRIGGERED_KEY "recv_trig"
// AP不可达自动重试计数器键名，uint8_t类型，每次STA_UNREACHABLE加一
#define APP_STORAGE_RECOVERY_RETRY_KEY "recv_retry"
#define APP_STORAGE_COMMAND_RESULT_NAMESPACE "cmd_result"
#define APP_STORAGE_COMMAND_RESULT_NEXT_KEY "next"
#define APP_STORAGE_COMMAND_RESULT_CACHE_SIZE 16
#define APP_STORAGE_COMMAND_RESULT_RECORD_VERSION 1
#define APP_STORAGE_COMMAND_PENDING_RECORD_VERSION 2

#ifdef APP_STORAGE_NVS_PARTITION
#define APP_STORAGE_STRINGIFY_VALUE(value) #value
#define APP_STORAGE_STRINGIFY(value) APP_STORAGE_STRINGIFY_VALUE(value)
#define APP_STORAGE_NVS_PARTITION_NAME \
    APP_STORAGE_STRINGIFY(APP_STORAGE_NVS_PARTITION)
#endif

static const char *TAG = "app_storage";
static SemaphoreHandle_t s_wifi_config_mutex = NULL;

static esp_err_t app_storage_open(
    const char *namespace_name,
    nvs_open_mode_t open_mode,
    nvs_handle_t *handle)
{
#ifdef APP_STORAGE_NVS_PARTITION
    return nvs_open_from_partition(
        APP_STORAGE_NVS_PARTITION_NAME,
        namespace_name,
        open_mode,
        handle);
#else
    return nvs_open(namespace_name, open_mode, handle);
#endif
}

typedef struct
{
    uint32_t version;
    char request_id[APP_COMMAND_REQUEST_ID_SIZE];
    uint32_t type;
    uint8_t success;
    char message[APP_COMMAND_MESSAGE_SIZE];
} app_storage_command_result_record_t;

static esp_err_t lock_wifi_config(void)
{
    if (s_wifi_config_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTake(s_wifi_config_mutex, portMAX_DELAY) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

static void unlock_wifi_config(void)
{
    xSemaphoreGive(s_wifi_config_mutex);
}

static bool is_production_command_type(app_command_type_t type)
{
    return type >= APP_COMMAND_TYPE_ALLOW &&
           type <= APP_COMMAND_TYPE_STAGE_WIFI_CONFIG;
}

static bool command_result_is_valid(const app_command_result_t *result)
{
    if (result == NULL || !is_production_command_type(result->type))
    {
        return false;
    }

    size_t request_id_length =
        strnlen(result->request_id, sizeof(result->request_id));
    size_t message_length =
        strnlen(result->message, sizeof(result->message));
    return request_id_length > 0 &&
           request_id_length < sizeof(result->request_id) &&
           message_length < sizeof(result->message);
}

static void command_result_slot_key(
    uint8_t slot,
    char *key,
    size_t key_size)
{
    snprintf(key, key_size, "result_%02u", (unsigned)slot);
}

static esp_err_t load_command_result_record(
    nvs_handle_t handle,
    uint8_t slot,
    app_storage_command_result_record_t *record)
{
    char key[16] = {0};
    command_result_slot_key(slot, key, sizeof(key));
    memset(record, 0, sizeof(*record));
    size_t size = sizeof(*record);
    return nvs_get_blob(handle, key, record, &size);
}

static bool command_result_record_is_valid(
    const app_storage_command_result_record_t *record)
{
    if (record == NULL ||
        record->version != APP_STORAGE_COMMAND_RESULT_RECORD_VERSION ||
        record->type < (uint32_t)APP_COMMAND_TYPE_ALLOW ||
        record->type > (uint32_t)APP_COMMAND_TYPE_STAGE_WIFI_CONFIG)
    {
        return false;
    }

    size_t request_id_length =
        strnlen(record->request_id, sizeof(record->request_id));
    size_t message_length =
        strnlen(record->message, sizeof(record->message));
    return request_id_length > 0 &&
           request_id_length < sizeof(record->request_id) &&
           message_length < sizeof(record->message);
}

static bool command_result_record_has_identity(
    const app_storage_command_result_record_t *record)
{
    if (record == NULL ||
        (record->version != APP_STORAGE_COMMAND_RESULT_RECORD_VERSION &&
         record->version != APP_STORAGE_COMMAND_PENDING_RECORD_VERSION) ||
        record->type < (uint32_t)APP_COMMAND_TYPE_ALLOW ||
        record->type > (uint32_t)APP_COMMAND_TYPE_STAGE_WIFI_CONFIG)
    {
        return false;
    }

    size_t request_id_length =
        strnlen(record->request_id, sizeof(record->request_id));
    return request_id_length > 0 &&
           request_id_length < sizeof(record->request_id);
}

static void command_result_from_record(
    const app_storage_command_result_record_t *record,
    app_command_result_t *result)
{
    memset(result, 0, sizeof(*result));
    snprintf(
        result->request_id,
        sizeof(result->request_id),
        "%s",
        record->request_id);
    result->type = (app_command_type_t)record->type;
    result->success = record->success != 0;
    snprintf(
        result->message,
        sizeof(result->message),
        "%s",
        record->message);
}

static app_storage_command_result_record_t command_result_record(
    const app_command_result_t *result)
{
    app_storage_command_result_record_t record = {0};
    record.version = APP_STORAGE_COMMAND_RESULT_RECORD_VERSION;
    snprintf(
        record.request_id,
        sizeof(record.request_id),
        "%s",
        result->request_id);
    record.type = (uint32_t)result->type;
    record.success = result->success ? 1 : 0;
    snprintf(
        record.message,
        sizeof(record.message),
        "%s",
        result->message);
    return record;
}

// 检查调用方提供的上游WiFi平局释放可以安全保存
static esp_err_t validate_wifi_credentials(const app_storage_wifi_credentials_t *credentials)
{
    if (credentials == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // strnlen最多检查数组容量个字符
    // 如果数组中没有'\0'，它会返回整个数组容量，不会继续越界读取
    size_t ssid_length = strnlen(credentials->ssid, sizeof(credentials->ssid));
    size_t password_length = strnlen(credentials->password, sizeof(credentials->password));

    // SSID不能为空，并且数组中必须存在字符串结束符
    if (ssid_length == 0 || ssid_length >= sizeof(credentials->ssid))
    {
        return ESP_ERR_INVALID_ARG;
    }

    // 密码数组中也必须存在字符串结束符
    if (password_length >= sizeof(credentials->password))
    {
        return ESP_ERR_INVALID_ARG;
    }

    // 当前允许开放式上游网络，或者使用8至63字符的WPA2密码。
    if (password_length > 0 && password_length < 8)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t validate_wifi_config(const app_storage_wifi_config_t *config)
{
    if (config == NULL || config->config_version == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t request_id_length = strnlen(config->request_id, sizeof(config->request_id));
    if (request_id_length == 0 || request_id_length >= sizeof(config->request_id))
    {
        return ESP_ERR_INVALID_ARG;
    }

    return validate_wifi_credentials(&config->credentials);
}

static bool wifi_credentials_equal(
    const app_storage_wifi_credentials_t *left,
    const app_storage_wifi_credentials_t *right)
{
    return left != NULL &&
           right != NULL &&
           strcmp(left->ssid, right->ssid) == 0 &&
           strcmp(left->password, right->password) == 0;
}

static esp_err_t load_wifi_credentials_from_handle(
    nvs_handle_t handle,
    app_storage_wifi_credentials_t *credentials)
{
    if (credentials == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(credentials, 0, sizeof(*credentials));
    size_t ssid_size = sizeof(credentials->ssid);
    size_t password_size = sizeof(credentials->password);
    esp_err_t err = nvs_get_str(
        handle,
        APP_STORAGE_WIFI_SSID_KEY,
        credentials->ssid,
        &ssid_size);
    if (err == ESP_OK)
    {
        err = nvs_get_str(
            handle,
            APP_STORAGE_WIFI_PASSWORD_KEY,
            credentials->password,
            &password_size);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK)
    {
        err = validate_wifi_credentials(credentials);
    }
    if (err != ESP_OK)
    {
        memset(credentials, 0, sizeof(*credentials));
    }
    return err;
}

static esp_err_t erase_key_if_present(nvs_handle_t handle, const char *key, bool *changed)
{
    esp_err_t err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        return ESP_OK;
    }
    if (err == ESP_OK && changed != NULL)
    {
        *changed = true;
    }
    return err;
}

static esp_err_t erase_candidate_from_handle(nvs_handle_t handle, bool *changed)
{
    const char *keys[] = {
        APP_STORAGE_CANDIDATE_SSID_KEY,
        APP_STORAGE_CANDIDATE_PASSWORD_KEY,
        APP_STORAGE_CANDIDATE_REQUEST_ID_KEY,
        APP_STORAGE_CANDIDATE_VERSION_KEY,
    };

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
    {
        esp_err_t err = erase_key_if_present(handle, keys[i], changed);
        if (err != ESP_OK)
        {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t load_candidate_from_handle(
    nvs_handle_t handle,
    app_storage_wifi_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));
    size_t ssid_size = sizeof(config->credentials.ssid);
    size_t password_size = sizeof(config->credentials.password);
    size_t request_id_size = sizeof(config->request_id);

    esp_err_t err = nvs_get_str(
        handle,
        APP_STORAGE_CANDIDATE_SSID_KEY,
        config->credentials.ssid,
        &ssid_size);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        return ESP_ERR_NOT_FOUND;
    }
    if (err == ESP_OK)
    {
        err = nvs_get_str(
            handle,
            APP_STORAGE_CANDIDATE_PASSWORD_KEY,
            config->credentials.password,
            &password_size);
    }
    if (err == ESP_OK)
    {
        err = nvs_get_str(
            handle,
            APP_STORAGE_CANDIDATE_REQUEST_ID_KEY,
            config->request_id,
            &request_id_size);
    }
    if (err == ESP_OK)
    {
        err = nvs_get_u32(
            handle,
            APP_STORAGE_CANDIDATE_VERSION_KEY,
            &config->config_version);
    }
    if (err != ESP_OK)
    {
        memset(config, 0, sizeof(*config));
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_INVALID_STATE : err;
    }

    err = validate_wifi_config(config);
    if (err != ESP_OK)
    {
        memset(config, 0, sizeof(*config));
    }
    return err;
}

static esp_err_t load_metadata_pair(
    nvs_handle_t handle,
    const char *request_id_key,
    const char *version_key,
    char *request_id,
    size_t request_id_size,
    uint32_t *version,
    bool *present)
{
    if (request_id == NULL || request_id_size == 0 || version == NULL || present == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    request_id[0] = '\0';
    *version = 0;
    *present = false;

    size_t stored_size = request_id_size;
    esp_err_t request_err = nvs_get_str(handle, request_id_key, request_id, &stored_size);
    esp_err_t version_err = nvs_get_u32(handle, version_key, version);

    if (request_err == ESP_ERR_NVS_NOT_FOUND && version_err == ESP_ERR_NVS_NOT_FOUND)
    {
        return ESP_OK;
    }
    if (request_err != ESP_OK || version_err != ESP_OK || request_id[0] == '\0' || *version == 0)
    {
        request_id[0] = '\0';
        *version = 0;
        return request_err != ESP_OK && request_err != ESP_ERR_NVS_NOT_FOUND
                   ? request_err
                   : (version_err != ESP_OK && version_err != ESP_ERR_NVS_NOT_FOUND
                          ? version_err
                          : ESP_ERR_INVALID_STATE);
    }

    *present = true;
    return ESP_OK;
}

esp_err_t app_storage_increment_recovery_retry(void)
{
    nvs_handle_t handle;
    esp_err_t err = app_storage_open(APP_STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        return err;
    }

    // 先读取当前值，再写入加一后的值
    uint8_t count = 0;
    esp_err_t read_err = nvs_get_u8(handle, APP_STORAGE_RECOVERY_RETRY_KEY, &count);
    if (read_err != ESP_OK && read_err != ESP_ERR_NVS_NOT_FOUND)
    {
        nvs_close(handle);
        return read_err;
    }

    // 防止整数回绕，到达 255 后不再增加
    if (count < 255)
    {
        count++;
    }

    err = nvs_set_u8(handle, APP_STORAGE_RECOVERY_RETRY_KEY, count);
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Recovery retry count incremented to %u", (unsigned)count);
    }
    return err;
}

uint8_t app_storage_get_recovery_retry_count(void)
{
    nvs_handle_t handle;
    esp_err_t err = app_storage_open(APP_STORAGE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK)
    {
        return 0;
    }

    uint8_t count = 0;
    err = nvs_get_u8(handle, APP_STORAGE_RECOVERY_RETRY_KEY, &count);
    nvs_close(handle);

    // 键不存在也返回 0，符合"从未触发过恢复"的语义
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        return 0;
    }
    return count;
}

esp_err_t app_storage_save_wifi_credentials(const app_storage_wifi_credentials_t *credentials)
{
    esp_err_t err = validate_wifi_credentials(credentials);
    if (err != ESP_OK)
    {
        return err;
    }

    err = lock_wifi_config();
    if (err != ESP_OK)
    {
        return err;
    }

    nvs_handle_t handle;

    // 以可读写模式打开wifi_config命名空间。
    // handle是后续读写这个命名空间时使用的操作句柄。
    err = app_storage_open(APP_STORAGE_NAMESPACE, NVS_READWRITE, &handle);

    if (err != ESP_OK)
    {
        unlock_wifi_config();
        return err;
    }

    // 把SSID保存到sta_ssid字段
    err = nvs_set_str(handle, APP_STORAGE_WIFI_SSID_KEY, credentials->ssid);

    // 前一步成功后才保存密码，避免覆盖真正的错误码。
    if (err == ESP_OK)
    {
        err = nvs_set_str(handle, APP_STORAGE_WIFI_PASSWORD_KEY, credentials->password);
    }

    // Portal 新配网代表一份不带 MQTT 版本的新 current，旧控制元数据必须同步清理。
    bool changed = false;
    if (err == ESP_OK)
    {
        err = erase_candidate_from_handle(handle, &changed);
    }
    if (err == ESP_OK)
    {
        err = erase_key_if_present(handle, APP_STORAGE_ACTIVE_REQUEST_ID_KEY, &changed);
    }
    if (err == ESP_OK)
    {
        err = erase_key_if_present(handle, APP_STORAGE_ACTIVE_VERSION_KEY, &changed);
    }
    if (err == ESP_OK)
    {
        err = erase_key_if_present(handle, APP_STORAGE_RECOVERY_TRIGGERED_KEY, &changed);
    }
    if (err == ESP_OK)
    {
        err = erase_key_if_present(handle, APP_STORAGE_RECOVERY_RETRY_KEY, &changed);
    }
    if (err == ESP_OK)
    {
        // current、候选清理和恢复标记清理在同一次提交中生效。
        err = nvs_commit(handle);
    }

    // 只要nvs_open成功，无论后面的操作成功还是失败，都必须关闭句柄
    nvs_close(handle);
    unlock_wifi_config();

    if (err == ESP_OK)
    {
        // 可以记录SSID，但绝对不能把用户的WiFi密码打印到串口日志。
        ESP_LOGI(TAG, "Upstream WiFi credentials saved, ssid=%s", credentials->ssid);
    }
    else
    {
        ESP_LOGE(TAG, "Save upstream WiFi credentials failed: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t app_storage_load_wifi_credentials(app_storage_wifi_credentials_t * credentials)
{
    if (credentials == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // 先清空调用方的结构体，避免读取失败时残留旧凭据
    memset(credentials, 0, sizeof(*credentials));

    nvs_handle_t handle;

    // 读取时只需要NVS_READONLY权限
    esp_err_t err = app_storage_open(APP_STORAGE_NAMESPACE, NVS_READONLY, &handle);

    if (err != ESP_OK)
    {
        // NVS命名空间不存在，对外统一表示“上游配置不存在”。
        if (err == ESP_ERR_NVS_NOT_FOUND)
        {
            ESP_LOGW(TAG, "Upstream WiFi credentials not found");
            return ESP_ERR_NOT_FOUND;
        }

        ESP_LOGE(TAG, "Open WiFi configuration namespace failed: %s", esp_err_to_name(err));

        return err;
    }

    // length传入的是目标数组的总容量。
    // nvs_get_str成功后，length会被更新为实际读取长度，
    // 并且这个长度包含字符串结尾的'\0'。
    size_t ssid_size = sizeof(credentials->ssid);
    size_t password_size = sizeof(credentials->password);

    err = nvs_get_str(handle, APP_STORAGE_WIFI_SSID_KEY, credentials->ssid,&ssid_size);
    if (err == ESP_OK)
    {
        err = nvs_get_str(handle, APP_STORAGE_WIFI_PASSWORD_KEY, credentials->password, &password_size);
    }

    // 无论读取成功还是失败，只要打开过句柄，就必须关闭。
    nvs_close(handle);

    if (err != ESP_OK)
    {
        if (err == ESP_ERR_NVS_NOT_FOUND)
        {
            ESP_LOGW(TAG, "Upstream WiFi credentials not found");
            return ESP_ERR_NOT_FOUND;
        }

        ESP_LOGE(TAG, "Load upstream WiFi credentials failed: %s", esp_err_to_name(err));
        return err;
    }

    // 再次检查从NVS读取的数据释放符合当前规则
    err = validate_wifi_credentials(credentials);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Stored WiFi credentials are invalid");
        memset(credentials, 0, sizeof(*credentials));
        return err;
    }

    ESP_LOGI(TAG, "Upstream WiFi credentials loaded ssid=%s", credentials->ssid);
    return ESP_OK;
} 

esp_err_t app_storage_load_candidate_wifi_config(app_storage_wifi_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));
    esp_err_t err = lock_wifi_config();
    if (err != ESP_OK)
    {
        return err;
    }

    nvs_handle_t handle;
    err = app_storage_open(APP_STORAGE_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        unlock_wifi_config();
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK)
    {
        unlock_wifi_config();
        return err;
    }

    err = load_candidate_from_handle(handle, config);
    nvs_close(handle);
    unlock_wifi_config();
    return err;
}

esp_err_t app_storage_stage_candidate_wifi_config(
    const app_storage_wifi_config_t *config,
    app_storage_wifi_stage_result_t *stage_result)
{
    if (stage_result == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *stage_result = APP_STORAGE_WIFI_STAGE_STORED;

    esp_err_t err = validate_wifi_config(config);
    if (err != ESP_OK)
    {
        return err;
    }

    err = lock_wifi_config();
    if (err != ESP_OK)
    {
        return err;
    }

    nvs_handle_t handle;
    err = app_storage_open(APP_STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        unlock_wifi_config();
        return err;
    }

    app_storage_wifi_config_t pending = {0};
    bool pending_present = false;
    err = load_candidate_from_handle(handle, &pending);
    if (err == ESP_OK)
    {
        pending_present = true;
    }
    else if (err == ESP_ERR_NOT_FOUND)
    {
        err = ESP_OK;
    }

    char active_request_id[APP_STORAGE_WIFI_REQUEST_ID_SIZE] = {0};
    uint32_t active_version = 0;
    bool active_present = false;
    if (err == ESP_OK)
    {
        err = load_metadata_pair(
            handle,
            APP_STORAGE_ACTIVE_REQUEST_ID_KEY,
            APP_STORAGE_ACTIVE_VERSION_KEY,
            active_request_id,
            sizeof(active_request_id),
            &active_version,
            &active_present);
    }

    if (err == ESP_OK && pending_present &&
        pending.config_version == config->config_version &&
        strcmp(pending.request_id, config->request_id) == 0)
    {
        *stage_result = wifi_credentials_equal(
                            &pending.credentials,
                            &config->credentials)
                            ? APP_STORAGE_WIFI_STAGE_IDEMPOTENT
                            : APP_STORAGE_WIFI_STAGE_VERSION_CONFLICT;
        nvs_close(handle);
        unlock_wifi_config();
        return ESP_OK;
    }
    if (err == ESP_OK && active_present &&
        active_version == config->config_version &&
        strcmp(active_request_id, config->request_id) == 0)
    {
        app_storage_wifi_credentials_t active_credentials = {0};
        err = load_wifi_credentials_from_handle(
            handle,
            &active_credentials);
        if (err == ESP_OK)
        {
            *stage_result = wifi_credentials_equal(
                                &active_credentials,
                                &config->credentials)
                                ? APP_STORAGE_WIFI_STAGE_IDEMPOTENT
                                : APP_STORAGE_WIFI_STAGE_VERSION_CONFLICT;
        }
        nvs_close(handle);
        unlock_wifi_config();
        return err;
    }

    uint32_t highest_version = 0;
    if (pending_present && pending.config_version > highest_version)
    {
        highest_version = pending.config_version;
    }
    if (active_present && active_version > highest_version)
    {
        highest_version = active_version;
    }

    if (err == ESP_OK && config->config_version < highest_version)
    {
        *stage_result = APP_STORAGE_WIFI_STAGE_STALE_VERSION;
        nvs_close(handle);
        unlock_wifi_config();
        return ESP_OK;
    }
    if (err == ESP_OK && config->config_version == highest_version && highest_version != 0)
    {
        *stage_result = APP_STORAGE_WIFI_STAGE_VERSION_CONFLICT;
        nvs_close(handle);
        unlock_wifi_config();
        return ESP_OK;
    }

    if (err == ESP_OK)
    {
        err = nvs_set_str(handle, APP_STORAGE_CANDIDATE_SSID_KEY, config->credentials.ssid);
    }
    if (err == ESP_OK)
    {
        err = nvs_set_str(handle, APP_STORAGE_CANDIDATE_PASSWORD_KEY, config->credentials.password);
    }
    if (err == ESP_OK)
    {
        err = nvs_set_str(handle, APP_STORAGE_CANDIDATE_REQUEST_ID_KEY, config->request_id);
    }
    if (err == ESP_OK)
    {
        err = nvs_set_u32(handle, APP_STORAGE_CANDIDATE_VERSION_KEY, config->config_version);
    }
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    unlock_wifi_config();
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Candidate WiFi configuration stored, requestId=%s, version=%lu",
                 config->request_id, (unsigned long)config->config_version);
    }
    return err;
}

esp_err_t app_storage_promote_candidate_wifi_config(
    const char *expected_request_id,
    uint32_t expected_version)
{
    if (expected_request_id == NULL || expected_request_id[0] == '\0' || expected_version == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = lock_wifi_config();
    if (err != ESP_OK)
    {
        return err;
    }

    nvs_handle_t handle;
    err = app_storage_open(APP_STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        unlock_wifi_config();
        return err;
    }

    app_storage_wifi_config_t pending = {0};
    err = load_candidate_from_handle(handle, &pending);
    if (err == ESP_OK &&
        (pending.config_version != expected_version ||
         strcmp(pending.request_id, expected_request_id) != 0))
    {
        err = ESP_ERR_INVALID_STATE;
    }

    if (err == ESP_OK)
    {
        err = nvs_set_str(handle, APP_STORAGE_WIFI_SSID_KEY, pending.credentials.ssid);
    }
    if (err == ESP_OK)
    {
        err = nvs_set_str(handle, APP_STORAGE_WIFI_PASSWORD_KEY, pending.credentials.password);
    }
    if (err == ESP_OK)
    {
        err = nvs_set_str(handle, APP_STORAGE_ACTIVE_REQUEST_ID_KEY, pending.request_id);
    }
    if (err == ESP_OK)
    {
        err = nvs_set_u32(handle, APP_STORAGE_ACTIVE_VERSION_KEY, pending.config_version);
    }

    bool changed = false;
    if (err == ESP_OK)
    {
        err = erase_candidate_from_handle(handle, &changed);
    }
    if (err == ESP_OK)
    {
        err = erase_key_if_present(handle, APP_STORAGE_RECOVERY_TRIGGERED_KEY, &changed);
    }
    if (err == ESP_OK)
    {
        err = erase_key_if_present(handle, APP_STORAGE_RECOVERY_RETRY_KEY, &changed);
    }
    if (err == ESP_OK)
    {
        // current 写入、active 元数据移动和 candidate 清空必须共用一次提交。
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    unlock_wifi_config();
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Candidate WiFi configuration promoted, requestId=%s, version=%lu",
                 expected_request_id, (unsigned long)expected_version);
    }
    return err;
}

esp_err_t app_storage_load_wifi_config_status(app_storage_wifi_config_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status, 0, sizeof(*status));
    esp_err_t err = lock_wifi_config();
    if (err != ESP_OK)
    {
        return err;
    }

    nvs_handle_t handle;
    err = app_storage_open(APP_STORAGE_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        unlock_wifi_config();
        return ESP_OK;
    }
    if (err != ESP_OK)
    {
        unlock_wifi_config();
        return err;
    }

    bool active_present = false;
    err = load_metadata_pair(
        handle,
        APP_STORAGE_ACTIVE_REQUEST_ID_KEY,
        APP_STORAGE_ACTIVE_VERSION_KEY,
        status->active_request_id,
        sizeof(status->active_request_id),
        &status->active_version,
        &active_present);

    app_storage_wifi_config_t pending = {0};
    if (err == ESP_OK)
    {
        esp_err_t pending_err = load_candidate_from_handle(handle, &pending);
        if (pending_err == ESP_OK)
        {
            snprintf(status->pending_request_id, sizeof(status->pending_request_id), "%s", pending.request_id);
            status->pending_version = pending.config_version;
        }
        else if (pending_err != ESP_ERR_NOT_FOUND)
        {
            err = pending_err;
        }
    }

    nvs_close(handle);
    unlock_wifi_config();
    return err;
}

esp_err_t app_storage_load_command_result(
    const char *request_id,
    app_command_result_t *result)
{
    if (request_id == NULL || request_id[0] == '\0' || result == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    esp_err_t err = lock_wifi_config();
    if (err != ESP_OK)
    {
        return err;
    }

    nvs_handle_t handle;
    err = app_storage_open(
        APP_STORAGE_COMMAND_RESULT_NAMESPACE,
        NVS_READONLY,
        &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        unlock_wifi_config();
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK)
    {
        unlock_wifi_config();
        return err;
    }

    err = ESP_ERR_NOT_FOUND;
    for (uint8_t slot = 0;
         slot < APP_STORAGE_COMMAND_RESULT_CACHE_SIZE;
         slot++)
    {
        app_storage_command_result_record_t record = {0};
        esp_err_t read_err =
            load_command_result_record(handle, slot, &record);
        if (read_err == ESP_ERR_NVS_NOT_FOUND)
        {
            continue;
        }
        if (read_err != ESP_OK)
        {
            err = read_err;
            break;
        }
        if (!command_result_record_is_valid(&record))
        {
            continue;
        }
        if (strcmp(record.request_id, request_id) == 0)
        {
            snprintf(
                result->request_id,
                sizeof(result->request_id),
                "%s",
                record.request_id);
            result->type = (app_command_type_t)record.type;
            result->success = record.success != 0;
            snprintf(
                result->message,
                sizeof(result->message),
                "%s",
                record.message);
            err = ESP_OK;
            break;
        }
    }

    nvs_close(handle);
    unlock_wifi_config();
    return err;
}

esp_err_t app_storage_claim_command(
    const char *request_id,
    app_command_type_t type,
    app_storage_command_claim_result_t *claim_result,
    app_command_result_t *replay_result)
{
    if (request_id == NULL ||
        request_id[0] == '\0' ||
        !is_production_command_type(type) ||
        claim_result == NULL ||
        replay_result == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t request_id_length =
        strnlen(request_id, APP_COMMAND_REQUEST_ID_SIZE);
    if (request_id_length >= APP_COMMAND_REQUEST_ID_SIZE)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *claim_result = APP_STORAGE_COMMAND_CLAIMED;
    memset(replay_result, 0, sizeof(*replay_result));

    esp_err_t err = lock_wifi_config();
    if (err != ESP_OK)
    {
        return err;
    }

    nvs_handle_t handle;
    err = app_storage_open(
        APP_STORAGE_COMMAND_RESULT_NAMESPACE,
        NVS_READWRITE,
        &handle);
    if (err != ESP_OK)
    {
        unlock_wifi_config();
        return err;
    }

    for (uint8_t slot = 0;
         slot < APP_STORAGE_COMMAND_RESULT_CACHE_SIZE;
         slot++)
    {
        app_storage_command_result_record_t existing = {0};
        esp_err_t read_err =
            load_command_result_record(handle, slot, &existing);
        if (read_err == ESP_ERR_NVS_NOT_FOUND)
        {
            continue;
        }
        if (read_err != ESP_OK)
        {
            err = read_err;
            break;
        }
        if (!command_result_record_has_identity(&existing) ||
            strcmp(existing.request_id, request_id) != 0)
        {
            continue;
        }

        if (existing.type != (uint32_t)type)
        {
            *claim_result = APP_STORAGE_COMMAND_TYPE_CONFLICT;
        }
        else if (command_result_record_is_valid(&existing))
        {
            *claim_result = APP_STORAGE_COMMAND_REPLAY;
            command_result_from_record(&existing, replay_result);
        }
        else
        {
            *claim_result = APP_STORAGE_COMMAND_INTERRUPTED;
            snprintf(
                replay_result->request_id,
                sizeof(replay_result->request_id),
                "%s",
                existing.request_id);
            replay_result->type = (app_command_type_t)existing.type;
            replay_result->success = false;
            snprintf(
                replay_result->message,
                sizeof(replay_result->message),
                "%s",
                "command execution interrupted");
        }

        nvs_close(handle);
        unlock_wifi_config();
        return ESP_OK;
    }

    uint8_t next_slot = 0;
    if (err == ESP_OK)
    {
        esp_err_t next_err = nvs_get_u8(
            handle,
            APP_STORAGE_COMMAND_RESULT_NEXT_KEY,
            &next_slot);
        if (next_err == ESP_ERR_NVS_NOT_FOUND)
        {
            next_slot = 0;
        }
        else if (next_err != ESP_OK)
        {
            err = next_err;
        }
    }
    if (next_slot >= APP_STORAGE_COMMAND_RESULT_CACHE_SIZE)
    {
        next_slot = 0;
    }

    app_storage_command_result_record_t pending = {0};
    pending.version = APP_STORAGE_COMMAND_PENDING_RECORD_VERSION;
    snprintf(
        pending.request_id,
        sizeof(pending.request_id),
        "%s",
        request_id);
    pending.type = (uint32_t)type;

    char key[16] = {0};
    command_result_slot_key(next_slot, key, sizeof(key));
    if (err == ESP_OK)
    {
        err = nvs_set_blob(handle, key, &pending, sizeof(pending));
    }
    if (err == ESP_OK)
    {
        uint8_t following_slot =
            (uint8_t)((next_slot + 1) %
                      APP_STORAGE_COMMAND_RESULT_CACHE_SIZE);
        err = nvs_set_u8(
            handle,
            APP_STORAGE_COMMAND_RESULT_NEXT_KEY,
            following_slot);
    }
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    unlock_wifi_config();
    return err;
}

esp_err_t app_storage_complete_command_result(
    const app_command_result_t *result)
{
    if (!command_result_is_valid(result))
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = lock_wifi_config();
    if (err != ESP_OK)
    {
        return err;
    }

    nvs_handle_t handle;
    err = app_storage_open(
        APP_STORAGE_COMMAND_RESULT_NAMESPACE,
        NVS_READWRITE,
        &handle);
    if (err != ESP_OK)
    {
        unlock_wifi_config();
        return err;
    }

    err = ESP_ERR_NOT_FOUND;
    for (uint8_t slot = 0;
         slot < APP_STORAGE_COMMAND_RESULT_CACHE_SIZE;
         slot++)
    {
        app_storage_command_result_record_t existing = {0};
        esp_err_t read_err =
            load_command_result_record(handle, slot, &existing);
        if (read_err == ESP_ERR_NVS_NOT_FOUND)
        {
            continue;
        }
        if (read_err != ESP_OK)
        {
            err = read_err;
            break;
        }
        if (!command_result_record_has_identity(&existing) ||
            strcmp(existing.request_id, result->request_id) != 0)
        {
            continue;
        }
        if (existing.type != (uint32_t)result->type)
        {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        if (command_result_record_is_valid(&existing))
        {
            err = ESP_OK;
            break;
        }

        app_storage_command_result_record_t terminal =
            command_result_record(result);
        char key[16] = {0};
        command_result_slot_key(slot, key, sizeof(key));
        err = nvs_set_blob(handle, key, &terminal, sizeof(terminal));
        if (err == ESP_OK)
        {
            err = nvs_commit(handle);
        }
        break;
    }

    nvs_close(handle);
    unlock_wifi_config();
    return err;
}

esp_err_t app_storage_save_command_result(
    const app_command_result_t *result)
{
    if (!command_result_is_valid(result))
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = lock_wifi_config();
    if (err != ESP_OK)
    {
        return err;
    }

    nvs_handle_t handle;
    err = app_storage_open(
        APP_STORAGE_COMMAND_RESULT_NAMESPACE,
        NVS_READWRITE,
        &handle);
    if (err != ESP_OK)
    {
        unlock_wifi_config();
        return err;
    }

    for (uint8_t slot = 0;
         slot < APP_STORAGE_COMMAND_RESULT_CACHE_SIZE && err == ESP_OK;
         slot++)
    {
        app_storage_command_result_record_t existing = {0};
        esp_err_t read_err =
            load_command_result_record(handle, slot, &existing);
        if (read_err == ESP_ERR_NVS_NOT_FOUND)
        {
            continue;
        }
        if (read_err != ESP_OK)
        {
            err = read_err;
            break;
        }
        if (command_result_record_is_valid(&existing) &&
            strcmp(existing.request_id, result->request_id) == 0)
        {
            nvs_close(handle);
            unlock_wifi_config();
            return existing.type == (uint32_t)result->type
                       ? ESP_OK
                       : ESP_ERR_INVALID_STATE;
        }
    }

    uint8_t next_slot = 0;
    if (err == ESP_OK)
    {
        esp_err_t next_err = nvs_get_u8(
            handle,
            APP_STORAGE_COMMAND_RESULT_NEXT_KEY,
            &next_slot);
        if (next_err == ESP_ERR_NVS_NOT_FOUND)
        {
            next_slot = 0;
        }
        else if (next_err != ESP_OK)
        {
            err = next_err;
        }
    }
    if (next_slot >= APP_STORAGE_COMMAND_RESULT_CACHE_SIZE)
    {
        next_slot = 0;
    }

    app_storage_command_result_record_t record =
        command_result_record(result);

    char key[16] = {0};
    command_result_slot_key(next_slot, key, sizeof(key));
    if (err == ESP_OK)
    {
        err = nvs_set_blob(handle, key, &record, sizeof(record));
    }
    if (err == ESP_OK)
    {
        uint8_t following_slot =
            (uint8_t)((next_slot + 1) %
                      APP_STORAGE_COMMAND_RESULT_CACHE_SIZE);
        err = nvs_set_u8(
            handle,
            APP_STORAGE_COMMAND_RESULT_NEXT_KEY,
            following_slot);
    }
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    unlock_wifi_config();
    return err;
}

esp_err_t app_storage_clear_wifi_credentials(void)
{
    esp_err_t err = lock_wifi_config();
    if (err != ESP_OK)
    {
        return err;
    }

    nvs_handle_t handle;

    err = app_storage_open(APP_STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        unlock_wifi_config();
        return ESP_OK;
    }
    if (err != ESP_OK)
    {
        unlock_wifi_config();
        return err;
    }

    bool changed = false;
    err = erase_key_if_present(handle, APP_STORAGE_WIFI_SSID_KEY, &changed);
    if (err == ESP_OK)
    {
        err = erase_key_if_present(handle, APP_STORAGE_WIFI_PASSWORD_KEY, &changed);
    }
    if (err == ESP_OK)
    {
        err = erase_key_if_present(handle, APP_STORAGE_ACTIVE_REQUEST_ID_KEY, &changed);
    }
    if (err == ESP_OK)
    {
        err = erase_key_if_present(handle, APP_STORAGE_ACTIVE_VERSION_KEY, &changed);
    }
    if (err == ESP_OK && changed)
    {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    unlock_wifi_config();

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Upstream WiFi credentials cleared");
    }
    else
    {
        ESP_LOGE(TAG, "Clear upstream WiFi credentials failed: %s", esp_err_to_name(err));
    }

    return err; 
}

esp_err_t app_storage_set_recovery_triggered(void)
{
    nvs_handle_t handle;
    esp_err_t err = app_storage_open(APP_STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        return err;
    }

    uint8_t value = 1;
    err = nvs_set_u8(handle, APP_STORAGE_RECOVERY_TRIGGERED_KEY, value);
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Recovery triggered flag set");
    }
    return err;
}

esp_err_t app_storage_clear_recovery_triggered(void)
{
    nvs_handle_t handle;
    esp_err_t err = app_storage_open(APP_STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        // 命名空间不存在也可以视为已经清除
        if (err == ESP_ERR_NVS_NOT_FOUND)
        {
            return ESP_OK;
        }
        return err;
    }

    err = nvs_erase_key(handle, APP_STORAGE_RECOVERY_TRIGGERED_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        err = ESP_OK; // 键本来就不存在，不算错误
    }
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

bool app_storage_is_recovery_triggered(void)
{
    nvs_handle_t handle;
    esp_err_t err = app_storage_open(APP_STORAGE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK)
    {
        return false;
    }

    uint8_t value = 0;
    err = nvs_get_u8(handle, APP_STORAGE_RECOVERY_TRIGGERED_KEY, &value);
    nvs_close(handle);

    return (err == ESP_OK && value == 1);
}

esp_err_t app_storage_init_nvs(void)
{
    //初始化nvs
#ifdef APP_STORAGE_NVS_PARTITION
    esp_err_t err = nvs_flash_init_partition(APP_STORAGE_NVS_PARTITION_NAME);
#else
    esp_err_t err = nvs_flash_init();
#endif

    //如果nvs分区异常就直接进行擦除
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS needs erase, reinitializing");

        //擦除
#ifdef APP_STORAGE_NVS_PARTITION
        err = nvs_flash_erase_partition(APP_STORAGE_NVS_PARTITION_NAME);
#else
        err = nvs_flash_erase();
#endif
        if (err != ESP_OK)
        {
            return err;
        }

        //重新初始化
#ifdef APP_STORAGE_NVS_PARTITION
        err = nvs_flash_init_partition(APP_STORAGE_NVS_PARTITION_NAME);
#else
        err = nvs_flash_init();
#endif
    }
    
    if (err == ESP_OK && s_wifi_config_mutex == NULL)
    {
        s_wifi_config_mutex = xSemaphoreCreateMutex();
        if (s_wifi_config_mutex == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "NVS initialized");
    }
    
    return err;
}


