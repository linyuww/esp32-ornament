# ESP32 Codex Desktop Ornament Software Development Guide

## 1. Goal

This project builds an ESP32-S3 desktop ornament with an ST7789 SPI display. It shows Codex/Claude
task state, Codex quota, weather, time, and completion alerts.

The system has two software sides:

- PC side: `codex-ornament-bridge`, a local HTTP bridge that reads quota, receives Codex hooks, and
  exposes display state to ESP32.
- Device side: `firmware/esp32-ornament`, ESP-IDF firmware that connects to Wi-Fi, polls state,
  renders the display, and triggers alerts.

## 2. Development Plan

| Phase | Goal | Deliverable |
| --- | --- | --- |
| P0 | Define scope and boundaries | Requirements, architecture, API, security docs |
| P1 | Make PC bridge runnable | `/health`, `/quota`, `/state`, `/hook/codex` |
| P2 | Connect Codex hooks | `notify` and lifecycle hook forwarding script |
| P3 | Create ESP32 framework | Wi-Fi, HTTP, JSON, state model, display boundary |
| P4 | Bring up display | ST7789 SPI driver and first UI screen |
| P5 | Complete interaction | Web console, voice reminder, standby, and error states |
| P6 | Deploy and accept | Windows startup, firmware flashing, acceptance notes |

## 3. Document List

- `docs/software-engineering/00-plan.md`: overall plan.
- `docs/software-engineering/01-requirements.md`: software requirements specification.
- `docs/software-engineering/02-architecture.md`: software architecture design.
- `docs/software-engineering/03-api-contract.md`: HTTP API contract.
- `docs/software-engineering/04-data-design.md`: data design.
- `docs/software-engineering/05-security.md`: security design.
- `docs/software-engineering/06-test-plan.md`: test plan.
- `docs/software-engineering/07-deployment.md`: deployment guide.
- `docs/software-engineering/08-development-guide.md`: development rules.
- `docs/software-engineering/09-risk-register.md`: risk register.
- `docs/software-engineering/10-traceability.md`: requirements traceability matrix.
- `docs/software-engineering/11-hardware-software-interface.md`: hardware/software interface.
- `docs/esp32-ornament-bridge.md`: quick bridge and hook guide.
- `crates/codex-ornament-bridge/README.md`: PC bridge manual deployment.

## 4. Program Structure

```text
crates/codex-ornament-bridge
  src/main.rs                    PC local HTTP bridge

scripts/codex-ornament-hook.ps1  Codex hook forwarder

firmware/esp32-ornament
  main/app_main.c                Firmware entry and task orchestration
  main/wifi.c                    Wi-Fi connection
  main/bridge_client.c           HTTP polling and JSON parsing
  main/ornament_state.c          State model
  main/display.c                 ST7789 display boundary
  main/display_core.c            RGB565 drawing helpers
```

## 5. Local Run

```powershell
cd D:\Desktop\codex\codex-quota-widget
cargo run -p codex-ornament-bridge
```

Check service:

```powershell
Invoke-RestMethod http://127.0.0.1:8787/health
Invoke-RestMethod http://127.0.0.1:8787/state
```

Simulate a Codex completion event:

```powershell
Invoke-RestMethod -Method Post `
  -Uri http://127.0.0.1:8787/hook/codex `
  -ContentType application/json `
  -Body '{"hook_event_name":"Stop","message":"demo task complete"}'
```

## 6. Codex Hook Configuration

`%USERPROFILE%\.codex\config.toml`:

```toml
notify = [
  "powershell.exe",
  "-NoProfile",
  "-ExecutionPolicy",
  "Bypass",
  "-File",
  "D:\\Desktop\\codex\\codex-quota-widget\\scripts\\codex-ornament-hook.ps1"
]
```

`%USERPROFILE%\.codex\hooks.json`:

```json
{
  "hooks": {
    "UserPromptSubmit": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"D:\\Desktop\\codex\\codex-quota-widget\\scripts\\codex-ornament-hook.ps1\""
          }
        ]
      }
    ],
    "Stop": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"D:\\Desktop\\codex\\codex-quota-widget\\scripts\\codex-ornament-hook.ps1\""
          }
        ]
      }
    ]
  }
}
```

## 7. ESP32 Development Flow

1. Confirm the ST7789 SPI screen pinout.
2. Install ESP-IDF manually.
3. Enter `firmware/esp32-ornament`.
4. Run `idf.py set-target esp32s3`.
5. Run `idf.py menuconfig`, then configure Wi-Fi and bridge URL.
6. Run `idf.py build`.
7. Run `idf.py -p COMx flash monitor`.

Firmware implementation references:

- `https://github.com/espressif/esp-idf/tree/master/components/esp_lcd`
- `https://github.com/espressif/esp-idf/tree/master/examples/wifi/getting_started/station`
- `https://github.com/espressif/esp-idf/tree/master/examples/protocols/esp_http_client`

## 8. Acceptance Criteria

- ESP32 shows `done` within 5 seconds after a Codex task completes.
- ESP32 shows five-hour and weekly quota percentages.
- ESP32 renders an offline state only after consecutive bridge failures, so transient fetch errors do not flicker the panel.
- Codex keeps running normally when the hook script cannot reach the bridge.
- Credentials remain on the PC; the device does not store Codex tokens.

## 9. Code Review Commands

Use local tools only. Do not install an environment during review.

```powershell
cargo fmt --check -p codex-ornament-bridge
cargo check -p codex-ornament-bridge
cargo test -p codex-ornament-bridge
cargo test -p quota-core
cargo clippy -p codex-ornament-bridge -- -D warnings
cargo clippy -p quota-core -- -D warnings
```
