# Firmware Verification Layers

- `host/`: H0 pure logic tests, built with CMake and CTest.
- `../test_apps/component_test/`: H1 single ESP32 component test image.
- `hil/`: H2 production-firmware hardware scenarios and evidence index.

Task and stage identifiers do not select a test environment. Changes are
mapped to H0, H1, and H2 by affected behavior.
