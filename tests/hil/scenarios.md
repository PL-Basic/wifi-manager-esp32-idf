# H2 Scenario Index

| ID | Trigger | Required evidence | Default state |
| --- | --- | --- | --- |
| H2-BOOT | Production boot | WiFi, MQTT, partition, and startup recovery result | NOT_RUN |
| H2-CMD-REPLAY | Duplicate requestId after reconnect | Original terminal result replayed with no repeated side effect | NOT_RUN |
| H2-KICK | KICK command and restart | Terminal result persisted before restart and replayed after boot | NOT_RUN |
| H2-WIFI-CANDIDATE | Candidate WiFi success and rollback paths | Promotion only after IP and a new MQTT generation | NOT_RUN |
| H2-MQTT-RECOVERY | Broker disconnect and reconnect | Bounded recovery and command/result continuity | NOT_RUN |
| H2-RESOURCE | Final production candidate | Flash, RAM, task stack, queue, and timing budgets | NOT_RUN |

Each execution record must identify the frozen Git SHA, production firmware
SHA-256, test device, approved port, scenario IDs, and final disposition.
