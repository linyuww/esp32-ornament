# Software Requirements Specification

## Functional Requirements

FR-01: The PC bridge shall expose `GET /health` for service health checks.

FR-02: The PC bridge shall expose `GET /quota` using the existing `quota-core` quota retrieval logic.

FR-03: The PC bridge shall expose `GET /state` combining the latest task event and quota snapshot.

FR-04: The PC bridge shall accept `POST /hook/codex` from local Codex hook scripts.

FR-05: The hook script shall support Codex `notify`, where Codex passes one JSON string argument.

FR-06: The hook script shall support lifecycle hooks, where Codex sends JSON on stdin.

FR-07: ESP32 firmware shall connect to a configured Wi-Fi network.

FR-08: ESP32 firmware shall poll the configured bridge state URL at a configurable interval.

FR-09: ESP32 firmware shall parse task status, task message, quota status, five-hour quota, and weekly quota.

FR-10: ESP32 firmware shall render boot, connected, error, running, and done states.

FR-11: The system shall trigger a visual or audible alert when task status changes to `done`.

## Non-Functional Requirements

NFR-01: Credentials must remain on the PC. ESP32 must never store Codex auth tokens.

NFR-02: The bridge must accept unauthenticated POST only from loopback.

NFR-03: LAN POST requests must require `X-Codex-Ornament-Token` when configured.

NFR-04: ESP32 polling payloads should remain under 8 KB.

NFR-05: ESP32 shall tolerate bridge downtime and keep rendering a clear offline state.

NFR-06: PC bridge code should avoid adding heavy dependencies unless needed.

NFR-07: Documentation must be sufficient for a second developer to build, flash, and test the system.

## Assumptions

- Codex is logged in locally and `%USERPROFILE%\.codex\auth.json` exists.
- The quota endpoint format remains compatible with `quota-core`.
- The PC and ESP32 are on the same trusted LAN.
- The round screen is electrically compatible with ESP32-S3 3.3 V IO.
