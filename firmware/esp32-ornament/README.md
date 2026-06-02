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
- `POST /reboot` 重启 ESP32。
- `POST /clear-config` 清空 Wi-Fi 与 Bridge URL 后重启到配网模式。

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
