#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <ctype.h>
#include <string.h>

#include "access_filter.h"
#include "client_access.h"
#include "tls_sni_parser.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"

#include "freertos/FreeRTOS.h"

#include "lwip/def.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ip.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/udp.h"
#include "lwip/prot/tcp.h"
#include "lwip/sys.h"
#include "lwip/inet_chksum.h"

// 最多并行检查 12 条 TLS 流，约占用 25 KB 静态内存。
#define ACCESS_FILTER_TLS_FLOW_CAPACITY 12

// ClientHello 长时间不完整时释放检查流。
#define ACCESS_FILTER_TLS_INSPECTION_TIMEOUT_MS 15000U

// 已判定流空闲两分钟后释放，避免四元组永久占用。
#define ACCESS_FILTER_TLS_DECISION_TIMEOUT_MS 120000U

// 限制单条流为完成 ClientHello 可提交给解析器的 TCP 字节数。
#define ACCESS_FILTER_TLS_STREAM_LIMIT 4096U

#define ACCESS_FILTER_TLS_FEED_CHUNK_SIZE 256U

static const char *TAG = "access_filter";

typedef enum
{
    TLS_FLOW_FREE = 0,
    TLS_FLOW_INSPECTING,
    TLS_FLOW_ALLOW,
    TLS_FLOW_BLOCK
} tls_flow_state_t;

typedef struct
{
    tls_flow_state_t state;

    // 四元组唯一标识一条客户端到服务器的 TCP 流。
    uint32_t source_ipv4;
    uint32_t destination_ipv4;
    uint16_t source_port;
    uint16_t destination_port;

    // 用于识别重传、重叠和序列号缺口。
    uint32_t next_sequence;
    uint32_t syn_sequence;
    bool saw_syn;

    // 限制解析资源和清理空闲流。
    size_t inspected_bytes;
    uint32_t last_seen_ms;

    tls_sni_parser_t parser;
} tls_flow_t;

static tls_flow_t *create_tls_flow(uint32_t source_ipv4, uint32_t destination_ipv4, uint16_t source_port, uint16_t destination_port, uint32_t now_ms);
static void expire_tls_flows(uint32_t now_ms);
static tls_flow_t *find_tls_flow(uint32_t source_ipv4, uint32_t destination_ipv4, uint16_t source_port, uint16_t destination_port);

// 保存默认SoftAP对应的lwIP底层网络接口
// 过滤器只处理从这个接口进入的数据包
static struct netif *s_softap_netif = NULL;
// 保存已经转换为网络字节序的外部Portal服务器IPv4地址
static ip4_addr_t s_portal_server_ipv4 = {0};
// false表示当前尚未配置外部Portal服务器白名单
static bool s_portal_server_enabled = false;

// 规则只追加、不修改，重启后自动清空。
static uint32_t s_blocked_destination_ipv4[ACCESS_FILTER_BLOCKED_IPV4_CAPACITY];
static size_t s_blocked_destination_count = 0;

static char s_blocked_hostnames[ACCESS_FILTER_BLOCKED_HOSTNAME_CAPACITY][ACCESS_FILTER_HOSTNAME_SIZE];
static size_t s_blocked_hostname_count = 0;

// MQTT 任务写规则，lwIP 线程读取规则。
static portMUX_TYPE s_rule_lock = portMUX_INITIALIZER_UNLOCKED;

// 仅由 lwIP TCP/IP 线程访问，不需要跨任务锁。
static tls_flow_t s_tls_flows[ACCESS_FILTER_TLS_FLOW_CAPACITY];


// 判断当前是否存在需要执行 SNI 检查的 hostname 规则。
static bool has_hostname_rules(void)
{
    bool has_rules = false;

    portENTER_CRITICAL(&s_rule_lock);
    has_rules = s_blocked_hostname_count > 0;
    portEXIT_CRITICAL(&s_rule_lock);

    return has_rules;
}

// 从可能由多个 pbuf 组成的数据包中安全复制指定区域。
// 不能假设 IPv4、TCP 或 UDP 头一定完整位于 packet->payload。
static bool copy_packet_bytes(const struct pbuf *packet, u16_t offset, void *output, u16_t length)
{
    if (packet == NULL || output == NULL)
    {
        return false;
    }

    if (offset > packet->tot_len || length > packet->tot_len - offset)
    {
        return false;
    }

    return pbuf_copy_partial(packet, output, length, offset) == length;
}

static bool ipv4_checksum_valid(const struct pbuf *packet, u16_t header_length)
{
    uint8_t header[IP_HLEN_MAX];

    if (!copy_packet_bytes(packet, 0, header, header_length))
    {
        return false;
    }

    return inet_chksum(header, header_length) == 0;
}

static bool tcp_checksum_valid(struct pbuf *packet, u16_t ip_header_length, u16_t tcp_length, const struct ip_hdr *ip_header)
{
    if (ip_header_length > packet->len || pbuf_remove_header(packet, ip_header_length) != 0)
    {
        return false;
    }

    ip4_addr_t source = {.addr = ip_header->src.addr};
    ip4_addr_t destination = {.addr = ip_header->dest.addr};

    u16_t checksum = inet_chksum_pseudo_partial( packet, IP_PROTO_TCP, tcp_length, tcp_length, &source, &destination);

    // 必须恢复原始 pbuf，否则 lwIP 后续看不到 IPv4 头。
    bool restored = pbuf_add_header(packet, ip_header_length) == 0;

    return restored && checksum == 0;
}

// 钩子消费数据包后必须负责释放 pbuf。
static int discard_packet(struct pbuf *packet)
{
    pbuf_free(packet);
    return 1;
}

static bool is_ascii_letter_or_digit(unsigned char value)
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9');
}

static esp_err_t normalize_hostname(const char *input, char output[ACCESS_FILTER_HOSTNAME_SIZE])
{
    if (input == NULL || output == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t length = strlen(input);

    // DNS 绝对域名允许一个结尾点，规则内部统一移除。
    if (length > 0 && input[length - 1] == '.')
    {
        length--;
    }

    if (length == 0 || length > 253 || length >= ACCESS_FILTER_HOSTNAME_SIZE)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t label_length = 0;

    for (size_t i = 0; i < length; i++)
    {
        unsigned char value = (unsigned char)input[i];

        if (value == '.')
        {
            // 不允许空标签或以连字符结尾的标签。
            if (label_length == 0 || output[i - 1] == '-')
            {
                return ESP_ERR_INVALID_ARG;
            }

            output[i] = '.';
            label_length = 0;
            continue;
        }

        if (!is_ascii_letter_or_digit(value) && value != '-')
        {
            return ESP_ERR_INVALID_ARG;
        }

        // DNS 标签不能以连字符开始。
        if (label_length == 0 && value == '-')
        {
            return ESP_ERR_INVALID_ARG;
        }

        label_length++;

        if (label_length > 63)
        {
            return ESP_ERR_INVALID_ARG;
        }

        output[i] = (char)tolower(value);
    }

    if (label_length == 0 || output[length - 1] == '-')
    {
        return ESP_ERR_INVALID_ARG;
    }

    output[length] = '\0';
    return ESP_OK;
}

bool access_filter_is_hostname_blocked(const char *hostname)
{
    // 没有域名规则时不能影响普通 DNS。
    if (!has_hostname_rules())
    {
        return false;
    }

    char normalized_hostname[ACCESS_FILTER_HOSTNAME_SIZE] = {0};

    // 规则启用后，非法 hostname 不能用于绕过。
    if (normalize_hostname(hostname, normalized_hostname) != ESP_OK)
    {
        return true;
    }

    size_t hostname_length = strlen(normalized_hostname);
    bool blocked = false;

    portENTER_CRITICAL(&s_rule_lock);

    for (size_t i = 0; i < s_blocked_hostname_count; i++)
    {
        const char *rule = s_blocked_hostnames[i];
        size_t rule_length = strlen(rule);

        bool exact_match = hostname_length == rule_length && memcmp(normalized_hostname, rule, rule_length) == 0;

        bool subdomain_match = hostname_length > rule_length && normalized_hostname[hostname_length - rule_length - 1] == '.' && memcmp(normalized_hostname + hostname_length - rule_length, rule, rule_length) == 0;

        if (exact_match || subdomain_match)
        {
            blocked = true;
            break;
        }
    }

    portEXIT_CRITICAL(&s_rule_lock);
    return blocked;
}
// 将 TLS 解析结果转换为该 TCP 流的最终策略。
static void apply_tls_parse_result(tls_flow_t *flow, tls_sni_parse_result_t result, const char *parsed_hostname)
{
    if (flow == NULL || result == TLS_SNI_PARSE_NEED_MORE)
    {
        return;
    }

    // ECH 隐藏真实 hostname，存在规则时必须 fail closed。
    if (flow->parser.ech_present || result == TLS_SNI_PARSE_NO_SNI || result == TLS_SNI_PARSE_INVALID)
    {
        flow->state = TLS_FLOW_BLOCK;
        return;
    }

    if (result != TLS_SNI_PARSE_FOUND)
    {
        flow->state = TLS_FLOW_BLOCK;
        return;
    }

    flow->state = access_filter_is_hostname_blocked(parsed_hostname) ? TLS_FLOW_BLOCK : TLS_FLOW_ALLOW;
}

// 将链式 pbuf 中连续的新 TCP payload 分块提交给 TLS 解析器。
static void feed_tls_stream(tls_flow_t *flow, const struct pbuf *packet, u16_t payload_offset, u16_t payload_length)
{
    uint8_t chunk[ACCESS_FILTER_TLS_FEED_CHUNK_SIZE];

    while (payload_length > 0 && flow->state == TLS_FLOW_INSPECTING)
    {
        if (flow->inspected_bytes >= ACCESS_FILTER_TLS_STREAM_LIMIT)
        {
            flow->state = TLS_FLOW_BLOCK;
            return;
        }

        size_t budget = ACCESS_FILTER_TLS_STREAM_LIMIT - flow->inspected_bytes;

        u16_t chunk_length = payload_length;

        if (chunk_length > sizeof(chunk))
        {
            chunk_length = sizeof(chunk);
        }

        if (chunk_length > budget)
        {
            chunk_length = (u16_t)budget;
        }

        if (chunk_length == 0 || !copy_packet_bytes( packet, payload_offset, chunk, chunk_length))
        {
            flow->state = TLS_FLOW_BLOCK;
            return;
        }

        char parsed_hostname[TLS_SNI_HOSTNAME_SIZE] = {0};

        tls_sni_parse_result_t result = tls_sni_parser_feed( &flow->parser, chunk, chunk_length, parsed_hostname, sizeof(parsed_hostname));

        flow->inspected_bytes += chunk_length;
        payload_offset += chunk_length;
        payload_length -= chunk_length;

        apply_tls_parse_result(flow, result, parsed_hostname);
    }

    // 达到流预算仍未得到完整 ClientHello，按不可解析处理。
    if (flow->state == TLS_FLOW_INSPECTING &&
        flow->inspected_bytes >= ACCESS_FILTER_TLS_STREAM_LIMIT)
    {
        flow->state = TLS_FLOW_BLOCK;
    }
}

static void clear_tls_flow(tls_flow_t *flow)
{
    if (flow != NULL)
    {
        memset(flow, 0, sizeof(*flow));
    }
}

static bool inspect_tls_tcp_segment(const struct pbuf *packet, const struct ip_hdr *ip_header, const struct tcp_hdr *tcp_header, u16_t ip_header_length, u16_t tcp_header_length, u16_t ip_total_length)
{
    uint32_t now_ms = sys_now();
    expire_tls_flows(now_ms);

    uint32_t source_ipv4 = ip_header->src.addr;
    uint32_t destination_ipv4 = ip_header->dest.addr;
    uint16_t source_port = lwip_ntohs(tcp_header->src);
    uint16_t destination_port = lwip_ntohs(tcp_header->dest);
    uint32_t sequence = lwip_ntohl(tcp_header->seqno);
    uint8_t flags = TCPH_FLAGS(tcp_header);

    u16_t payload_offset = ip_header_length + tcp_header_length;
    u16_t payload_length = ip_total_length - payload_offset;
    uint32_t payload_sequence = sequence + ((flags & TCP_SYN) != 0 ? 1U : 0U);

    tls_flow_t *flow = find_tls_flow(source_ipv4, destination_ipv4, source_port, destination_port);

    // RST 只负责终止连接，不携带可继续使用的 TLS 数据。
    if ((flags & TCP_RST) != 0)
    {
        clear_tls_flow(flow);
        return false;
    }

    if ((flags & TCP_SYN) != 0)
    {
        bool simple_syn_retransmission = flow != NULL && flow->state == TLS_FLOW_INSPECTING && flow->saw_syn && flow->syn_sequence == sequence && flow->next_sequence == sequence + 1U && payload_length == 0;

        if (!simple_syn_retransmission)
        {
            clear_tls_flow(flow);

            flow = create_tls_flow( source_ipv4, destination_ipv4, source_port, destination_port, now_ms);

            if (flow == NULL)
            {
                return true;
            }

            flow->saw_syn = true;
            flow->syn_sequence = sequence;
            flow->next_sequence = sequence + 1U;
        }
    }

    // 支持规则安装前已经建立、当前首次被观察到的 TLS 流。
    if (flow == NULL && payload_length > 0)
    {
        flow = create_tls_flow(source_ipv4, destination_ipv4, source_port, destination_port, now_ms);

        if (flow == NULL)
        {
            return true;
        }

        flow->next_sequence = payload_sequence;
    }

    // 未知流的纯 ACK/FIN 不携带数据，不需要占用流表。
    if (flow == NULL)
    {
        return false;
    }

    flow->last_seen_ms = now_ms;

    if (flow->state == TLS_FLOW_ALLOW || flow->state == TLS_FLOW_BLOCK)
    {
        bool blocked = flow->state == TLS_FLOW_BLOCK;

        if ((flags & TCP_FIN) != 0)
        {
            clear_tls_flow(flow);
        }

        return blocked;
    }

    if (payload_length > 0)
    {
        int32_t sequence_delta = (int32_t)(payload_sequence - flow->next_sequence);

        if (sequence_delta > 0)
        {
            // 出现缺口或乱序，不猜测缺失内容。
            flow->state = TLS_FLOW_BLOCK;
        }
        else
        {
            uint32_t overlap = flow->next_sequence - payload_sequence;

            if (overlap < payload_length)
            {
                u16_t new_length = payload_length - (u16_t)overlap;

                u16_t new_offset = payload_offset + (u16_t)overlap;

                feed_tls_stream(flow, packet, new_offset, new_length);
                flow->next_sequence += new_length;
            }
            // overlap >= payload_length 表示完整重传，不重复喂给解析器。
        }
    }

    // ClientHello 尚未完成就关闭连接，按不可解析处理。
    if ((flags & TCP_FIN) != 0 && flow->state == TLS_FLOW_INSPECTING)
    {
        flow->state = TLS_FLOW_BLOCK;
    }

    bool blocked = flow->state == TLS_FLOW_BLOCK;

    if ((flags & TCP_FIN) != 0)
    {
        clear_tls_flow(flow);
    }

    return blocked;
}

static void expire_tls_flows(uint32_t now_ms)
{
    for (size_t i = 0; i < ACCESS_FILTER_TLS_FLOW_CAPACITY; i++)
    {
        tls_flow_t *flow = &s_tls_flows[i];

        if (flow->state == TLS_FLOW_FREE)
        {
            continue;
        }

        uint32_t timeout_ms = flow->state == TLS_FLOW_INSPECTING ? ACCESS_FILTER_TLS_INSPECTION_TIMEOUT_MS : ACCESS_FILTER_TLS_DECISION_TIMEOUT_MS;

        // 无符号减法可以正确处理 sys_now() 回绕。
        if ((uint32_t)(now_ms - flow->last_seen_ms) >= timeout_ms)
        {
            clear_tls_flow(flow);
        }
    }
}

static tls_flow_t *find_tls_flow(uint32_t source_ipv4, uint32_t destination_ipv4, uint16_t source_port, uint16_t destination_port)
{
    for (size_t i = 0; i < ACCESS_FILTER_TLS_FLOW_CAPACITY; i++)
    {
        tls_flow_t *flow = &s_tls_flows[i];

        if (flow->state != TLS_FLOW_FREE &&
            flow->source_ipv4 == source_ipv4 &&
            flow->destination_ipv4 == destination_ipv4 &&
            flow->source_port == source_port &&
            flow->destination_port == destination_port)
        {
            return flow;
        }
    }

    return NULL;
}

static tls_flow_t *create_tls_flow(uint32_t source_ipv4, uint32_t destination_ipv4, uint16_t source_port, uint16_t destination_port, uint32_t now_ms)
{
    for (size_t i = 0; i < ACCESS_FILTER_TLS_FLOW_CAPACITY; i++)
    {
        tls_flow_t *flow = &s_tls_flows[i];

        if (flow->state == TLS_FLOW_FREE)
        {
            clear_tls_flow(flow);
            flow->state = TLS_FLOW_INSPECTING;
            flow->source_ipv4 = source_ipv4;
            flow->destination_ipv4 = destination_ipv4;
            flow->source_port = source_port;
            flow->destination_port = destination_port;
            flow->last_seen_ms = now_ms;
            tls_sni_parser_init(&flow->parser);
            return flow;
        }
    }

    // 调用方必须对容量耗尽采用 fail closed。
    return NULL;
}

static bool is_destination_ipv4_blocked(uint32_t destination_ipv4)
{
    size_t count = 0;

    // 表项发布后不再修改，只需在锁内取得已发布数量。
    portENTER_CRITICAL(&s_rule_lock);
    count = s_blocked_destination_count;
    portEXIT_CRITICAL(&s_rule_lock);

    for (size_t i = 0; i < count; i++)
    {
        if (s_blocked_destination_ipv4[i] == destination_ipv4)
        {
            return true;
        }
    }

    return false;
}

esp_err_t access_filter_block_traffic(const char *destination_ipv4, const char *sni)
{
    if (destination_ipv4 == NULL || destination_ipv4[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_softap_netif == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    ip4_addr_t candidate = {0};

    if (ip4addr_aton(destination_ipv4, &candidate) == 0 || candidate.addr == 0 || ip4_addr_ismulticast(&candidate) || ip4_addr_isbroadcast(&candidate, s_softap_netif))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (ip4_addr_cmp( &candidate, netif_ip4_addr(s_softap_netif)) || (s_portal_server_enabled && ip4_addr_cmp( &candidate, &s_portal_server_ipv4)))
    {
        return ESP_ERR_INVALID_ARG;
    }

    bool has_hostname = sni != NULL && sni[0] != '\0';
    char normalized_hostname[ACCESS_FILTER_HOSTNAME_SIZE] = {0};

    if (has_hostname)
    {
        esp_err_t normalize_err = normalize_hostname(sni, normalized_hostname);

        if (normalize_err != ESP_OK)
        {
            return normalize_err;
        }
    }

    bool ipv4_exists = false;
    bool hostname_exists = !has_hostname;

    portENTER_CRITICAL(&s_rule_lock);

    for (size_t i = 0; i < s_blocked_destination_count; i++)
    {
        if (s_blocked_destination_ipv4[i] == candidate.addr)
        {
            ipv4_exists = true;
            break;
        }
    }

    if (has_hostname)
    {
        for (size_t i = 0; i < s_blocked_hostname_count; i++)
        {
            if (strcmp( s_blocked_hostnames[i], normalized_hostname) == 0)
            {
                hostname_exists = true;
                break;
            }
        }
    }

    bool ipv4_full = !ipv4_exists && s_blocked_destination_count >= ACCESS_FILTER_BLOCKED_IPV4_CAPACITY;

    bool hostname_full = !hostname_exists && s_blocked_hostname_count >= ACCESS_FILTER_BLOCKED_HOSTNAME_CAPACITY;

    // 在写入任意表项前检查两个表，保证原子性。
    if (ipv4_full || hostname_full)
    {
        portEXIT_CRITICAL(&s_rule_lock);
        return ESP_ERR_NO_MEM;
    }

    if (!ipv4_exists)
    {
        s_blocked_destination_ipv4[s_blocked_destination_count++] = candidate.addr;
    }

    if (!hostname_exists)
    {
        size_t hostname_length = strlen(normalized_hostname) + 1;

        memcpy(s_blocked_hostnames[s_blocked_hostname_count], normalized_hostname, hostname_length);

        s_blocked_hostname_count++;
    }

    portEXIT_CRITICAL(&s_rule_lock);

    ESP_LOGW(TAG, "Traffic block installed, dstIp=%s, sni=%s", destination_ipv4, has_hostname ? normalized_hostname : "-");

    return ESP_OK;
}

// lwIP 收到 SoftAP 客户端发出的 IPv4 数据包后先进入此函数。
int access_filter_lwip_ip4_input( struct pbuf *packet, struct netif *input_netif)
{
    if (packet == NULL || input_netif == NULL || s_softap_netif == NULL)
    {
        return 0;
    }

    // 只处理从 SoftAP 客户端进入的数据包。
    // ESP32 自身的 STA、MQTT 等流量不能被过滤。
    if (input_netif != s_softap_netif)
    {
        return 0;
    }

    struct ip_hdr ip_header = {0};

    // pbuf 可能是链表，不能直接把 packet->payload 强转成 IPv4 头。
    if (!copy_packet_bytes( packet, 0, &ip_header, sizeof(ip_header)))
    {
        // lwIP 会继续执行自身的畸形包检查。
        return 0;
    }

    u16_t ip_header_length = IPH_HL_BYTES(&ip_header);
    u16_t ip_total_length = lwip_ntohs(IPH_LEN(&ip_header));

    // 检查 IPv4 版本、头长度、总长度以及 pbuf 实际长度。
    if (IPH_V(&ip_header) != 4 || ip_header_length < IP_HLEN || ip_header_length > IP_HLEN_MAX || ip_total_length < ip_header_length || ip_total_length > packet->tot_len)
    {
        return 0;
    }

    uint32_t source_ip = ip_header.src.addr;

    ip4_addr_t destination_ip = {
        .addr = ip_header.dest.addr
    };

    // SoftAP 本机通信必须保留，Portal 和 DNS 服务依赖该路径。
    if (ip4_addr_cmp( &destination_ip, netif_ip4_addr(input_netif)))
    {
        return 0;
    }

    // DHCP 等广播流量必须保留。
    if (ip4_addr_isbroadcast( &destination_ip, input_netif))
    {
        return 0;
    }

    // 本地组播不属于通过 NAPT 访问外网的普通单播流量。
    if (ip4_addr_ismulticast(&destination_ip))
    {
        return 0;
    }

    // 外部 Portal 服务器属于显式白名单。
    if (s_portal_server_enabled && ip4_addr_cmp( &destination_ip, &s_portal_server_ipv4))
    {
        return 0;
    }

    // IPv4 黑名单优先于客户端授权状态。
    if (is_destination_ipv4_blocked(destination_ip.addr))
    {
        return discard_packet(packet);
    }

    // 未授权客户端不能占用 TLS 流表。
    if (!client_access_can_forward_ipv4(source_ip))
    {
        return discard_packet(packet);
    }

    if (!has_hostname_rules())
    {
        return 0;
    }

    u16_t fragment_field = lwip_ntohs(IPH_OFFSET(&ip_header));
    bool is_fragmented = (fragment_field & (IP_MF | IP_OFFMASK)) != 0;

    if (IPH_PROTO(&ip_header) == IP_PROTO_UDP)
    {
        // 无法从非首分片确认 UDP 端口，因此严格拒绝。
        if (is_fragmented || ip_total_length < ip_header_length + UDP_HLEN)
        {
            return discard_packet(packet);
        }

        struct udp_hdr udp_header = {0};

        if (!copy_packet_bytes(packet, ip_header_length, &udp_header, sizeof(udp_header)))
        {
            return discard_packet(packet);
        }

        u16_t udp_length = lwip_ntohs(udp_header.len);
        u16_t ip_payload_length = ip_total_length - ip_header_length;

        if (udp_length < UDP_HLEN || udp_length > ip_payload_length)
        {
            return discard_packet(packet);
        }

        return lwip_ntohs(udp_header.dest) == 443 ? discard_packet(packet) : 0;
    }

    if (IPH_PROTO(&ip_header) != IP_PROTO_TCP)
    {
        return 0;
    }

    // hostname 规则存在时不接受分片 TCP，防止绕过端口和序列号检查。
    if (is_fragmented || ip_total_length < ip_header_length + TCP_HLEN)
    {
        return discard_packet(packet);
    }

    struct tcp_hdr tcp_header = {0};

    if (!copy_packet_bytes(packet, ip_header_length, &tcp_header, sizeof(tcp_header)))
    {
        return discard_packet(packet);
    }

    u16_t tcp_header_length = TCPH_HDRLEN_BYTES(&tcp_header);

    if (tcp_header_length < TCP_HLEN || tcp_header_length > TCP_HLEN + TCP_MAX_OPTION_BYTES || ip_total_length < ip_header_length + tcp_header_length)
    {
        return discard_packet(packet);
    }

    // SNI 只处理标准 HTTPS；其他 TCP 端口保持原有转发行为。
    if (lwip_ntohs(tcp_header.dest) != 443)
    {
        return 0;
    }

    u16_t tcp_length = ip_total_length - ip_header_length;

    // 钩子早于 lwIP 自身校验，必须先验证再更新流状态。
    if (ip_header_length > packet->len || !ipv4_checksum_valid(packet, ip_header_length) || !tcp_checksum_valid( packet, ip_header_length, tcp_length, &ip_header))
    {
        return discard_packet(packet);
    }

    if (inspect_tls_tcp_segment(packet, &ip_header, &tcp_header, ip_header_length, tcp_header_length, ip_total_length))
    {
        return discard_packet(packet);
    }

    return 0;
}

esp_err_t access_filter_start(const access_filter_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_softap_netif != NULL)
    {
        return ESP_OK;
    }

    if (config->portal_server_ipv4 == NULL)
    {
        s_portal_server_enabled = false;
        s_portal_server_ipv4.addr = 0;
    }
    else
    {
        // 非NULL但内容为空，属于配置错误
        if (config->portal_server_ipv4[0] == '\0')
        {
            return ESP_ERR_INVALID_ARG;
        }

        // 将"192.168.137.1"这样的文本转换为底层IPv4值。
        // 只在启动时转换一次，不能让每一个数据包都重新解析字符串。
        if (ip4addr_aton(config->portal_server_ipv4,&s_portal_server_ipv4) == 0)
        {
            return ESP_ERR_INVALID_ARG;
        }

        s_portal_server_enabled = true;
    }

    // 根据默认SoftAP
    // 接口标识找到ESP-NETIF对象
    esp_netif_t *softap_esp_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

    if (softap_esp_netif == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    // lwIP钩子收到的是struct netif，因此需要取得ESP-NETIF底层实现
    s_softap_netif = (struct netif *)esp_netif_get_netif_impl(softap_esp_netif);

    if (s_softap_netif == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "SoftAP IPv4 access filter started");
    return ESP_OK;
}