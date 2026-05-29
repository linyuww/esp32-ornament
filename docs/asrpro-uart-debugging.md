# ESP32 到天问 ASRPRO 串口完成提醒排障记录

日期：2026-05-27

## 目标

ESP32 在任务完成时从桥服务读取到 `doneSeq` 增加，然后通过 UART 向天问 ASRPRO 发送 `codex_done`。ASRPRO 收到后播放任务完成提示音。

## 最终结论

链路最终验证成功：

```text
ESP> I (...) asrpro_link: ASRPRO done trigger sent
ASR> Serial1 matched codex_done, play 10500
```

这说明：

- 桥服务 `/state` 正常输出任务完成状态。
- ESP32 能轮询到 `doneSeq` 增加。
- ESP32 已经从 UART1 / GPIO17 发出 `codex_done`。
- ASRPRO 的 `Serial1 @ 9600` 已收到并匹配该字符串。
- ASRPRO 已执行 `play_audio(10500)`。

如果后续仍然没有声音，问题应优先查 ASRPRO 播放侧：音量、喇叭、功放、`10500` 音频资源是否存在，而不是再查串口链路。

## 硬件和端口

| 设备 | 端口 | 用途 | 波特率 |
| --- | --- | --- | --- |
| ESP32-S3 | `COM6` | ESP 日志和调试 | `115200` |
| ASRPRO CH340K | `COM8` | ASRPRO USB 调试口 | `115200` |
| ESP GPIO17 -> ASRPRO PA_3/RX | 硬件线 | ESP 到 ASRPRO 指令输入 | `9600` |

接线要求：

```text
ESP32 GPIO17 TX -> ASRPRO PA_3 / RX / Serial1 RX
ESP32 GND       -> ASRPRO GND
```

注意：`COM8` 是 ASRPRO 的 USB 调试口，不是 ESP 接入 ASRPRO 的 `Serial1` 线。`COM8` 看日志用 `115200`，ESP 发给 ASRPRO 的 `Serial1` 用 `9600`。

## 代码状态

ASRPRO 代码位置：

```text
D:\天问Block\asrpro\_asr.cpp
D:\天问Block\asrpro\asr.cpp
```

ESP 固件位置：

```text
D:\Desktop\codex\codex-quota-widget\firmware\esp32-ornament
```

桥服务位置：

```text
D:\Desktop\codex\codex-quota-widget\target\debug\codex-ornament-bridge.exe
```

ESP 配置：

```text
UART: UART1
Baud: 9600
TX GPIO: 17
RX GPIO: -1
Trigger: codex_done
```

ASRPRO 逻辑：

- `Serial.begin(115200)`：USB 调试口，供 `COM8` 查看日志和直接测试。
- `Serial1.begin(9600)`：接收 ESP 发来的 `codex_done`。
- 创建 FreeRTOS 后台任务持续轮询 `Serial1` 和 `Serial`。
- 使用逐字节滑动匹配 `codex_done`，匹配后调用 `play_audio(10500)`。

## Bug 过程

### 1. 最初 ASRPRO 串口读取位置错误

最初把串口读取逻辑放在 `ASR_CODE()` 里。实际 SDK 中 `ASR_CODE()` 只会在语音识别结果回调 `sys_asr_result_hook()` 触发时调用，不是主循环。

结果是：ESP 即使发出了串口数据，ASRPRO 也大概率没有在那个时间点读取串口。

修复方式：在 `hardware_init()` 中创建 FreeRTOS 后台任务，持续轮询串口。

### 2. COM8 乱码和没反应

现象：

- 串口助手打开 `COM8` 时出现乱码。
- 有时发送 `codex_done` 看起来没有反应。

原因：

- `COM8` 是 ASRPRO 的 USB 调试口，波特率应为 `115200`。
- `9600` 是 ESP -> ASRPRO `Serial1` 的通信波特率，不能用来查看 `COM8` 日志。
- 串口被天问Block或串口助手占用时，PowerShell 无法同时打开。

验证结果：

```text
BAUD=115200 RX=[Serial0 matched codex_done, play 10500\r\n]
```

这证明 ASRPRO 固件已经包含调试逻辑，`COM8 @ 115200` 可正常接收和打印。

### 3. 桥服务曾停止，ESP 无法获取状态

现象：

ESP 日志曾出现：

```text
failed to fetch bridge state: ESP_ERR_HTTP_CONNECT
```

原因：

桥服务进程没有运行，或 ESP 无法访问电脑的 `8787` 端口。

处理：

```powershell
Start-Process -FilePath 'D:\Desktop\codex\codex-quota-widget\target\debug\codex-ornament-bridge.exe' -WorkingDirectory 'D:\Desktop\codex\codex-quota-widget' -WindowStyle Hidden
Invoke-RestMethod -Uri 'http://127.0.0.1:8787/health'
```

恢复后健康检查返回：

```text
ok
```

### 4. 排查 ESP 桥地址配置

电脑当前局域网 IP：

```text
192.168.1.104
```

ESP 状态接口：

```text
http://192.168.1.102/status
```

ESP 返回的关键状态：

```json
{
  "fetch_error": "ESP_OK",
  "bridge_url": "http://192.168.1.104:8787/state",
  "task": {
    "done_seq": 2,
    "status": "done"
  }
}
```

这证明 ESP 已经正确指向当前电脑桥服务，并且能正常获取 `/state`。

### 5. 最终双串口联调

同时打开：

- `COM6 @ 115200`：看 ESP 日志。
- `COM8 @ 115200`：看 ASRPRO 日志。

触发一次新的任务完成事件后，得到：

```text
ESP> I (...) asrpro_link: ASRPRO done trigger sent
ASR> Serial1 matched codex_done, play 10500
```

这是最终闭环证据。

## 常用检查命令

查看串口：

```powershell
Get-CimInstance Win32_PnPEntity |
  Where-Object { $_.Name -match '\(COM\d+\)' } |
  Select-Object Name,Manufacturer,DeviceID,Status |
  Format-List
```

检查桥服务：

```powershell
Invoke-RestMethod -Uri 'http://127.0.0.1:8787/health'
Invoke-RestMethod -Uri 'http://127.0.0.1:8787/state' | ConvertTo-Json -Depth 5
```

检查 ESP 状态：

```powershell
Invoke-RestMethod -Uri 'http://192.168.1.102/status' | ConvertTo-Json -Depth 5
```

直接测试 ASRPRO 的 USB 调试口：

```powershell
$port = [System.IO.Ports.SerialPort]::new('COM8', 115200)
$port.ReadTimeout = 200
$port.WriteTimeout = 1000
$port.Open()
try {
  $port.Write('codex_done')
  Start-Sleep -Seconds 2
  if ($port.BytesToRead -gt 0) {
    $port.ReadExisting()
  }
} finally {
  $port.Close()
}
```

预期输出：

```text
Serial0 matched codex_done, play 10500
```

## 判断表

| 现象 | 结论 | 下一步 |
| --- | --- | --- |
| `COM8 @ 115200` 发 `codex_done` 返回 `Serial0 matched...` | ASRPRO 固件和播放调用路径有效 | 继续查 ESP 到 ASRPRO 的硬件线 |
| ESP 日志有 `ASRPRO done trigger sent`，ASRPRO 有 `Serial1 matched...` | 串口链路已通 | 查音频资源、音量、喇叭 |
| ESP `/status` 中 `fetch_error` 不是 `ESP_OK` | ESP 没拿到桥状态 | 查桥服务、电脑 IP、防火墙、bridge URL |
| 桥服务 `doneSeq` 增加，但 ESP `/status` 的 `done_seq` 不增加 | ESP 没轮询到新状态 | 查网络或等待轮询周期 |
| ESP 有 `trigger sent`，ASRPRO 无 `Serial1` 日志 | 硬件线或 ASRPRO RX 引脚问题 | 查 GPIO17 -> PA_3/RX 和共地 |
| COM8 乱码 | 波特率错误 | 用 `115200 8N1` |

## 后续建议

1. ASRPRO 音量当前如果是 `vol_set(1)`，听不清时可改为 `vol_set(4)` 或 `vol_set(5)` 后重新烧录。
2. 保留 `Serial1 matched codex_done` 这类日志，后续能快速区分“串口没通”和“音频没播”。
3. 如果电脑 IP 变化，需要确认 ESP 的 `bridge_url` 是否仍指向当前电脑 IP，例如 `http://192.168.1.104:8787/state`。
4. 联调时优先只打开 `COM8` 看 ASRPRO，避免打开 `COM6` 导致 ESP 复位；只有需要确认 ESP 是否真的发送时，再同时抓 `COM6`。
