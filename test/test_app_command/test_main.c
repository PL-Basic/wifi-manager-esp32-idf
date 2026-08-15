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
    TEST_ASSERT_TRUE_MESSAGE(
        request.session_id == 42,
        "sessionId mismatch");
    TEST_ASSERT_TRUE_MESSAGE(
        request.ttl_seconds == 60,
        "ttlSeconds mismatch");
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

static void test_revoke_access_command_preserves_contract_fields(void)
{
    const char *topic = "wifi/device/demo/cmd/revoke-access";
    const char *payload =
        "{\"requestId\":\"req-revoke-v1\","
        "\"mac\":\"AA:BB:CC:DD:EE:FF\","
        "\"sessionId\":42}";
    app_command_request_t request = {0};

    TEST_ASSERT_EQUAL(ESP_OK, parse_command(topic, payload, &request));
    TEST_ASSERT_EQUAL(APP_COMMAND_TYPE_REVOKE_ACCESS, request.type);
    TEST_ASSERT_EQUAL_STRING("req-revoke-v1", request.request_id);
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", request.mac);
    TEST_ASSERT_TRUE_MESSAGE(
        request.session_id == 42,
        "sessionId mismatch");
}

static void test_kick_command_uses_independent_reason_buffer(void)
{
    const char *topic = "wifi/device/demo/cmd/kick";
    const char *payload =
        "{\"requestId\":\"req-kick-v1\","
        "\"deviceCode\":\"demo\","
        "\"reason\":\"fixed test reason\"}";
    app_command_request_t request = {0};

    TEST_ASSERT_EQUAL(ESP_OK, parse_command(topic, payload, &request));
    TEST_ASSERT_EQUAL(APP_COMMAND_TYPE_KICK, request.type);
    TEST_ASSERT_EQUAL_STRING("req-kick-v1", request.request_id);
    TEST_ASSERT_EQUAL_STRING("demo", request.device_code);
    TEST_ASSERT_EQUAL_STRING("fixed test reason", request.reason);
    TEST_ASSERT_EQUAL_STRING("", request.mac);
}

static void test_kick_command_rejects_topic_payload_device_mismatch(void)
{
    const char *topic = "wifi/device/demo/cmd/kick";
    const char *payload =
        "{\"requestId\":\"req-kick-mismatch-v1\","
        "\"deviceCode\":\"other-device\","
        "\"reason\":\"fixed test reason\"}";
    app_command_request_t request = {0};

    TEST_ASSERT_NOT_EQUAL(
        ESP_OK,
        parse_command(topic, payload, &request));
    TEST_ASSERT_EQUAL(APP_COMMAND_TYPE_KICK, request.type);
}

static void test_kick_command_rejects_missing_device_code(void)
{
    const char *topic = "wifi/device/demo/cmd/kick";
    const char *payload =
        "{\"requestId\":\"req-kick-missing-v1\","
        "\"reason\":\"fixed test reason\"}";
    app_command_request_t request = {0};

    TEST_ASSERT_NOT_EQUAL(
        ESP_OK,
        parse_command(topic, payload, &request));
    TEST_ASSERT_EQUAL(APP_COMMAND_TYPE_KICK, request.type);
}

static void test_disconnect_mac_command_preserves_contract_fields(void)
{
    const char *topic = "wifi/device/demo/cmd/disconnect-mac";
    const char *payload =
        "{\"requestId\":\"req-disconnect-v1\","
        "\"mac\":\"AA:BB:CC:DD:EE:FF\","
        "\"alertId\":0}";
    app_command_request_t request = {0};

    TEST_ASSERT_EQUAL(ESP_OK, parse_command(topic, payload, &request));
    TEST_ASSERT_EQUAL(APP_COMMAND_TYPE_DISCONNECT_MAC, request.type);
    TEST_ASSERT_EQUAL_STRING("req-disconnect-v1", request.request_id);
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", request.mac);
    TEST_ASSERT_TRUE_MESSAGE(
        request.alert_id == 0,
        "alertId mismatch");
}

static void test_block_traffic_command_preserves_contract_fields(void)
{
    const char *topic = "wifi/device/demo/cmd/block-traffic";
    const char *payload =
        "{\"requestId\":\"req-block-v1\","
        "\"dstIp\":\"203.0.113.10\","
        "\"sni\":\"blocked.test\","
        "\"alertId\":0}";
    app_command_request_t request = {0};

    TEST_ASSERT_EQUAL(ESP_OK, parse_command(topic, payload, &request));
    TEST_ASSERT_EQUAL(APP_COMMAND_TYPE_BLOCK_TRAFFIC, request.type);
    TEST_ASSERT_EQUAL_STRING("req-block-v1", request.request_id);
    TEST_ASSERT_EQUAL_STRING("203.0.113.10", request.dst_ip);
    TEST_ASSERT_EQUAL_STRING("blocked.test", request.sni);
    TEST_ASSERT_TRUE_MESSAGE(
        request.alert_id == 0,
        "alertId mismatch");
}

static void test_stage_wifi_command_requires_complete_candidate(void)
{
    const char *topic = "wifi/device/demo/cmd/stage-wifi-config";
    const char *payload =
        "{\"requestId\":\"req-wifi-1\","
        "\"deviceCode\":\"demo\","
        "\"ssid\":\"DemoWifi\","
        "\"password\":\"invalid-test-password\","
        "\"configVersion\":7}";
    app_command_request_t request = {0};

    TEST_ASSERT_EQUAL(ESP_OK, parse_command(topic, payload, &request));
    TEST_ASSERT_EQUAL(APP_COMMAND_TYPE_STAGE_WIFI_CONFIG, request.type);
    TEST_ASSERT_EQUAL_STRING("req-wifi-1", request.request_id);
    TEST_ASSERT_EQUAL_STRING("demo", request.device_code);
    TEST_ASSERT_EQUAL_STRING("DemoWifi", request.wifi_ssid);
    TEST_ASSERT_EQUAL_STRING(
        "invalid-test-password",
        request.wifi_password);
    TEST_ASSERT_EQUAL_UINT32(7, request.wifi_config_version);
}

static void test_stage_wifi_command_rejects_missing_request_id(void)
{
    const char *topic = "wifi/device/demo/cmd/stage-wifi-config";
    const char *payload =
        "{\"deviceCode\":\"demo\","
        "\"ssid\":\"DemoWifi\","
        "\"password\":\"invalid-test-password\","
        "\"configVersion\":7}";
    app_command_request_t request = {0};

    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        parse_command(topic, payload, &request));
}

static void test_stage_wifi_command_rejects_zero_config_version(void)
{
    const char *topic = "wifi/device/demo/cmd/stage-wifi-config";
    const char *payload =
        "{\"requestId\":\"req-wifi-zero-v1\","
        "\"deviceCode\":\"demo\","
        "\"ssid\":\"DemoWifi\","
        "\"password\":\"invalid-test-password\","
        "\"configVersion\":0}";
    app_command_request_t request = {0};

    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        parse_command(topic, payload, &request));
}

static void test_stage_wifi_command_rejects_topic_payload_device_mismatch(void)
{
    const char *topic = "wifi/device/demo/cmd/stage-wifi-config";
    const char *payload =
        "{\"requestId\":\"req-wifi-mismatch-v1\","
        "\"deviceCode\":\"other-device\","
        "\"ssid\":\"DemoWifi\","
        "\"password\":\"invalid-test-password\","
        "\"configVersion\":7}";
    app_command_request_t request = {0};

    TEST_ASSERT_NOT_EQUAL(
        ESP_OK,
        parse_command(topic, payload, &request));
    TEST_ASSERT_EQUAL(APP_COMMAND_TYPE_STAGE_WIFI_CONFIG, request.type);
}

static void test_allow_command_rejects_wrong_ttl_type(void)
{
    const char *topic = "wifi/device/demo/cmd/allow";
    const char *payload =
        "{\"requestId\":\"req-wrong-type-v1\","
        "\"mac\":\"AA:BB:CC:DD:EE:FF\","
        "\"sessionId\":42,"
        "\"ttlSeconds\":\"60\"}";
    app_command_request_t request = {0};

    TEST_ASSERT_NOT_EQUAL(
        ESP_OK,
        parse_command(topic, payload, &request));
}

static void test_command_rejects_request_id_over_visible_limit(void)
{
    const char *topic = "wifi/device/demo/cmd/allow";
    const char *payload =
        "{\"requestId\":"
        "\"1234567890123456789012345678901234567890123456789012345678901234\","
        "\"mac\":\"AA:BB:CC:DD:EE:FF\","
        "\"sessionId\":42,"
        "\"ttlSeconds\":60}";
    app_command_request_t request = {0};

    TEST_ASSERT_NOT_EQUAL(
        ESP_OK,
        parse_command(topic, payload, &request));
}

static void test_repeated_request_id_parses_to_same_command_identity(void)
{
    const char *topic = "wifi/device/demo/cmd/allow";
    const char *payload =
        "{\"requestId\":\"req-duplicate-v1\","
        "\"mac\":\"AA:BB:CC:DD:EE:FF\","
        "\"sessionId\":42,"
        "\"ttlSeconds\":60}";
    app_command_request_t first = {0};
    app_command_request_t repeated = {0};

    TEST_ASSERT_EQUAL(ESP_OK, parse_command(topic, payload, &first));
    TEST_ASSERT_EQUAL(ESP_OK, parse_command(topic, payload, &repeated));
    TEST_ASSERT_EQUAL_STRING(first.request_id, repeated.request_id);
    TEST_ASSERT_EQUAL(first.type, repeated.type);
    TEST_ASSERT_TRUE_MESSAGE(
        first.session_id == repeated.session_id,
        "repeated sessionId mismatch");
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
    RUN_TEST(test_revoke_access_command_preserves_contract_fields);
    RUN_TEST(test_kick_command_uses_independent_reason_buffer);
    RUN_TEST(test_kick_command_rejects_topic_payload_device_mismatch);
    RUN_TEST(test_kick_command_rejects_missing_device_code);
    RUN_TEST(test_disconnect_mac_command_preserves_contract_fields);
    RUN_TEST(test_block_traffic_command_preserves_contract_fields);
    RUN_TEST(test_stage_wifi_command_requires_complete_candidate);
    RUN_TEST(test_stage_wifi_command_rejects_missing_request_id);
    RUN_TEST(test_stage_wifi_command_rejects_zero_config_version);
    RUN_TEST(test_stage_wifi_command_rejects_topic_payload_device_mismatch);
    RUN_TEST(test_allow_command_rejects_wrong_ttl_type);
    RUN_TEST(test_command_rejects_request_id_over_visible_limit);
    RUN_TEST(test_repeated_request_id_parses_to_same_command_identity);
    RUN_TEST(test_unknown_topic_does_not_become_production_command);
    UNITY_END();
}
