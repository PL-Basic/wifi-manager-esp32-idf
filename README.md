# wifi-manager-esp32-idf

Wifi Manager 项目的 ESP32 网关固件。

本仓库是 Wifi Manager 系统中的嵌入式网关端，基于 ESP32、ESP-IDF 和 PlatformIO 开发。

## 当前已验证功能

- ESP32 通过 STA 模式连接上游 WiFi。
- ESP32 通过 SoftAP 模式向下游设备开放热点。
- 已开启 IPv4 NAPT 转发，下游设备可以通过 ESP32 访问外网。
- 已采集设备状态，并将设备状态序列化为 JSON。
- 已通过 MQTT 将设备状态上报到 broker，并被后端消费。
- 已订阅 MQTT 命令 topic。
- 已验证 ESP32 能接收下行 MQTT 命令，并通过命令回调分发。

## 当前阶段

- 状态上报链路已验证：

```text
ESP32 -> MQTT broker -> 后端 -> 数据库
```

- 命令下发链路已验证：

```text
后端 / broker -> MQTT -> ESP32
```

- 命令执行逻辑尚未实现。当前命令回调只用于验证命令接收和分发。

## 项目结构

```text
src/
  main.c

components/
  app_storage/      NVS 初始化
  wifi_gateway/     STA + SoftAP + NAPT 网关逻辑
  device_status/    设备状态快照与 JSON 序列化
  app_mqtt/         MQTT 连接、发布、订阅与命令回调

include/
  env/
    secrets.example.h
    secrets.h       本地私有配置，已被 git 忽略
```

## 构建

```powershell
& 'C:\Users\45333\.platformio\penv\Scripts\platformio.exe' run
```

## 烧录

```powershell
& 'C:\Users\45333\.platformio\penv\Scripts\platformio.exe' run --target upload
```

## 本地配置

复制配置模板：

```text
include/env/secrets.example.h
```

为：

```text
include/env/secrets.h
```

然后填写本地 WiFi、SoftAP、MQTT broker 和设备编号。

`include/env/secrets.h` 已被 `.gitignore` 忽略，不会上传到仓库。

需要配置的本地值：

- 上游 WiFi SSID / 密码
- ESP32 SoftAP SSID / 密码
- MQTT broker URI
- 设备编号

## MQTT Topic

设备状态上报：

```text
wifi/device/{DEVICE_CODE}/event/status
```

设备命令订阅：

```text
wifi/device/{DEVICE_CODE}/command/#
```
