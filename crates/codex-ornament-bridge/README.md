# Codex 桌面摆件桥接服务

`codex-ornament-bridge` 是运行在 PC 上的本地 HTTP 服务，给 ESP32 桌面摆件提供 Codex 额度和任务状态。

## 职责

- 通过 `POST /hook/codex` 接收 Codex `notify` 和生命周期 hook JSON。
- 复用 `quota-core` 读取 Codex 额度。
- 通过 `GET /state` 输出 ESP32 友好的状态 JSON。
- Codex 凭据只保存在 PC 上，ESP32 不接触 access token。

## 手动构建

本项目不自动安装 Rust 或其他环境。需要本机已有 Rust 工具链。

```powershell
cd D:\Desktop\codex\codex-quota-widget
cargo build -p codex-ornament-bridge --release
```

输出文件：

```text
target\release\codex-ornament-bridge.exe
```

## 手动运行

开发运行：

```powershell
cargo run -p codex-ornament-bridge
```

Release 运行：

```powershell
.\target\release\codex-ornament-bridge.exe
```

可选环境变量：

```powershell
$env:CODEX_ORNAMENT_BIND = "0.0.0.0:8787"
$env:CODEX_ORNAMENT_TOKEN = "change-this-if-lan-post-is-needed"
$env:CODEX_ORNAMENT_ENDPOINT = "http://127.0.0.1:8787/hook/codex"
```

默认监听：

```text
http://0.0.0.0:8787
```

## HTTP 接口

```text
GET  /health       健康检查
GET  /quota        当前 Codex 额度快照
GET  /state        给 ESP32 使用的任务 + 额度状态
POST /hook/codex   Codex hook 事件接收接口
POST /event        /hook/codex 的别名
```

安全策略：

- 本机 loopback 发来的 `POST` 可以不带 token。
- 局域网发来的 `POST` 默认拒绝。
- 如果需要允许局域网写事件，设置 `CODEX_ORNAMENT_TOKEN`，请求头带 `X-Codex-Ornament-Token`。
- ESP32 正常只访问 `GET /state`。

## Codex Hook 脚本

脚本位置：

```text
D:\Desktop\codex\codex-quota-widget\scripts\codex-ornament-hook.ps1
```

支持两种 Codex 输入方式：

- `notify`：Codex 把单个 JSON 字符串作为命令行参数传入。
- 生命周期 hook：Codex 把 JSON 写入 stdin，脚本在 stdout 返回 `{"continue":true}`。

首次启用生命周期 hook 后，需要在 Codex 里执行 `/hooks` 并信任对应命令，否则不会运行非托管 hook。

## 手动烟测

启动桥接服务后执行：

```powershell
Invoke-RestMethod http://127.0.0.1:8787/health

Invoke-RestMethod `
  -Method Post `
  -Uri http://127.0.0.1:8787/hook/codex `
  -ContentType application/json `
  -Body '{"hook_event_name":"Stop","message":"manual smoke test"}'

Invoke-RestMethod http://127.0.0.1:8787/state
```

预期结果：

- `/health` 返回 `ok`。
- `/hook/codex` 返回 `ok: true`。
- `/state` 中 `status` 变成 `done`，并包含额度快照。

## 代码审查命令

只使用本地已有工具，不自动安装环境。

```powershell
cargo fmt --check -p codex-ornament-bridge
cargo check -p codex-ornament-bridge
cargo test -p codex-ornament-bridge
cargo clippy -p codex-ornament-bridge -- -D warnings
```
