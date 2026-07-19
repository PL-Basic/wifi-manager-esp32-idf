#include <string.h>

#include "app_storage.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

// NVS使用namespace隔离不同模块的数据
#define APP_STORAGE_NAMESPACE "wifi_config"
// namespace内部的两个字段名
#define APP_STORAGE_WIFI_SSID_KEY "sta_ssid"
#define APP_STORAGE_WIFI_PASSWORD_KEY "sta_password"

static const char *TAG = "app_storage";

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

esp_err_t app_storage_save_wifi_credentials(const app_storage_wifi_credentials_t *credentials)
{
    esp_err_t err = validate_wifi_credentials(credentials);
    if (err != ESP_OK)
    {
        return err;
    }

    nvs_handle_t handle;

    // 以可读写模式打开wifi_config命名空间。
    // handle是后续读写这个命名空间时使用的操作句柄。
    err = nvs_open(APP_STORAGE_NAMESPACE, NVS_READWRITE, &handle);

    if (err != ESP_OK)
    {
        return err;
    }

    // 把SSID保存到sta_ssid字段
    err = nvs_set_str(handle, APP_STORAGE_WIFI_SSID_KEY, credentials->ssid);

    // 前一步成功后才保存密码，避免覆盖真正的错误码。
    if (err == ESP_OK)
    {
        err = nvs_set_str(handle, APP_STORAGE_WIFI_PASSWORD_KEY, credentials->password);
    }

    // nvs_set_str修改的是待提交数据。
    // nvs_commit成功才表示修改已经正式提交到Flash。
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }

    // 只要nvs_open成功，无论后面的操作成功还是失败，都必须关闭句柄
    nvs_close(handle);

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
    esp_err_t err = nvs_open(APP_STORAGE_NAMESPACE, NVS_READONLY, &handle);

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

esp_err_t app_storage_clear_wifi_credentials(void)
{
    nvs_handle_t handle;

    //删除凭据需要修改NVS，因此必须使用读写模式
    esp_err_t err = nvs_open(APP_STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        return err;
    }

    // 删除SSID
    esp_err_t ssid_err = nvs_erase_key(handle, APP_STORAGE_WIFI_SSID_KEY);
    // 没有这个key不算失败，因为目标本来就是“确保它不存在”。
    if (ssid_err != ESP_OK && ssid_err != ESP_ERR_NVS_NOT_FOUND)
    {
        nvs_close(handle);
        return ssid_err;
    }
    
    //删除密码
    esp_err_t password_err = nvs_erase_key(handle, APP_STORAGE_WIFI_PASSWORD_KEY);

    if (password_err != ESP_OK && password_err != ESP_ERR_NVS_NOT_FOUND)
    {
        nvs_close(handle);
        return password_err;
    }
    
    // 只有确实删除过至少一个key时，才提交Flash修改
    if (ssid_err == ESP_OK || password_err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    else
    {
        // 两个key本来旧不存在，当前状态已经满足“没有凭据”
        err = ESP_OK;
    }

    nvs_close(handle);

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

esp_err_t app_storage_init_nvs(void)
{
    //初始化nvs
    esp_err_t err = nvs_flash_init();

    //如果nvs分区异常就直接进行擦除
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS needs erase, reinitializing");

        //擦除
        err = nvs_flash_erase();
        if (err != ESP_OK)
        {
            return err;
        }

        //重新初始化
        err = nvs_flash_init();
    }
    
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "NVS initialized");
    }
    
    return err;
}


