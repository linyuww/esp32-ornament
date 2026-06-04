# Ghost Task Bug Data: Codex Desktop History Dialog

Collected on 2026-06-04 for a repeated false `running -> done` task flash after opening an old Codex Desktop conversation.

## Observed Symptom

- User-visible stale conversation ID: `019e9239-bf80-7290-84f6-a28a1409d919`.
- The conversation was not actually running.
- Opening the history dialog caused the ornament bridge state to briefly show `running`, then `done`.

## Bridge State Evidence

Current real active workspace task at collection time:

- `sessionId`: `019e910c-4029-7b60-bb7f-dce3af4e3310`
- `turnId`: `019e923b-7072-7002-b637-0c0526226a98`
- `cwd`: `D:\Desktop\codex\codex-quota-widget`

Suspicious event accepted by the bridge as the latest done task:

- `sessionId`: `019e923b-3517-7c33-a1e7-71f192182cc1`
- `turnId`: `019e923b-389d-7d41-bc51-730c6f204965`
- `cwd`: `C:\Program Files\WindowsApps\OpenAI.Codex_26.601.2237.0_x64__2p2nqsd0c76g0\app`
- `model`: `gpt-5.4-mini`
- `message`: `{"exclude":[]}`

The user-reported ID appeared in current conversation text/search output, not as a separate active session log file. The suspicious bridge event came from a Codex Desktop app install directory, not from the workspace.

## Hook Configuration

Global hooks are installed for both lifecycle events:

- `UserPromptSubmit`
- `Stop`

Both invoke `scripts\codex-ornament-hook.ps1`, which adds:

- `session_id` from `CODEX_THREAD_ID`
- `cwd` from the current process directory

That means Codex Desktop UI activity can emit hook payloads even when the visible history conversation is not a real running workspace task.

## Root Cause

The bridge filtered top-level control payloads such as `{"exclude":[]}`, but did not filter lifecycle payloads after the hook script had wrapped them with `hook_event_name`, `session_id`, `turn_id`, and `cwd`. As a result, a control-only Desktop history event could be normalized as:

- `UserPromptSubmit` -> `running`
- `Stop` -> `done`

## Fix Requirements

- Ignore Codex lifecycle events whose message is a control-only payload.
- Ignore Codex Desktop app-directory lifecycle control events from `C:\Program Files\WindowsApps\OpenAI.Codex_...\app`.
- Keep real workspace lifecycle events working normally.
- Log filtered lifecycle events with enough metadata to diagnose future false positives.
