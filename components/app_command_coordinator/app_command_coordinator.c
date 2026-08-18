#include <stdio.h>
#include <string.h>

#include "app_command_coordinator.h"

static bool is_production_command(app_command_type_t type)
{
    return type >= APP_COMMAND_TYPE_ALLOW &&
           type <= APP_COMMAND_TYPE_STAGE_WIFI_CONFIG;
}

static app_command_result_t initial_result(
    const app_command_request_t *request)
{
    app_command_result_t result = {0};
    snprintf(
        result.request_id,
        sizeof(result.request_id),
        "%s",
        request->request_id);
    result.type = request->type;
    result.success = false;
    return result;
}

static esp_err_t publish_fixed_failure(
    const app_command_coordinator_dependencies_t *dependencies,
    app_command_result_t *result,
    const char *message,
    esp_err_t operation_result)
{
    result->success = false;
    snprintf(
        result->message,
        sizeof(result->message),
        "%s",
        message);
    esp_err_t publish_result =
        dependencies->publish(dependencies->context, result);
    return operation_result != ESP_OK
               ? operation_result
               : publish_result;
}

static esp_err_t coordinate_command(
    const app_command_request_t *request,
    const char *local_device_code,
    const app_command_coordinator_dependencies_t *dependencies,
    bool reject_invalid_payload)
{
    if (request == NULL ||
        local_device_code == NULL ||
        dependencies == NULL ||
        dependencies->claim == NULL ||
        dependencies->complete == NULL ||
        (!reject_invalid_payload && dependencies->execute == NULL) ||
        dependencies->publish == NULL ||
        !is_production_command(request->type))
    {
        return ESP_ERR_INVALID_ARG;
    }

    app_command_result_t result = initial_result(request);
    if (request->request_id[0] == '\0')
    {
        return publish_fixed_failure(
            dependencies,
            &result,
            "requestId required",
            ESP_ERR_INVALID_ARG);
    }
    if (!app_command_targets_device(request, local_device_code))
    {
        return publish_fixed_failure(
            dependencies,
            &result,
            "device code mismatch",
            ESP_ERR_INVALID_ARG);
    }

    app_storage_command_claim_result_t claim_result =
        APP_STORAGE_COMMAND_CLAIMED;
    app_command_result_t replay_result = {0};
    esp_err_t claim_err = dependencies->claim(
        dependencies->context,
        request->request_id,
        request->type,
        &claim_result,
        &replay_result);
    if (claim_err != ESP_OK)
    {
        return publish_fixed_failure(
            dependencies,
            &result,
            "command result cache unavailable",
            claim_err);
    }

    if (claim_result == APP_STORAGE_COMMAND_REPLAY ||
        claim_result == APP_STORAGE_COMMAND_INTERRUPTED)
    {
        return dependencies->publish(
            dependencies->context,
            &replay_result);
    }
    if (claim_result == APP_STORAGE_COMMAND_TYPE_CONFLICT)
    {
        return publish_fixed_failure(
            dependencies,
            &result,
            "requestId command type conflict",
            ESP_OK);
    }
    if (claim_result != APP_STORAGE_COMMAND_CLAIMED)
    {
        return publish_fixed_failure(
            dependencies,
            &result,
            "command claim state invalid",
            ESP_ERR_INVALID_STATE);
    }

    if (reject_invalid_payload)
    {
        result.success = false;
        snprintf(
            result.message,
            sizeof(result.message),
            "%s",
            APP_COMMAND_COORDINATOR_PAYLOAD_INVALID);
    }
    else
    {
        esp_err_t execute_err = dependencies->execute(
            dependencies->context,
            request,
            &result);
        if (execute_err != ESP_OK)
        {
            result.success = false;
            snprintf(
                result.message,
                sizeof(result.message),
                "%s",
                "command execution failed");
        }
    }

    esp_err_t complete_err = dependencies->complete(
        dependencies->context,
        &result);
    if (complete_err != ESP_OK)
    {
        return publish_fixed_failure(
            dependencies,
            &result,
            APP_COMMAND_COORDINATOR_OUTCOME_UNCERTAIN,
            complete_err);
    }

    esp_err_t publish_err = dependencies->publish(
        dependencies->context,
        &result);
    if (!reject_invalid_payload &&
        dependencies->after_terminal != NULL)
    {
        dependencies->after_terminal(
            dependencies->context,
            request,
            &result,
            publish_err);
    }
    return publish_err;
}

esp_err_t app_command_coordinator_handle(
    const app_command_request_t *request,
    const char *local_device_code,
    const app_command_coordinator_dependencies_t *dependencies)
{
    return coordinate_command(
        request,
        local_device_code,
        dependencies,
        false);
}

esp_err_t app_command_coordinator_reject_invalid_payload(
    const app_command_request_t *request,
    const char *local_device_code,
    const app_command_coordinator_dependencies_t *dependencies)
{
    return coordinate_command(
        request,
        local_device_code,
        dependencies,
        true);
}
