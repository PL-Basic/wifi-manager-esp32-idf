#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "client_access.h"

#include "freertos/FreeRTOS.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"

static const char *TAG = "client_access";

// 保存一个在线客户端及其访问状态
typedef struct
{
    // 当前数组位置是否正在使用
    bool in_use;
    // 客户端的六字节 MAC
    uint8_t mac[6];
    // 客户端当前访问状态
    client_access_state_t state;
    // 当前授权对应的后端会话编号
    int64_t session_id;
    // DHCP服务器分配给该客户端的IPv4地址
    esp_ip4_addr_t ip;
    // 授权时刻的FreeRTOS系统时间戳，单位毫秒
    int64_t authorized_at_ticks;
    // 授权有效时长（已转换为tick数），0表示永不过期
    int64_t ttl_ticks;
    // 客户端到本设备SoftAP的信号强度，单位dBm，用于蹭网检测
    int8_t rssi;
} client_access_entry_t;

// 传递给TCP/IP任务的ARP查询参数和查询结果。
typedef struct
{
    // 只查询SoftAP接口的ARP记录。
    struct netif *netif;
    // 需要查询的客户端IPv4地址。
    ip4_addr_t ipv4;
    // 查询成功后保存对应的六字节MAC。
    uint8_t mac[6];
} client_arp_lookup_context_t;

// 使用ESP-IDF当前芯片支持的最大SoftAP客户端数量
static client_access_entry_t s_clients[ESP_WIFI_MAX_CONN_NUM];

// MQTT回调和WIFI事件可能在不同任务执行，因此需要保护状态表
static portMUX_TYPE s_clients_lock = portMUX_INITIALIZER_UNLOCKED;

static bool s_started = false;

static esp_event_handler_instance_t s_connected_handler;
static esp_event_handler_instance_t s_disconnected_handler;
// 保存DHCP分配客户端IP事件的处理器实例
static esp_event_handler_instance_t s_ip_assigned_handler;

// 该函数只能在已经进入临界区后调用
static int find_client_index_locked(const uint8_t mac[6])
{
    for (int i = 0; i < ESP_WIFI_MAX_CONN_NUM; i++)
    {
        if (s_clients[i].in_use && memcmp(s_clients[i].mac, mac, 6) == 0)
        {
            return i;
        }
    }

    return -1;
}

// 将新连接的客户端加入状态表
static esp_err_t track_connected_client(const uint8_t mac[6])
{
    if (mac == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    int client_index = -1;

    portENTER_CRITICAL(&s_clients_lock);
    
    // 如果客户端已经存在，直接复用原位置
    client_index = find_client_index_locked(mac);

    // 不存在时寻找一个空位置
    if (client_index < 0)
    {
        for (int i = 0; i < ESP_WIFI_MAX_CONN_NUM; i++)
        {
            if (!s_clients[i].in_use)
            {
                client_index = i;
                break;
            }   
        }
    }

    if (client_index >= 0)
    {
        s_clients[client_index].in_use = true;
        memcpy(s_clients[client_index].mac, mac, 6);


        // 新连接或重新连接后都必须重新认证
        s_clients[client_index].state = CLIENT_ACCESS_STATE_UNAUTHORIZED;
        s_clients[client_index].session_id = 0;
        // 连接事件发生时DHCP可能还没有分配IP，先清除旧地址
        s_clients[client_index].ip.addr = 0;
        s_clients[client_index].authorized_at_ticks = 0;
        s_clients[client_index].ttl_ticks = 0;
        s_clients[client_index].rssi = 0;
    }
    
    portEXIT_CRITICAL(&s_clients_lock);

    if (client_index < 0)
    {
        ESP_LOGE(TAG, "Client access table is full");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Client tracked, mac=%02X:%02X:%02X:%02X:%02X:%02X, state=%s", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], client_access_state_to_string(CLIENT_ACCESS_STATE_UNAUTHORIZED));

    return ESP_OK;
}



// 根据MAC找到在线客户端，并保存DHCP为其分配的IPv4地址
static esp_err_t track_client_ip(const uint8_t mac[6], const esp_ip4_addr_t *ip)
{
    if (mac == NULL || ip == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_clients_lock);

    int client_index = find_client_index_locked(mac);

    if (client_index >= 0)
    {
        // 在同一个临界区中更新IP，防止数据包过滤器读取到写了一半的数据
        s_clients[client_index].ip = *ip;
    }

    portEXIT_CRITICAL(&s_clients_lock);

    if (client_index < 0)
    {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG,"Client IP tracked, mac=%02X:%02X:%02X:%02X:%02X:%02X, ip=" IPSTR,mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],IP2STR(ip));

    return ESP_OK;
}

// 补录client_access启动前已经连接SoftAP的客户端。
static esp_err_t track_existing_softap_clients(void)
{
    wifi_sta_list_t station_list = {0};

    // 读取当前已经连接SoftAP的客户端MAC列表
    esp_err_t err = esp_wifi_ap_get_sta_list(&station_list);
    if (err != ESP_OK)
    {
        return err;
    }

    // 当前没有提交连接的客户端，不需要补录
    if (station_list.num == 0)
    {
        return ESP_OK;
    }

    // 项目使用的是ESP-IDF默认SoftAP接口。
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    // 这个数组同时保存查询输入MAC和查询输出IP。
    esp_netif_pair_mac_ip_t client_pairs[ESP_WIFI_MAX_CONN_NUM] = {0};

    for (int i = 0; i < station_list.num; i++)
    {
        memcpy(client_pairs[i].mac, station_list.sta[i].mac, sizeof(client_pairs[i].mac));
    }
    // 根据MAC查询DHCP服务器中已经存在的租约IP。
    err = esp_netif_dhcps_get_clients_by_mac(ap_netif, station_list.num, client_pairs);
    if (err != ESP_OK)
    {
        return err;
    }

    for (int i = 0; i < station_list.num; i++)
    {
        // 首先补录MAC，并将启动时的认证状态设为UNAUTHORIZED。
        err = track_connected_client(client_pairs[i].mac);
        if (err != ESP_OK)
        {
            return err;
        }

        // IP为0说明DHCP目前还没完成分配
        // 这种情况不算启动失败，后续IP事件仍会完成记录
        if (client_pairs[i].ip.addr != 0)
        {
            err = track_client_ip(client_pairs[i].mac, &client_pairs[i].ip);

            if (err != ESP_OK)
            {
                return err;
            }
        }
    }
    return ESP_OK;
}

// 客户端离开SoftAP后，从在线状态表中删除
static void remove_disconnect_client(const uint8_t mac[6])
{
    if (mac == NULL)
    {
        return;
    }

    portENTER_CRITICAL(&s_clients_lock);
    
    int client_index = find_client_index_locked(mac);

    if (client_index >= 0)
    {
        memset(&s_clients[client_index],0,sizeof(s_clients[client_index]));
    }

    portEXIT_CRITICAL(&s_clients_lock);

    ESP_LOGI(TAG, "Client removed, mac=%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// 接收ESP-IDF的SoftAP客户端连接和断开事件
static void client_access_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base != WIFI_EVENT || event_data == NULL)
    {
        return;
    }

    // 客户端连接：加入在线状态表，默认未认证
    if (event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        const wifi_event_ap_staconnected_t *event = (const wifi_event_ap_staconnected_t *)event_data;

        esp_err_t err = track_connected_client(event->mac);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Track connected client failed: %s", esp_err_to_name(err));
        }
    }
    // 客户端断开：从在线状态表删除
    else if (event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        const wifi_event_ap_stadisconnected_t *event = (const wifi_event_ap_stadisconnected_t *)event_data;
        remove_disconnect_client(event->mac);
    }
}

// DHCP服务器为SoftAP客户端分配IP后，将MAC和IP关联到状态表
static void client_access_ip_event_handler(void *arg,esp_event_base_t event_base,int32_t event_id,void *event_data)
{
    if (event_base != IP_EVENT || event_id != IP_EVENT_ASSIGNED_IP_TO_CLIENT || event_data == NULL)
    {
        return;
    }
    
    const ip_event_assigned_ip_to_client_t *event = (const ip_event_assigned_ip_to_client_t *)event_data;

    esp_err_t err = track_client_ip(event->mac, &event->ip);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Track client IP failed: %s",esp_err_to_name(err));
    }

}

// 将字符串 MAC 转换为六字节 MAC
static esp_err_t parse_mac_address(const char *mac_text, uint8_t mac[6])
{
    if (mac_text == NULL || mac == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(mac_text) != 17 || mac_text[2] != ':' || mac_text[5] != ':' || mac_text[8] != ':' || mac_text[11] != ':' || mac_text[14] != ':')
    {
        return ESP_ERR_INVALID_ARG;
    }

    unsigned int parts[6] = {0};

    int parsed = sscanf(mac_text, "%2x:%2x:%2x:%2x:%2x:%2x", &parts[0], &parts[1], &parts[2], &parts[3], &parts[4], &parts[5]);

    if (parsed != 6)
    {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < 6; i++)
    {
        if (parts[i] > 0xFF)
        {
            return ESP_ERR_INVALID_ARG;
        }
        mac[i] = (uint8_t)parts[i];
        
    }
    
    return ESP_OK;
}

// 修改在线客户端的访问状态
esp_err_t client_access_set_state(const uint8_t mac[6], client_access_state_t state)
{
    if (mac == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (state != CLIENT_ACCESS_STATE_UNAUTHORIZED && state != CLIENT_ACCESS_STATE_AUTHORIZED && state != CLIENT_ACCESS_STATE_BLOCKED)
    {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_clients_lock);
    
    int client_index = find_client_index_locked(mac);

    if (client_index >= 0)
    {
        s_clients[client_index].state = state;
    }

    portEXIT_CRITICAL(&s_clients_lock);

    if (client_index < 0)
    {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Client state changed, mac=%02X:%02X:%02X:%02X:%02X:%02X, state=%s", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], client_access_state_to_string(state));

    return ESP_OK;
}

// 查询在线客户端状态
esp_err_t client_access_get_state(const uint8_t mac[6], client_access_state_t *state)
{
    if (mac == NULL || state == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_clients_lock);

    int client_index = find_client_index_locked(mac);

    if (client_index >= 0)
    {
        *state = s_clients[client_index].state;
    }

    portEXIT_CRITICAL(&s_clients_lock);

    if (client_index < 0)
    {
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

// 该函数由esp_netif_tcpip_exec()安排到lwIP TCP/IP任务中执行。
static esp_err_t find_client_mac_in_arp_table(void *context)
{
    if (context == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    client_arp_lookup_context_t *lookup =
        (client_arp_lookup_context_t *)context;

    struct eth_addr *ethernet_address = NULL;
    const ip4_addr_t *matched_ip = NULL;

    // 根据SoftAP接口和源IPv4地址查找稳定的ARP记录。
    ssize_t arp_index = etharp_find_addr(
        lookup->netif,
        &lookup->ipv4,
        &ethernet_address,
        &matched_ip);

    if (arp_index < 0 || ethernet_address == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(
        lookup->mac,
        ethernet_address->addr,
        sizeof(lookup->mac));

    return ESP_OK;
}

// 根据实际收到的数据包源IP，从SoftAP的ARP表恢复客户端MAC。
static esp_err_t resolve_client_mac_by_ipv4(uint32_t source_ip, uint8_t mac[6])
{
    if (source_ip == 0 || mac == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

    if (ap_netif == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    struct netif *softap_netif = (struct netif *)esp_netif_get_netif_impl(ap_netif);

    if (softap_netif == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    client_arp_lookup_context_t lookup = {
        .netif = softap_netif,
        .ipv4 = {
        .addr = source_ip
        }
    };

    // ARP表属于lwIP，必须进入TCP/IP任务中安全查询。
    esp_err_t err = esp_netif_tcpip_exec(find_client_mac_in_arp_table, &lookup);

    if (err != ESP_OK)
    {
        return err;
    }

    memcpy(mac, lookup.mac, sizeof(lookup.mac));

    return ESP_OK;
}

// 只查询当前状态表，不执行ARP恢复。
static esp_err_t copy_client_snapshot_by_ipv4(uint32_t source_ip, client_access_snapshot_t *snapshot)
{
    esp_err_t result = ESP_ERR_NOT_FOUND;

    portENTER_CRITICAL(&s_clients_lock);

    for (int i = 0; i < ESP_WIFI_MAX_CONN_NUM; i++)
    {
        if (!s_clients[i].in_use)
        {
            continue;
        }

        if (s_clients[i].ip.addr != source_ip)
        {
            continue;
        }

        memcpy(snapshot->mac, s_clients[i].mac, sizeof(snapshot->mac));

        snapshot->ipv4 = s_clients[i].ip.addr;
        snapshot->state = s_clients[i].state;
        snapshot->rssi = s_clients[i].rssi;
        snapshot->session_id = s_clients[i].session_id;

        result = ESP_OK;
        break;
    }

    portEXIT_CRITICAL(&s_clients_lock);

    return result;
}

esp_err_t client_access_get_snapshot_by_ipv4(uint32_t source_ip, client_access_snapshot_t *snapshot)
{
    // 0.0.0.0不是有效的客户端地址，snapshot也必须指向有效空间
    if (source_ip == 0 || snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // 查询失败时，避免调用方拿到之前残留的数据
    memset(snapshot, 0, sizeof(*snapshot));

    // 首先走正常状态表查询。
    esp_err_t err = copy_client_snapshot_by_ipv4(source_ip, snapshot);
    if (err == ESP_OK)
    {
        return ESP_OK;
    }

    // DHCP事件可能没有出现，尝试从实际网络通信产生的ARP表恢复MAC。
    uint8_t recovered_mac[6] = {0};
    err = resolve_client_mac_by_ipv4(source_ip, recovered_mac);
    if (err != ESP_OK)
    {
        return err;
    }

    esp_ip4_addr_t recovered_ip = {
        .addr = source_ip
    };

    // 这里只能给已经通过WiFi连接事件记录的MAC补充IP。
    // 陌生ARP记录不能凭空创建已连接客户端。
    err = track_client_ip(recovered_mac, &recovered_ip);

    if (err != ESP_OK)
    {
        return err;
    }

    // IP恢复完成后，重新读取一份完整快照。
    return copy_client_snapshot_by_ipv4(source_ip, snapshot);
}

// 根据数据包源IP查询客户端是否具有外网转发权限
bool client_access_can_forward_ipv4(uint32_t source_ip)
{
    // 0.0.0.0 不是已经分配给客户端的有效地址，不能获得转发权限
    if (source_ip == 0)
    {
        return false;
    }

    // 默认拒绝。只有找到明确的AUTHORIZED记录后才改为允许
    bool can_forward = false;

    portENTER_CRITICAL(&s_clients_lock);

    for (int i = 0; i < ESP_WIFI_MAX_CONN_NUM; i++)
    {
        // 跳过当前没有保护客户端的空位置
        if (!s_clients[i].in_use)
        {
            continue;
        }
        
        // 当前记录的IP不是数据包源IP，继续检查下一条记录
        if (s_clients[i].ip.addr != source_ip)
        {
            continue;
        }

        if (s_clients[i].state == CLIENT_ACCESS_STATE_AUTHORIZED)
        {
            // 检查是否已经过期
            if (s_clients[i].ttl_ticks > 0)
            {
                // elapsed 必须用 TickType_t（uint32_t），利用无符号减法的自然溢出回绕特性
                // 如果用 int64_t，tick 溢出后结果为负数，导致授权永不过期
                TickType_t elapsed = xTaskGetTickCount() - (TickType_t)s_clients[i].authorized_at_ticks;
                if (elapsed >= s_clients[i].ttl_ticks)
                {
                    // 授权已经过期，撤销认证
                    s_clients[i].state = CLIENT_ACCESS_STATE_UNAUTHORIZED;
                    s_clients[i].session_id = 0;
                    s_clients[i].ttl_ticks = 0;

                    ESP_LOGI(TAG, "Client authorization expired, mac=%02X:%02X:%02X:%02X:%02X:%02X", s_clients[i].mac[0], s_clients[i].mac[1], s_clients[i].mac[2], s_clients[i].mac[3], s_clients[i].mac[4], s_clients[i].mac[5]);
                }
                else
                {
                    // 授权仍然有效，允许转发
                    can_forward = true;
                }
            }
            else
            {
                // 永不过期的授权，允许转发
                can_forward = true;
            }
        }
        break;
    }
    
    portEXIT_CRITICAL(&s_clients_lock);
    
    return can_forward;
}

const char *client_access_state_to_string(client_access_state_t state)
{
    switch (state)
    {
    case CLIENT_ACCESS_STATE_UNAUTHORIZED:
        return "UNAUTHORIZED";

    case CLIENT_ACCESS_STATE_AUTHORIZED:
        return "AUTHORIZED";

    case CLIENT_ACCESS_STATE_BLOCKED:
        return "BLOCKED";

    case CLIENT_ACCESS_STATE_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

// 将内部状态表复制到外部数组，供其他模块读取客户端快照
esp_err_t client_access_copy_snapshots(client_access_snapshot_t *snapshots, size_t capacity, size_t *snapshot_count)
{
    if (snapshots == NULL || snapshot_count == NULL || capacity == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *snapshot_count = 0;

    memset(snapshots, 0, sizeof(client_access_snapshot_t) * capacity);

    portENTER_CRITICAL(&s_clients_lock);

    for (int i = 0; i < ESP_WIFI_MAX_CONN_NUM && *snapshot_count < capacity; i++)
    {
        if (!s_clients[i].in_use)
        {
            continue;
        }

        client_access_snapshot_t *snapshot = &snapshots[*snapshot_count];
        memcpy(snapshot->mac, s_clients[i].mac, sizeof(snapshot->mac));
        snapshot->ipv4 = s_clients[i].ip.addr;
        snapshot->state = s_clients[i].state;
        snapshot->session_id = s_clients[i].session_id;
        snapshot->rssi = s_clients[i].rssi;
        (*snapshot_count)++;
    }

    portEXIT_CRITICAL(&s_clients_lock);
    return ESP_OK;
}

// 实现授权
esp_err_t client_access_authorize(const char *mac_text, int64_t session_id, uint32_t ttl_seconds)
{
    if (session_id <= 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t mac[6] = {0};

    esp_err_t err = parse_mac_address(mac_text, mac);
    if (err != ESP_OK)
    {
        return err;
    }

    portENTER_CRITICAL(&s_clients_lock);

    int client_index = find_client_index_locked(mac);
    if (client_index >= 0)
    {
        s_clients[client_index].state = CLIENT_ACCESS_STATE_AUTHORIZED;
        s_clients[client_index].session_id = session_id;
        s_clients[client_index].authorized_at_ticks = xTaskGetTickCount();
        s_clients[client_index].ttl_ticks = pdMS_TO_TICKS((uint32_t)(ttl_seconds * 1000));
    }

    portEXIT_CRITICAL(&s_clients_lock);

    if (client_index < 0)
    {
        // 不能授权一个当前并未连接到SoftAP的客户端
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Client authorized, mac=%s, sessionId=%lld", mac_text, (long long)session_id);
    
    return ESP_OK;
}

// 撤销指定客户端当前会话的认证权限
esp_err_t client_access_revoke_authorization(const char *mac_text, int64_t session_id)
{
    // sessionId必须是后端创建的有效正数编号
    if (session_id <= 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // 将字符串形式的MAC转换为内部使用的六字节MAC
    uint8_t mac[6] = {0};
    esp_err_t err = parse_mac_address(mac_text, mac);
    if (err != ESP_OK)
    {
        return err;
    }

    int client_index = -1;
    bool revoked = false;

    portENTER_CRITICAL(&s_clients_lock);

    client_index = find_client_index_locked(mac);

    // 只有MAC、当前状态和sessionId全部匹配，
    // 才能撤销这一次真实存在的认证会话。
    if (client_index >= 0 && s_clients[client_index].state == CLIENT_ACCESS_STATE_AUTHORIZED && s_clients[client_index].session_id == session_id)
    {
        s_clients[client_index].state = CLIENT_ACCESS_STATE_UNAUTHORIZED;
        // 当前会话已经失效，清除 sessionId 和 TTL 残留
        s_clients[client_index].session_id = 0;
        s_clients[client_index].ttl_ticks = 0;
        s_clients[client_index].ttl_ticks = 0;
        revoked = true;
    }

    portEXIT_CRITICAL(&s_clients_lock);

    // MAC对应的客户端当前没有连接SoftAP
    if (client_index < 0)
    {
        return ESP_ERR_NOT_FOUND;
    }

    // 客户端存在，但不是指定的已认证会话。
    // 可能是sessionId不匹配、尚未认证，或者已经被BLOCKED。
    if (!revoked)
    {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Client authorization revoked, mac=%s, sessionId=%lld", mac_text, (long long)session_id);

    return ESP_OK;
}

// 周期性扫描所有在线客户端，将已过期的授权状态降为 UNAUTHORIZED。
void client_access_expire_check(void)
{
    // 获取当前 tick 计数，在临界区外获取避免占用临界区时间
    TickType_t now = xTaskGetTickCount();

    portENTER_CRITICAL(&s_clients_lock);
    for (int i = 0; i < ESP_WIFI_MAX_CONN_NUM; i++)
    {
        // 跳过未占用位置和非授权状态的客户端
        if (!s_clients[i].in_use)
            continue;
        if (s_clients[i].state != CLIENT_ACCESS_STATE_AUTHORIZED)
            continue;
        // ttl_ticks 为 0 表示永不过期（管理员手动授权）
        if (s_clients[i].ttl_ticks <= 0)
            continue;
        // 计算从授权时刻到现在经过的 tick 数
        TickType_t elapsed = now - s_clients[i].authorized_at_ticks;
        if (elapsed >= s_clients[i].ttl_ticks)
        {
            // 已过期：降为 UNAUTHORIZED，清除会话关联
            s_clients[i].state = CLIENT_ACCESS_STATE_UNAUTHORIZED;
            s_clients[i].session_id = 0;
            s_clients[i].ttl_ticks = 0;

            ESP_LOGI(TAG, "Client authorization expired (periodic), "
                          "mac=%02X:%02X:%02X:%02X:%02X:%02X",
                     s_clients[i].mac[0], s_clients[i].mac[1], s_clients[i].mac[2],
                     s_clients[i].mac[3], s_clients[i].mac[4], s_clients[i].mac[5]);
        }
    }
    portEXIT_CRITICAL(&s_clients_lock);
    
}

// 通过WiFi驱动查询所有SoftAP客户端的实时RSSI，更新内部状态表
// 由main.c定时调用，为后端蹭网检测提供数据
esp_err_t client_access_update_rssi_all(void)
{
    wifi_sta_list_t station_list = {0};
    esp_err_t err = esp_wifi_ap_get_sta_list(&station_list);
    // 读取当前连接SoftAP的客户端列表，每条记录包含MAC和RSSI
    if (err != ESP_OK)
    {
        return err;  // 查询失败返回错误，不影响主循环
    }

    portENTER_CRITICAL(&s_clients_lock);
    // 先使上一轮测量失效。
    for (int i = 0; i < ESP_WIFI_MAX_CONN_NUM; i++)
    {
        if (s_clients[i].in_use)
        {
            s_clients[i].rssi = 0;
        }
    }

    // 只恢复本轮驱动实际返回的 RSSI。
    for (int i = 0; i < station_list.num; i++)
    {
        int index = find_client_index_locked(station_list.sta[i].mac);
        if (index >= 0)
        {
            s_clients[index].rssi = station_list.sta[i].rssi;
        }
    }

    portEXIT_CRITICAL(&s_clients_lock);
    return ESP_OK;
}

// 注册事件处理器，并接管后续客户端状态变化
esp_err_t client_access_start(void)
{
    if (s_started)
    {
        return ESP_OK;
    }

    esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, client_access_event_handler, NULL, &s_connected_handler);

    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, client_access_event_handler, NULL, &s_disconnected_handler);

    if (err != ESP_OK)
    {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, s_connected_handler);
        return err;
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, client_access_ip_event_handler, NULL, &s_ip_assigned_handler);

    if (err != ESP_OK)
    {
        // IP事件注册失败时，撤销前面已经注册的两个WiFi事件
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, s_connected_handler);

        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, s_disconnected_handler);
    
        return err;
    }

    // 事件处理器注册完成后，补录此前已经连接并取得IP的客户端。
    err = track_existing_softap_clients();

    if (err != ESP_OK)
    {
        esp_event_handler_instance_unregister(
            WIFI_EVENT,
            WIFI_EVENT_AP_STACONNECTED,
            s_connected_handler);

        esp_event_handler_instance_unregister(
            WIFI_EVENT,
            WIFI_EVENT_AP_STADISCONNECTED,
            s_disconnected_handler);

        esp_event_handler_instance_unregister(
            IP_EVENT,
            IP_EVENT_ASSIGNED_IP_TO_CLIENT,
            s_ip_assigned_handler);

        return err;
    }

    s_started = true;

    ESP_LOGI(TAG, "Client access started");

    return ESP_OK;
}
