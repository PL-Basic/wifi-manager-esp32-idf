#include <assert.h>

#include "esp_err.h"
#include "esp_task_wdt.h"
#include "unity.h"

void app_main(void)
{
    esp_err_t task_wdt_result = esp_task_wdt_deinit();
    assert(
        task_wdt_result == ESP_OK ||
        task_wdt_result == ESP_ERR_INVALID_STATE);
    unity_run_menu();
}
