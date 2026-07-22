# Firmware-2 reverse LoRa-to-BLE bridge

This Zephyr application runs on two nRF52840-QIAA boards with SX1262 radios.
Board A sends identified LoRa requests; Board B receives them and advertises
matching BLE responses. Board A scans, validates the correlation ID and payload,
and completes the bridge only for an exact match.

## Build

From the repository root, run:

```text
scripts\build.cmd
```

The entry point uses the installed NCS/toolchain, fetches the official
LoRa Basics Modem dependency at the pinned revision, checks it out under
`modules\lora-basics-modem`, sets `ZEPHYR_EXTRA_MODULES` to that repository-local
path, and performs a pristine `nrf52840dk/nrf52840` build. Set `NCS_ROOT` or
`NCS_TOOLCHAIN_ROOT` if the local installations are elsewhere.

## Shell commands

```text
status
role initiator|responder
lora send <payload>
ble adv <hex-id> <payload>
ble scan
bridge start <payload>
```

`status` reports radio/BLE readiness, roles, counters, and mismatch drops.

## Startup runbook

Both images boot as initiators. After every reset or power cycle, configure the
pair before using the bridge:

```text
# Board A / COM16
role initiator

# Board B / COM15
role responder
```

Then confirm `status` reports Board A scanning and Board B responding. Manual
LoRa and BLE commands use the same production driver paths as bridge traffic.
