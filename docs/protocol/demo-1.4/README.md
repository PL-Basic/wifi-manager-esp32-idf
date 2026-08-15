# Demo 1.4 MQTT protocol fixtures

`mqtt-protocol-v1.json` is the firmware-local copy of the frozen backend
contract. It covers the six production commands, command-result success and
failure, status, client-signal, client-disconnect, and the traffic shape.

Traffic remains `BACKEND_CONSUMER_ONLY`: this directory does not claim that the
firmware publishes `/event/traffic`.

`negative-samples-v1.json` fixes the highest-risk invalid inputs. WiFi
credentials use only the invalid test value `invalid-test-password`.

Unity parsing coverage is in `test/test_app_command`. NVS isolation remains in
`test/test_nvs_boundary`; hardware execution is required before claiming
terminal-cache or dual-slot persistence behavior from these fixtures.
