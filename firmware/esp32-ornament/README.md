# ESP32-S3 Codex Ornament Firmware

这是 Codex 桌面摆件的 ESP-IDF 固件。固件运行在 ESP32-S3 上，通过 Wi-Fi 轮询 PC 上的 `codex-ornament-bridge`，显示任务状态、额度、时间、天气和 Wi-Fi 信号。

## 当前硬件

默认配置来自 `main/Kconfig.projbuild` 和 `sdkconfig.defaults`：

| 项目 | 默认值 |
| --- | --- |
| 主控 | ESP32-S3 |
| 显示屏 | 1.54 inch ST7789 SPI, 240x240 |
| 逻辑电平 | 3.3 V |
| 数据源 | `http://<PC-LAN-IP>:8787/state` |
| 配网方式 | ESP32 SoftAP + 手机浏览器 |
| 小智 AI | 可选 WebSocket + Opus，默认不自动启动 |
| 页面按键 | GPIO15 短按切换页面 |

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

- 开机后连接已保存 Wi-Fi；没有配置时进入 SoftAP 配网。
- 自动发现 PC 网桥，并保存可用的 `/state` URL。
- 每 `CONFIG_ORNAMENT_POLL_INTERVAL_MS` 轮询一次网桥，默认 3000 ms。
- 连续 `CONFIG_ORNAMENT_BRIDGE_OFFLINE_FAILURES` 次拉取失败后才显示 `Bridge offline`，默认 3 次。
- 空闲 `CONFIG_ORNAMENT_STANDBY_CLOCK_MS` 后进入待机时钟页，默认 60000 ms。
- GPIO15 页面按键可短按切换 `Auto` / `Quota` / `Tasks` / `Clock` / `Xiaozhi`。
- 网页 `Start AI` 会启动小智后台监听并立即切到 AI 页面；进入监听待机后，屏幕默认回到额度/待机页。
- 小智检测到唤醒词或新的对话活动后，屏幕会自动切回小智页面显示 STT/TTS。
- 网页 `Stop AI` 会彻底关闭小智会话；关闭后仅喊唤醒词不会重新启动 AI。
- 任务完成后边框闪烁 `CONFIG_ORNAMENT_DONE_FLASH_MS`，默认 5000 ms。
- 本地 Web 控制台提供状态查看、Bridge URL 测试、重启、清空配置和语音音量设置。

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

## 烧录

把 `COMx` 换成设备管理器中的实际串口，例如 `COM5`：

```cmd
idf.py -p COM5 flash
```

如需查看串口：

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
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

## 首次配网

没有保存 Wi-Fi 时，ESP32 会启动配置热点：

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

联网后可访问：

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

- 首次配网页可填写 Xiaozhi WebSocket URL 和 token。
- 联网后 Web 控制台的 `Xiaozhi AI` 区域可保存 URL/token、启动/停止会话、查看 STT/TTS 文本和上下行帧计数。
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

INMP441 `SCK`/`BCLK` 接 GPIO4，`WS`/`LRCLK` 接 GPIO5，`SD`/`DOUT` 接 GPIO14，`L/R` 接 GND 使用左声道。若 `L/R` 改接 3V3，需要启用 `CONFIG_ORNAMENT_XIAOZHI_MIC_SLOT_RIGHT`。小智会话播放 TTS 时会占用 I2S 输出；此时任务完成提示音会跳过，避免两个音频源同时写同一喇叭。

页面切换按键：按钮一端接 GPIO15，另一端接 GND。默认启用内部上拉，短按循环 `Auto -> Quota -> Tasks -> Clock -> Xiaozhi -> Auto`。手动切到的页面会保持显示到下一次按键，并优先于小智会话的自动页面聚焦；切回 `Auto` 后恢复自动显示逻辑。

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
