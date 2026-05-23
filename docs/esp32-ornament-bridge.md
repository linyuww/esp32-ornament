# ESP32 摆件桥接服务快速说明

这个桥接服务通过本地 HTTP 端口输出 Codex 额度和任务完成状态，供 ESP32-S3 桌面摆件读取。

## 运行

```powershell
cd D:\Desktop\codex\codex-quota-widget
cargo run -p codex-ornament-bridge
```

默认地址：

```text
Bind: 0.0.0.0:8787
State: GET http://<PC-LAN-IP>:8787/state
Quota: GET http://<PC-LAN-IP>:8787/quota
Hook:  POST http://127.0.0.1:8787/hook/codex
```

可选环境变量：

```powershell
$env:CODEX_ORNAMENT_BIND = "0.0.0.0:8787"
$env:CODEX_ORNAMENT_TOKEN = "change-me"
$env:CODEX_ORNAMENT_ENDPOINT = "http://127.0.0.1:8787/hook/codex"
```

本机 `POST` 默认放行。局域网 `POST` 需要设置 `CODEX_ORNAMENT_TOKEN` 并发送 `X-Codex-Ornament-Token`。ESP32 正常只需要 `GET /state`。

## Codex Notify

把下面配置加入用户级配置文件：

```text
%USERPROFILE%\.codex\config.toml
```

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

`notify` 当前会发送 `agent-turn-complete`，桥接服务会映射为：

```json
{
  "status": "done",
  "title": "Codex done"
}
```

## 生命周期 Hook

如果需要看到 `running` 到 `done` 的状态变化，可以配置生命周期 hook。

创建或修改：

```text
%USERPROFILE%\.codex\hooks.json
```

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

配置后在 Codex 中运行 `/hooks`，信任这条 PowerShell 命令。

状态映射：

| Codex 事件 | 摆件状态 |
| --- | --- |
| `UserPromptSubmit` | `running` |
| `Stop` | `done` |
| `agent-turn-complete` | `done` |
| 非法 JSON | `error` |

## ESP32 读取载荷

`GET /state` 示例：

```json
{
  "status": "done",
  "task": {
    "kind": "agent-turn-complete",
    "status": "done",
    "title": "Codex done",
    "message": "last assistant message excerpt",
    "receivedAt": "2026-05-21T13:00:00+08:00"
  },
  "quota": {
    "status": "ok",
    "primaryRemainingPercent": 64,
    "secondaryRemainingPercent": 92,
    "primaryResetsAt": "2026-05-21T17:00:00+08:00",
    "secondaryResetsAt": "2026-05-28T17:00:00+08:00"
  }
}
```

额度代码复用 `quota-core`，读取本地 Codex 登录信息后请求：

```text
https://chatgpt.com/backend-api/wham/usage
```
