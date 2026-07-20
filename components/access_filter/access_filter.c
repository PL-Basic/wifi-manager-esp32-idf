#include <stdint.h>
#include <stdbool.h>

#include "access_filter.h"
#include "client_access.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"

#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ip4.h"

static const char *TAG = "access_filter";

// 保存默认SoftAP对应的lwIP底层网络接口
// 过滤器只处理从这个接口进入的数据包
static struct netif *s_softap_netif = NULL;
// 保存已经转换为网络字节序的外部Portal服务器IPv4地址
static ip4_addr_t s_portal_server_ipv4 = {0};
// false表示当前尚未配置外部Portal服务器白名单
static bool s_portal_server_enabled = false;

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

// lwIP收到IPv4数据包后会先进入这个函数
int access_filter_lwip_ip4_input(struct pbuf *packet, struct netif *input_netif)
{
    if (packet == NULL || input_netif == NULL || s_softap_netif == NULL)
    {
        // 返回0表示本过滤器没有消费数据包，交给lwIP继续处理
        return 0;
    }

    // 只过滤手机进入SoftAP的数据包
    // ESP32自己的STA上游网络和MQTT流量不能被这里拦截
    if (input_netif != s_softap_netif)
    {
        return 0;
    }

    // 数据长度不足以容纳IPv4头时，交给lwIP自身进行合法性检查
    if (packet->len < sizeof(struct ip_hdr))
    {
        return 0;
    }

    const struct ip_hdr *ip_header = (const struct ip_hdr *)packet->payload;

    uint32_t source_ip = ip_header->src.addr;

    ip4_addr_t destination_ip = {
        .addr = ip_header->dest.addr
    };

    // 发往ESP32 SoftAP自身的数据必须保留
    // 后面的Portal网页和DNS服务都依赖本地通信
    if (ip4_addr_cmp(&destination_ip, netif_ip4_addr(input_netif)))
    {
        return 0;
    }

    // 广播流量需要保留，例如DHCP初次获取地址使用广播
    if (ip4_addr_isbroadcast(&destination_ip, input_netif))
    {
        return 0;
    }

    // 本地组播不是需要经过NAPT访问外网的普通单播流量
    if (ip4_addr_ismulticast(&destination_ip))
    {
        return 0;
    }

    if (s_portal_server_enabled && ip4_addr_cmp(&destination_ip, &s_portal_server_ipv4))
    {
        return 0;
    }
    
    // 只有明确处于AUTHORIZED状态的源IP才能继续进入NAPT
    if (client_access_can_forward_ipv4(source_ip))
    {
        return 0;
    }

    // 返回非0表示钩子消费了数据包
    // 消费数据包后，释放pbuf是钩子自己的责任
    pbuf_free(packet);
    
    return 1;
}
