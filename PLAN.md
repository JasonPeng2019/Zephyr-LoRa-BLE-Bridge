# Firmware-2 acceptance trial plan and evidence

Status: complete; both boards are deliberately running `artifacts/final-hil-lifecycle/firmware.elf`.

## Hardware and setup

- MCU: nRF52840-QIAA, established from `Nano_BLE_MCU-nRF52840_PS_v1.1.pdf`.
- Radio: SX1262; XIAO/Wio wiring composed from image5/image6 of `SX1262InterCon.docx`:
  MOSI=P1.15, MISO=P1.14, SCK=P1.13, NSS=P0.04, BUSY=P0.29, RESET=P0.28,
  DIO1=P0.03, exposed RF_SW=P0.05.
- Overlay enables SX1262 DIO2 antenna-switch control, DIO3 1.8 V TCXO control, and
  DCDC mode (no `regulator-ldo`). The final application uses US915 915000000 Hz.
- Board A: probe ending 710208, COM16. Board B: probe ending 854191, COM15.
  Both profiles and live silicon validation passed through the BYO-Firmware MCP server.

## Backend and dependency

- Local NCS/Zephyr: `C:\ncs\v3.3.1`; target `nrf52840dk/nrf52840`.
- The final build selects `CONFIG_LORA_MODULE_BACKEND_LORA_BASICS_MODEM=y`,
  `CONFIG_LORA_SX126X=y`, `CONFIG_LORA_BASICS_MODEM_ASYNC_RX_MAX_PAYLOAD=64`, and deferred init.
- `scripts/fetch_lora_basics_modem.ps1` pins the official dependency to
  `a8ddc544043e72807cf7db532478e1dda734ae7c`; builds use it through `ZEPHYR_EXTRA_MODULES`.
- The prior loramac-node image was rejected as unsuitable after its unbounded SX126x busy wait;
  it is not used by the final application.

## Canonical artifacts

- Corrected lifecycle/ID-guard ELF: `artifacts/final-hil-lifecycle/firmware.elf`, SHA-256
  `b01310a25493809954af87f8440831176072f76994fddbbe566fab93b4f4bf1c`.
  HEX SHA-256 `6adfda15c21b003aab3ca5a4a3581d7b041a2c55dafb8a9a99e3ede364e149ab`;
  BIN SHA-256 `8ff193ab347c96cdf9bdd32552659db6c17357bbe2b5dd3a0b2646f3aa7444bd`;
  MAP SHA-256 `f4865871cfcf5367c3c08c37061eb08c85ae9bddbbb7d890a3fa40f5e937eb72`.

## HIL evidence

- Both final boots reported `RB2_BOOT`, corrected wiring, `LORA_READY freq=915000000 bw=125 sf=9`,
  native split-controller Bluetooth startup, `BLE_READY`, and `BLE_SCAN_READY`.
- Corrected lifecycle manual LoRa: A reported `LORA_SEND rc=0 payload=manual`; B status reported
  `lora_rx=1`. B then reported `LORA_SEND rc=0 payload=return`; A status advanced to `lora_rx=2`,
  proving A resumed receive after its TX.
- Manual BLE mismatch: B reported `BLE_ADV rc=0 id=00000000 payload=three`; A captured
  `BLE_RX id=00000000 payload=three` while its status retained `ble_match=3` and increased
  `ble_drop` from 13 to 18. Reserved ID 0 therefore cannot falsely complete a request.
- Final bridge, with B in responder role and A in initiator role, completed repeatedly:
  - ID `00000001`, payload `one`: A `BRIDGE_SENT`; B status `lora_rx=2`, `ble_adv=2`; A status
    `ble_match=1`.
  - ID `00000002`, payload `two`: B status `lora_rx=3`, `lora_drop=0`, `ble_adv=3`; A status
    `ble_match=2`.
  - ID `00000003`, payload `three`: B status `lora_rx=4`, `lora_drop=0`, `ble_adv=4`; A status
    `ble_match=3`.
  The exact ID/payload matcher increments only on equality; mismatched BLE traffic accumulated
  in `ble_drop` and never incremented `ble_match`.
- Both UART shells responded to `status`, role assignment, manual LoRa, manual BLE, and bridge
  commands. Both boards were left running the final image after the last observation.

## Final verification command

```text
scripts\build.cmd
```

This fetches the pinned module into `modules\lora-basics-modem`, sets
`ZEPHYR_EXTRA_MODULES` to that repository-local path, and performs the pristine
`nrf52840dk/nrf52840` build. Override `NCS_ROOT` or `NCS_TOOLCHAIN_ROOT` when
the installed NCS/toolchain uses another location.

## Independent manager verification

Using the same canonical ELF `artifacts/final-hil-lifecycle/firmware.elf`:

- Manual LoRa A→B and B→A both incremented the receiver counters.
- Exact bridge completions succeeded for `posttx`, `verify-two`, and `verify-four`.
- After the successful bridge, advertising ID `0` with the completed payload left
  `ble_match` unchanged and increased `ble_drop`.
- For `verify-four`, Board B advanced `lora_rx` 5→6 and `ble_adv` 5→6; Board A
  advanced `ble_match` 3→4.
- Final runtime roles were Board A initiator with scanning on and Board B responder.
