#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tls_sni_parser.h"
#include "unity.h"

static void write_u16(uint8_t *buffer, size_t *offset, uint16_t value)
{
    buffer[(*offset)++] = (uint8_t)(value >> 8);
    buffer[(*offset)++] = (uint8_t)value;
}

static void build_client_hello(
    uint8_t *buffer,
    size_t buffer_size,
    const char *hostname,
    size_t *hello_length)
{
    TEST_ASSERT_NOT_NULL(buffer);
    TEST_ASSERT_NOT_NULL(hello_length);
    TEST_ASSERT_TRUE(buffer_size >= 128);

    size_t offset = 5;
    size_t handshake_start = offset;
    buffer[offset++] = 1;
    offset += 3;

    write_u16(buffer, &offset, 0x0303);
    memset(buffer + offset, 0x5a, 32);
    offset += 32;

    buffer[offset++] = 0;
    write_u16(buffer, &offset, 2);
    write_u16(buffer, &offset, 0x1301);
    buffer[offset++] = 1;
    buffer[offset++] = 0;

    size_t extensions_length_offset = offset;
    offset += 2;
    size_t extensions_start = offset;

    if (hostname != NULL)
    {
        size_t hostname_length = strlen(hostname);
        TEST_ASSERT_TRUE(hostname_length > 0);
        TEST_ASSERT_TRUE(hostname_length < 64);

        write_u16(buffer, &offset, 0);
        size_t extension_length_offset = offset;
        offset += 2;
        size_t extension_start = offset;

        write_u16(buffer, &offset, (uint16_t)(hostname_length + 3));
        buffer[offset++] = 0;
        write_u16(buffer, &offset, (uint16_t)hostname_length);
        memcpy(buffer + offset, hostname, hostname_length);
        offset += hostname_length;

        uint16_t extension_length =
            (uint16_t)(offset - extension_start);
        buffer[extension_length_offset] =
            (uint8_t)(extension_length >> 8);
        buffer[extension_length_offset + 1] =
            (uint8_t)extension_length;
    }

    uint16_t extensions_length =
        (uint16_t)(offset - extensions_start);
    buffer[extensions_length_offset] =
        (uint8_t)(extensions_length >> 8);
    buffer[extensions_length_offset + 1] =
        (uint8_t)extensions_length;

    size_t handshake_length = offset - handshake_start;
    size_t body_length = handshake_length - 4;
    buffer[handshake_start + 1] = (uint8_t)(body_length >> 16);
    buffer[handshake_start + 2] = (uint8_t)(body_length >> 8);
    buffer[handshake_start + 3] = (uint8_t)body_length;

    buffer[0] = 22;
    buffer[1] = 0x03;
    buffer[2] = 0x03;
    buffer[3] = (uint8_t)(handshake_length >> 8);
    buffer[4] = (uint8_t)handshake_length;
    *hello_length = offset;
}

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_fragmented_client_hello_finds_normalized_sni(void)
{
    uint8_t hello[128] = {0};
    size_t hello_length = 0;
    build_client_hello(
        hello,
        sizeof(hello),
        "Portal.TEST",
        &hello_length);
    tls_sni_parser_t parser;
    char hostname[TLS_SNI_HOSTNAME_SIZE] = {0};

    tls_sni_parser_init(&parser);
    TEST_ASSERT_EQUAL(
        TLS_SNI_PARSE_NEED_MORE,
        tls_sni_parser_feed(
            &parser,
            hello,
            7,
            hostname,
            sizeof(hostname)));
    TEST_ASSERT_EQUAL(
        TLS_SNI_PARSE_FOUND,
        tls_sni_parser_feed(
            &parser,
            hello + 7,
            hello_length - 7,
            hostname,
            sizeof(hostname)));
    TEST_ASSERT_EQUAL_STRING("portal.test", hostname);
}

static void test_client_hello_without_sni_is_explicit(void)
{
    uint8_t hello[128] = {0};
    size_t hello_length = 0;
    build_client_hello(
        hello,
        sizeof(hello),
        NULL,
        &hello_length);
    tls_sni_parser_t parser;
    char hostname[TLS_SNI_HOSTNAME_SIZE] = {0};

    tls_sni_parser_init(&parser);
    TEST_ASSERT_EQUAL(
        TLS_SNI_PARSE_NO_SNI,
        tls_sni_parser_feed(
            &parser,
            hello,
            hello_length,
            hostname,
            sizeof(hostname)));
    TEST_ASSERT_EQUAL_STRING("", hostname);
}

static void test_non_handshake_record_is_rejected(void)
{
    const uint8_t record[] = {
        23, 0x03, 0x03, 0x00, 0x01, 0x00
    };
    tls_sni_parser_t parser;
    char hostname[TLS_SNI_HOSTNAME_SIZE] = {0};

    tls_sni_parser_init(&parser);
    TEST_ASSERT_EQUAL(
        TLS_SNI_PARSE_INVALID,
        tls_sni_parser_feed(
            &parser,
            record,
            sizeof(record),
            hostname,
            sizeof(hostname)));
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1000));

    UNITY_BEGIN();
    RUN_TEST(test_fragmented_client_hello_finds_normalized_sni);
    RUN_TEST(test_client_hello_without_sni_is_explicit);
    RUN_TEST(test_non_handshake_record_is_rejected);
    UNITY_END();
}
