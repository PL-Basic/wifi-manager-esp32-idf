#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "app_command.h"
#include "app_command_coordinator.h"
#include "app_storage.h"
#include "esp_err.h"
#include "esp_partition.h"
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

static void init_app_storage(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, app_storage_init_nvs());
}

static app_command_result_t make_command_result(
    const char *request_id,
    app_command_type_t type,
    bool success,
    const char *message)
{
    app_command_result_t result = {0};
    snprintf(
        result.request_id,
        sizeof(result.request_id),
        "%s",
        request_id);
    result.type = type;
    result.success = success;
    snprintf(
        result.message,
        sizeof(result.message),
        "%s",
        message);
    return result;
}

static app_storage_wifi_config_t make_wifi_config(
    const char *request_id,
    uint32_t version,
    const char *ssid)
{
    app_storage_wifi_config_t config = {0};
    snprintf(
        config.request_id,
        sizeof(config.request_id),
        "%s",
        request_id);
    config.config_version = version;
    snprintf(
        config.credentials.ssid,
        sizeof(config.credentials.ssid),
        "%s",
        ssid);
    snprintf(
        config.credentials.password,
        sizeof(config.credentials.password),
        "%s",
        "invalid-test-password");
    return config;
}

static void claim_and_complete(
    const app_command_result_t *terminal)
{
    app_storage_command_claim_result_t claim =
        APP_STORAGE_COMMAND_REPLAY;
    app_command_result_t replay = {0};
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_claim_command(
            terminal->request_id,
            terminal->type,
            &claim,
            &replay));
    TEST_ASSERT_EQUAL(APP_STORAGE_COMMAND_CLAIMED, claim);
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_complete_command_result(terminal));
}

typedef struct
{
    int execute_count;
    int complete_count;
    int publish_count;
    int after_terminal_count;
    app_command_result_t published_result;
} malformed_command_fixture_t;

static esp_err_t malformed_claim(
    void *context,
    const char *request_id,
    app_command_type_t type,
    app_storage_command_claim_result_t *claim_result,
    app_command_result_t *replay_result)
{
    (void)context;
    return app_storage_claim_command(
        request_id,
        type,
        claim_result,
        replay_result);
}

static esp_err_t malformed_complete(
    void *context,
    const app_command_result_t *result)
{
    malformed_command_fixture_t *fixture = context;
    fixture->complete_count++;
    return app_storage_complete_command_result(result);
}

static esp_err_t malformed_execute(
    void *context,
    const app_command_request_t *request,
    app_command_result_t *result)
{
    (void)request;
    (void)result;
    malformed_command_fixture_t *fixture = context;
    fixture->execute_count++;
    return ESP_OK;
}

static esp_err_t malformed_publish(
    void *context,
    const app_command_result_t *result)
{
    malformed_command_fixture_t *fixture = context;
    fixture->publish_count++;
    fixture->published_result = *result;
    return ESP_OK;
}

static void malformed_after_terminal(
    void *context,
    const app_command_request_t *request,
    const app_command_result_t *result,
    esp_err_t publish_result)
{
    (void)request;
    (void)result;
    (void)publish_result;
    malformed_command_fixture_t *fixture = context;
    fixture->after_terminal_count++;
}

void setUp(void)
{
}

void tearDown(void)
{
    erase_test_partition();
}

TEST_CASE("test_partition_is_separate_from_default_nvs", "[h1][app_storage]")
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

TEST_CASE("test_values_round_trip_only_in_test_partition", "[h1][app_storage]")
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

TEST_CASE("test_terminal_replays_at_one_three_and_sixteen_duplicates", "[h1][app_storage]")
{
    init_app_storage();
    app_command_result_t terminal = make_command_result(
        "req-repeat-boundary",
        APP_COMMAND_TYPE_ALLOW,
        true,
        "allowed");
    claim_and_complete(&terminal);

    for (int repeat = 1; repeat <= 16; repeat++)
    {
        app_storage_command_claim_result_t claim =
            APP_STORAGE_COMMAND_CLAIMED;
        app_command_result_t replay = {0};
        TEST_ASSERT_EQUAL(
            ESP_OK,
            app_storage_claim_command(
                terminal.request_id,
                terminal.type,
                &claim,
                &replay));
        TEST_ASSERT_EQUAL(APP_STORAGE_COMMAND_REPLAY, claim);
        TEST_ASSERT_EQUAL_STRING(terminal.request_id, replay.request_id);
        TEST_ASSERT_EQUAL(terminal.type, replay.type);
        TEST_ASSERT_TRUE(replay.success);
        if (repeat == 1 || repeat == 3 || repeat == 16)
        {
            TEST_ASSERT_EQUAL_STRING("allowed", replay.message);
        }
    }
}

TEST_CASE("test_seventeenth_terminal_evicts_only_oldest_slot", "[h1][app_storage]")
{
    init_app_storage();

    for (int index = 0; index < 16; index++)
    {
        char request_id[APP_COMMAND_REQUEST_ID_SIZE] = {0};
        snprintf(request_id, sizeof(request_id), "req-slot-%02d", index);
        app_command_result_t terminal = make_command_result(
            request_id,
            APP_COMMAND_TYPE_ALLOW,
            true,
            "allowed");
        claim_and_complete(&terminal);
    }

    app_command_result_t seventeenth = make_command_result(
        "req-slot-16",
        APP_COMMAND_TYPE_ALLOW,
        true,
        "allowed");
    claim_and_complete(&seventeenth);

    app_command_result_t loaded = {0};
    TEST_ASSERT_EQUAL(
        ESP_ERR_NOT_FOUND,
        app_storage_load_command_result("req-slot-00", &loaded));
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_load_command_result("req-slot-01", &loaded));
    TEST_ASSERT_EQUAL_STRING("req-slot-01", loaded.request_id);
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_load_command_result("req-slot-16", &loaded));
    TEST_ASSERT_EQUAL_STRING("req-slot-16", loaded.request_id);
}

TEST_CASE("test_terminal_survives_partition_restart", "[h1][app_storage]")
{
    init_app_storage();
    app_command_result_t terminal = make_command_result(
        "req-restart-terminal",
        APP_COMMAND_TYPE_BLOCK_TRAFFIC,
        false,
        "blocked command failed");
    claim_and_complete(&terminal);

    TEST_ASSERT_EQUAL(
        ESP_OK,
        nvs_flash_deinit_partition(TEST_NVS_PARTITION));
    TEST_ASSERT_EQUAL(ESP_OK, app_storage_init_nvs());

    app_storage_command_claim_result_t claim =
        APP_STORAGE_COMMAND_CLAIMED;
    app_command_result_t replay = {0};
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_claim_command(
            terminal.request_id,
            terminal.type,
            &claim,
            &replay));
    TEST_ASSERT_EQUAL(APP_STORAGE_COMMAND_REPLAY, claim);
    TEST_ASSERT_FALSE(replay.success);
    TEST_ASSERT_EQUAL_STRING(terminal.message, replay.message);
}

TEST_CASE("test_pending_claim_survives_restart_without_reexecution", "[h1][app_storage]")
{
    init_app_storage();
    app_storage_command_claim_result_t claim =
        APP_STORAGE_COMMAND_REPLAY;
    app_command_result_t replay = {0};
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_claim_command(
            "req-restart-pending",
            APP_COMMAND_TYPE_DISCONNECT_MAC,
            &claim,
            &replay));
    TEST_ASSERT_EQUAL(APP_STORAGE_COMMAND_CLAIMED, claim);

    TEST_ASSERT_EQUAL(
        ESP_OK,
        nvs_flash_deinit_partition(TEST_NVS_PARTITION));
    TEST_ASSERT_EQUAL(ESP_OK, app_storage_init_nvs());

    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_claim_command(
            "req-restart-pending",
            APP_COMMAND_TYPE_DISCONNECT_MAC,
            &claim,
            &replay));
    TEST_ASSERT_EQUAL(APP_STORAGE_COMMAND_INTERRUPTED, claim);
    TEST_ASSERT_FALSE(replay.success);
    TEST_ASSERT_EQUAL_STRING(
        "command execution interrupted",
        replay.message);
}

TEST_CASE("test_uninitialized_nvs_rejects_claim_before_side_effect", "[h1][app_storage]")
{
    init_app_storage();
    TEST_ASSERT_EQUAL(
        ESP_OK,
        nvs_flash_deinit_partition(TEST_NVS_PARTITION));

    app_storage_command_claim_result_t claim =
        APP_STORAGE_COMMAND_REPLAY;
    app_command_result_t replay = {0};
    TEST_ASSERT_NOT_EQUAL(
        ESP_OK,
        app_storage_claim_command(
            "req-nvs-unavailable",
            APP_COMMAND_TYPE_KICK,
            &claim,
            &replay));
    TEST_ASSERT_EQUAL(APP_STORAGE_COMMAND_CLAIMED, claim);
}

TEST_CASE("test_same_request_id_with_different_type_is_conflict", "[h1][app_storage]")
{
    init_app_storage();
    app_command_result_t terminal = make_command_result(
        "req-type-conflict",
        APP_COMMAND_TYPE_ALLOW,
        true,
        "allowed");
    claim_and_complete(&terminal);

    app_storage_command_claim_result_t claim =
        APP_STORAGE_COMMAND_CLAIMED;
    app_command_result_t replay = {0};
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_claim_command(
            terminal.request_id,
            APP_COMMAND_TYPE_REVOKE_ACCESS,
            &claim,
            &replay));
    TEST_ASSERT_EQUAL(APP_STORAGE_COMMAND_TYPE_CONFLICT, claim);

    app_command_result_t loaded = {0};
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_load_command_result(terminal.request_id, &loaded));
    TEST_ASSERT_EQUAL(APP_COMMAND_TYPE_ALLOW, loaded.type);
}

TEST_CASE("test_malformed_command_terminal_is_immutable_and_type_safe", "[h1][app_storage]")
{
    init_app_storage();
    const char *allow_topic =
        "wifi/device/esp32-gateway-001/cmd/allow";
    const char *allow_payload =
        "{\"requestId\":\"req-malformed-terminal\","
        "\"mac\":\"AA:BB:CC:DD:EE:FF\","
        "\"sessionId\":42,"
        "\"ttlSeconds\":0}";
    app_command_envelope_t envelope = {0};
    app_command_request_t request = {0};
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_command_parse_production_envelope(
            allow_topic,
            (int)strlen(allow_topic),
            allow_payload,
            (int)strlen(allow_payload),
            &envelope));
    TEST_ASSERT_EQUAL_STRING(
        "esp32-gateway-001",
        envelope.device_code);
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        app_command_parse_production_payload(
            allow_payload,
            (int)strlen(allow_payload),
            &envelope,
            &request));

    malformed_command_fixture_t fixture = {0};
    app_command_coordinator_dependencies_t dependencies = {
        .context = &fixture,
        .claim = malformed_claim,
        .complete = malformed_complete,
        .execute = malformed_execute,
        .publish = malformed_publish,
        .after_terminal = malformed_after_terminal,
    };

    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_command_coordinator_reject_invalid_payload(
            &request,
            "esp32-gateway-001",
            &dependencies));
    TEST_ASSERT_EQUAL(0, fixture.execute_count);
    TEST_ASSERT_EQUAL(1, fixture.complete_count);
    TEST_ASSERT_EQUAL(1, fixture.publish_count);
    TEST_ASSERT_EQUAL(0, fixture.after_terminal_count);
    TEST_ASSERT_FALSE(fixture.published_result.success);
    TEST_ASSERT_EQUAL_STRING(
        APP_COMMAND_COORDINATOR_PAYLOAD_INVALID,
        fixture.published_result.message);

    app_command_result_t stored = {0};
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_load_command_result(
            envelope.request_id,
            &stored));
    TEST_ASSERT_EQUAL(APP_COMMAND_TYPE_ALLOW, stored.type);
    TEST_ASSERT_FALSE(stored.success);
    TEST_ASSERT_EQUAL_STRING(
        APP_COMMAND_COORDINATOR_PAYLOAD_INVALID,
        stored.message);

    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_command_coordinator_reject_invalid_payload(
            &request,
            "esp32-gateway-001",
            &dependencies));
    TEST_ASSERT_EQUAL(0, fixture.execute_count);
    TEST_ASSERT_EQUAL(1, fixture.complete_count);
    TEST_ASSERT_EQUAL(2, fixture.publish_count);
    TEST_ASSERT_EQUAL(0, fixture.after_terminal_count);
    TEST_ASSERT_EQUAL(APP_COMMAND_TYPE_ALLOW, fixture.published_result.type);
    TEST_ASSERT_EQUAL_STRING(stored.message, fixture.published_result.message);

    const char *revoke_topic =
        "wifi/device/esp32-gateway-001/cmd/revoke-access";
    const char *revoke_payload =
        "{\"requestId\":\"req-malformed-terminal\","
        "\"mac\":\"AA:BB:CC:DD:EE:FF\","
        "\"sessionId\":0}";
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_command_parse_production_envelope(
            revoke_topic,
            (int)strlen(revoke_topic),
            revoke_payload,
            (int)strlen(revoke_payload),
            &envelope));
    TEST_ASSERT_EQUAL(
        APP_COMMAND_TYPE_REVOKE_ACCESS,
        envelope.type);
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        app_command_parse_production_payload(
            revoke_payload,
            (int)strlen(revoke_payload),
            &envelope,
            &request));
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_command_coordinator_reject_invalid_payload(
            &request,
            "esp32-gateway-001",
            &dependencies));
    TEST_ASSERT_EQUAL(0, fixture.execute_count);
    TEST_ASSERT_EQUAL(1, fixture.complete_count);
    TEST_ASSERT_EQUAL(3, fixture.publish_count);
    TEST_ASSERT_EQUAL(0, fixture.after_terminal_count);
    TEST_ASSERT_EQUAL(
        APP_COMMAND_TYPE_REVOKE_ACCESS,
        fixture.published_result.type);
    TEST_ASSERT_FALSE(fixture.published_result.success);
    TEST_ASSERT_EQUAL_STRING(
        "requestId command type conflict",
        fixture.published_result.message);

    memset(&stored, 0, sizeof(stored));
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_load_command_result(
            "req-malformed-terminal",
            &stored));
    TEST_ASSERT_EQUAL(APP_COMMAND_TYPE_ALLOW, stored.type);
    TEST_ASSERT_EQUAL_STRING(
        APP_COMMAND_COORDINATOR_PAYLOAD_INVALID,
        stored.message);
}

TEST_CASE("test_wifi_config_version_four_way_classification", "[h1][app_storage]")
{
    init_app_storage();
    app_storage_wifi_stage_result_t stage_result =
        APP_STORAGE_WIFI_STAGE_VERSION_CONFLICT;

    app_storage_wifi_config_t initial =
        make_wifi_config("req-wifi-7", 7, "FixtureWifi");
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_stage_candidate_wifi_config(
            &initial,
            &stage_result));
    TEST_ASSERT_EQUAL(APP_STORAGE_WIFI_STAGE_STORED, stage_result);

    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_stage_candidate_wifi_config(
            &initial,
            &stage_result));
    TEST_ASSERT_EQUAL(APP_STORAGE_WIFI_STAGE_IDEMPOTENT, stage_result);

    app_storage_wifi_config_t same_version =
        make_wifi_config("req-wifi-7-other", 7, "OtherWifi");
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_stage_candidate_wifi_config(
            &same_version,
            &stage_result));
    TEST_ASSERT_EQUAL(
        APP_STORAGE_WIFI_STAGE_VERSION_CONFLICT,
        stage_result);

    app_storage_wifi_config_t stale =
        make_wifi_config("req-wifi-6", 6, "StaleWifi");
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_stage_candidate_wifi_config(
            &stale,
            &stage_result));
    TEST_ASSERT_EQUAL(APP_STORAGE_WIFI_STAGE_STALE_VERSION, stage_result);

    app_storage_wifi_config_t higher =
        make_wifi_config("req-wifi-8", 8, "HigherWifi");
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_stage_candidate_wifi_config(
            &higher,
            &stage_result));
    TEST_ASSERT_EQUAL(APP_STORAGE_WIFI_STAGE_STORED, stage_result);

    app_storage_wifi_config_t loaded = {0};
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_load_candidate_wifi_config(&loaded));
    TEST_ASSERT_EQUAL_STRING(higher.request_id, loaded.request_id);
    TEST_ASSERT_EQUAL_UINT32(higher.config_version, loaded.config_version);
    TEST_ASSERT_EQUAL_STRING(
        higher.credentials.ssid,
        loaded.credentials.ssid);
}

TEST_CASE("test_pending_same_identity_rejects_changed_credentials", "[h1][app_storage]")
{
    init_app_storage();
    app_storage_wifi_config_t initial =
        make_wifi_config("req-pending-content", 11, "PendingWifi");
    app_storage_wifi_stage_result_t stage_result =
        APP_STORAGE_WIFI_STAGE_VERSION_CONFLICT;

    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_stage_candidate_wifi_config(
            &initial,
            &stage_result));
    TEST_ASSERT_EQUAL(APP_STORAGE_WIFI_STAGE_STORED, stage_result);

    app_storage_wifi_config_t changed_ssid = initial;
    snprintf(
        changed_ssid.credentials.ssid,
        sizeof(changed_ssid.credentials.ssid),
        "%s",
        "ChangedPendingWifi");
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_stage_candidate_wifi_config(
            &changed_ssid,
            &stage_result));
    TEST_ASSERT_EQUAL(
        APP_STORAGE_WIFI_STAGE_VERSION_CONFLICT,
        stage_result);

    app_storage_wifi_config_t changed_password = initial;
    snprintf(
        changed_password.credentials.password,
        sizeof(changed_password.credentials.password),
        "%s",
        "changed-pending-password");
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_stage_candidate_wifi_config(
            &changed_password,
            &stage_result));
    TEST_ASSERT_EQUAL(
        APP_STORAGE_WIFI_STAGE_VERSION_CONFLICT,
        stage_result);
}

TEST_CASE("test_active_same_identity_rejects_changed_credentials", "[h1][app_storage]")
{
    init_app_storage();
    app_storage_wifi_config_t initial =
        make_wifi_config("req-active-content", 12, "ActiveWifi");
    app_storage_wifi_stage_result_t stage_result =
        APP_STORAGE_WIFI_STAGE_VERSION_CONFLICT;

    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_stage_candidate_wifi_config(
            &initial,
            &stage_result));
    TEST_ASSERT_EQUAL(APP_STORAGE_WIFI_STAGE_STORED, stage_result);
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_promote_candidate_wifi_config(
            initial.request_id,
            initial.config_version));

    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_stage_candidate_wifi_config(
            &initial,
            &stage_result));
    TEST_ASSERT_EQUAL(APP_STORAGE_WIFI_STAGE_IDEMPOTENT, stage_result);

    app_storage_wifi_config_t changed_ssid = initial;
    snprintf(
        changed_ssid.credentials.ssid,
        sizeof(changed_ssid.credentials.ssid),
        "%s",
        "ChangedActiveWifi");
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_stage_candidate_wifi_config(
            &changed_ssid,
            &stage_result));
    TEST_ASSERT_EQUAL(
        APP_STORAGE_WIFI_STAGE_VERSION_CONFLICT,
        stage_result);

    app_storage_wifi_config_t changed_password = initial;
    snprintf(
        changed_password.credentials.password,
        sizeof(changed_password.credentials.password),
        "%s",
        "changed-active-password");
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_stage_candidate_wifi_config(
            &changed_password,
            &stage_result));
    TEST_ASSERT_EQUAL(
        APP_STORAGE_WIFI_STAGE_VERSION_CONFLICT,
        stage_result);
}

TEST_CASE("test_stage_wifi_identity_flows_from_topic_to_candidate", "[h1][app_storage]")
{
    init_app_storage();
    const char *topic =
        "wifi/device/esp32-gateway-001/cmd/stage-wifi-config";
    const char *payload =
        "{\"requestId\":\"req-wifi-identity\","
        "\"deviceCode\":\"esp32-gateway-001\","
        "\"ssid\":\"FixtureWifi\","
        "\"password\":\"invalid-test-password\","
        "\"configVersion\":9}";
    app_command_request_t request = {0};
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_command_parse(
            topic,
            (int)strlen(topic),
            payload,
            (int)strlen(payload),
            &request));
    TEST_ASSERT_TRUE(
        app_command_targets_device(&request, "esp32-gateway-001"));

    app_storage_wifi_config_t candidate = {0};
    snprintf(
        candidate.request_id,
        sizeof(candidate.request_id),
        "%s",
        request.request_id);
    snprintf(
        candidate.credentials.ssid,
        sizeof(candidate.credentials.ssid),
        "%s",
        request.wifi_ssid);
    snprintf(
        candidate.credentials.password,
        sizeof(candidate.credentials.password),
        "%s",
        request.wifi_password);
    candidate.config_version = request.wifi_config_version;

    app_storage_wifi_stage_result_t stage_result =
        APP_STORAGE_WIFI_STAGE_VERSION_CONFLICT;
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_stage_candidate_wifi_config(
            &candidate,
            &stage_result));
    TEST_ASSERT_EQUAL(APP_STORAGE_WIFI_STAGE_STORED, stage_result);

    app_storage_wifi_config_t loaded = {0};
    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_storage_load_candidate_wifi_config(&loaded));
    TEST_ASSERT_EQUAL_STRING(request.request_id, loaded.request_id);
    TEST_ASSERT_EQUAL_UINT32(
        request.wifi_config_version,
        loaded.config_version);
    TEST_ASSERT_EQUAL_STRING(request.wifi_ssid, loaded.credentials.ssid);
}
