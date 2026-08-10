#include <stdint.h>

#include "esp_err.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "unity.h"

#define TEST_NVS_PARTITION "nvs_test"
#define TEST_NVS_NAMESPACE "wifi_test"

static esp_err_t init_test_partition(void)
{
    esp_err_t err = nvs_flash_init_partition(TEST_NVS_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        TEST_ASSERT_EQUAL(
            ESP_OK,
            nvs_flash_erase_partition(TEST_NVS_PARTITION));
        err = nvs_flash_init_partition(TEST_NVS_PARTITION);
    }
    return err;
}

static void erase_test_partition(void)
{
    esp_err_t err = nvs_flash_deinit_partition(TEST_NVS_PARTITION);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_INITIALIZED)
    {
        TEST_FAIL_MESSAGE("Failed to deinitialize nvs_test");
    }
    TEST_ASSERT_EQUAL(
        ESP_OK,
        nvs_flash_erase_partition(TEST_NVS_PARTITION));
}

void setUp(void)
{
}

void tearDown(void)
{
    erase_test_partition();
}

static void test_partition_is_separate_from_default_nvs(void)
{
    const esp_partition_t *default_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_NVS,
        "nvs");
    const esp_partition_t *test_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_NVS,
        TEST_NVS_PARTITION);

    TEST_ASSERT_NOT_NULL(default_partition);
    TEST_ASSERT_NOT_NULL(test_partition);
    TEST_ASSERT_NOT_EQUAL(default_partition->address, test_partition->address);
    TEST_ASSERT_NOT_EQUAL(default_partition->size, 0);
    TEST_ASSERT_NOT_EQUAL(test_partition->size, 0);
}

static void test_values_round_trip_only_in_test_partition(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, init_test_partition());

    nvs_handle_t handle;
    TEST_ASSERT_EQUAL(
        ESP_OK,
        nvs_open_from_partition(
            TEST_NVS_PARTITION,
            TEST_NVS_NAMESPACE,
            NVS_READWRITE,
            &handle));
    TEST_ASSERT_EQUAL(
        ESP_OK,
        nvs_set_u32(handle, "run_marker", 0x1a2b3c4d));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(handle));
    nvs_close(handle);

    uint32_t marker = 0;
    TEST_ASSERT_EQUAL(
        ESP_OK,
        nvs_open_from_partition(
            TEST_NVS_PARTITION,
            TEST_NVS_NAMESPACE,
            NVS_READONLY,
            &handle));
    TEST_ASSERT_EQUAL(
        ESP_OK,
        nvs_get_u32(handle, "run_marker", &marker));
    nvs_close(handle);

    TEST_ASSERT_EQUAL_HEX32(0x1a2b3c4d, marker);
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1000));

    UNITY_BEGIN();
    RUN_TEST(test_partition_is_separate_from_default_nvs);
    RUN_TEST(test_values_round_trip_only_in_test_partition);
    UNITY_END();
}
