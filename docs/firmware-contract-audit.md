# Firmware contract audit

P-0 audit baseline for the ESP-IDF firmware and the Java backend. This file records the implemented contract; it does not introduce a new protocol version.

## Build and runtime baseline

- PlatformIO default environment: `esp32dev`.
- Framework: ESP-IDF, currently resolved as 6.0.1.
- Default upload and monitor port: `COM7`; monitor speed: 115200.
- MQTT client subscribes after connection and relies on ESP-MQTT reconnect behavior.
- Command subscription and all event publications use QoS 1 and are not retained.
- Maximum incoming command topic length is 127 bytes; maximum assembled payload length is 2047 bytes.

## Topics

Backend to firmware:

- `wifi/device/{deviceCode}/cmd/allow`
- `wifi/device/{deviceCode}/cmd/revoke-access`
- `wifi/device/{deviceCode}/cmd/kick`
- `wifi/device/{deviceCode}/cmd/disconnect-mac`
- `wifi/device/{deviceCode}/cmd/block-traffic`
- `wifi/device/{deviceCode}/cmd/stage-wifi-config`

Firmware to backend:

- `wifi/device/{deviceCode}/event/status`
- `wifi/device/{deviceCode}/event/traffic`
- `wifi/device/{deviceCode}/event/client-signal`
- `wifi/device/{deviceCode}/event/client-disconnect`
- `wifi/device/{deviceCode}/event/command-result`

The backend treats the topic device code as the trusted identity and rejects a conflicting payload `deviceCode`.

## Command payload rules

- Shared `requestId`: optional for the legacy commands, maximum 63 visible bytes. It is required and non-empty for `STAGE_WIFI_CONFIG`.
- `ALLOW`: requires `mac`, positive `sessionId`, and `ttlSeconds` in 1..86400.
- `REVOKE_ACCESS`: requires `mac` and positive `sessionId`.
- `DISCONNECT_MAC`: requires `mac` and `alertId >= 0`.
- `KICK`: requires `deviceCode`; `reason` is optional in the backend contract.
- `BLOCK_TRAFFIC`: requires `alertId >= 0` and `dstIp`; `sni` is optional. Firmware buffers allow 45 visible bytes for `dstIp` and 255 for `sni`.
- `STAGE_WIFI_CONFIG`: requires non-empty `requestId`, `deviceCode`, and `ssid`, plus `password` and integral `configVersion` in 1..4294967295. SSID is 1..32 bytes. Password is empty for an open network or 8..63 bytes.

Firmware command result fields are `deviceCode`, `requestId`, `type`, `success`, and `message`. The result message buffer allows 95 visible bytes.

## Portal configuration

The checked-in example is a development-LAN template:

- URL: `http://portal.test:5173/portal`
- Domain: `portal.test`
- IPv4: replace `your_portal_server_ipv4` with the frontend host address reachable by clients connected to the ESP32 SoftAP.

The client device must resolve `portal.test` to that IPv4 address. Production deployments must use the real HTTPS Portal URL, its exact hostname, and matching DNS/TLS configuration.

## Deferred protocol debt

- P-3B: the firmware currently stores the KICK reason through the command request MAC buffer. The wire payload remains compatible, but the internal field ownership is misleading and length-limited. Correct it only in the planned P-3B protocol stage.
- `PING` and `GET_STATUS` exist as internal command types, but the current topic parser accepts only the six production command topics listed above. Do not advertise them as externally supported until both publisher and parser contracts are implemented.
