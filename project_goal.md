# Firmware-2 Project Goal

Build a fresh Zephyr application for two identical Nordic nRF52840 DK boards, each
connected to a Semtech SX1262 LoRa module over SPI and accessible through a UART shell.
Use Zephyr's Bluetooth and LoRa APIs. Treat the supplied local PDFs and SX1262
interconnect document as the initial hardware source of truth; derive and record any
necessary board setup state through the configured firmware MCP server.

## Distinct cross-radio demonstration

This project must use the radios in the reverse direction of the prior relay project:

1. Board A, in **initiator** mode, receives `bridge start <payload>` over its UART shell
   and sends an identified LoRa request to Board B.
2. Board B, in **responder** mode, continuously receives the LoRa request, validates it,
   and emits a BLE advertisement containing the same correlation ID and payload in a
   documented project frame. It prints a UART success marker.
3. Board A continuously scans BLE advertisements, accepts only the documented project
   response matching its outstanding correlation ID and payload, and prints a completion
   marker. A stale, malformed, foreign, or mismatched response must not complete a request.

The app must also expose manual shell commands that independently exercise LoRa send/receive
and BLE advertise/scan using the same production drivers as the bridge flow. It must expose
status showing roles, readiness, packet/drop counters, and active radio configuration.

## Acceptance evidence

- Fresh board setup and validation are recorded beneath this repository's `.firm/`; do not
  reuse board YAML or runtime state from Testing_Repo.
- The source builds from this repository with the selected local Nordic/Zephyr toolchain.
- Both boards boot and their UART shells respond.
- Manual LoRa and BLE exchanges succeed in both relevant receive paths.
- The reverse LoRa-to-BLE bridge succeeds repeatedly on real attached boards, with exact
  matching correlation IDs and payloads at both endpoints.
- Evidence includes artifact identity, board-to-connection mapping, commands, UART output,
  and any limitations. A successful flash alone is not behavior proof.