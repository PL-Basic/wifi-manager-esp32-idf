# H1 ESP32 Component Test Application

This directory is the only target-board component test application. Its
PlatformIO configuration, build output, partition table, and sdkconfig are
isolated from the production firmware.

The CMake component allowlist starts with `main` and adds only its declared
dependencies:

- `unity`
- `app_command`
- `app_command_coordinator`
- `app_storage`
- `nvs_flash`
- `esp_partition`
- transitive dependencies declared by those components

The application uses the ESP-IDF Unity menu. One successful build and upload
can run any number of groups without recompilation:

- `[app_command]`
- `[app_command_coordinator]`
- `[app_storage]`
- `*` for all H1 cases

Commands require the firmware integration gate and an explicitly confirmed
test port:

```powershell
platformio run -e h1-esp32dev
platformio run -e h1-esp32dev --target upload --upload-port <CONFIRMED_PORT>
platformio device monitor --port <CONFIRMED_PORT> --baud 115200
```

The test image may erase only `nvs_test`. It must not initialize, open, erase,
or rewrite the production `nvs` partition.
