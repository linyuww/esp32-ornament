# ESP32 摆件桥接服务

桥接服务运行在 PC 上，负责读取 Codex 额度、接收 Codex/Claude hook 事件，并向 ESP32 与网页面板提供统一状态。

## 运行

```powershell
cd D:\Desktop\codex\codex-quota-widget
cargo run -p codex-ornament-bridge
```

默认监听：

```text
http://0.0.0.0:8787
```

常用接口：

```text
GET  /health       健康检查
GET  /ready        持久化 readiness，检查事件日志可写
GET  /metrics      Prometheus 文本指标
GET  /discover     返回局域网可访问的 state/health URL
GET  /state        任务、额度、天气和桥接状态
GET  /quota        当前额度快照
GET  /v1/music/resolve 解析歌曲元数据、歌词、播放流和封面流 URL
GET  /v1/music/stream  输出 16 kHz mono s16le PCM 音频流
GET  /v1/music/cover   输出 96x96 RGB565LE 专辑封面原始像素
POST /hook/codex   Codex/Claude hook 事件入口
POST /event        /hook/codex 的别名
```

## 音乐播放桥接

ESP32 只负责拉取固定格式数据：音频为 `audio/L16; rate=16000; channels=1`，封面为 96x96 RGB565LE 原始像素，歌词随 `/v1/music/resolve` 的 `lyrics` 字段返回。桥接服务负责调用音乐 API、获取专辑图和歌词，并使用本机 `ffmpeg` 转码音频与封面。

`/v1/music/resolve?song=...&artist=...&index=1` 返回 `title`、`artist`、`album`、`picture`、`coverUrl`、`url` 和可选 `lyrics`。ESP32 播放时先解析元数据，再拉取 `coverUrl` 显示专辑封面，同时从 `lyrics` 做播放页滚动显示。

运行音乐功能前确认 `ffmpeg` 在 PATH 中：

```powershell
ffmpeg -version
```

## 自动发现

ESP32 会通过 UDP `8787` 广播 `codex-ornament-discover-v1` 来发现桥接服务。桥接服务返回当前可用 LAN 地址，并过滤 `198.18.0.0/15` 这类代理或虚拟网卡地址。

如果需要手动指定返回给 ESP32 的 LAN 地址：

```powershell
$env:CODEX_ORNAMENT_LAN_IP = "192.168.1.101"
```

## Hook 配置

Codex lifecycle hook 推荐放在：

```text
%USERPROFILE%\.codex\hooks.json
```

`UserPromptSubmit` 映射为 `running`，`Stop` 映射为 `done`。`notify` 或 `agent-turn-complete` 只产生 `done`。

Claude hook 使用同一个脚本，并增加 `-Source Claude` 或环境变量 `CODEX_ORNAMENT_SOURCE=Claude`。Web 面板会并列显示 Codex 与 Claude 状态；ESP 硬件屏幕保持合并任务视图。

非法 JSON 请求会被记录并返回 HTTP 400，不会更新面板任务状态。

## 任务状态规则

- 多生产者、多消费者事件通过桥接服务内部队列串行落盘到状态模型。
- 事件日志默认写入 `<CODEX_HOME>/ornament/bridge-events.jsonl`，超过
  `CODEX_ORNAMENT_EVENT_LOG_MAX_BYTES` 后保留最近
  `CODEX_ORNAMENT_EVENT_LOG_COMPACT_KEEP_EVENTS` 条有效事件，适合常驻服务长期运行。
- Codex 与 Claude 使用 source 隔离，互不错误完成对方任务。
- 带 `turn_id` 的任务以 `source + turn_id` 作为强身份；同一 turn 在不同 session 中重开时不会重复计数。
- 只有匹配到对应 turn 的完成事件才会关闭带 turn 身份的 active task。
- Claude hook payload 中带 `.claude` transcript path 时会自动按 Claude 处理，并派生稳定 `turn_id` 复用同一套匹配逻辑。
- session-only stop 不会误关已有 turn 身份的 active task；Codex 和 Claude 均按相同 source/turn/session 规则闭合任务。
- 桥接服务重启后会从最近 session 日志尾部恢复 active task，避免大日志拖慢 `/state`。

## 验证

```powershell
Invoke-RestMethod http://127.0.0.1:8787/health
Invoke-RestMethod http://127.0.0.1:8787/ready
Invoke-RestMethod http://127.0.0.1:8787/metrics
Invoke-RestMethod http://127.0.0.1:8787/discover
Invoke-RestMethod http://127.0.0.1:8787/state
```

预期：

- `/health` 返回 `ok`。
- `/ready` 返回 JSON，`ok` 和 `eventLogWritable` 为 `true`。
- `/metrics` 返回 Prometheus 文本指标，不触发额度、天气或 session 日志恢复。
- `/discover` 返回真实 LAN IP，例如 `192.168.1.101`。
- `/state` 在正常情况下快速返回 JSON。
