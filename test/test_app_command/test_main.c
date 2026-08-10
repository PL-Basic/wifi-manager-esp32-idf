#include <string.h>

#include "app_command.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"

static esp_err_t parse_command(
    const char *topic,
    const char *payload,
    app_command_request_t *request)
{
    return app_command_parse(
        topic,
        (int)strlen(topic),
        payload,
        (int)strlen(payload),
        request);
}

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_allow_command_preserves_contract_fields(void)
{
    const char *topic = "wifi/device/demo/cmd/allow";
    const char *payload =
        "{\"requestId\":\"req-allow-1\","
        "\"mac\":\"AA:BB:CC:DD:EE:FF\","
        "\"sessionId\":42,"
        "\"ttlSeconds\":60}";
    app_command_request_t request = {0};

    TEST_ASSERT_EQUAL(ESP_OK, parse_command(topic, payload, &request));
    TEST_ASSERT_EQUAL(APP_COMMAND_TYPE_ALLOW, request.type);
    TEST_ASSERT_EQUAL_STRING("req-allow-1", request.request_id);
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", request.mac);
    TEST_ASSERT_EQUAL_INT64(42, request.session_id);
    TEST_ASSERT_EQUAL_INT64(60, request.ttl_seconds);
}

static void test_allow_command_rejects_zero_ttl(void)
{
    const char *topic = "wifi/device/demo/cmd/allow";
    const char *payload =
        "{\"requestId\":\"req-allow-2\","
        "\"mac\":\"AA:BB:CC:DD:EE:FF\","
        "\"sessionId\":42,"
        "\"ttlSeconds\":0}";
    app_command_request_t request = {0};

    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        parse_command(topic, payload, &request));
    TEST_ASSERT_EQUAL(APP_COMMAND_TYPE_ALLOW, request.type);
}

static void test_stage_wifi_command_requires_complete_candidate(void)
{
    const char *topic = "wifi/device/demo/cmd/stage-wifi-config";
    const char *payload =
        "{\"requestId\":\"req-wifi-1\","
        "\"deviceCode\":\"demo\","
        "\"ssid\":\"DemoWifi\","
        "\"password\":\"password\","
        "\"configVersion\":7}";
    app_command_request_t request = {0};

    TEST_ASSERT_EQUAL(ESP_OK, parse_command(topic, payload, &request));
    TEST_ASSERT_EQUAL(APP_COMMAND_TYPE_STAGE_WIFI_CONFIG, request.type);
    TEST_ASSERT_EQUAL_STRING("req-wifi-1", request.request_id);
    TEST_ASSERT_EQUAL_STRING("demo", request.device_code);
    TEST_ASSERT_EQUAL_STRING("DemoWifi", request.wifi_ssid);
    TEST_ASSERT_EQUAL_STRING("password", request.wifi_password);
    TEST_ASSERT_EQUAL_UINT32(7, request.wifi_config_version);
}

static void test_unknown_topic_does_not_become_production_command(void)
{
    const char *topic = "wifi/device/demo/cmd/not-supported";
    const char *payload = "{\"requestId\":\"req-unknown-1\"}";
    app_command_request_t request = {0};

    TEST_ASSERT_EQUAL(
        ESP_ERR_NOT_FOUND,
        parse_command(topic, payload, &request));
    TEST_ASSERT_EQUAL(APP_COMMAND_TYPE_UNKNOWN, request.type);
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1000));

    UNITY_BEGIN();
    RUN_TEST(test_allow_command_preserves_contract_fields);
    RUN_TEST(test_allow_command_rejects_zero_ttl);
    RUN_TEST(test_stage_wifi_command_requires_complete_candidate);
    RUN_TEST(test_unknown_topic_does_not_become_production_command);
    UNITY_END();
}
