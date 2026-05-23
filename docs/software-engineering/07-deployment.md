# Deployment Guide

## PC Bridge

Run manually:

```powershell
cargo run -p codex-ornament-bridge
```

Build release:

```powershell
cargo build -p codex-ornament-bridge --release
```

Recommended environment:

```powershell
$env:CODEX_ORNAMENT_BIND = "0.0.0.0:8787"
$env:CODEX_ORNAMENT_ENDPOINT = "http://127.0.0.1:8787/hook/codex"
```

## Codex Config

Add notify to `%USERPROFILE%\.codex\config.toml`:

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

For lifecycle hooks, create `%USERPROFILE%\.codex\hooks.json`:

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

## ESP32 Firmware

1. Confirm screen pinout and set display driver implementation.
2. Run `idf.py set-target esp32s3`.
3. Run `idf.py menuconfig`.
4. Set Wi-Fi SSID/password and `Bridge state URL`.
5. Run `idf.py build`.
6. Flash with `idf.py -p COMx flash monitor`.

## Firewall

Allow inbound TCP 8787 on private networks so ESP32 can reach the bridge.
