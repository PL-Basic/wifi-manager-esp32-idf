#pragma once

#include "app_command.h"
#include "app_storage.h"

#define APP_COMMAND_COORDINATOR_OUTCOME_UNCERTAIN \
    "command outcome uncertain: terminal persistence failed"
#define APP_COMMAND_COORDINATOR_PAYLOAD_INVALID \
    "command payload invalid"

typedef esp_err_t (*app_command_claim_fn)(
    void *context,
    const char *request_id,
    app_command_type_t type,
    app_storage_command_claim_result_t *claim_result,
    app_command_result_t *replay_result);

typedef esp_err_t (*app_command_complete_fn)(
    void *context,
    const app_command_result_t *result);

typedef esp_err_t (*app_command_execute_fn)(
    void *context,
    const app_command_request_t *request,
    app_command_result_t *result);

typedef esp_err_t (*app_command_publish_fn)(
    void *context,
    const app_command_result_t *result);

typedef void (*app_command_after_terminal_fn)(
    void *context,
    const app_command_request_t *request,
    const app_command_result_t *result,
    esp_err_t publish_result);

typedef struct
{
    void *context;
    app_command_claim_fn claim;
    app_command_complete_fn complete;
    app_command_execute_fn execute;
    app_command_publish_fn publish;
    app_command_after_terminal_fn after_terminal;
} app_command_coordinator_dependencies_t;

// 统一保证 claim、真实副作用、终态写入和外发的固定顺序。
esp_err_t app_command_coordinator_handle(
    const app_command_request_t *request,
    const char *local_device_code,
    const app_command_coordinator_dependencies_t *dependencies);

// 可识别身份的非法 payload 必须先 claim，再固化固定失败终态。
esp_err_t app_command_coordinator_reject_invalid_payload(
    const app_command_request_t *request,
    const char *local_device_code,
    const app_command_coordinator_dependencies_t *dependencies);
