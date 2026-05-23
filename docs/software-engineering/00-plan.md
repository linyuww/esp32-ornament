# Codex ESP32 Desktop Ornament Plan

## Goal

Build a desktop ornament based on ESP32-S3 and a round ST77916 screen. It shows Codex quota, displays
Codex task state, and alerts when a Codex task finishes.

## Scope

- PC bridge service: local HTTP server, Codex hook receiver, quota snapshot provider.
- Codex hook script: forwards `notify` and lifecycle hook payloads to the bridge.
- ESP32 firmware: Wi-Fi connection, HTTP polling, JSON parsing, display rendering framework.
- Documentation: requirements, design, API, testing, deployment, security, operations.

## Milestones

1. PC bridge skeleton: `/health`, `/quota`, `/state`, `/hook/codex`.
2. Hook forwarding: PowerShell script compatible with `notify` argument and lifecycle stdin.
3. ESP32 firmware skeleton: Wi-Fi, HTTP client, parsed state model, display stub.
4. Display bring-up: confirm screen pinout, implement ST77916 SPI/QSPI driver.
5. Alert behavior: buzzer/LED/button acknowledgement.
6. Packaging: Windows startup task and firmware flash guide.

## Open Decisions

- Exact ST77916 interface: SPI or QSPI.
- Exact 16P FPC pinout and backlight current path.
- Whether PC bridge should be integrated into the Tauri widget later or kept as a separate daemon.
