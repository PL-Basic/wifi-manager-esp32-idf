#include <ctype.h>
#include <string.h>

#include "tls_sni_parser.h"

#define TLS_CONTENT_TYPE_HANDSHAKE 22
#define TLS_HANDSHAKE_CLIENT_HELLO 1
#define TLS_EXTENSION_SERVER_NAME 0
#define TLS_EXTENSION_ECH 0xFE0D

static uint16_t read_u16(const uint8_t *data)
{
    return ((uint16_t)data[0] << 8) | data[1];
}

static tls_sni_parse_result_t parse_client_hello(tls_sni_parser_t *parser, char *hostname, size_t hostname_size)
{
    const uint8_t *hello = parser->client_hello;
    size_t length = parser->client_hello_expected;
    size_t offset = 4;

    // legacy_version + random
    if (length < offset + 34)
    {
        return TLS_SNI_PARSE_INVALID;
    }
    offset += 34;

    // legacy_session_id
    if (offset >= length)
    {
        return TLS_SNI_PARSE_INVALID;
    }
    uint8_t session_length = hello[offset++];
    if (offset + session_length > length)
    {
        return TLS_SNI_PARSE_INVALID;
    }
    offset += session_length;

    // cipher_suites
    if (offset + 2 > length)
    {
        return TLS_SNI_PARSE_INVALID;
    }
    uint16_t cipher_length = read_u16(hello + offset);
    offset += 2;
    if (cipher_length < 2 || (cipher_length & 1) != 0 || offset + cipher_length > length)
    {
        return TLS_SNI_PARSE_INVALID;
    }
    offset += cipher_length;

    // legacy_compression_methods
    if (offset >= length)
    {
        return TLS_SNI_PARSE_INVALID;
    }
    uint8_t compression_length = hello[offset++];
    if (compression_length == 0 || offset + compression_length > length)
    {
        return TLS_SNI_PARSE_INVALID;
    }
    offset += compression_length;

    if (offset == length)
    {
        return TLS_SNI_PARSE_NO_SNI;
    }

    if (offset + 2 > length)
    {
        return TLS_SNI_PARSE_INVALID;
    }

    uint16_t extensions_length = read_u16(hello + offset);
    offset += 2;

    // extensions 必须正好占满 ClientHello 剩余内容，不接受尾随字节。
    if ((size_t)extensions_length != length - offset)
    {
        return TLS_SNI_PARSE_INVALID;
    }

    size_t extensions_end = offset + extensions_length;
    bool sni_found = false;

    while (offset < extensions_end)
    {
        // 一个扩展至少需要 type 和 length，共 4 字节。
        if (extensions_end - offset < 4)
        {
            return TLS_SNI_PARSE_INVALID;
        }

        uint16_t type = read_u16(hello + offset);
        uint16_t extension_length = read_u16(hello + offset + 2);
        offset += 4;

        // 使用减法检查，避免 offset + length 发生整数溢出。
        if ((size_t)extension_length > extensions_end - offset)
        {
            return TLS_SNI_PARSE_INVALID;
        }

        size_t extension_end = offset + extension_length;

        // 即使此前已经找到 SNI，也必须继续扫描全部扩展。
        if (type == TLS_EXTENSION_ECH)
        {
            parser->ech_present = true;
        }

        if (type == TLS_EXTENSION_SERVER_NAME)
        {
            // ServerNameList 长度字段 2 字节，至少还要有一个
            // name_type 和 name_length，共 3 字节。
            if (extension_length < 5)
            {
                return TLS_SNI_PARSE_INVALID;
            }

            uint16_t name_list_length = read_u16(hello + offset);
            size_t name_offset = offset + 2;

            // ServerNameList 必须完整占据 server_name 扩展。
            if ((size_t)name_list_length != extension_length - 2)
            {
                return TLS_SNI_PARSE_INVALID;
            }

            size_t name_list_end = name_offset + name_list_length;

            while (name_offset < name_list_end)
            {
                if (name_list_end - name_offset < 3)
                {
                    return TLS_SNI_PARSE_INVALID;
                }

                uint8_t name_type = hello[name_offset++];
                uint16_t name_length = read_u16(hello + name_offset);
                name_offset += 2;

                if ((size_t)name_length > name_list_end - name_offset)
                {
                    return TLS_SNI_PARSE_INVALID;
                }

                if (name_type == 0)
                {
                    // RFC 6066 不允许重复的 host_name。
                    if (sni_found || name_length == 0 || name_length >= hostname_size)
                    {
                        return TLS_SNI_PARSE_INVALID;
                    }

                    for (uint16_t i = 0; i < name_length; i++)
                    {
                        uint8_t value = hello[name_offset + i];

                        // 拒绝 NUL、控制字符和非 ASCII hostname。
                        if (value == 0 || value < 0x21 || value > 0x7E)
                        {
                            return TLS_SNI_PARSE_INVALID;
                        }

                        hostname[i] = (char)tolower((unsigned char)value);
                    }

                    hostname[name_length] = '\0';
                    sni_found = true;
                }

                name_offset += name_length;
            }

            if (name_offset != name_list_end)
            {
                return TLS_SNI_PARSE_INVALID;
            }
        }

        offset = extension_end;
    }

    if (offset != extensions_end)
    {
        return TLS_SNI_PARSE_INVALID;
    }

    return sni_found ? TLS_SNI_PARSE_FOUND : TLS_SNI_PARSE_NO_SNI;
}

void tls_sni_parser_init(tls_sni_parser_t *parser)
{
    if (parser != NULL)
    {
        memset(parser, 0, sizeof(*parser));
    }
}

tls_sni_parse_result_t tls_sni_parser_feed(tls_sni_parser_t *parser, const uint8_t *data, size_t data_length, char *hostname, size_t hostname_size)
{
    if (parser == NULL || data == NULL || hostname == NULL || hostname_size == 0 || parser->finished)
    {
        return TLS_SNI_PARSE_INVALID;
    }
    hostname[0] = '\0';
    size_t input_offset = 0;

    while (input_offset < data_length)
    {
        while (parser->record_header_used < 5 &&
               input_offset < data_length)
        {
            parser->record_header[parser->record_header_used++] = data[input_offset++];
        }

        if (parser->record_header_used < 5)
        {
            return TLS_SNI_PARSE_NEED_MORE;
        }
        if (parser->record_remaining == 0)
        {
            if (parser->record_header[0] != TLS_CONTENT_TYPE_HANDSHAKE)
            {
                parser->finished = true;
                return TLS_SNI_PARSE_INVALID;
            }

            parser->record_remaining = read_u16(parser->record_header + 3);

            if (parser->record_remaining == 0)
            {
                parser->finished = true;
                return TLS_SNI_PARSE_INVALID;
            }
        }

        size_t wanted = parser->client_hello_expected == 0 ? 4 - parser->client_hello_used : parser->client_hello_expected - parser->client_hello_used;

        size_t available = data_length - input_offset;
        size_t copy_length = available;

        if (copy_length > parser->record_remaining)
        {
            copy_length = parser->record_remaining;
        }
        if (copy_length > wanted)
        {
            copy_length = wanted;
        }
        memcpy(parser->client_hello + parser->client_hello_used, data + input_offset, copy_length);

        parser->client_hello_used += copy_length;
        parser->record_remaining -= (uint16_t)copy_length;
        input_offset += copy_length;

        if (parser->client_hello_used >= 1 && parser->client_hello[0] != TLS_HANDSHAKE_CLIENT_HELLO)
        {
            parser->finished = true;
            return TLS_SNI_PARSE_INVALID;
        }

        if (parser->client_hello_used == 4 && parser->client_hello_expected == 0)
        {
            size_t body_length = ((size_t)parser->client_hello[1] << 16) | ((size_t)parser->client_hello[2] << 8) | parser->client_hello[3];

            parser->client_hello_expected = body_length + 4;

            if (parser->client_hello_expected > sizeof(parser->client_hello) || parser->client_hello_expected <= 4)
            {
                parser->finished = true;
                return TLS_SNI_PARSE_INVALID;
            }
        }

        if (parser->client_hello_expected > 0 && parser->client_hello_used == parser->client_hello_expected)
        {
            parser->finished = true;
            return parse_client_hello( parser, hostname, hostname_size);
        }

        if (parser->record_remaining == 0)
        {
            parser->record_header_used = 0;
            memset(parser->record_header, 0, sizeof(parser->record_header));
        }
    }

    return TLS_SNI_PARSE_NEED_MORE;
}