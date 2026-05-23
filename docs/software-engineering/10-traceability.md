# Requirements Traceability Matrix

| Requirement | Implementation | Test |
| --- | --- | --- |
| FR-01 | `GET /health` in `codex-ornament-bridge` | Health endpoint manual check |
| FR-02 | `GET /quota` using `quota-core` | Quota checks |
| FR-03 | `GET /state` merged response | State endpoint manual check |
| FR-04 | `POST /hook/codex` | POST hook tests |
| FR-05 | `codex-ornament-hook.ps1` argv handling | Notify manual test |
| FR-06 | `codex-ornament-hook.ps1` stdin handling | Lifecycle manual test |
| FR-07 | `wifi.c` | Wi-Fi monitor log |
| FR-08 | `bridge_client.c` polling task | Bridge restart test |
| FR-09 | `parse_state_json` | JSON fixture test, planned |
| FR-10 | `display.c` render boundary | Display bring-up test |
| FR-11 | Alert behavior | Planned after buzzer wiring |
