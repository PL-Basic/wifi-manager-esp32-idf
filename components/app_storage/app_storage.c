#include "app_storage.h"

#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "app_storage";

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


