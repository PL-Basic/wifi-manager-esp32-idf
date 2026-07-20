#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "portal_dns.h"
#include "client_access.h"

// DNS标准端口
#define PORTAL_DNS_PORT 53
// 普通UDP DNS报文最大长度通常为512字节
#define PORTAL_DNS_PACKET_SIZE 512
// DNS协议固定头部长度
#define DNS_HEADER_SIZE 12
// 当前只处理IPv4地址查询
#define DNS_TYPE_A 1
// Internet地址类型
#define DNS_CLASS_IN 1
// DNS 响应标志
#define DNS_FLAG_RESPONSE 0x8000
// DNS权威回答标志。
#define DNS_FLAG_AUTHORITATIVE 0x0400
// 客户端希望递归查询。
#define DNS_FLAG_RECURSION_DESIRED 0x0100
// DNS服务器支持递归查询。
#define DNS_FLAG_RECURSION_AVAILABLE 0x0080
// DNS操作类型掩码。
#define DNS_OPCODE_MASK 0x7800
// DNS服务器解析失败。
#define DNS_RCODE_SERVER_FAILURE 2
// DNS应答中的域名使用压缩指针，指回请求中偏移12的位置。
#define DNS_NAME_POINTER 0xC00C
// Portal假地址只缓存5秒。
// 认证完成后，旧的192.168.4.1解析结果可以较快失效。
#define PORTAL_DNS_TTL_SECONDS 5

static const char *TAG = "portal_dns";

// 保存DNS任务，防止重复启动
static TaskHandle_t s_dns_task = NULL;
// 保存监听UDP 53端口的socket
static int s_dns_socket = -1;
// 保存SoftAP本机IPv4地址
// 未认证客户端查询任何A记录时，都返回这个地址
static uint32_t s_portal_ipv4 = 0;
// DNS域名最长允许保存255个字符和一个结束符
static char s_external_portal_domain[256] = {0};
// 已转换为网络字节序的外部Portal服务器IPv4
static uint32_t s_external_portal_ipv4 = 0;
// false表示当前未启用外部Portal域名例外
static bool s_external_portal_enabled = false;
// DHCP选项114需要长期保存Portal地址
static char s_portal_uri[32] = {0};

// DNS固定头部
typedef struct __attribute__((packed))
{
    uint16_t id;
    uint16_t flags;
    uint16_t question_count;
    uint16_t answer_count;
    uint16_t authority_count;
    uint16_t additional_count;
} dns_header_t;

// DNS IPv4回答
typedef struct __attribute__((packed))
{
    uint16_t name_pointer;
    uint16_t type;
    uint16_t class_code;
    uint32_t ttl;
    uint16_t address_length;
    uint32_t ipv4;
} dns_answer_a_t;

// 从DNS报文中读取域名、查询类型和查询结束位置
static esp_err_t parse_dns_question(const uint8_t *packet, size_t packet_length, char *domain_name, size_t domain_name_size, size_t *question_end, uint16_t *query_type, uint16_t *query_class)
{
    if (packet == NULL || domain_name == NULL || domain_name_size == 0 || question_end == NULL || query_type == NULL || query_class == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (packet_length < DNS_HEADER_SIZE)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    const dns_header_t *header = (const dns_header_t *)packet;
    uint16_t request_flags = ntohs(header->flags);

    // 这里只处理标准DNS请求，不处理DNS响应或其他操作类型
    if ((request_flags & DNS_FLAG_RESPONSE) != 0 || (request_flags & DNS_OPCODE_MASK) != 0)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    // 当前MVP每次只处理一个DNS问题
    if (ntohs(header->question_count) != 1)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    size_t packet_offset = DNS_HEADER_SIZE;
    size_t name_length = 0;

    while (packet_offset < packet_length)
    {
        uint8_t label_length = packet[packet_offset];
        packet_offset++;

        // 长度为0表示域名读取结束
        if (label_length == 0)
        {
            break;
        }

        // DNS压缩指针的高两位为11
        // 普通客户端请求一般不会在问题区域使用压缩
        // 当前阶段不处理这种复杂报文
        if ((label_length & 0xC0) != 0)
        {
            return ESP_ERR_NOT_SUPPORTED;
        }
        
        // 单个DNS标签最长63字节。
        if (label_length > 63 || packet_offset + label_length > packet_length)
        {
            return ESP_ERR_INVALID_SIZE;
        }

        // 多段域名之间补充点号
        if (name_length > 0)
        {
            if (name_length + 1 >= domain_name_size)
            {
                return ESP_ERR_INVALID_SIZE;
            }
            
            domain_name[name_length] = '.';
            name_length++;
        }

        if (name_length + label_length >= domain_name_size)
        {
            return ESP_ERR_INVALID_SIZE;
        }

        memcpy(domain_name + name_length, packet + packet_offset, label_length);
        
        name_length += label_length;
        packet_offset += label_length;
    }

    // 如果没有遇到域名结束标志，说明报文不完整。
    if (packet_offset > packet_length)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    domain_name[name_length] = '\0';
    
    // 域名后面还需要有2字节type和2字节class
    if (packet_offset + 4 > packet_length)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    uint16_t network_value = 0;

    memcpy(&network_value, packet + packet_offset, sizeof(network_value));
    *query_type = ntohs(network_value);
    packet_offset += sizeof(network_value);

    memcpy(&network_value, packet + packet_offset, sizeof(network_value));
    *query_class = ntohs(network_value);
    packet_offset += sizeof(network_value);

    *question_end = packet_offset;
    return ESP_OK;
}    

// 已认证客户端需要得到域名的真实上游IPv4地址
static esp_err_t resolve_authorized_ipv4(const char *domain_name, uint32_t *resolve_ipv4)
{
    if (domain_name == NULL || domain_name[0] == '\0' || resolve_ipv4 == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM
    };

    struct addrinfo *address_result = NULL;

    // getaddrinfo()使用ESP32当前STA获得的上游DNS，
    // 将域名解析成真实IPv4地址。
    int lookup_result = getaddrinfo(domain_name, NULL, &hints, &address_result);

    if (lookup_result != 0 || address_result == NULL)
    {
        return ESP_FAIL;
    }

    const struct sockaddr_in *ipv4_address = (const struct sockaddr_in *)address_result->ai_addr;
    
    *resolve_ipv4 = ipv4_address->sin_addr.s_addr;

    // getaddrinfo()内部申请了地址结果，使用后必须释放。
    freeaddrinfo(address_result);

    if (*resolve_ipv4 == 0)
    {
        return ESP_FAIL;
    }

    return ESP_OK;
}


// 根据客户端认证状态构造DNS响应
static int build_dns_response(const uint8_t *request, size_t request_length, bool client_authorized, uint8_t *response, size_t response_capacity)
{
    char domain_name[256] = {0};
    size_t question_end = 0;
    uint16_t query_type = 0;
    uint16_t query_class = 0;

    esp_err_t err = parse_dns_question(request, request_length, domain_name, sizeof(domain_name), &question_end, &query_type, &query_class);

    if (err != ESP_OK)
    {
        return -1;
    }

    if (question_end > response_capacity)
    {
        return -1;
    }

    // 只复制DNS头部和问题区域。
    // 客户端可能带有EDNS附加记录，当前响应不需要原样保留。
    memcpy(response, request, question_end);

    const dns_header_t *request_header = (const dns_header_t *) request;
    dns_header_t *response_header = (dns_header_t *)response;

    uint16_t request_flags = ntohs(request_header->flags);
    uint16_t response_flags = DNS_FLAG_RESPONSE | (request_flags & DNS_FLAG_RECURSION_DESIRED);

    if (client_authorized)
    {
        // 已认证客户端允许ESP32使用上游DNS递归解析
        response_flags |= DNS_FLAG_RECURSION_AVAILABLE;
    }
    else
    {
        // 未认证客户端收到的是Portal本地权威回答
        response_flags |= DNS_FLAG_AUTHORITATIVE;
    }
    
    bool has_ipv4_answer = false;
    uint32_t answer_ipv4 = 0;
    uint16_t response_code = 0;
    
    if (query_type == DNS_TYPE_A && query_class == DNS_CLASS_IN)
    {
        if (!client_authorized)
        {
            // Portal域名必须指向外部服务器，不能继续指向ESP32本机
            if (s_external_portal_enabled && strcasecmp(domain_name, s_external_portal_domain) == 0)
            {
                answer_ipv4 = s_external_portal_ipv4;

                ESP_LOGI(TAG, "External Portal DNS matched, domain=%s", domain_name);
            }
            else
            {
                // 其他未认证域名继续指向ESP32本地Portal
                answer_ipv4 = s_portal_ipv4;
            }

            has_ipv4_answer = true;
        }
        else
        {
            // 已认证客户端：解析并返回真实外网IPv4地址
            err = resolve_authorized_ipv4(domain_name, &answer_ipv4);
        
            if (err == ESP_OK)
            {
                has_ipv4_answer = true;
            }
            else
            {
                response_code = DNS_RCODE_SERVER_FAILURE;
            }   
        }
    }

    response_flags |= response_code;

    response_header->flags = htons(response_flags);
    response_header->question_count = htons(1);
    response_header->answer_count = htons(has_ipv4_answer ? 1 : 0);
    response_header->authority_count = 0;
    response_header->additional_count = 0;
    
    if (!has_ipv4_answer)
    {
        return (int)question_end;
    }

    if (question_end + sizeof(dns_answer_a_t) > response_capacity)
    {
        return -1;
    }
    
    dns_answer_a_t *answer = (dns_answer_a_t *)(response + question_end);

    // 0xC00C表示回答中的域名引用请求报文偏移12处的域名
    answer->name_pointer = htons(DNS_NAME_POINTER);
    answer->type = htons(DNS_TYPE_A);
    answer->class_code = htons(DNS_CLASS_IN);
    answer->ttl = htonl(PORTAL_DNS_TTL_SECONDS);
    answer->address_length = htons(sizeof(answer->ipv4));

    // ESP-NETIF和socket中的IPv4本身就是网络字节序，
    // 因此这里不需要再次htonl()。
    answer->ipv4 = answer_ipv4;
    
    return (int)(question_end + sizeof(dns_answer_a_t));
}

// 独立任务持续接收客户端DNS查询
static void portal_dns_task(void *argument)
{
    (void)argument;

    uint8_t request_buffer[PORTAL_DNS_PACKET_SIZE];
    uint8_t response_buffer[PORTAL_DNS_PACKET_SIZE];

    while (s_dns_socket >= 0)
    {
        struct sockaddr_in client_address = {0};
        socklen_t client_address_length = sizeof(client_address);
    
        int received_length = recvfrom(s_dns_socket, request_buffer, sizeof(request_buffer), 0, (struct sockaddr *)&client_address, &client_address_length);
        
        if (received_length < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            
            ESP_LOGE(TAG, "DNS recvfrom failed, errno=%d", errno);
            break;
        }

        // DNS请求的源IP就是当前发起查询的SoftAP客户端IP。
        // 使用client_access统一判断它是否已获得外网权限。
        bool client_authorized = client_access_can_forward_ipv4(client_address.sin_addr.s_addr);
    
        int response_length = build_dns_response(request_buffer, (size_t)received_length, client_authorized, response_buffer, sizeof(response_buffer));
        
        if (response_length <= 0)
        {
            // 非标准或暂不支持的DNS报文直接忽略。
            continue;
        }

        int sent_length = sendto(s_dns_socket, response_buffer, (size_t)response_length, 0, (struct sockaddr *)&client_address, client_address_length);
        
        if (sent_length < 0)
        {
            ESP_LOGE(TAG, "DNS sendto failed, errno=%d", errno);
        }
    }    

    if (s_dns_socket >= 0)
    {
        close(s_dns_socket);
        s_dns_socket = -1;
    }

    s_dns_task = NULL;
    
    vTaskDelete(NULL);
}

// 配置SoftAp的DHCP服务
// 手机连接热点并获取IP地址时，DHCP除了分配IP，
// 还会告诉手机：
// 1. DNS服务器是ESP32的SoftAP地址；
// 2. 当前网络的Captive Portal地址是什么。
static esp_err_t configure_softap_dhcp(esp_netif_t *ap_netif, const esp_netif_ip_info_t *ap_ip_info)
{
/*生成Portal URL
→ 查询DHCP状态
→ 停止DHCP
→ 开启DNS地址下发
→ 设置DNS地址为ESP32
→ 设置Portal URI
→ 恢复DHCP*/
    if (ap_netif == NULL || ap_ip_info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // 根据实际SoftAP IP生成Portal地址
    int written = snprintf(s_portal_uri, sizeof(s_portal_uri), "http://" IPSTR "/", IP2STR(&ap_ip_info->ip));
    
    if (written < 0 || (size_t)written >= sizeof(s_portal_uri))
    {
        return ESP_ERR_NO_MEM;
    }

    esp_netif_dhcp_status_t dhcp_status = ESP_NETIF_DHCP_INIT;

    esp_err_t err = esp_netif_dhcps_get_status(ap_netif, &dhcp_status);

    if (err != ESP_OK)
    {
        return err;
    }

    // ESP-IDF要求修改DHCP Server选项前先停止DHCP服务
    if (dhcp_status == ESP_NETIF_DHCP_STARTED)
    {
        err = esp_netif_dhcps_stop(ap_netif);

        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Stop SoftAP DHCP failed: %s", esp_err_to_name(err));
            return err;
        }
        
    }
    
    esp_err_t configure_err = ESP_OK;
    const char *failed_step = NULL;
    
    // 非零表示DHCP需要向客户端提供DNS服务器选项
    uint8_t offer_dns = 1;

    configure_err = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns));

    if (configure_err != ESP_OK)
    {
        failed_step = "enable DNS offer";
    }

    // DHCP提供给手机的DNS地址就是ESP32 SoftAP本机地址
    esp_netif_dns_info_t dns_info = {0};

    dns_info.ip.type = IPADDR_TYPE_V4;
    dns_info.ip.u_addr.ip4.addr = ap_ip_info->ip.addr;
    
    if (configure_err == ESP_OK)
    {
        configure_err = esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns_info);
        if (configure_err != ESP_OK)
        {
            failed_step = "set DNS address";
        }
    }

    // DHCP选项114用于声明Captive Portal地址
    // 支持该选项的手机系统可以直接知道：
    // 当前网络需要打开这个认证页面。
    if (configure_err == ESP_OK)
    {
        configure_err = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, s_portal_uri, strlen(s_portal_uri));
        if (configure_err != ESP_OK)
        {
            failed_step = "set captive portal URI";
        }
    }

    // 无论前面的某个配置是否失败，都要尝试恢复DHCP服务。
    // 否则DHCP停止后，新客户端将无法获得IP地址。
    esp_err_t start_err = esp_netif_dhcps_start(ap_netif);

    if (start_err != ESP_OK && start_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
    {
        ESP_LOGE(TAG, "Restart SoftAP DHCP failed: %s", esp_err_to_name(start_err));
        return start_err;
    }

    if (configure_err != ESP_OK)
    {
        ESP_LOGE(TAG, "Configure SoftAP DHCP failed at %s: %s", failed_step, esp_err_to_name(configure_err));
        return configure_err;
    }

    ESP_LOGI(TAG, "SoftAP DHCP configured, DNS=" IPSTR ", portal=%s", IP2STR(&ap_ip_info->ip), s_portal_uri);
    
    return ESP_OK;
}

static esp_err_t configure_external_portal(const char *external_portal_domain, const char *external_portal_ipv4)
{
    // 两个参数都为NULL，表示当前不启用外部Portal域名例外
    if (external_portal_domain == NULL && external_portal_ipv4 == NULL)
    {
        s_external_portal_enabled = false;
        s_external_portal_domain[0] = '\0';
        s_external_portal_ipv4 = 0;

        return ESP_OK;
    }

    // 域名和IPv4必须同时配置，不能只提供其中一个
    if (external_portal_domain == NULL || external_portal_ipv4 == NULL || external_portal_domain[0] == '\0' || external_portal_ipv4[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(s_external_portal_domain, sizeof(s_external_portal_domain), "%s", external_portal_domain);
    if (written < 0 || (size_t)written >= sizeof(s_external_portal_domain))
    {
        return ESP_ERR_NO_MEM;
    }
    
    ip4_addr_t parsed_ipv4 = {0};

    // 将IPv4文本转换为DNS响应需要的底层地址
    if (ip4addr_aton(external_portal_ipv4, &parsed_ipv4) == 0)
    {
        s_external_portal_domain[0] = '\0';
        return ESP_ERR_INVALID_ARG;
    }

    s_external_portal_ipv4 = parsed_ipv4.addr;
    s_external_portal_enabled = true;

    ESP_LOGI(TAG, "External Portal DNS configured, domain=%s, ip=%s", s_external_portal_domain, external_portal_ipv4);

    return ESP_OK;
}


esp_err_t portal_dns_start(const char *external_portal_domain, const char *external_portal_ipv4)
{
    esp_err_t err = configure_external_portal(external_portal_domain, external_portal_ipv4);
    if (err != ESP_OK)
    {
        return err;
    }
    

    if (s_dns_task != NULL || s_dns_socket >= 0)
    {
        return ESP_OK;
    }
    
    // 查找默认SoftAP网络接口
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

    if (ap_netif == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_ip_info_t ap_ip_info = {0};

    err = esp_netif_get_ip_info(ap_netif, &ap_ip_info);

    if (err != ESP_OK)
    {
        return err;
    }

    if (ap_ip_info.ip.addr == 0)
    {
        return ESP_ERR_INVALID_STATE;
    }
    
    s_portal_ipv4 = ap_ip_info.ip.addr;
    
    // 创建IPv4 UDP socket
    int dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (dns_socket < 0)
    {
        ESP_LOGE(TAG, "Create DNS socket failed, errno=%d", errno);

        return ESP_FAIL;
    }

    int reuse_address = 1;

    err = setsockopt(dns_socket, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address));

    if (err < 0)
    {
        ESP_LOGE(TAG, "Set DNS socket option failed, errno=%d", errno);
        
        close(dns_socket);
        return ESP_FAIL;
    }
    
    struct sockaddr_in server_address = {
        .sin_family = AF_INET,
        .sin_port = htons(PORTAL_DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    // 将socket绑定到UDP 53端口。
    err = bind(dns_socket, (struct sockaddr *)&server_address, sizeof(server_address));

    if (err < 0)
    {
        ESP_LOGE(TAG, "Bind DNS socket failed, errno=%d", errno);
        
        close(dns_socket);
        return ESP_FAIL;
    }

    // UDP 53端口准备完成后，再让DHCP通知客户端使用该DNS。
    // 如果DHCP配置失败，关闭刚创建的socket，
    // 不留下半启动的DNS服务。
    err = configure_softap_dhcp(ap_netif, &ap_ip_info);

    if (err != ESP_OK)
    {
        close(dns_socket);
        return err;
    }


    s_dns_socket = dns_socket;

    BaseType_t task_created = xTaskCreate(portal_dns_task, "portal_dns", 4096, NULL, 4, &s_dns_task);
    
    if (task_created != pdPASS)
    {
        close(s_dns_socket);
        s_dns_socket = -1;
        s_dns_task = NULL;

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Portal DNS service started, address=" IPSTR ", UDP port=%d", IP2STR(&ap_ip_info.ip), PORTAL_DNS_PORT);
    return ESP_OK;
}
