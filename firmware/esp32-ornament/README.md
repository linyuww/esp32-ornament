# ESP32-S3 Codex Ornament Firmware

这是 Codex 桌面摆件的 ESP-IDF 固件。固件运行在 ESP32-S3 上，通过 Wi-Fi 轮询 PC 上的 `codex-ornament-bridge`，显示任务状态、额度、时间、天气和 Wi-Fi 信号。

当前发布版本：`V3.0.0`。

## 当前硬件

默认配置来自 `main/Kconfig.projbuild` 和 `sdkconfig.defaults`：

| 项目 | 默认值 |
| --- | --- |
| 主控 | ESP32-S3 |
| 显示屏 | 1.54 inch ST7789 SPI, 240x240 |
| 逻辑电平 | 3.3 V |
| 数据源 | `http://<PC-LAN-IP>:8787/state` |
| 配网方式 | 默认关闭板端 SoftAP 配网页；需要时开启 `CONFIG_ORNAMENT_CONFIG_PORTAL_ENABLED` |
| 小智 AI | 默认通过 PC bridge 代理，直连 WebSocket + Opus 仅作备用 |
| 页面按键 | GPIO15 短按切换 `Standby -> Quota -> Xiaozhi` |
| AI 启停按键 | GPIO16，仅在 Xiaozhi 页生效 |
| 音量按键 | GPIO17，调节音乐和 AI 助手共享扬声器音量 |
| 任务完成提示 | 边框闪烁；可选 MAX98357A I2S 本地 PCM 提示音 |

默认引脚：

| 屏幕信号 | ESP32-S3 GPIO |
| --- | ---: |
| SCL/SCLK | 12 |
| SDA/MOSI | 11 |
| CS | 10 |
| DC | 13 |
| RST | 8 |
| BLK | 7 |

## 功能

- 开机后连接已保存 Wi-Fi；默认 bridge-first 构建不启动 SoftAP 配网页，需要手机配网时开启 `CONFIG_ORNAMENT_CONFIG_PORTAL_ENABLED`。
- 自动发现 PC 网桥，并保存可用的 `/state` URL。
- 每 `CONFIG_ORNAMENT_POLL_INTERVAL_MS` 轮询一次网桥，默认 3000 ms。
- 连续 `CONFIG_ORNAMENT_BRIDGE_OFFLINE_FAILURES` 次拉取失败后才显示 `Bridge offline`，默认 3 次。
- 空闲 `CONFIG_ORNAMENT_STANDBY_CLOCK_MS` 后进入待机时钟页，默认 60000 ms。
- GPIO15 页面按键可短按循环 `Standby` / `Quota` / `Xiaozhi` 三个手动页。
- GPIO16 AI 按键仅在手动 Xiaozhi 页生效，用于启动或停止小智会话。
- GPIO17 音量按键每次增加 10% 共享扬声器音量，超过 100% 后回到 0%，音乐播放和小智 AI 语音使用同一音量值。
- 开启本地 Web 控制台后，网页 `Start AI` 会启动小智后台监听并立即切到 AI 页面；进入监听待机后，屏幕默认回到额度/待机页。
- 小智检测到唤醒词或新的对话活动后，屏幕会自动切回小智页面显示 STT/TTS。
- 开启本地 Web 控制台后，网页 `Stop AI` 会彻底关闭小智会话；关闭后仅喊唤醒词不会重新启动 AI。
- 任务完成后边框闪烁 `CONFIG_ORNAMENT_DONE_FLASH_MS`，默认 5000 ms。
- 小智页面显示 bridge/会话返回的实时 STT、TTS、激活码和错误状态，不再显示固定示例对话。
- 默认 bridge-first 瘦身构建会关闭板端 Web 控制台、SoftAP 配网页和本地天气客户端；需要本地调试时可在 Kconfig 中重新开启。
- 可选 `CONFIG_ORNAMENT_ST77916_DISPLAY_ENABLED` 支持 ST77916 SPI/QSPI 点屏诊断、启动色块测试和固定参考图，默认关闭。
- 本地 Web 控制台开启后提供状态查看、Bridge URL 测试、重启、清空配置和语音音量设置。
- 外部 UART 语音模块工程已移除；固件不会发送任务完成 UART 触发，也不会接收 UART 语音命令。

## 构建

使用 ESP-IDF 5.4.1：

```cmd
cd /d D:\Desktop\codex\codex-quota-widget\firmware\esp32-ornament
call D:\Espressif\frameworks\esp-idf-v5.4.1\export.bat
idf.py build
```

指定目标：

```cmd
idf.py set-target esp32s3
```

默认配置优先控制固件体积和内部 RAM 压力：`CONFIG_ORNAMENT_XIAOZHI_TRANSPORT_BRIDGE=y`，
`CONFIG_ORNAMENT_WEB_CONSOLE_ENABLED=n`，`CONFIG_ORNAMENT_CONFIG_PORTAL_ENABLED=n`，
`CONFIG_ORNAMENT_LOCAL_WEATHER_ENABLED=n`，`CONFIG_ORNAMENT_RICH_XIAOZHI_DISPLAY_ENABLED=n`。
如果启用直连小智、Web 控制台、LVGL 中文富显示或 ST77916 支持，请重新运行 `idf.py build` 和
`idf.py size` 检查 app 分区、IRAM/DRAM 余量。

## 烧录

把 `COMx` 换成设备管理器中的实际串口，例如 `COM5`：

```cmd
idf.py -p COM5 flash
```

发布包包含一体化镜像，可从 `firmware/esp32-ornament` 目录烧录：

```powershell
python -m esptool --chip esp32s3 -p COM5 -b 460800 write_flash 0x0 release/codex_ornament_v3.0.0_merged.bin
```

注意：一体化镜像从 `0x0` 烧录会覆盖 NVS 区域，Wi-Fi、Bridge URL 和本地音量等设备配置会被清空。烧录后需要重新配网，或改用开发流程的 `idf.py -p COM5 flash` 保留已有 NVS 配置。

烧录后建议继续监视启动日志：

```cmd
idf.py -p COM5 monitor
```

如需查看串口：

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
```

最近一次硬件验证：

```text
2026-06-14
V3.0.0 main 固件 idf.py build 通过，发布镜像 `codex_ornament_v3.0.0_merged.bin` 从 0x0 烧录成功。
ESP32-S3 MAC：e0:72:a1:d3:4a:d4。
启动日志显示 App version: v3.0.0；因 NVS 被发布镜像擦除，设备进入 Setup AP: 192.168.4.1 等待重新配网。
```

## 启动 PC 网桥

ESP32 不直接读取 Codex 登录凭据。PC 网桥负责查询额度、接收 hook，并把状态暴露给 ESP32：

```powershell
cd D:\Desktop\codex\codex-quota-widget
scripts\start-codex-ornament-bridge.ps1
```

验证：

```powershell
Invoke-RestMethod http://127.0.0.1:8787/health
Invoke-RestMethod http://127.0.0.1:8787/discover
Invoke-RestMethod http://127.0.0.1:8787/state
```

ESP32 访问的是 PC 的局域网 IP，不是 `127.0.0.1`。`/discover` 会返回类似：

```json
{
  "localIp": "192.168.1.101",
  "stateUrl": "http://192.168.1.101:8787/state"
}
```

如果需要让网桥长期运行，或评估放到服务器上运行，请看
[`docs/bridge-persistent-runtime.md`](docs/bridge-persistent-runtime.md)。局域网 PC
常驻推荐用 Windows Scheduled Task；云服务器常驻需要让 Codex/Claude hook 主动转发到服务器，并手动配置 ESP32 的 Bridge URL。

## 首次配网

默认 bridge-first 固件关闭 SoftAP 配网页。需要首次手机配网时，先开启
`CONFIG_ORNAMENT_CONFIG_PORTAL_ENABLED`；没有保存 Wi-Fi 时，ESP32 才会启动配置热点：

```text
SSID: Codex-Ornament-xxxx
Password: codex1234
URL: http://192.168.4.1
```

手机连接热点后打开 `http://192.168.4.1`：

1. 选择家庭 Wi-Fi SSID。
2. 输入 Wi-Fi 密码。
3. 让设备自动匹配网桥，或手动填写 `http://<PC-LAN-IP>:8787/state`。
4. 保存后 ESP32 会重启并连接家庭 Wi-Fi。

## 本地 Web 控制台

默认 bridge-first 固件关闭本地 Web 控制台。需要本地调试时，开启
`CONFIG_ORNAMENT_WEB_CONSOLE_ENABLED`；如需 `.local` 地址，再开启
`CONFIG_ORNAMENT_MDNS_ENABLED`。联网后可访问：

```text
http://codex-ornament-4ad4.local/
http://<ESP32-IP>/
```

控制台提供：

- 当前 Wi-Fi、RSSI、时间、额度、天气和任务状态。
- `GET /status` 机器可读 JSON。
- `POST /test-bridge` 测试当前 Bridge URL。
- `POST /save-xiaozhi` 保存小智 WebSocket URL 和 token。
- `POST /xiaozhi-start` / `POST /xiaozhi-stop` 启动或停止小智语音会话。
- `POST /reboot` 重启 ESP32。
- `POST /clear-config` 清空 Wi-Fi 与 Bridge URL 后重启到配网模式。

## 小智 AI 语音

固件集成了兼容 `78/xiaozhi-esp32` WebSocket 协议的轻量客户端：设备发送 `hello`，随后上传 16 kHz mono Opus 音频帧，并播放服务端下发的 Opus TTS 音频。当前使用 binary protocol version 1，也就是 WebSocket binary payload 直接承载 raw Opus frame。

小智配置入口：

- 默认通过 PC bridge 代理小智会话，ESP32 不需要保存 Xiaozhi WebSocket URL/token。
- 启用直连小智和本地控制台后，首次配网页或 Web 控制台的 `Xiaozhi AI` 区域可保存 URL/token、启动/停止会话、查看 STT/TTS 文本和上下行帧计数。
- `Start AI` 用于开启后台监听。启动瞬间屏幕会进入小智页；连接成功后如果没有人说话，屏幕会恢复为额度/待机页。
- 保持 `Start AI` 开启时，官方小智后台的唤醒词 `你好小智` 仍然有效。识别到新的语音对话或 TTS 播放时，屏幕会自动切回小智页。
- `Stop AI` 会终止当前小智会话并关闭唤醒监听。停止后就算喊 `你好小智`，设备也不会重新进入 AI。
- 屏幕的小智页面使用 `78/xiaozhi-fonts` 普惠中文字体和 LVGL 字形渲染，STT/TTS 中文文本会直接显示在当前 ST7789 帧缓冲页面中。

音频硬件配置：

| 信号 | 默认值 | 说明 |
| --- | ---: | --- |
| Speaker BCLK | GPIO4 | 复用现有 I2S BCLK |
| Speaker LRC/WS | GPIO5 | 复用现有 I2S WS |
| Speaker DIN | GPIO6 | MAX98357A 或兼容 I2S 功放输入 |
| Mic DOUT | GPIO14 | INMP441 `SD`/`DOUT` 输入 |
| Page Button | GPIO15 | 另一端接 GND，内部上拉，低电平触发 |
| AI Button | GPIO16 | 另一端接 GND，内部上拉，低电平触发 |
| Volume Button | GPIO17 | 另一端接 GND，内部上拉，低电平触发 |

INMP441 `SCK`/`BCLK` 接 GPIO4，`WS`/`LRCLK` 接 GPIO5，`SD`/`DOUT` 接 GPIO14，`L/R` 接 GND 使用左声道。若 `L/R` 改接 3V3，需要启用 `CONFIG_ORNAMENT_XIAOZHI_MIC_SLOT_RIGHT`。小智会话播放 TTS 时会占用 I2S 输出；此时任务完成提示音会跳过，避免两个音频源同时写同一喇叭。

页面切换按键：按钮一端接 GPIO15，另一端接 GND。默认启用内部上拉，短按循环 `Standby -> Quota -> Xiaozhi -> Standby`。切到 Xiaozhi 页时会自动启动小智会话；从 Xiaozhi 页切到其他手动页时会自动停止小智会话。未进入手动页覆盖时，屏幕仍按原有自动额度/待机逻辑显示。

AI 启停按键：按钮一端接 GPIO16，另一端接 GND。默认启用内部上拉，仅在当前手动页为 Xiaozhi 时响应。进入 Xiaozhi 页后会自动启动一次 AI；如果在该页内手动按 GPIO16 停止，会保持停止直到再次按 GPIO16 启动，或切走后再切回 Xiaozhi 页时重新自动启动。

音量按键：按钮一端接 GPIO17，另一端接 GND。默认启用内部上拉，短按按 10% 步进调节共享扬声器音量；网页端 `AI Assistant Volume` 保存的音量和实体音量键写入同一个 NVS 音量值，音乐播放与小智 TTS 都使用该值。

如果开启 Clash Verge Rev TUN 后 `.local` 访问失败，推荐在 Clash Verge Rev 全局扩展中添加静态 hosts，或在路由器中给 ESP32 绑定 DHCP 静态地址。不要只依赖 `DOMAIN-SUFFIX,local,DIRECT`，因为它不能解决 mDNS 解析被 TUN/DNS 劫持的问题。

## 故障检查

### Bridge offline

固件会在连续 3 次拉取失败后显示 `Bridge offline`。按顺序检查：

1. PC 网桥是否运行：`Invoke-RestMethod http://127.0.0.1:8787/health`
2. Windows 防火墙是否允许局域网访问 `8787`。
3. ESP32 与 PC 是否在同一个 Wi-Fi/局域网。
4. ESP32 保存的 Bridge URL 是否是 PC LAN IP，不是 `127.0.0.1`。
5. PC 上是否能访问：`Invoke-RestMethod http://<PC-LAN-IP>:8787/state`

### TUN 下无法访问 `.local`

先确认当前解析：

```powershell
Resolve-DnsName codex-ornament-4ad4.local
```

如果 TUN 开启后解析失败或解析到 fake-ip，给 Clash Verge Rev 添加 hosts：

```yaml
profile:
  store-selected: true

dns:
  use-hosts: true

hosts:
  codex-ornament-4ad4.local: 192.168.1.100
  codex-ornament-4ad4: 192.168.1.100
```

如果 ESP32 IP 会变，优先在路由器 DHCP 中绑定 ESP32 MAC 到固定 IP。
