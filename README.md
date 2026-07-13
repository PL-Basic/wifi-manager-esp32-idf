# wifi-manager-esp32-idf

ESP-IDF gateway firmware for Wifi Manager.

This project is the embedded gateway side of the Wifi Manager system. It runs on ESP32 with ESP-IDF through PlatformIO.

Current verified features:

- ESP32 STA connects to an upstream WiFi network.
- ESP32 SoftAP allows downstream clients to connect.
- IPv4 NAPT forwarding is enabled, so downstream clients can access the Internet through ESP32.
- Device status is collected and serialized as JSON.
- Device status is published to MQTT and consumed by the backend.
- MQTT command topic is subscribed.
- Downstream MQTT commands can be received by ESP32 and dispatched through a command callback.

Current stage:

- Status reporting path is verified: ESP32 -> MQTT broker -> backend -> database.
- Command receiving path is verified: broker/backend -> MQTT -> ESP32.
- Command execution is not implemented yet. The current command callback only verifies receipt and dispatch.

Project structure:

```text
src/
  main.c

components/
  app_storage/      NVS initialization
  wifi_gateway/     STA + SoftAP + NAPT gateway logic
  device_status/    device status snapshot and JSON serialization
  app_mqtt/         MQTT connect, publish, subscribe, and command callback

include/
  env/
    secrets.example.h
    secrets.h       local-only config, ignored by git
```

Build:

```powershell
& 'C:\Users\45333\.platformio\penv\Scripts\platformio.exe' run
```

Upload:

```powershell
& 'C:\Users\45333\.platformio\penv\Scripts\platformio.exe' run --target upload
```

## Local config

Copy `include/env/secrets.example.h` to `include/env/secrets.h` and fill your local WiFi values.

`include/env/secrets.h` is ignored by git.

Required local values:

- upstream WiFi SSID/password
- ESP32 SoftAP SSID/password
- MQTT broker URI
- device code

## MQTT topics

Status report:

```text
wifi/device/{DEVICE_CODE}/event/status
```

Command subscription:

```text
wifi/device/{DEVICE_CODE}/command/#
```
