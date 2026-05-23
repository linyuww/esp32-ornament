# Codex Quota OpenClaw Deployment

This project installs two Linux binaries:

- `codex-quota-mcp`: stdio MCP server for OpenClaw tools.
- `codex-quota-watch`: background watcher that refreshes quota every 30 seconds and writes state.
- `codex-quota-card`: direct command that writes an SVG quota card without LLM rewriting.

Server layout:

- Source: `/opt/codex-quota`
- Auth: `/etc/codex-quota/auth.json`
- State: `/var/lib/codex-quota/state.json`
- Binaries: `/usr/local/bin/codex-quota-mcp`, `/usr/local/bin/codex-quota-watch`

Direct card command:

```bash
codex-quota-card --refresh /tmp/codex-quota.svg
openclaw message send --channel <channel> --target <target> --media /tmp/codex-quota.svg
```

OpenClaw MCP registration:

```bash
openclaw mcp set codex-quota '{"command":"/usr/local/bin/codex-quota-mcp","env":{"CODEX_AUTH_PATH":"/etc/codex-quota/auth.json","CODEX_QUOTA_STATE":"/var/lib/codex-quota/state.json"}}'
```

If the server must access ChatGPT through the local proxy on port 10809, register MCP with proxy variables:

```bash
openclaw mcp set codex-quota '{"command":"/usr/local/bin/codex-quota-mcp","env":{"CODEX_AUTH_PATH":"/etc/codex-quota/auth.json","CODEX_QUOTA_STATE":"/var/lib/codex-quota/state.json","HTTP_PROXY":"http://127.0.0.1:10809","HTTPS_PROXY":"http://127.0.0.1:10809","ALL_PROXY":"socks5h://127.0.0.1:10809"}}'
```

Service checks:

```bash
systemctl status codex-quota-watch
journalctl -u codex-quota-watch -f
openclaw mcp show codex-quota
```
