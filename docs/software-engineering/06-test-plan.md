# Test Plan

## PC Bridge Unit Checks

- `GET /health` returns `ok`.
- `POST /hook/codex` with `{"hook_event_name":"UserPromptSubmit"}` changes `/state.status` to `running`.
- `POST /hook/codex` with `{"hook_event_name":"Stop"}` changes `/state.status` to `done`.
- Invalid JSON maps to `error`.
- LAN POST without token is rejected when not loopback.

## Quota Checks

- With valid auth, `/quota.status` is `ok`.
- With missing auth, `/quota.status` is `auth_required`.
- With changed response shape, `/quota.status` is `no_data` or `parse_error`.

## ESP32 Firmware Checks

- Wi-Fi connects with configured SSID/password.
- Bridge downtime renders offline state.
- Valid `/state` JSON populates task title/message and quota percentages.
- Oversized response is rejected without heap corruption.
- Polling interval respects `CONFIG_ORNAMENT_POLL_INTERVAL_MS`.

## Hardware Checks

- ST77916 shows solid color test.
- Backlight PWM works without flicker.
- Buzzer alert triggers once on `done`.
- Button acknowledges and silences current alert.

## Acceptance Criteria

- A Codex completed turn appears on the round display within 5 seconds.
- Five-hour and weekly quota percentages appear on the display.
- ESP32 recovers automatically after bridge restart.
- Codex continues normally if the bridge is offline.
