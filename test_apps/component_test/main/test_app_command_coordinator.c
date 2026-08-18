#include <stdio.h>
#include <string.h>

#include "app_command_coordinator.h"
#include "unity.h"

#define TEST_DEVICE_CODE "esp32-gateway-001"

typedef struct
{
    char events[16];
    size_t event_count;
    esp_err_t claim_err;
    app_storage_command_claim_result_t claim_result;
    esp_err_t execute_err;
    esp_err_t complete_err;
    esp_err_t publish_err;
    int claim_count;
    int execute_count;
    int complete_count;
    int publish_count;
    int after_terminal_count;
    app_command_request_t executed_request;
    app_command_result_t completed_result;
    app_command_result_t published_result;
} coordinator_fixture_t;

static void record_event(coordinator_fixture_t *fixture, char event)
{
    TEST_ASSERT_LESS_THAN(sizeof(fixture->events) - 1, fixture->event_count);
    fixture->events[fixture->event_count++] = event;
    fixture->events[fixture->event_count] = '\0';
}

static coordinator_fixture_t make_fixture(void)
{
    coordinator_fixture_t fixture = {
        .claim_err = ESP_OK,
        .claim_result = APP_STORAGE_COMMAND_CLAIMED,
        .execute_err = ESP_OK,
        .complete_err = ESP_OK,
        .publish_err = ESP_OK,
    };
    return fixture;
}

static app_command_request_t make_request(app_command_type_t type)
{
    app_command_request_t request = {0};
    request.type = type;
    snprintf(
        request.request_id,
        sizeof(request.request_id),
        "%s",
        "req-coordinator-v1");
    snprintf(
        request.device_code,
        sizeof(request.device_code),
        "%s",
        TEST_DEVICE_CODE);
    return request;
}

static esp_err_t fixture_claim(
    void *context,
    const char *request_id,
    app_command_type_t type,
    app_storage_command_claim_result_t *claim_result,
    app_command_result_t *replay_result)
{
    coordinator_fixture_t *fixture = context;
    record_event(fixture, 'C');
    fixture->claim_count++;
    TEST_ASSERT_EQUAL_STRING("req-coordinator-v1", request_id);
    *claim_result = fixture->claim_result;
    if (fixture->claim_result == APP_STORAGE_COMMAND_REPLAY ||
        fixture->claim_result == APP_STORAGE_COMMAND_INTERRUPTED)
    {
        snprintf(
            replay_result->request_id,
            sizeof(replay_result->request_id),
            "%s",
            request_id);
        replay_result->type = type;
        replay_result->success = false;
        snprintf(
            replay_result->message,
            sizeof(replay_result->message),
            "%s",
            "stored command outcome");
    }
    return fixture->claim_err;
}

static esp_err_t fixture_execute(
    void *context,
    const app_command_request_t *request,
    app_command_result_t *result)
{
    coordinator_fixture_t *fixture = context;
    record_event(fixture, 'E');
    fixture->execute_count++;
    fixture->executed_request = *request;
    result->success = true;
    snprintf(
        result->message,
        sizeof(result->message),
        "%s",
        "side effect applied");
    return fixture->execute_err;
}

static esp_err_t fixture_complete(
    void *context,
    const app_command_result_t *result)
{
    coordinator_fixture_t *fixture = context;
    record_event(fixture, 'T');
    fixture->complete_count++;
    fixture->completed_result = *result;
    return fixture->complete_err;
}

static esp_err_t fixture_publish(
    void *context,
    const app_command_result_t *result)
{
    coordinator_fixture_t *fixture = context;
    record_event(fixture, 'P');
    fixture->publish_count++;
    fixture->published_result = *result;
    return fixture->publish_err;
}

static void fixture_after_terminal(
    void *context,
    const app_command_request_t *request,
    const app_command_result_t *result,
    esp_err_t publish_result)
{
    coordinator_fixture_t *fixture = context;
    record_event(fixture, 'A');
    fixture->after_terminal_count++;
    TEST_ASSERT_EQUAL(request->type, result->type);
    TEST_ASSERT_EQUAL(fixture->publish_err, publish_result);
}

static app_command_coordinator_dependencies_t make_dependencies(
    coordinator_fixture_t *fixture)
{
    app_command_coordinator_dependencies_t dependencies = {
        .context = fixture,
        .claim = fixture_claim,
        .complete = fixture_complete,
        .execute = fixture_execute,
        .publish = fixture_publish,
        .after_terminal = fixture_after_terminal,
    };
    return dependencies;
}

TEST_CASE("test_all_production_commands_follow_persisted_side_effect_order", "[h1][app_command_coordinator]")
{
    const app_command_type_t types[] = {
        APP_COMMAND_TYPE_ALLOW,
        APP_COMMAND_TYPE_REVOKE_ACCESS,
        APP_COMMAND_TYPE_KICK,
        APP_COMMAND_TYPE_DISCONNECT_MAC,
        APP_COMMAND_TYPE_BLOCK_TRAFFIC,
        APP_COMMAND_TYPE_STAGE_WIFI_CONFIG,
    };

    for (size_t index = 0; index < sizeof(types) / sizeof(types[0]); index++)
    {
        coordinator_fixture_t fixture = make_fixture();
        app_command_coordinator_dependencies_t dependencies =
            make_dependencies(&fixture);
        app_command_request_t request = make_request(types[index]);

        TEST_ASSERT_EQUAL(
            ESP_OK,
            app_command_coordinator_handle(
                &request,
                TEST_DEVICE_CODE,
                &dependencies));
        TEST_ASSERT_EQUAL_STRING("CETPA", fixture.events);
        TEST_ASSERT_EQUAL(1, fixture.claim_count);
        TEST_ASSERT_EQUAL(1, fixture.execute_count);
        TEST_ASSERT_EQUAL(1, fixture.complete_count);
        TEST_ASSERT_EQUAL(1, fixture.publish_count);
        TEST_ASSERT_EQUAL(1, fixture.after_terminal_count);
        TEST_ASSERT_EQUAL(types[index], fixture.executed_request.type);
        TEST_ASSERT_EQUAL_STRING(
            TEST_DEVICE_CODE,
            fixture.executed_request.device_code);
        TEST_ASSERT_TRUE(fixture.completed_result.success);
        TEST_ASSERT_TRUE(fixture.published_result.success);
    }
}

TEST_CASE("test_claim_failure_has_zero_side_effects", "[h1][app_command_coordinator]")
{
    coordinator_fixture_t fixture = make_fixture();
    fixture.claim_err = ESP_FAIL;
    app_command_coordinator_dependencies_t dependencies =
        make_dependencies(&fixture);
    app_command_request_t request = make_request(APP_COMMAND_TYPE_ALLOW);

    TEST_ASSERT_EQUAL(
        ESP_FAIL,
        app_command_coordinator_handle(
            &request,
            TEST_DEVICE_CODE,
            &dependencies));
    TEST_ASSERT_EQUAL_STRING("CP", fixture.events);
    TEST_ASSERT_EQUAL(0, fixture.execute_count);
    TEST_ASSERT_EQUAL(0, fixture.complete_count);
    TEST_ASSERT_EQUAL(0, fixture.after_terminal_count);
    TEST_ASSERT_FALSE(fixture.published_result.success);
    TEST_ASSERT_EQUAL_STRING(
        "command result cache unavailable",
        fixture.published_result.message);
}

TEST_CASE("test_type_conflict_has_zero_side_effects", "[h1][app_command_coordinator]")
{
    coordinator_fixture_t fixture = make_fixture();
    fixture.claim_result = APP_STORAGE_COMMAND_TYPE_CONFLICT;
    app_command_coordinator_dependencies_t dependencies =
        make_dependencies(&fixture);
    app_command_request_t request =
        make_request(APP_COMMAND_TYPE_REVOKE_ACCESS);

    TEST_ASSERT_EQUAL(
        ESP_OK,
        app_command_coordinator_handle(
            &request,
            TEST_DEVICE_CODE,
            &dependencies));
    TEST_ASSERT_EQUAL_STRING("CP", fixture.events);
    TEST_ASSERT_EQUAL(0, fixture.execute_count);
    TEST_ASSERT_EQUAL(0, fixture.complete_count);
    TEST_ASSERT_EQUAL(0, fixture.after_terminal_count);
    TEST_ASSERT_FALSE(fixture.published_result.success);
    TEST_ASSERT_EQUAL_STRING(
        "requestId command type conflict",
        fixture.published_result.message);
}

TEST_CASE("test_device_code_mismatch_is_rejected_before_claim", "[h1][app_command_coordinator]")
{
    coordinator_fixture_t fixture = make_fixture();
    app_command_coordinator_dependencies_t dependencies =
        make_dependencies(&fixture);
    app_command_request_t request =
        make_request(APP_COMMAND_TYPE_BLOCK_TRAFFIC);
    snprintf(
        request.device_code,
        sizeof(request.device_code),
        "%s",
        "esp32-gateway-002");

    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        app_command_coordinator_handle(
            &request,
            TEST_DEVICE_CODE,
            &dependencies));
    TEST_ASSERT_EQUAL_STRING("P", fixture.events);
    TEST_ASSERT_EQUAL(0, fixture.claim_count);
    TEST_ASSERT_EQUAL(0, fixture.execute_count);
    TEST_ASSERT_EQUAL(0, fixture.complete_count);
    TEST_ASSERT_EQUAL(0, fixture.after_terminal_count);
    TEST_ASSERT_FALSE(fixture.published_result.success);
    TEST_ASSERT_EQUAL_STRING(
        "device code mismatch",
        fixture.published_result.message);
}

TEST_CASE("test_complete_failure_publishes_only_uncertain_failure", "[h1][app_command_coordinator]")
{
    coordinator_fixture_t fixture = make_fixture();
    fixture.complete_err = ESP_FAIL;
    app_command_coordinator_dependencies_t dependencies =
        make_dependencies(&fixture);
    app_command_request_t request =
        make_request(APP_COMMAND_TYPE_DISCONNECT_MAC);

    TEST_ASSERT_EQUAL(
        ESP_FAIL,
        app_command_coordinator_handle(
            &request,
            TEST_DEVICE_CODE,
            &dependencies));
    TEST_ASSERT_EQUAL_STRING("CETP", fixture.events);
    TEST_ASSERT_TRUE(fixture.completed_result.success);
    TEST_ASSERT_FALSE(fixture.published_result.success);
    TEST_ASSERT_EQUAL_STRING(
        APP_COMMAND_COORDINATOR_OUTCOME_UNCERTAIN,
        fixture.published_result.message);
    TEST_ASSERT_EQUAL(0, fixture.after_terminal_count);
}

TEST_CASE("test_replay_and_interrupted_claims_never_execute_side_effects", "[h1][app_command_coordinator]")
{
    const app_storage_command_claim_result_t claim_results[] = {
        APP_STORAGE_COMMAND_REPLAY,
        APP_STORAGE_COMMAND_INTERRUPTED,
    };

    for (size_t index = 0;
         index < sizeof(claim_results) / sizeof(claim_results[0]);
         index++)
    {
        coordinator_fixture_t fixture = make_fixture();
        fixture.claim_result = claim_results[index];
        app_command_coordinator_dependencies_t dependencies =
            make_dependencies(&fixture);
        app_command_request_t request =
            make_request(APP_COMMAND_TYPE_STAGE_WIFI_CONFIG);

        TEST_ASSERT_EQUAL(
            ESP_OK,
            app_command_coordinator_handle(
                &request,
                TEST_DEVICE_CODE,
                &dependencies));
        TEST_ASSERT_EQUAL_STRING("CP", fixture.events);
        TEST_ASSERT_EQUAL(0, fixture.execute_count);
        TEST_ASSERT_EQUAL(0, fixture.complete_count);
        TEST_ASSERT_EQUAL(0, fixture.after_terminal_count);
        TEST_ASSERT_EQUAL_STRING(
            "stored command outcome",
            fixture.published_result.message);
    }
}
