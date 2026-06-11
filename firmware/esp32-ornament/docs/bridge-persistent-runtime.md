# Bridge Persistent Runtime Plan

This firmware can use a bridge that runs as a long-lived service. Keep the
deployment choice tied to where Codex and Claude events are produced.

## Recommended Topology

### LAN PC Service

Run `codex-ornament-bridge` on the PC that runs Codex/Claude. This is the
lowest-risk setup because the bridge can read local Codex state, receive local
hook calls, answer UDP discovery on port `8787`, and stream music/audio over the
same LAN address.

Use this when the ESP32 and the development machine are on the same Wi-Fi:

```powershell
cd D:\Desktop\codex\codex-quota-widget
.\scripts\start-codex-ornament-bridge.ps1
```

For persistence on Windows, create a logon task that runs the script with the
repository root as the working directory:

```powershell
$root = "D:\Desktop\codex\codex-quota-widget"
$script = Join-Path $root "scripts\start-codex-ornament-bridge.ps1"
$action = New-ScheduledTaskAction -Execute "powershell.exe" -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$script`"" -WorkingDirectory $root
$trigger = New-ScheduledTaskTrigger -AtLogOn
$settings = New-ScheduledTaskSettingsSet -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
Register-ScheduledTask -TaskName "Codex Ornament Bridge" -Action $action -Trigger $trigger -Settings $settings -Description "Keeps the Codex ESP32 ornament bridge online"
```

The existing startup script already borrows mature daemon practices: it loads
`.env`/`.env.local`, selects a usable LAN IP, avoids proxy/virtual adapter
addresses, restarts stale bridge processes when the advertised IP changed, and
redirects logs to `bridge-stdout.log` and `bridge-stderr.log`.

### Server Relay Service

Run the bridge on a Linux server only if Codex/Claude hook events can reach that
server. In this mode the ESP32 polls a fixed HTTPS/LAN URL and local desktops
push hook events to the server.

Use this when the ESP32 should keep showing state while the development PC IP
changes or multiple machines report to one ornament:

```ini
# /etc/systemd/system/codex-ornament-bridge.service
[Unit]
Description=Codex Ornament Bridge
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/opt/codex-quota-widget
EnvironmentFile=/etc/codex-ornament-bridge.env
ExecStart=/opt/codex-quota-widget/target/release/codex-ornament-bridge
Restart=always
RestartSec=3
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full
ProtectHome=read-only

[Install]
WantedBy=multi-user.target
```

Example environment file:

```text
CODEX_ORNAMENT_BIND=0.0.0.0:8787
CODEX_ORNAMENT_TOKEN=replace-with-a-long-random-token
CODEX_ORNAMENT_WEATHER_PROVIDER=auto
CODEX_ORNAMENT_WEATHER_LAT=39.99540087499999
CODEX_ORNAMENT_WEATHER_LON=116.34162524999999
CODEX_ORNAMENT_WEATHER_LABEL=HAIDIAN
CODEX_ORNAMENT_MUSIC_PUBLIC_BASE_URL=https://ornament.example.com
CODEX_ORNAMENT_LOCK_MUSIC_PUBLIC_BASE_URL=1
CODEX_ORNAMENT_EVENT_LOG=/var/lib/codex-ornament/bridge-events.jsonl
CODEX_ORNAMENT_EVENT_LOG_MAX_BYTES=8388608
CODEX_ORNAMENT_EVENT_LOG_COMPACT_KEEP_EVENTS=4096
```

Enable and check the service:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now codex-ornament-bridge
systemctl status codex-ornament-bridge
curl -fsS http://127.0.0.1:8787/health
curl -fsS http://127.0.0.1:8787/ready
curl -fsS http://127.0.0.1:8787/metrics
```

Put Caddy, Nginx, or another reverse proxy in front for TLS. Do not expose
plain HTTP or unauthenticated hook endpoints to the internet. Keep
`CODEX_ORNAMENT_TOKEN` enabled and make hook scripts send the same bearer token.

## ESP32 Configuration

LAN auto-discovery only works on the local broadcast domain. For a cloud server
or routed network, configure the device bridge URL explicitly:

```text
http://<server-or-lan-ip>:8787/state
https://ornament.example.com/state
```

Use the firmware web console when enabled, the provisioning portal, or
`CONFIG_ORNAMENT_BRIDGE_URL`/NVS to store the URL. The bridge client already
does single retry, UDP discovery, nearby-subnet probing, and offline standby, so
the device continues rendering the last useful state while the bridge recovers.

## Durability Model

The bridge uses an append-only task event journal. By default it writes accepted
task lifecycle events to:

```text
<CODEX_HOME>/ornament/bridge-events.jsonl
```

Set `CODEX_ORNAMENT_EVENT_LOG` to put the journal under a server-managed data
directory, for example `/var/lib/codex-ornament/bridge-events.jsonl`. Set
`CODEX_ORNAMENT_PERSIST_EVENTS=0` to disable the journal. Long-running services
compact the journal after it exceeds `CODEX_ORNAMENT_EVENT_LOG_MAX_BYTES`
(default 8 MiB) by keeping the latest
`CODEX_ORNAMENT_EVENT_LOG_COMPACT_KEEP_EVENTS` valid events (default 4096). Set
`CODEX_ORNAMENT_EVENT_LOG_MAX_BYTES=0` to disable compaction if an external log
rotation policy owns the file.

On startup, the bridge rebuilds its bounded in-memory `BridgeState` by replaying
the journal. Corrupt lines are skipped so a partial write does not prevent the
service from starting. Done and unmatched histories remain bounded by the
firmware-facing state limits; quota and weather remain cache entries, not durable
truth.

The `/ready` endpoint reports service readiness and verifies that the event log
can be opened for append. Use it for systemd, reverse-proxy, or container
readiness checks when persistence matters. The `/metrics` endpoint exposes a
small Prometheus-compatible text snapshot for bridge readiness, journal bytes,
active tasks, done counters, unmatched stops, and refresh workers without
triggering quota, weather, or session-log recovery work. The `/state` response
also reports the journal under `bridge.eventLogEnabled`, `bridge.eventLogPath`,
and `bridge.eventLogBytes`.

This mirrors mature embedded/server bridges: durable input log first, bounded
in-memory projection second, health endpoint for supervisors, and automatic
restart from the service manager.

## Operational Checks

Minimum checks before pointing the ornament at a persistent bridge:

```powershell
Invoke-RestMethod http://<bridge-host>:8787/health
Invoke-RestMethod http://<bridge-host>:8787/ready
Invoke-RestMethod http://<bridge-host>:8787/state
Invoke-RestMethod http://<bridge-host>:8787/metrics
Invoke-RestMethod http://<esp32-ip>/status
```

To verify persistence specifically:

```powershell
(Invoke-RestMethod http://<bridge-host>:8787/ready) |
    Select-Object ok,eventLogEnabled,eventLogPath,eventLogWritable,eventLogError
(Invoke-RestMethod http://<bridge-host>:8787/state).bridge
```

For server mode, also verify:

- The firewall allows TCP `8787` only from trusted networks or through HTTPS.
- UDP discovery is not expected unless the server is on the same LAN.
- Hook producers can reach `/hook/codex` or `/event`.
- Secrets live in `.env.local`, `/etc/codex-ornament-bridge.env`, or the service
  manager, not in firmware defaults.
