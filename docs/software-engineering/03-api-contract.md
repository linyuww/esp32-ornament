# API Contract

Base URL:

```text
http://<PC-LAN-IP>:8787
```

## GET /health

Response:

```text
ok
```

## GET /quota

Returns the `quota-core` `QuotaSnapshot`.

Important fields:

```json
{
  "status": "ok",
  "planType": "plus",
  "primaryRemainingPercent": 64,
  "primaryResetsAt": "2026-05-21T17:00:00+08:00",
  "secondaryRemainingPercent": 92,
  "secondaryResetsAt": "2026-05-28T17:00:00+08:00"
}
```

## GET /state

Returns the complete ESP32 payload.

```json
{
  "status": "done",
  "task": {
    "kind": "agent-turn-complete",
    "status": "done",
    "title": "Codex done",
    "message": "short task summary",
    "receivedAt": "2026-05-21T13:00:00+08:00"
  },
  "quota": {
    "status": "ok",
    "primaryRemainingPercent": 64,
    "secondaryRemainingPercent": 92
  },
  "bridge": {
    "service": "codex-ornament-bridge",
    "observedAt": "2026-05-21T13:00:03+08:00"
  }
}
```

## POST /hook/codex

Accepts Codex hook JSON. Loopback POST is allowed. LAN POST requires `X-Codex-Ornament-Token`
when `CODEX_ORNAMENT_TOKEN` is configured.

Example:

```json
{
  "hook_event_name": "Stop",
  "session_id": "abc",
  "cwd": "D:\\Desktop\\codex"
}
```

Response:

```json
{
  "ok": true,
  "event": {
    "kind": "Stop",
    "status": "done",
    "title": "Codex done"
  }
}
```

## Status Mapping

| Input event | Output status |
| --- | --- |
| `UserPromptSubmit` | `running` |
| `Stop` | `done` |
| `agent-turn-complete` | `done` |
| invalid JSON | `error` |
| other event | `event` |
