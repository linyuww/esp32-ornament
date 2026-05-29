# ESP32-S3 Codex 桌面摆件固件

这是 Codex 桌面摆件的 ESP-IDF 固件。固件运行在 ESP32-S3 上，驱动 1.54 寸 240x240 ST7789 SPI 方屏，通过 Wi-Fi 轮询 PC 上的本地桥接服务，显示 Codex 额度、任务 hook 状态、时间日期和 Wi-Fi 信号。

当前版本已经加入：

- 屏幕内的软件 RGB 边框，不需要外接 LED 灯环。
- SNTP 本地时钟小组件，显示 `HH:MM`、`MM-DD`、`WiFi -62dBm`。
- 低额度告警：当前额度或周额度低于 `25%` 显示橙色，低于 `10%` 显示红色。
- 任务完成后边框闪烁约 `10 秒`，文字和额度条不闪烁。
- 手机连接 ESP32 热点配网，自动扫描 Wi-Fi SSID，只需要选择 SSID 并填写密码。
- 连接 Wi-Fi 后提供 ESP32 本地 Web 控制台，可查看状态 JSON、测试 Bridge URL、重启、清空配置。
- 空闲一段时间后自动切换到待机时钟页，显示大号时间、日期、Wi-Fi 和额度摘要。
- 单文件合并固件 `release/codex_ornament_merged.bin`，从地址 `0x0` 烧录即可。

## 一句话流程

1. 按本文接好 ESP32-S3 和 1.54 寸 ST7789 SPI 方屏。
2. 烧录 `release/codex_ornament_merged.bin`。
3. 在 PC 上启动 `codex-ornament-bridge`。
4. 手机连接 ESP32 热点，打开 `http://192.168.4.1`，选择 Wi-Fi SSID，只填写密码和 PC 桥接地址。
5. 用 `/health`、`/state`、`/hook/codex` 做烟测。

## 目标硬件

- 主控：ESP32-S3-N16R8，16 MB Flash，8 MB PSRAM。
- 屏幕：1.54 寸 240x240 ST7789 IPS TFT 方屏，SPI，8 针模块。
- 逻辑电平：3.3 V。
- 桥接服务：PC 局域网地址上的 `http://<PC-LAN-IP>:8787/state`。
- 配网方式：ESP32 SoftAP 热点 + 手机浏览器。

## 接线

### 接线前检查

- ESP32-S3 和屏幕 IO 都按 3.3 V 连接，不要接 5 V 信号。
- `VCC` 接 3V3，所有 `GND` 都要和 ESP32 共地。
- 背光电流通常不能由 GPIO 直接供电。`GPIO7` 只适合作为 MOSFET 控制信号。
- 如果转接板已经集成背光限流和控制电路，优先按转接板丝印；否则按下面的 MOSFET 方案。

### 屏幕 8 针接口

参考 `1.54-SPI原理图.pdf`，模块接口定义如下：

| 模块引脚 | 功能 | 接法 |
| --- | --- | --- |
| GND | 电源地 | GND |
| VCC | 3.3 V 电源 | 3V3 |
| SCL | SPI 时钟 | ESP32-S3 GPIO12 |
| SDA | SPI MOSI 数据 | ESP32-S3 GPIO11 |
| RES | 显示复位 | ESP32-S3 GPIO8 |
| DC | 数据/命令选择 | ESP32-S3 GPIO13 |
| CS | SPI 片选，低有效 | ESP32-S3 GPIO10 |
| BLK | 背光开关 | ESP32-S3 GPIO7 |

### 固件默认 GPIO

这些值来自 `main/Kconfig.projbuild`，重新编译前可用 `idf.py menuconfig` 修改。

| 配置项 | ESP32-S3 GPIO | 屏幕信号 |
| --- | ---: | --- |
| `CONFIG_ORNAMENT_LCD_PIN_SCLK` | GPIO12 | SCL |
| `CONFIG_ORNAMENT_LCD_PIN_CS` | GPIO10 | CS |
| `CONFIG_ORNAMENT_LCD_PIN_RST` | GPIO8 | RESET |
| `CONFIG_ORNAMENT_LCD_PIN_MOSI` | GPIO11 | SDA |
| `CONFIG_ORNAMENT_LCD_PIN_DC` | GPIO13 | DC |
| `CONFIG_ORNAMENT_LCD_PIN_BL` | GPIO7 | 背光控制 |

### 背光 MOSFET 接法

推荐使用一个小信号 N-MOS 做低边开关：

```text
屏幕 VCC     -> 3V3
屏幕 BLK     -> N-MOS Drain
N-MOS Source -> GND
N-MOS Gate   -> ESP32-S3 GPIO7
ESP32-S3 GND -> 屏幕 GND / MOSFET Source 共地
```

如果没有 MOSFET，且转接板已经有背光限流，可以先让背光常亮：

```text
屏幕 VCC -> 3V3
屏幕 BLK -> 3V3
```

不要把屏幕背光 `BLK` 直接接到 ESP32 GPIO 当作供电路径，除非确认模块背光控制脚只是逻辑输入且电流在 GPIO 规格内。

## 烧录固件

推荐烧录单文件固件：

```text
D:\Desktop\codex\codex-quota-widget\firmware\esp32-ornament\release\codex_ornament_merged.bin
```

在 `ESP-IDF 5.4 CMD` 或已配置 ESP-IDF 环境的终端里执行：

```cmd
cd /d D:\Desktop\codex\codex-quota-widget\firmware\esp32-ornament\release
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash 0x0 codex_ornament_merged.bin
```

如果需要指定串口，例如 `COM5`：

```cmd
python -m esptool --chip esp32s3 -p COM5 -b 460800 --before default_reset --after hard_reset write_flash 0x0 codex_ornament_merged.bin
```

也可以多文件烧录：

```cmd
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash ^
  0x0 bootloader.bin ^
  0x8000 partition-table.bin ^
  0x10000 codex_ornament.bin
```

## 启动 PC 桥接服务

ESP32 不直接读取 Codex 登录凭据。PC 上的桥接服务负责查询 Codex 额度、接收本地 hook，然后把状态提供给 ESP32。

开发运行：

```powershell
cd D:\Desktop\codex\codex-quota-widget
cargo run -p codex-ornament-bridge
```

Release 运行：

```powershell
cd D:\Desktop\codex\codex-quota-widget
cargo build -p codex-ornament-bridge --release
.\target\release\codex-ornament-bridge.exe
```

默认监听：

```text
http://0.0.0.0:8787
```

ESP32 要访问 PC 的局域网 IP，不能填 `127.0.0.1`。在 PC 上查看 IPv4：

```powershell
ipconfig
```

假设 PC 的 IPv4 是 `192.168.1.23`，配网页面的 `Bridge State URL` 填：

```text
http://192.168.1.23:8787/state
```

如果屏幕显示 `Bridge offline`，先确认 Windows 防火墙允许局域网访问 `8787` 端口。

## 手机热点配网

首次烧录后，如果 NVS 中没有保存 Wi-Fi，ESP32 会启动配置热点：

```text
SSID: Codex-Ornament-xxxx
Password: codex1234
URL: http://192.168.4.1
```

手机操作：

1. 给 ESP32 上电。
2. 手机连接 `Codex-Ornament-xxxx`。
3. 浏览器打开 `http://192.168.4.1`。
4. 页面会自动扫描附近 Wi-Fi，并生成 SSID 下拉列表。
5. 选择目标 SSID，只填写 Wi-Fi 密码。
6. `Bridge State URL` 填 PC 局域网地址，例如 `http://192.168.1.23:8787/state`。
7. 可点击 `Test Wi-Fi and Bridge URL`；ESP32 会临时用当前 SSID/密码连接 Wi-Fi，然后请求 Bridge URL。
8. 点击保存，ESP32 写入 NVS 后自动重启。

启动逻辑：

1. 从 NVS 读取已保存的 Wi-Fi 和 bridge URL。
2. 没有保存 Wi-Fi 时进入 SoftAP 配网。
3. 已保存 Wi-Fi 连接失败时也会进入 SoftAP 配网。
4. 页面提交后写入 NVS，并自动重启。

## ESP32 本地 Web 控制台

配网完成、ESP32 连上家庭 Wi-Fi 后，串口日志会打印设备 IP，例如：

```text
I (...) esp_netif_handlers: sta ip: 192.168.1.108
I (...) web_console: web console started on http://<device-ip>/
```

在同一局域网浏览器打开：

```text
http://<ESP32-IP>/
```

控制台提供：

- 当前 Wi-Fi、RSSI、SNTP 时间、额度百分比、任务状态。
- `GET /status`：机器可读 JSON，包含 uptime、bridge URL、last fetch、heap、quota、task。
- `POST /test-bridge`：测试当前输入的 Bridge State URL 是否返回 HTTP 200 和有效 JSON。
- `POST /reboot`：重启 ESP32。
- `POST /clear-config`：清空 NVS 中保存的 Wi-Fi 和 Bridge URL，然后重启回到配网模式。

快速检查：

```powershell
Invoke-RestMethod http://<ESP32-IP>/status
```

注意：本地控制台没有密码，默认只适合放在可信局域网中调试使用。

## 屏幕 UI 行为

主界面保持 `03-clean-dashboard` 风格：

- 顶部：`CODEX QUOTA`。
- 标题下方：`HH:MM`、`MM-DD`、`WiFi -62dBm`；未校时时显示 `--:--`。
- 中部：`CURRENT` 和 `WEEKLY` 两个额度面板。
- 底部：`AGENT ACTIVE`、`TASK DONE`、`HOOK ERROR` 等状态；执行中 `AGENT ACTIVE` 前的小圆点会以 4Hz 呼吸闪烁。
- 外围软件边框：模拟 RGB 灯环，不需要真实灯环硬件。
- 待机时钟页：当 Bridge 返回 `idle` 持续默认 `120 秒` 后，自动切到大号时钟页面；一旦出现运行、完成、错误状态，就回到额度/状态页面。

颜色优先级：

1. `status=error`：红色最高优先级。
2. 任务刚完成：边框闪烁 10 秒。
3. 当前额度或周额度低于 `10%`：红色告警。
4. 当前额度或周额度低于 `25%`：橙色告警。
5. 正常：青色。

低额度判断使用 `min(primaryRemainingPercent, secondaryRemainingPercent)`。如果没有额度数据，固件不会触发低额度告警。

## 可配置项

这些配置在 `main/Kconfig.projbuild` 中：

| 配置项 | 默认值 | 说明 |
| --- | --- | --- |
| `ORNAMENT_SNTP_SERVER` | `pool.ntp.org` | SNTP 校时服务器 |
| `ORNAMENT_TIMEZONE` | `CST-8` | POSIX 时区，`CST-8` 表示 UTC+8 |
| `ORNAMENT_QUOTA_WARN_PERCENT` | `25` | 低额度橙色阈值 |
| `ORNAMENT_QUOTA_CRITICAL_PERCENT` | `10` | 严重低额度红色阈值 |
| `ORNAMENT_DONE_FLASH_MS` | `10000` | 任务完成后边框闪烁时长 |
| `ORNAMENT_POLL_INTERVAL_MS` | `3000` | ESP32 轮询桥接服务间隔 |
| `ORNAMENT_UI_FRAME_MS` | `250` | running/完成闪烁等本地 UI 动画刷新间隔 |
| `ORNAMENT_STANDBY_CLOCK_MS` | `120000` | idle 后切到待机时钟页的等待时间，设为 `0` 可关闭 |

修改后重新构建：

```cmd
cd /d D:\Desktop\codex\codex-quota-widget\firmware\esp32-ornament
idf.py menuconfig
idf.py build
```

## 功能测试

### 1. 桥接服务健康检查

PC 上执行：

```powershell
Invoke-RestMethod http://127.0.0.1:8787/health
```

预期返回：

```text
ok
```

### 2. 额度和状态接口

PC 上执行：

```powershell
Invoke-RestMethod http://127.0.0.1:8787/state
```

预期能看到 JSON，包含：

```text
status
quota
bridge
```

如果 `quota.status` 不是 `ok`，先确认 PC 上 Codex/ChatGPT 的登录状态和本地凭据。

### 3. 任务完成 hook

PC 上执行：

```powershell
Invoke-RestMethod `
  -Method Post `
  -Uri http://127.0.0.1:8787/hook/codex `
  -ContentType application/json `
  -Body '{"hook_event_name":"Stop","message":"manual smoke test"}'
```

然后检查：

```powershell
Invoke-RestMethod http://127.0.0.1:8787/state
```

预期：

- `/hook/codex` 返回 `ok: true`。
- `/state` 中 `status` 变成 `done`。
- ESP32 屏幕底部显示 `TASK DONE`。
- 软件边框闪烁约 10 秒，文字和额度条保持稳定。

### 4. 低额度告警

如果要手动测试低额度，可以让桥接服务返回类似字段：

```json
{
  "status": "running",
  "quota": {
    "status": "ok",
    "primaryRemainingPercent": 24,
    "secondaryRemainingPercent": 58,
    "primaryResetsAt": "2026-05-22T21:59:00+08:00",
    "secondaryResetsAt": "2026-05-18T16:14:00+08:00"
  }
}
```

预期：

- `24%` 触发橙色 UI。
- 改成 `8%` 触发红色 UI。
- 当前额度和周额度只要任意一个低于阈值，就按更低值告警。

### 5. ESP32 是否连上桥接服务

配网完成并重启后，屏幕预期流程：

```text
CODEX BOOTING
Wi-Fi connected
额度页面 / hook 状态页面
```

如果屏幕显示：

```text
ERROR
Bridge offline
```

按顺序检查：

1. PC 桥接服务是否还在运行。
2. 配网页面填写的 `Bridge State URL` 是否是 PC 局域网 IP，不是 `127.0.0.1`。
3. ESP32 和 PC 是否在同一个 Wi-Fi/局域网。
4. Windows 防火墙是否放行 `8787` 端口。
5. PC 上 `Invoke-RestMethod http://<PC-LAN-IP>:8787/state` 是否能访问。

### 6. 本地控制台和 Bridge URL 测试

配网成功后，在串口日志里找到 ESP32 IP，然后执行：

```powershell
Invoke-RestMethod http://<ESP32-IP>/status
```

预期：

- `wifi.connected` 为 `true`。
- `bridge_url` 是配网页保存的 `http://<PC-LAN-IP>:8787/state`。
- `fetch_error` 为 `ESP_OK` 时，说明 ESP32 已经能正常拉取 Bridge。

也可以打开 `http://<ESP32-IP>/`，在页面中点击 `Test Bridge URL`。如果失败，优先检查 PC 防火墙和 Bridge URL 是否误填 `127.0.0.1`。

### 7. 待机时钟页

确保 Bridge `/state` 返回 `status: idle`，保持默认约 120 秒不触发 hook。预期屏幕从额度页切到 `CODEX CLOCK` 时钟页，显示：

```text
CODEX CLOCK
HH:MM
MM-DD
WIFI -xxDBM
QUOTA xx%/xx%
```

触发一次 `/hook/codex` 或让状态变成 `running/done/error` 后，屏幕应退出时钟页并回到任务/额度状态页。

## UI 预览

预览工具使用和固件相同的 `display_core.c` 渲染核心，能在 PC 上生成 240x240 方屏 UI 图。

生成预览：

```powershell
cd D:\Desktop\codex\codex-quota-widget\firmware\esp32-ornament
python .\docs\render_ui_preview.py
```

输出文件：

| 文件 | 场景 |
| --- | --- |
| `docs\large-ui-preview-normal.png` | 正常额度，青色边框 |
| `docs\large-ui-preview-running-bright.png` | `AGENT ACTIVE` 小圆点亮帧 |
| `docs\large-ui-preview-running-dim.png` | `AGENT ACTIVE` 小圆点暗帧 |
| `docs\large-ui-preview-warn.png` | 额度低于 25%，橙色 |
| `docs\large-ui-preview-critical.png` | 额度低于 10%，红色 |
| `docs\large-ui-preview-done-flash.png` | 任务完成闪烁窗口，绿色边框 |
| `docs\large-ui-preview-unsynced.png` | 未校时，显示 `--:--` |
| `docs\large-ui-preview-clock.png` | 待机时钟页 |

预览只覆盖软件 UI，不代表真实屏幕的亮度、色温和视角。真实颜色以屏幕和驱动配置为准。

## 开发构建

本仓库不自动安装 ESP-IDF。手动安装 ESP-IDF v5.4.1 后，在 `ESP-IDF 5.4 CMD` 中执行：

```cmd
cd /d D:\Desktop\codex\codex-quota-widget\firmware\esp32-ornament
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

把 `COMx` 换成设备管理器中的串口号，例如 `COM5`。

重新生成单文件 bin：

```cmd
cd /d D:\Desktop\codex\codex-quota-widget\firmware\esp32-ornament\release
python -m esptool --chip esp32s3 merge_bin -o codex_ornament_merged.bin --flash_mode dio --flash_freq 80m --flash_size 16MB ^
  0x0 bootloader.bin ^
  0x8000 partition-table.bin ^
  0x10000 codex_ornament.bin
```

当前已验证构建环境：

```text
ESP-IDF: v5.4.1
target: esp32s3
flash: 16MB
PSRAM: Octal 80MHz
display driver: ESP-IDF built-in ST7789 SPI panel
```

## 构建故障处理

如果构建停在 ESP-IDF 自带文件，例如：

```text
components/esp_lcd/rgb/esp_lcd_panel_rgb.c: internal compiler error: Segmentation fault
```

这通常不是本项目源码语法错误，而是 Windows 下 ESP-IDF v5.4.1 的 xtensa GCC/ccache 构建链路偶发崩溃。工程顶层 `CMakeLists.txt` 已固定关闭 ccache：

```cmake
set(CCACHE_ENABLE 0 CACHE BOOL "Disable ccache to avoid ESP-IDF 5.4.1 xtensa GCC ICE on Windows")
```

首次遇到该错误后，清掉旧 CMake/Ninja 缓存再构建：

```cmd
cd /d D:\Desktop\codex\codex-quota-widget\firmware\esp32-ornament
idf.py fullclean
idf.py build
```

## 常见问题

- 找不到串口：检查设备管理器串口号，换数据线，必要时按住 BOOT 再点 EN/RESET。
- 屏幕不亮：先查 3V3/GND/BLK，再查 SCL/SDA/CS/DC/RES。
- 显示 `Bridge offline`：确认 PC 桥接服务运行、`Bridge State URL` 是 PC 局域网 IP、Windows 防火墙放行 `8787`。
- 看不到配网热点：如果设备已经连上保存的 Wi-Fi，就不会停留在配置热点；需要重配时可先擦除 flash 后重烧。
- 时间一直 `--:--`：确认 Wi-Fi 可访问公网 DNS/NTP，或把 `ORNAMENT_SNTP_SERVER` 改成局域网可访问的 NTP 服务器。

## 本版验证记录

- `python .\docs\render_ui_preview.py`：通过，生成 8 类预览图。
- `gcc -std=c11 -O2 -Wall -Wextra -Werror` 编译显示核心预览：通过。
- `idf.py build`：通过。
- `release\codex_ornament_merged.bin`：已重新合并，可从 `0x0` 单文件烧录。
- `ESP32 本地 Web 控制台`、`Bridge URL 测试按钮`、`待机时钟页`：源码已实现并通过 `idf.py build`。

硬件实机显示和电源稳定性仍需要在你的 ESP32-S3 + ST7789 方屏实物上验证。
