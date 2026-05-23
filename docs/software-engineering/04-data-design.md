# Data Design

## Runtime State

The PC bridge stores only the latest task event in memory. Quota state is read from the existing
`quota-core` cache or refreshed when no cache is available.

## ESP32 State Model

```c
typedef struct {
    ornament_status_t status;
    char task_title[192];
    char task_message[192];
    char quota_status[32];
    int primary_remaining_percent;
    int secondary_remaining_percent;
    bool has_task;
    bool has_quota;
} ornament_state_t;
```

## Retention

- Hook payloads are not written to disk by the bridge.
- Quota cache follows the existing `CODEX_QUOTA_STATE` path behavior.
- ESP32 keeps only the latest fetched state in RAM.

## Privacy

Task messages are clipped before being exposed to ESP32. The current limit is 160 characters.
If task prompts may contain sensitive content, disable message forwarding or replace it with fixed text.
