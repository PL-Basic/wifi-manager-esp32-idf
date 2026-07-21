# wifi-manager-esp32-idf

WiFi Manager 系统的 ESP32 嵌入式网关固件。基于 ESP-IDF 6.0.1 和 PlatformIO 构建。

本固件将 ESP32 变为一台可远程管理的 WiFi 网关：上游连接路由器，下游开放热点，通过 MQTT 与 Java 后端通信，实现客户端认证、访问控制、命令执行和状态上报。

## 系统架构

```
手机/电脑 ─→ ESP32 SoftAP ─→ ESP32 STA ─→ 上游路由器 ─→ 互联网
                │                    │
                ▼                    ▼
         Captive Portal         MQTT Broker
         (认证页面)                 │
                              ┌─────┴─────┐
                              ▼           ▼
                         Java 后端    Vue 前端
```

## 功能概览

**网关基础**：APSTA 双模运行，lwIP NAPT 转发，下游设备通过 ESP32 上网。

**访问控制**：客户端连接后默认禁止外网。后端通过 MQTT 下发 ALLOW 放行指定客户端，数据包过滤器在 lwIP 层拦截未授权流量。

**客户端认证**：连接开放热点后弹出 Portal 页面，认证流程由后端和前端配合完成，固件负责接收 MQTT 放行指令并执行。

**会话管理**：每个授权有 TTL（1-86400 秒），到期自动撤销。双重过期机制：数据包到达时即时检查（惰性），每 10 秒扫描兜底（周期性）。

**MQTT 命令**：支持 5 种命令——ALLOW（放行客户端）、DISCONNECT_MAC（断开客户端）、REVOKE_ACCESS（撤销认证）、KICK（设备复位）、BLOCK_TRAFFIC（流量阻断）。

**上游 WiFi 恢复**：密码错误自动清凭据进配网模式。AP 暂时不可达（路由器关机）保留凭据，每次重启自动重试，多次失败后切换配网页面等人操作。

**本地配网**：NVS 无凭据或 AP 不可达时，Portal 页面切换为配网表单，浏览器访问热点 IP 即可修改上游 WiFi 凭据，保存后自动重启连接。

## 项目结构

```
src/
  main.c                       # 入口，模块编排，主循环，命令分发

components/
  wifi_gateway/                # STA + SoftAP + NAPT，WiFi 事件处理，状态机
  client_access/               # 客户端 MAC/IP/状态表，TTL 授权与过期
  access_filter/               # lwIP IPv4 输入钩子，按源 IP 过滤外网数据包
  captive_portal/              # HTTP Server，DNS 劫持（UDP 53），Portal 页面，配网表单
  portal_dns.c/h               #   独立的 DNS 子模块，支持外部 Portal 域名例外
  app_mqtt/                    # MQTT 连接、发布、订阅、命令回调分发
  app_command/                 # 从 MQTT topic 识别命令类型，解析 JSON payload
  device_status/               # 设备状态快照与 JSON 序列化
  app_storage/                 # NVS 凭据存储、恢复标记、重试计数器

include/env/
  secrets.example.h            # 配置模板（复制为 secrets.h 后填写）
```

## 快速开始

### 环境准备

VS Code 安装 PlatformIO 扩展。用 PlatformIO 打开本项目。

### 配置

复制 `include/env/secrets.example.h` 为 `include/env/secrets.h`，填写以下内容：

```c
// 开放热点（客户端连接这个）
#define AP_SSID "WifiManager-ESP32"
#define AP_PASSWORD ""

// 配网热点（管理员配网用）
#define PROVISION_AP_SSID "WifiManager-Setup"
#define PROVISION_AP_PASSWORD "change-this-password"

// MQTT 服务器
#define MQTT_BROKER_URI "mqtt://192.168.137.1:1883"
#define DEVICE_CODE "esp32-gateway-001"

// 外部 Portal 地址（后续前端实现后可配置真实地址）
#define PORTAL_EXTERNAL_URL "https://portal.example.com/"
#define PORTAL_EXTERNAL_DOMAIN "portal.example.com"
#define PORTAL_SERVER_IPV4 "192.168.137.1"
```

### 构建与烧录

VS Code 底部状态栏 PlatformIO 工具栏：

- **编译**：点击 ✓（Build）
- **烧录**：点击 →（Upload）
- **串口监视**：点击 🔌（Monitor）

首次配网时设备没有上游凭据，会自动启动配网热点 `WifiManager-Setup`。手机连接后浏览器访问 `http://192.168.4.1/` 输入上游 WiFi 凭据即可。

## 配网流程

设备启动时检查 NVS 中是否有上游凭据：

**无凭据**：启动配网热点 `WifiManager-Setup`（需密码），Portal 显示配网表单。用户填写 SSID 和密码提交后自动保存到 NVS 并重启。

**有凭据**：启动开放热点 `WifiManager-ESP32`（无密码），STA 尝试连接上游。连接成功后 DHCP 分配 IP，MQTT 上线。

**凭据错误**（密码不对）：ESP32 连续认证失败 3 次后清空 NVS 凭据并重启，进入配网模式。

**AP 不可达**（路由器关机）：连续扫描不到 AP 3 次后保留凭据，Portal 页面切换为配网表单。用户可打开路由器等待下次启动自动重连，或直接修改凭据换网。每次重启都会先尝试一次旧凭据。

## MQTT 协议

### 状态上报

设备每 10 秒通过以下 topic 上报状态（QoS 1）：

```
wifi/device/{deviceCode}/event/status
```

```json
{
  "deviceCode": "esp32-gateway-001",
  "status": 1,
  "wifiStatus": "STA_GOT_IP",
  "ip": "192.168.137.248",
  "currentClients": 2,
  "firmwareVersion": "0.1.0"
}
```

命令执行结果：

```
wifi/device/{deviceCode}/event/command-result
```

```json
{
  "deviceCode": "esp32-gateway-001",
  "requestId": "cmd-001",
  "type": "ALLOW",
  "success": true,
  "message": "client access state authorized"
}
```

### 命令下发

所有命令 topic 格式为 `wifi/device/{deviceCode}/cmd/{命令名}`，QoS 1。

**ALLOW** — 放行客户端上网

Topic: `cmd/allow`

```json
{"requestId":"req-1","mac":"AA:BB:CC:DD:EE:FF","sessionId":1001,"ttlSeconds":1800}
```

`ttlSeconds` 为必填，范围 1-86400。

**DISCONNECT_MAC** — 断开指定客户端

Topic: `cmd/disconnect-mac`

```json
{"mac":"AA:BB:CC:DD:EE:FF","alertId":2001}
```

**REVOKE_ACCESS** — 撤销客户端认证（不断开连接）

Topic: `cmd/revoke-access`

```json
{"mac":"AA:BB:CC:DD:EE:FF","sessionId":1001}
```

**KICK** — 设备安全复位

Topic: `cmd/kick`

```json
{"deviceCode":"esp32-gateway-001","reason":"security incident"}
```

固件收到后先返回 command-result，1 秒后执行 `esp_restart()`。

**BLOCK_TRAFFIC** — 全局流量阻断（记录参数，真实阻断待扩展）

Topic: `cmd/block-traffic`

```json
{"alertId":3001,"dstIp":"93.184.216.34"}
```

## 测试指南

### 准备

- MQTT 客户端工具（如 MQTTX，或命令行 `mosquitto_sub` / `mosquitto_pub`）
- 手机一台，连接到 `WifiManager-ESP32` 开放热点
- ESP32 串口监视器（VS Code PlatformIO → 🔌 Monitor）

### 1. 设备状态上报验证

烧录后等待 STA 获取 IP 并连接 MQTT。订阅状态 topic：

```
wifi/device/esp32-gateway-001/event/status
```

每 10 秒收到一条 JSON。确认包含 `"status":1`（STA 有 IP 时）和 `"wifiStatus":"STA_GOT_IP"`。

### 2. Portal 页面可用性

手机连接 `WifiManager-ESP32` 后，打开浏览器访问 `http://192.168.4.1/`，应显示 Portal 页面。此时手机无法访问外网（未认证）。

### 3. ALLOW 放行与 TTL 过期

从串口日志中找到手机 MAC 地址（`Client tracked, mac=XX:XX:XX:XX:XX:XX`）。

下发 ALLOW（30 秒授权）：

```bash
mosquitto_pub -h 192.168.137.1 -p 1883 \
  -t "wifi/device/esp32-gateway-001/cmd/allow" \
  -m '{"requestId":"t1","mac":"22:CF:F6:0A:25:CC","sessionId":1001,"ttlSeconds":30}' -q 1
```

手机立即恢复外网访问。订阅 `event/command-result` 应收到 `"type":"ALLOW","success":true`。

30 秒后手机刷新外网页面，应无法访问。串口日志出现 `Client authorization expired`。

**周期过期测试**：下发 ttlSeconds=10 的 ALLOW，立即锁屏手机（停止网络活动）。15-20 秒后日志出现 `Client authorization expired (periodic)`，解锁后手机无法上网。

### 4. DISCONNECT_MAC 断开客户端

```bash
mosquitto_pub -h 192.168.137.1 -p 1883 \
  -t "wifi/device/esp32-gateway-001/cmd/disconnect-mac" \
  -m '{"mac":"22:CF:F6:0A:25:CC","alertId":2001}' -q 1
```

手机 WiFi 断开后自动重连，状态重置为 UNAUTHORIZED。

### 5. KICK 设备复位

```bash
mosquitto_pub -h 192.168.137.1 -p 1883 \
  -t "wifi/device/esp32-gateway-001/cmd/kick" \
  -m '{"deviceCode":"esp32-gateway-001","reason":"manual test"}' -q 1
```

`command-result` 返回 `"type":"KICK","success":true`，ESP32 在 1 秒后重启。

### 6. 上游 WiFi 不可达恢复

在配网页面提交一个不存在的 SSID。ESP32 尝试连接，3 次断线后 Portal 自动切换为配网页面，设备不重启。再次提交真实 WiFi 凭据后保存并重启，正常连接。

## 发布包使用

`release/` 目录下的固件包为合并镜像，包含 bootloader、分区表和应用程序，可直接用 esptool 烧录，无需安装 PlatformIO。

烧录命令（COM 口根据实际修改）：

```bash
esptool.py --chip esp32 --port COM7 --baud 460800 write_flash 0x0 wifi-manager-esp32-v0.1.0.bin
```


## 开源协议

Apache License 2.0，详见 [LICENSE](LICENSE)。
