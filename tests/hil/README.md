# H2 Production Firmware HIL

H2 validates the production `esp32dev` image. It is separate from the H1
component test image and uses the root `platformio.ini` production environment.

Every H2 run requires:

1. a frozen production candidate and a successful production build;
2. an explicitly confirmed device and serial port;
3. an approved scenario set from `scenarios.md`;
4. one upload followed by all approved scenarios on that same image;
5. captured firmware hash, resource usage, serial evidence, and result.

Do not place credentials in evidence. Do not erase production NVS unless the
approved scenario explicitly requires a disposable test device reset.
