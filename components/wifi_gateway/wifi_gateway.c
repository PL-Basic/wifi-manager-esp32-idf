#include <string.h>
#include <stdio.h>

#include "wifi_gateway.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"


static const char *TAG = "wifi_gateway";

// 如果外部能直接访问 s_status，就可能绕过 wifi_gateway 模块的规则，随便读写状态。
static wifi_gateway_status_t s_status = WIFI_GATEWAY_STATUS_IDLE;
static char s_sta_ip[16] = "0.0.0.0";
static int s_current_clients = 0;

// getter方法
wifi_gateway_status_t wifi_gateway_get_status(void)
{
    return s_status;
}

// 将状态转化成字符串
/*内部状态枚举：适合 C 代码判断
字符串状态：适合日志、MQTT、JSON、后端阅读 */
const char *wifi_gateway_status_to_string(wifi_gateway_status_t status)
{
    switch (status)
    {
    case WIFI_GATEWAY_STATUS_IDLE:
        return "IDLE";
    case WIFI_GATEWAY_STATUS_STARTING:
        return "STARTING";
    case WIFI_GATEWAY_STATUS_PROVISIONING:
        return "PROVISIONING";
    case WIFI_GATEWAY_STATUS_STA_CONNECTING:
        return "STA_CONNECTING";
    case WIFI_GATEWAY_STATUS_STA_CONNECTED:
        return "STA_CONNECTED";
    case WIFI_GATEWAY_STATUS_STA_GOT_IP:
        return "STA_GOT_IP";
    case WIFI_GATEWAY_STATUS_STA_DISCONNECTED:
        return "STA_DISCONNECTED";
    default:
        return "UNKNOWN";
    }
}

const char *wifi_gateway_get_sta_ip(void)
{
    return s_sta_ip;
}

int wifi_gateway_get_current_clients(void)
{
    return s_current_clients;
}

// 将 AA:BB:CC:DD:EE:FF 格式的字符串转换成 WiFi 驱动使用的 6 字节 MAC
static esp_err_t parse_mac_address(const char *mac_text,uint8_t mac[6])
{
    if (mac_text == NULL || mac == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // 标准mac必须是17个可见字符，并且冒号位置固定
    if (strlen(mac_text) != 17 || mac_text[2] != ':' || mac_text[5] != ':' || mac_text[8] != ':' || mac_text[11] != ':' || mac_text[14] != ':')
    {
        return ESP_ERR_INVALID_ARG;
    }
    
    // sscanf的%x需要unsigned int 指针，不能直接串uint8_t指针
    unsigned int parts[6] = {0};

    int parsed = sscanf(mac_text,"%2x:%2x:%2x:%2x:%2x:%2x",&parts[0],&parts[1],&parts[2],&parts[3],&parts[4],&parts[5]);

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

// 断开功能
esp_err_t wifi_gateway_disconnect_client(const char *mac_text)
{
    uint8_t mac[6] = {0};

    esp_err_t err = parse_mac_address(mac_text, mac);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Invalid client MAC:%s",mac_text == NULL ? "(null)" : mac_text);
        return err;
    }

    //ESP-IDF 断开客户端需要AID，而不是直接使用MAC
    uint16_t aid = 0;
    
    err = esp_wifi_ap_get_sta_aid(mac, &aid);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Get client AID failed,mac=%s: %s",mac_text,esp_err_to_name(err));
        return err;
    }
    

    // 请求WiFi驱动取消该客户端的SoftAP认证
    err = esp_wifi_deauth_sta(aid);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,"Disconnect client failed, mac=%s, aid=%u: %s",mac_text,(unsigned)aid,esp_err_to_name(err));
        
        return err;
    }
    
    ESP_LOGI(TAG,"Client disconnect requested, mac=%s, aid=%u",mac_text,(unsigned)aid);
    
    return ESP_OK;
}

//配置信息校验
static esp_err_t validate_config(const wifi_gateway_config_t *config)
{
    //判断当前的配置是否为空
    if (config == NULL)
    {
        ESP_LOGE(TAG,"WIFI config is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // 本地配网模式不连接上游路由器，因此不要求STA凭据。
    // 正常网关模式必须提供一个非空SSID。
    if (config->sta_enabled)
    {
        // 判断当前的sta_ssid是否为空或为空字符串
        if (config->sta_ssid == NULL || strlen(config->sta_ssid) == 0)
        {
            ESP_LOGE(TAG, "STA SSID is empty");
            return ESP_ERR_INVALID_ARG;
        }

        // 上游网络允许开放式WiFi，密码可以是空字符串。
        // 但调用方不能传NULL，因为后续配置函数会读取它。
        if (config->sta_password == NULL)
        {
            ESP_LOGE(TAG, "STA password is NULL");
            return ESP_ERR_INVALID_ARG;
        }
    }

    //判断当前的ap_ssid是否为空或为空字符串
    if (config->ap_ssid == NULL || strlen(config->ap_ssid) == 0)
    {
        ESP_LOGE(TAG,"SoftAP SSID is empty");
        return ESP_ERR_INVALID_ARG;
    }

    //校验SoftAP密码：若密码非空，则长度必须 >= 8（WPA2规范要求）
    if (config->ap_password != NULL &&
        strlen(config->ap_password) > 0 && 
        strlen(config->ap_password) < 8)
    {
        ESP_LOGE(TAG,"SoftAP password must be empty or at least 8 characters");
        return ESP_ERR_INVALID_ARG;
    }
    
    return ESP_OK;
}

// 不先 esp_netif_init()，后面就没有准备好 ESP-IDF 网络接口系统，因此创建 STA / AP netif 可能失败。 
static esp_err_t init_network_stack(void)
{
    // 初始化esp_netif
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }

    // 创建默认事件循环
    err = esp_event_loop_create_default();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}


//处理事件函数
static void wifi_event_handler(void *arg,esp_event_base_t event_base,int32_t event_id,void *event_data)
{
    //如果是wifi事件并且事件id还是sta已连接
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
        s_status = WIFI_GATEWAY_STATUS_STA_CONNECTED;
        ESP_LOGI(TAG,"STA connected");
    } 
    //如果是ip事件并且事件id还为sta获取到ip
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_status = WIFI_GATEWAY_STATUS_STA_GOT_IP;
        snprintf(s_sta_ip,sizeof(s_sta_ip),IPSTR,IP2STR(&event->ip_info.ip));
        //IPSTR/IPS2STR表示按IPv4格式打印出来
        ESP_LOGI(TAG,"STA got IP:"IPSTR,IP2STR(&event->ip_info.ip));
    }
    //如果是wifi事件并且事件id还为sta未连接
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        s_status = WIFI_GATEWAY_STATUS_STA_DISCONNECTED;
        snprintf(s_sta_ip, sizeof(s_sta_ip), "0.0.0.0");
        ESP_LOGW(TAG,"STA disconnected");
        esp_err_t err = esp_wifi_connect();
        if (err == ESP_OK)
        {
            s_status = WIFI_GATEWAY_STATUS_STA_CONNECTING;
            ESP_LOGI(TAG, "Reconnect requested");
        } else {
            s_status = WIFI_GATEWAY_STATUS_STA_DISCONNECTED;
            ESP_LOGE(TAG, "Reconnect request failed: %s", esp_err_to_name(err));
        }
        
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        const wifi_event_ap_staconnected_t *event = (const wifi_event_ap_staconnected_t *)event_data;
        
        s_current_clients++;
        // 打印真实mac
        ESP_LOGI(TAG, "SoftAP client connected, mac=%02X:%02X:%02X:%02X:%02X:%02X, aid=%u, current clients=%d", event->mac[0], event->mac[1], event->mac[2], event->mac[3], event->mac[4], event->mac[5], (unsigned)event->aid, s_current_clients);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        const wifi_event_ap_stadisconnected_t *event = (const wifi_event_ap_stadisconnected_t *)event_data;

        if (s_current_clients > 0)
        {
            s_current_clients--;
        }

        ESP_LOGI(TAG, "SoftAP client disconnected,  mac=%02X:%02X:%02X:%02X:%02X:%02X, aid=%u, current clients=%d", event->mac[0], event->mac[1], event->mac[2], event->mac[3], event->mac[4],event->mac[5], (unsigned)event->aid, s_current_clients);
    
    }
    
    
}

//注册处理方法
static esp_err_t register_event_handlers(void)
{
    // 没创建事件循环前不能注册
    // 注册wifi事件
    esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Register failed: %s", esp_err_to_name(err));
        return err;
    }

    // 注册ip事件
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Register failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

//wifi驱动初始化
static esp_err_t init_wifi_driver(void)
{
    // 初始化WIFI驱动
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    // 打印驱动初始化
    ESP_LOGI(TAG, "WIFI driver initialized");

    return ESP_OK;
}

//创建wifi接口
static esp_err_t create_wifi_netifs(bool sta_enabled, esp_netif_t **ap_netif)
{
    if (ap_netif == NULL)
    {
        ESP_LOGE(TAG, "AP netif output pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // 正常网关模式才需要STA网络接口。
    // 本地配网模式没有上游连接，不创建无用的STA接口。
    if (sta_enabled)
    {
        // 为连接别人wifi的sta创建网络接口
        esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
        if (sta_netif == NULL)
        {
            ESP_LOGE(TAG, "Create wifi sta failed");
            return ESP_ERR_ESP_NETIF_INIT_FAILED;
        }
    }

    // 两种模式都必须创建SoftAP接口。
    // 本地Portal、DHCP和客户端连接都依赖该接口。
    *ap_netif = esp_netif_create_default_wifi_ap();
    if (*ap_netif == NULL)
    {
        ESP_LOGE(TAG, "Create wifi ap failed");
        return ESP_ERR_ESP_NETIF_INIT_FAILED;
    }

    
    return ESP_OK;
}

static esp_err_t configure_sta(const wifi_gateway_config_t *config)
{
    if (config == NULL)
    {
        ESP_LOGE(TAG,"Config is null");
        return ESP_ERR_INVALID_ARG;
    }
    
    // 给wifi驱动使用的sta配置结构体赋值
    // 先将WIFI配置结构体清零
    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, config->sta_ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, config->sta_password, sizeof(sta_config.sta.password) - 1);

    // 设置该配置，配置的是驱动中的sta参数
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Set sta config failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static esp_err_t configure_ap(const wifi_gateway_config_t *config)
{
    if (config == NULL)
    {
        ESP_LOGE(TAG, "Config is null");
        return ESP_ERR_INVALID_ARG;
    }

    // 给wifi驱动使用的ap配置结构体赋值
    // 依旧先清零
    wifi_config_t ap_config = {0};
    strncpy((char *)ap_config.ap.ssid, config->ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(config->ap_ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = config->ap_max_connection;

    if (config->ap_password != NULL && strlen(config->ap_password) > 0)
    {
        strncpy((char *)ap_config.ap.password, config->ap_password, sizeof(ap_config.ap.password) - 1);
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }
    else
    {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Set ap config failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static esp_err_t enable_softap_napt(esp_netif_t *ap_netif)
{
    if (ap_netif == NULL)
    {
        ESP_LOGE(TAG, "SoftAP netif is null");
        return ESP_ERR_INVALID_ARG;
    }

    // 对下游开放内网接口
    // 在 SoftAP 这个下游网络接口上开启 IPv4 NAPT 转发能力
    esp_err_t err = esp_netif_napt_enable(ap_netif);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Open the napt failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "NAPT enabled on SoftAP");

    return ESP_OK;
}

static esp_err_t set_wifi_mode(bool sta_enabled)
{
    // 正常网关需要AP和STA同时工作
    // 本地配置只需呀SoftAP
    // 设置WiFi模式
    wifi_mode_t mode = sta_enabled ? WIFI_MODE_APSTA : WIFI_MODE_AP;

    esp_err_t err = esp_wifi_set_mode(mode);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Set wifi mode failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "WiFi mode configured: %s", sta_enabled ? "APSTA" : "AP");

    return ESP_OK;
}

static esp_err_t start_wifi(void)
{
    // 启动WIFI功能
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "WIFI start failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "WIFI started");
    return ESP_OK;
}

static esp_err_t connect_sta(void)
{
    // 连接wifi
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "WIFI connect request sending failed: %s", esp_err_to_name(err));
        return err;
    }
    s_status = WIFI_GATEWAY_STATUS_STA_CONNECTING;
    ESP_LOGI(TAG, "STA connect requested");
    return ESP_OK;
}


// wifi启动并连接
esp_err_t wifi_gateway_start(const wifi_gateway_config_t *config)
{
    esp_err_t err = validate_config(config);
    if (err != ESP_OK)
    {
        return err;
    }

    s_status = WIFI_GATEWAY_STATUS_STARTING;

    err = init_network_stack();
    if (err != ESP_OK)
    {
        return err;
    }

    err = register_event_handlers();
    if (err != ESP_OK)
    {
        return err;
    }
    
   
    err = init_wifi_driver();
    if (err != ESP_OK)
    {
        return err;
    }
    
    esp_netif_t *ap_netif = NULL;

    // 不创建 STA netif，STA 即使发送连接请求，也没有完整的网络接口承接 IP 层能力；
    // 网关就没有真正的上游网络出口。
    err = create_wifi_netifs(config->sta_enabled, &ap_netif);
    if (err != ESP_OK)
    {
        return err;
    }

    err = set_wifi_mode(config->sta_enabled);
    if (err != ESP_OK)
    {
        return err;
    }
    
    // 只有存在上游凭据时，才配置STA驱动参数
    if (config->sta_enabled)
    {
        err = configure_sta(config);
        if (err != ESP_OK)
        {
            return err;
        }
        
    }
    
    // SoftAP是两种模式的共同入口，因此始终配置
    err = configure_ap(config);
    if (err != ESP_OK)
    {
        return err;
    }

    err = start_wifi();
    if (err != ESP_OK)
    {
        return err;
    }

    if (config->sta_enabled)
    {
        err = enable_softap_napt(ap_netif);
        if (err != ESP_OK)
        {
            return err;
        }

        err = connect_sta();
        if (err != ESP_OK)
        {
            return err;
        }

        ESP_LOGI(TAG, "WiFi gateway started, STA SSID=%s, SoftAP SSID=%s", config->sta_ssid, config->ap_ssid);
    }
    else
    {
        // SoftAP已经可以为管理员提供本地配网页面
        s_status = WIFI_GATEWAY_STATUS_PROVISIONING;

        ESP_LOGI(TAG, "WiFi provisioning mode started, SoftAP SSID=%s", config->ap_ssid);
    }

    return ESP_OK;
}
