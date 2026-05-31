# Task State Debugging Notes

Date: 2026-05-31

This note records the debugging work for duplicate task tracking and the related `hook error` observation on the Codex ornament bridge and ESP32 panel.

## Problem

The panel could show a task as still running after it had already stopped. The reproduced shape was:

1. A task starts in one Codex session/location.
2. The same task is opened again from another session/location.
3. The bridge treats the second open as an additional active task.
4. When the task stops, only one active entry is removed.
5. One stale active task remains, so the web/ESP panel keeps showing `running`.

Later, after a firmware flash, the ESP `/status` endpoint also showed:

```json
{
  "task": {
    "status": "error",
    "title": "Codex hook error",
    "message": "InvalidJson"
  }
}
```

That `hook error` state means the bridge received a POST event body that could not be parsed as JSON and normalized it as `InvalidJson`.

## Affected Code

Main task aggregation code:

- `crates/codex-ornament-bridge/src/main.rs`

Important functions:

- `apply_task_event`
- `active_task_key_for_start`
- `finish_active_task`
- `remove_active_tasks_for_same_turn`
- `dedupe_active_tasks_by_turn`
- `task_snapshot`

ESP only displays the bridge state. The stale-running bug was in the bridge task state model, not in the display firmware.

## Root Cause

The old active task key included both session and turn:

```text
source:{source}:session:{session_id}:turn:{turn_id}
```

This works for truly separate tasks, but fails when the same logical task is reopened in a different session/location with the same `turn_id`.

Example:

```text
source:codex:session:session-1:turn:turn-shared
source:codex:session:session-2:turn:turn-shared
```

Those two keys represent the same logical turn, but the bridge counted them as two active tasks. A later stop event from `session-2` could remove only the `session-2` key, leaving `session-1` as a stale active task.

## Fix

The bridge now treats `source + turn_id` as the stronger identity for the same logical task across sessions.

The fix has three parts:

1. On start, if the event has a `turn_id`, remove any active task with the same source and same `turn_id` before inserting the new active task.
2. On stop, remove the exact active key and also remove any remaining active tasks with the same source and same `turn_id`.
3. Before returning `/state`, dedupe existing active tasks by source and `turn_id`, keeping the newest active entry.

This preserves multi-task behavior because different tasks must use different `turn_id` values. It also keeps Codex and Claude independent because source is included in the identity.

## Regression Tests

Added/updated bridge tests:

- `reopening_same_turn_in_another_session_replaces_stale_active_task`
- `stopping_reopened_turn_clears_stale_active_task_from_other_session`
- `snapshot_dedupes_existing_duplicate_active_turns`

Adjusted existing multi-task/concurrency tests so distinct simulated tasks use distinct turn IDs. This avoids tests accidentally encoding the old broken behavior where the same `turn_id` across sessions counted as multiple Codex tasks.

## Verification

Formatting:

```powershell
cargo fmt -p codex-ornament-bridge
```

Bridge tests:

```powershell
cargo test -p codex-ornament-bridge
```

Result:

```text
57 passed; 0 failed
```

Runtime verification was done with an isolated bridge on port `18787` and an empty temporary `CODEX_HOME`, so the real panel state was not polluted by test events.

Synthetic event sequence:

1. POST `UserPromptSubmit` with `session_id=session-1`, `turn_id=turn-shared`
2. GET `/state`: `activeTaskCount=1`
3. POST `UserPromptSubmit` with `session_id=session-2`, `turn_id=turn-shared`
4. GET `/state`: `activeTaskCount=1`
5. POST `Stop` with `session_id=session-2`, `turn_id=turn-shared`
6. GET `/state`: `activeTaskCount=0`, `doneSeq=1`

The main bridge was then rebuilt and restarted. The main `/state` endpoint still showed one running Codex task, but that was the current live Codex session, not a stale duplicate from the reproduced scenario.

## Hook Error Observation

After a later firmware flash, ESP `/status` showed `Codex hook error` with `InvalidJson`.

Observed state:

```json
{
  "task": {
    "status": "error",
    "title": "Codex hook error",
    "message": "InvalidJson"
  }
}
```

The bridge creates this state when a POST to `/hook/codex` or `/event` cannot be parsed:

```rust
let payload = serde_json::from_slice::<Value>(&request.body).unwrap_or_else(|_| {
    json!({
        "hook_event_name": "InvalidJson",
        "raw": String::from_utf8_lossy(&request.body).to_string()
    })
});
```

Current conclusion:

- This is not caused by the ESP display firmware.
- It indicates some client sent an invalid/non-JSON POST body to the bridge.
- The bridge currently surfaces that malformed event as a visible panel error.

Recommended follow-up fix:

1. Log the peer address, endpoint, content type, body length, and a clipped raw body for invalid JSON.
2. Do not update the user-visible task state for malformed hook/event POSTs.
3. Return HTTP `400` with an error response instead of treating invalid JSON as a task event.
4. Add a regression test that invalid JSON does not change `task_snapshot`.

This would prevent accidental malformed probes or browser/client mistakes from showing `hook error` on the panel.

## Useful Commands

Check bridge state:

```powershell
Invoke-WebRequest -UseBasicParsing 'http://127.0.0.1:8787/state'
```

Check ESP status:

```powershell
Invoke-WebRequest -UseBasicParsing 'http://codex-ornament-4ad4.local/status'
```

Run bridge tests:

```powershell
cargo test -p codex-ornament-bridge
```

Build ESP firmware:

```powershell
idf.py build
```

Flash ESP firmware:

```powershell
idf.py -p COM6 -b 115200 flash
```
