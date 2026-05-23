# Security Design

## Assets

- Codex access token in `%USERPROFILE%\.codex\auth.json`.
- ChatGPT account id.
- Codex task prompt and completion excerpts.
- Local LAN access to bridge state.

## Controls

- ESP32 never receives Codex tokens.
- Bridge accepts POST without token only from loopback.
- Optional `CODEX_ORNAMENT_TOKEN` protects LAN POST.
- ESP32 only needs `GET /state`.
- Hook script suppresses network errors to avoid interrupting Codex work.

## Risks

R-01: Anyone on the LAN can read `GET /state` unless network isolation is added.

R-02: The quota method uses the ChatGPT web backend endpoint and local Codex auth. This is practical but less stable than a formal public API.

R-03: Task message excerpts may leak sensitive prompt text onto the display.

## Recommended Hardening

- Bind the bridge to the PC LAN IP instead of `0.0.0.0` if the network is not trusted.
- Add token protection for `GET /state` if the ESP32 firmware can keep a shared token.
- Redact task messages in `normalize_event` before public display.
- Keep Windows firewall limited to private network only.
