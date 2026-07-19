#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "client_access.h"

#include "freertos/FreeRTOS.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"

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

} client_access_entry_t;

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

        // 只有已经完成认证的客户端才允许外网转发
        can_forward = s_clients[i].state == CLIENT_ACCESS_STATE_AUTHORIZED;
        
        // 一个IP只应该对应一个在线客户端，找到后不在继续遍历

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

// 实现授权
esp_err_t client_access_authorize(const char *mac_text, int64_t session_id)
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
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, s_connected_handler);
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
    

    // 补录启动状态管理之前已经连接到 SoftAP 的客户端
    wifi_sta_list_t station_list = {0};

    err = esp_wifi_ap_get_sta_list(&station_list);
    if (err != ESP_OK)
    {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, s_connected_handler);
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, s_disconnected_handler);
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, s_ip_assigned_handler);
        
        return err;
    }

    for (int i = 0; i < station_list.num; i++)
    {
        err = track_connected_client(station_list.sta[i].mac);
        if (err != ESP_OK)
        {
            // 启动失败时撤销已经注册的全部事件，避免下次启动重复注册
            esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, s_connected_handler);
            esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, s_disconnected_handler);
            esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, s_ip_assigned_handler);    

            return err;
        }
    }

    s_started = true;

    ESP_LOGI(TAG, "Client access started, initial clients=%d", station_list.num);

    return ESP_OK;
}
