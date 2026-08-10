# Firmware Test Entry

The permanent firmware suites use the dedicated `esp32dev-test` environment.
Test discovery is read-only:

```powershell
platformio test --list-tests -e esp32dev-test
```

Do not run, upload, or monitor a suite until the test device and serial port
have been explicitly confirmed. Run one named suite at a time and preserve the
JUnit output:

```powershell
platformio test -e esp32dev-test -f test_app_command `
  --upload-port <CONFIRMED_TEST_PORT> `
  --test-port <CONFIRMED_TEST_PORT> `
  --junit-output-path test-results/test_app_command.xml
```

`test_nvs_boundary` may erase only the `nvs_test` partition and uses only the
`wifi_test` namespace. It must never initialize, open, erase, or rewrite the
default `nvs` partition that stores real credentials and command state.
