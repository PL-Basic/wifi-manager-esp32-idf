#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "captive_portal.h"
#include "portal_dns.h"

#include "esp_system.h"
#include "esp_http_server.h"
#include "esp_log.h"

// 浏览器application/x-www-form-urlencoded请求体的最大容量。
#define PROVISION_BODY_SIZE 384
// URL编码后，一个字节最多可能变成三个字符，例如空格变成%20。
#define PROVISION_ENCODED_SSID_SIZE 128
#define PROVISION_ENCODED_PASSWORD_SIZE 192
// 解码后的WiFi字段容量
#define PROVISION_SSID_SIZE 33
#define PROVISION_PASSWORD_SIZE 64

static const char *TAG = "captive_portal";

// 保存HTTP服务器实例，防止重复启动
static httpd_handle_t s_server = NULL;
static captive_portal_config_t s_config = {0};

// 未配置上游网络时显示的最小本地页面。
// 该页面必须保存在ESP32中，因为此时设备还不能访问外部服务器。
static const char s_provisioning_html[] =
    "<!doctype html>"
    "<html lang=\"zh-CN\">"
    "<head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>配置上游WiFi</title>"
    "</head>"
    "<body>"
    "<h1>配置上游WiFi</h1>"
    "<form method=\"post\" action=\"/api/provision\">"
    "<p><label>WiFi名称："
    "<input name=\"ssid\" maxlength=\"32\" required>"
    "</label></p>"
    "<p><label>WiFi密码："
    "<input name=\"password\" type=\"password\" maxlength=\"63\">"
    "</label></p>"
    "<button type=\"submit\">保存并连接</button>"
    "</form>"
    "</body>"
    "</html>";

// 当前先验证Portal页面和系统检测路径。
// 真正账号提交将在后端Portal会话接口完成后接入。
static const char s_portal_html[] =
    "<!DOCTYPE html>"
    "<html lang=\"zh-CN\">"
    "<head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>WiFi 登录认证</title>"
    "<style>"
    "body{"
    "margin:0;"
    "min-height:100vh;"
    "display:flex;"
    "align-items:center;"
    "justify-content:center;"
    "font-family:sans-serif;"
    "background:#f1f5f9;"
    "color:#0f172a;"
    "}"
    ".card{"
    "width:min(88%,380px);"
    "padding:28px;"
    "border-radius:18px;"
    "background:#fff;"
    "box-shadow:0 16px 40px rgba(15,23,42,.12);"
    "}"
    "h1{margin:0 0 14px;font-size:24px;}"
    "p{line-height:1.7;color:#475569;}"
    ".status{"
    "margin-top:20px;"
    "padding:12px;"
    "border-radius:10px;"
    "background:#fff7ed;"
    "color:#9a3412;"
    "}"
    "</style>"
    "</head>"
    "<body>"
    "<main class=\"card\">"
    "<h1>WiFi 登录认证</h1>"
    "<p>设备已经连接热点，但尚未获得互联网访问权限。</p>"
    "<div class=\"status\">Portal HTTP 服务已经启动</div>"
    "</main>"
    "</body>"
    "</html>";



// 将一个十六进制字符串转换为0至15
static int hex_character_to_value(char character)
{
    if (character >= '0' && character <= '9')
    {
        return character - '0';
    }

    if (character >= 'a' && character <= 'f')
    {
        return character - 'a' + 10;
    }

    if (character >= 'A' && character <= 'F')
    {
        return character - 'A' + 10;
    }
    
    return -1;
}

// 解码application/x-www-form-urlencoded中的字段值。
static esp_err_t decode_form_value(const char *encoded, char *decoded, size_t decoded_size)
{
    if (encoded == NULL || decoded == NULL || decoded_size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t write_index = 0;

    for (size_t read_index = 0; encoded[read_index] != '\0'; read_index++)
    {
        unsigned char decoded_character;
        
        if (encoded[read_index] == '+')
        {
            // HTML表单使用+表示空格
            decoded_character = ' ';
        }
        else if (encoded[read_index] == '%')
        {
            // %后面必须还有两个十六进制字符
            if (encoded[read_index + 1] == '\0' || encoded[read_index + 2] == '\0')
            {
                return ESP_ERR_INVALID_ARG;
            }

            int high = hex_character_to_value(encoded[read_index + 1]);
            int low = hex_character_to_value(encoded[read_index + 2]);
            
            if (high < 0 || low < 0)
            {
                return ESP_ERR_INVALID_ARG;
            }

            decoded_character = (unsigned char)((high << 4) | low);
            read_index += 2;
        }
        else
        {
            decoded_character = (unsigned char)encoded[read_index];
        }

        // 不允许通过%00在字符串中间注入结束符
        if (decoded_character == '\0')
        {
            return ESP_ERR_INVALID_ARG;
        }

        // 必须给最终'\0'预留一个位置
        if (write_index + 1 >= decoded_size)
        {
            return ESP_ERR_NO_MEM;
        }
        
        decoded[write_index++] = (char)decoded_character;
    }
    decoded[write_index] = '\0';
    return ESP_OK;
}

static esp_err_t portal_page_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");

    const char *page = s_config.provisioning_mode ? s_provisioning_html : s_portal_html;

    // 未配网时显示上游WiFi表单
    // 已配网时显示普通客户端认证页面
    return httpd_resp_send(request, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t provision_post_handler(httpd_req_t *request)
{
    if (!s_config.provisioning_mode || s_config.provision_handler == NULL)
    {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Provisioning mode is not active");
    }

    char body[PROVISION_BODY_SIZE] = {0};

    // content_len不包含我们需要补充的字符串结束符
    if (request->content_len == 0 || request->content_len >= sizeof(body))
    {
        return httpd_resp_send_err(request, HTTPD_413_CONTENT_TOO_LARGE, "Provision request is too large");
    }

    size_t received_size = 0;
    int timeout_count = 0;

    // TCP数据可能被拆成多段，不能假设一次recv就能读完
    while (received_size < request->content_len)
    {
        int received = httpd_req_recv(request, body + received_size, request->content_len - received_size);

        if (received == HTTPD_SOCK_ERR_TIMEOUT)
        {
            timeout_count++;

            if (timeout_count >= 3)
            {
                return ESP_FAIL;
            }

            continue;
        }
        if (received <= 0)
        {
            return ESP_FAIL;
        }

        timeout_count = 0;
        received_size += (size_t)received;
    }

    body[received_size] = '\0';

    char encoded_ssid[PROVISION_ENCODED_SSID_SIZE] = {0};
    char encoded_password[PROVISION_ENCODED_PASSWORD_SIZE] = {0};

    esp_err_t err = httpd_query_key_value(body, "ssid", encoded_ssid, sizeof(encoded_ssid));
    if (err != ESP_OK)
    {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "SSID is missing or invalid");
    }

    err = httpd_query_key_value(body, "password", encoded_password, sizeof(encoded_password));

    if (err != ESP_OK)
    {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Password field is missing");
    }

    char ssid[PROVISION_SSID_SIZE] = {0};
    char password[PROVISION_PASSWORD_SIZE] = {0};

    err = decode_form_value(encoded_ssid, ssid, sizeof(ssid));

    if (err != ESP_OK)
    {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "SSID decoding failed");
    }

    err = decode_form_value(encoded_password, password, sizeof(password));

    if (err != ESP_OK)
    {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Password decoding failed");
    }

    err = s_config.provision_handler(ssid, password);

    if (err == ESP_ERR_INVALID_ARG)
    {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "WiFi credentials are invalid");
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Save provisioning credentails failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Save WiFi credentials failed");
    }

    // 设置POST成功页面的响应类型。
    // 这里不是portal_page_handler()中的GET响应。
    httpd_resp_set_type(request, "text/html; charset=utf-8");

    esp_err_t send_err = httpd_resp_send(
        request, 
        "<!doctype html><meta charset=\"utf-8\">"
        "<h1>配置已保存</h1>"
        "<p>ESP32正在重启并连接上游WiFi。</p>",
         HTTPD_RESP_USE_STRLEN);

    if (send_err != ESP_OK)
    {
        return send_err;
    }

    // 先给浏览器时间接收成功响应，再重启设备。
    vTaskDelay(pdMS_TO_TICKS(1500));

    esp_restart();
}

// 处理没有注册的HTTP路径，如果这些URI没有单独注册就会进入404处理器
static esp_err_t portal_not_found_handler(httpd_req_t *request, httpd_err_code_t error_code)
{
    // 当前暂时不需要使用错误码，避免编译器产生未使用参数警告
    (void)error_code;

    // 明确设置302，保证Location响应头会触发重定向
    httpd_resp_set_status(request, "302 Found");

    // 告诉客户端：资源位置发生变化，需要跳转到Portal根路径
    // 使用相对路径，不写死ESP32的IP。
    // 浏览器保留原请求的Host，只把路径修改为根路径。
    httpd_resp_set_hdr(request, "Location", "/");

    // 防止客户端缓存旧的跳转结果
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_type(request, "text/plain; charset=utf-8");

    return httpd_resp_send(request, "Redirecting to captive portal", HTTPD_RESP_USE_STRLEN);
}

// 注册一个GET类型的Portal地址
static esp_err_t register_portal_uri(httpd_handle_t server, const char *uri)
{
    httpd_uri_t handler = {
        .uri = uri,
        .method = HTTP_GET,
        .handler = portal_page_handler,
        .user_ctx = NULL
    };

    return httpd_register_uri_handler(server, &handler);
}

static esp_err_t register_provision_uri(httpd_handle_t server)
{
    httpd_uri_t handler = {
        .uri = "/api/provision",
        .method = HTTP_POST,
        .handler = provision_post_handler,
        .user_ctx = NULL
    };

    return httpd_register_uri_handler(server, &handler);
}

esp_err_t captive_portal_start(const captive_portal_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // 配网模式必须提供接收SSID和密码的处理函数。
    if (config->provisioning_mode && config->provision_handler == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_server != NULL)
    {
        return ESP_OK;
    }

    // 复制整个配置结构体，而不是保存调用方结构体的地址。
    s_config = *config;

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();

    // 当前需要注册根路径和几个系统联网检测路径
    http_config.max_uri_handlers = 8;

    esp_err_t err = httpd_start(&s_server, &http_config);
    if (err != ESP_OK)
    {
        return err;
    }

    // 注册 HTTP 404 兜底处理器。
    // 未注册的 URI 不再直接返回普通 404，
    // 而是跳转到 Portal 根页面。
    err = httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, portal_not_found_handler);

    if (err != ESP_OK)
    {
        // 如果错误处理器注册失败，当前HTTP服务是不完整的
        // 因此停止服务并清空句柄，避免留下半启动状态
        httpd_stop(s_server);
        s_server = NULL;

        return err;
    }
    
    // 常规Portal入口
    const char *portal_uris[] = {
        "/",

        // Android常见联网检测地址
        "/generate_204",

        // Apple设备常见联网检测地址
        "/hotspot-detect.html",

        // Windows常见联网检测地址
        "/connecttest.txt",
        "/ncsi.txt"

    };

    const size_t portal_uri_count = sizeof(portal_uris) / sizeof(portal_uris[0]);

    for (size_t i = 0; i < portal_uri_count; i++)
    {
        err = register_portal_uri(s_server, portal_uris[i]);

        if (err != ESP_OK)
        {
            // 部分URI注册失败时不能保留一个残缺的HTTP服务
            httpd_stop(s_server);
            s_server = NULL;

            return err;
        }
    }

    err = register_provision_uri(s_server);
    if (err != ESP_OK)
    {
        httpd_stop(s_server);
        s_server = NULL;

        return err;
    }
    

    // HTTP路由完整后，再启动DNS服务
    err = portal_dns_start();

    if (err != ESP_OK)
    {
        httpd_stop(s_server);
        s_server = NULL;

        return err;
    }

    ESP_LOGI(TAG, "Captive Portal HTTP server started");
    return ESP_OK;
}
