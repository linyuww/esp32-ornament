# Development Guide

## Repository Layout

```text
crates/quota-core                  Shared quota logic
crates/codex-ornament-bridge       PC HTTP bridge
scripts/codex-ornament-hook.ps1    Codex hook forwarder
firmware/esp32-ornament            ESP-IDF firmware skeleton
docs/software-engineering          Engineering documentation
```

## Coding Standards

- Rust: keep bridge code dependency-light; use `cargo fmt` before commits.
- ESP-IDF C: keep modules small and isolate hardware drivers behind `display`.
- JSON: use camelCase on the wire because existing `quota-core` snapshots already serialize that way.
- Secrets: do not log tokens, cookies, full hook payloads, or full prompts.

## Branching and Review

- Hardware driver work should be separate from bridge/API work.
- Every display-driver change needs a photo or screenshot-style verification note.
- Every API-shape change must update `03-api-contract.md`.

## Local Verification Commands

```powershell
cargo fmt
cargo check -p codex-ornament-bridge
cargo test -p quota-core
cargo clippy -p codex-ornament-bridge -- -D warnings
cargo clippy -p quota-core -- -D warnings
```

Do not install ESP-IDF, Rust, or other toolchains automatically during agent work. Missing tools are
manual deployment prerequisites and should be documented in README files.

Manual bridge test:

```powershell
Invoke-RestMethod http://127.0.0.1:8787/health
Invoke-RestMethod -Method Post -Uri http://127.0.0.1:8787/hook/codex -ContentType application/json -Body '{"hook_event_name":"Stop"}'
Invoke-RestMethod http://127.0.0.1:8787/state
```
