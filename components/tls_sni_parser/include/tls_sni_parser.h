#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TLS_SNI_HOSTNAME_SIZE 256
#define TLS_CLIENT_HELLO_MAX_SIZE 2048

typedef enum
{
    TLS_SNI_PARSE_NEED_MORE = 0,
    TLS_SNI_PARSE_FOUND,
    TLS_SNI_PARSE_NO_SNI,
    TLS_SNI_PARSE_INVALID
} tls_sni_parse_result_t;

typedef struct
{
    uint8_t record_header[5];
    size_t record_header_used;
    uint16_t record_remaining;

    uint8_t client_hello[TLS_CLIENT_HELLO_MAX_SIZE];
    size_t client_hello_used;
    size_t client_hello_expected;

    bool ech_present;
    bool finished;
} tls_sni_parser_t;

void tls_sni_parser_init(tls_sni_parser_t *parser);
tls_sni_parse_result_t tls_sni_parser_feed(tls_sni_parser_t *parser,const uint8_t *data,size_t data_length,char *hostname,size_t hostname_size);