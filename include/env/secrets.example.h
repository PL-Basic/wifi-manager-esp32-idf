#pragma once

#define PORTAL_EXTERNAL_URL "https://portal.example.com/"
#define PORTAL_EXTERNAL_DOMAIN "portal.example.com"
#define PORTAL_SERVER_IPV4 "your_portal_server_ipv4"

// 普通客户端认证热点：保持开放，无需WiFi密码。
#define AP_SSID "WifiManager-ESP32"
#define AP_PASSWORD ""

// 管理员本地配网热点：使用独立的WPA2密码。
#define PROVISION_AP_SSID "WifiManager-Setup"
#define PROVISION_AP_PASSWORD "change-this-password"

#define MQTT_BROKER_URI "mqtt://192.168.137.1:1883"
#define DEVICE_CODE "esp32-gateway-001"
