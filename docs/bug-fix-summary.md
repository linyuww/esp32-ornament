# Bug 修复与处理过程总结

本文档总结 2026-06 前后 Codex Ornament 网桥、ESP32 固件和代理访问相关问题的定位、处理和验证结果。它记录最终结论，不保留调试过程中的错误推测。

## 1. Hook error 显示到面板

现象：

- ESP 或 Web 面板出现 `Codex hook error` / `InvalidJson`。
- 该状态来自某些非 JSON 请求被当成任务事件处理。

原因：

- 早期桥接服务会把无法解析的 hook body 归一化为 `InvalidJson` 事件。
- 面板把该事件作为用户可见任务状态显示。

处理：

- 桥接服务改为在接收到非法 JSON 时返回 HTTP 400。
- 非法 JSON 只记录日志，不进入任务状态机，不改变 `/state`。

验证：

- 单元测试覆盖非法 JSON 在归一化前被拒绝。
- 面板不再因 malformed POST 显示 hook error。

## 2. 自动审批触发任务误完成

现象：

- 自动允许权限请求时，面板会把当前任务标记为完成。
- full access 模式下不复现。

原因：

- hook 脚本曾从任意文本中猜测事件名，文本中出现 `Stop` 时可能被误判为停止事件。
- session-only `Stop` 也可能误关闭带 `turn_id` 的 active task。

处理：

- hook 脚本不再从自由文本中猜测 `UserPromptSubmit`、`Stop` 或 `agent-turn-complete`。
- 没有明确事件名时统一降级为 `codex-event`。
- 桥接服务要求带 `turn_id` 的 active task 只能由匹配 turn 的完成事件关闭。
- session-only stop 只处理没有 `turn_id` 的 session task。

验证：

- `session_only_stop_does_not_complete_identified_turn_task`
- 多任务和多来源并发测试继续通过。

## 3. 多任务和孤儿任务状态不一致

现象：

- 实际没有任务运行时，面板仍显示 running。
- 同一任务跨 session 打开后可能重复计数。
- context 压缩或另一个任务烧录固件后，某些 hook 结束事件缺失。

原因：

- 早期 active task 身份包含 session，导致同一 turn 在不同 session 中被算作不同任务。
- hook 缺失时，桥接服务只靠内存状态无法闭合 orphaned task。

处理：

- 对带 `turn_id` 的任务使用 `source + turn_id` 作为强身份。
- 开始同一 turn 时去重旧 active task。
- 完成同一 turn 时清理所有同源同 turn active task。
- 桥接服务从 Codex session 日志恢复或闭合 active task。
- 恢复逻辑保留 Codex 和 Claude source 隔离。

验证：

- 覆盖同 turn 重开、同 turn 停止、并发多任务完成、Codex/Claude 混合任务。
- 桥接服务测试集持续通过。

## 4. 网桥自动匹配选错 LAN 地址

现象：

- `/discover` 返回 `198.18.0.1`。
- ESP32 无法通过该地址访问 PC 网桥。

原因：

- 代理/TUN 虚拟网卡使用 `198.18.0.0/15`。
- 原自动探测逻辑没有过滤 benchmark/proxy 地址。

处理：

- Rust 网桥过滤 loopback、link-local、documentation、broadcast 和 `198.18.0.0/15`。
- 启动脚本自动选择真实 LAN IP，排除 Meta/Clash/VMware/WSL/Loopback 等接口。
- 支持 `CODEX_ORNAMENT_LAN_IP` 手动覆盖。

验证：

- `/discover` 返回 `192.168.1.101`。
- LAN health URL 可访问。
- UDP discovery 模拟返回同样 LAN 地址。

## 5. `/state` 偶发卡顿或超时

现象：

- `/health` 正常，但 `/state` 可能 30 秒超时。
- ESP/Web 面板因此误判网桥异常。

原因：

- 状态恢复扫描会读取最近 session jsonl。
- 本机 `.codex` session 文件总量很大，最新单文件可超过 20 MB。
- 旧逻辑在状态快照路径中全量读取多个大文件。

处理：

- session task 恢复和 terminal turn 检测改为读取每个 session 文件尾部 2 MB。
- 保留从日志恢复 orphan task 的能力，同时降低 `/state` 阻塞风险。

验证：

- `/state` 响应从超时恢复到约 0.5 秒。
- 全量桥接服务测试通过。

## 6. 额度刷新策略不明确

现象：

- 额度数据只在接口被请求时触发刷新，缺少明确后台检查节奏。

处理：

- 网桥启动后增加后台 quota 刷新循环。
- 每 1 分钟检查一次。
- 缓存未过期或已有刷新进行时不重复请求。
- `/state` 保持非阻塞，额度 API 慢时返回缓存或 pending。

验证：

- 新增 `quota_refresh_due_respects_one_minute_interval`。
- 80 个桥接服务测试通过。

## 7. ESP32 经常显示 Bridge offline

现象：

- PC 网桥实际在线，但 ESP 屏幕偶尔显示 `Bridge offline`。

原因：

- 固件早期一次 HTTP 拉取失败就向 UI 发布错误状态。
- Wi-Fi 抖动、HTTP 短超时或 PC 临时繁忙都会被放大成 offline。

处理：

- 固件增加连续失败计数。
- 默认连续 3 次失败后才显示 `Bridge offline`。
- 3 次以内继续显示上一帧有效状态。
- 恢复后记录 `bridge fetch recovered after N failure(s)`。

验证：

- ESP-IDF 5.4.1 构建通过。
- 固件已通过 COM5 烧录到 ESP32-S3。

## 8. Clash Verge Rev TUN 下无法访问 `.local`

现象：

- 开启 TUN 后，浏览器无法访问 `http://codex-ornament-4ad4.local/`。
- 直接加 `DOMAIN-SUFFIX,local,DIRECT` 不一定解决。

原因：

- `.local` 是 mDNS，需要发往 `224.0.0.251:5353` 的多播解析。
- TUN/DNS hijack 可能让浏览器请求进入 Clash/Mihomo DNS 流程。
- DIRECT 规则只决定连接走向，不能保证 mDNS 能解析出真实 IP。

处理建议：

- 最小可靠方案是在 Clash Verge Rev 全局扩展或 `dns_config.yaml` 中启用 hosts：

```yaml
profile:
  store-selected: true

dns:
  use-hosts: true

hosts:
  codex-ornament-4ad4.local: 192.168.1.100
  codex-ornament-4ad4: 192.168.1.100
```

- 如果 ESP32 IP 会变，优先在路由器中给 MAC `E0:72:A1:D3:4A:D4` 做 DHCP 静态绑定。
- 不推荐用脚本动态改 hosts，维护成本和故障面更大。

验证：

```powershell
Resolve-DnsName codex-ornament-4ad4.local
Invoke-WebRequest http://codex-ornament-4ad4.local/ -UseBasicParsing -TimeoutSec 5
```

## 当前维护要点

- Web 面板区分 Codex 与 Claude 并列任务状态。
- ESP 硬件屏幕保持合并任务视图，不区分 Codex/Claude。
- 网桥自动匹配优先返回真实 LAN IP。
- 任务状态由 hook 事件和 session 日志恢复共同维护。
- Claude transcript hook 会先补齐 source 与稳定 turn 身份，再按 Codex 相同的 source/turn/session 规则闭合任务，避免 stop 后继续导致任务数累加。
- malformed hook 不改变面板状态。
- ESP offline 是防抖后的连续失败状态，不代表 PC 网桥一定退出。
