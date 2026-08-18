# H0 Host Tests

H0 compiles pure C logic with the host compiler. It does not load ESP-IDF,
PlatformIO, a serial port, or device storage.

The build cache is local to this directory:

```powershell
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Only logic without ESP-IDF runtime semantics belongs here. NVS, FreeRTOS,
partition, WiFi, MQTT, and restart behavior must use H1 or H2.
