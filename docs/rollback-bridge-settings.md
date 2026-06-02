# 撤销网桥相关设置

本文档记录 Codex Ornament 网桥在 Windows 上可能做过的系统级配置，以及对应的撤销方式。

## 停止当前网桥进程

```powershell
Get-Process -Name codex-ornament-bridge -ErrorAction SilentlyContinue | Stop-Process -Force
```

## 删除开机自启任务

当前计划任务名为 `Codex Ornament Bridge`。

```powershell
schtasks /delete /tn "Codex Ornament Bridge" /f
```

也可以用 PowerShell 删除：

```powershell
Unregister-ScheduledTask -TaskName "Codex Ornament Bridge" -Confirm:$false
```

## 撤销 LAN IP 覆盖

启动脚本会自动选择可用 LAN 地址，并过滤代理/虚拟网卡地址。如果手动设置过 `CODEX_ORNAMENT_LAN_IP`，从下面位置删除该变量：

```text
D:\Desktop\codex\codex-quota-widget\.env
D:\Desktop\codex\codex-quota-widget\.env.local
```

也检查用户或系统环境变量：

```powershell
[Environment]::GetEnvironmentVariable("CODEX_ORNAMENT_LAN_IP", "User")
[Environment]::GetEnvironmentVariable("CODEX_ORNAMENT_LAN_IP", "Machine")
```

删除用户级变量：

```powershell
[Environment]::SetEnvironmentVariable("CODEX_ORNAMENT_LAN_IP", $null, "User")
```

## 撤销 Codex hook

Codex hook 通常位于：

```text
%USERPROFILE%\.codex\hooks.json
```

删除其中指向本仓库脚本的 `UserPromptSubmit` 和 `Stop` hook：

```text
D:\Desktop\codex\codex-quota-widget\scripts\codex-ornament-hook.ps1
```

如果 `%USERPROFILE%\.codex\config.toml` 中配置了 `notify` 并指向同一脚本，也删除该 `notify` 配置。

## 撤销 Claude hook

Claude 配置通常位于：

```text
%USERPROFILE%\.claude\settings.json
```

删除其中指向本仓库脚本的 `UserPromptSubmit` 和 `Stop` hook，尤其是带有 `-Source Claude` 的命令。

`PermissionRequest` 语音播放 hook 只负责权限提示音，不是任务状态上报的必要配置。是否删除取决于是否还需要权限提示音。

## 撤销 Clash Verge Rev 本地域名配置

当前代理目录：

```text
C:\Users\86147\AppData\Roaming\io.github.clash-verge-rev.clash-verge-rev
```

如果曾为 TUN 模式下访问 ESP32 控制面板添加过本地域名配置，检查全局扩展或 `dns_config.yaml`，移除下面内容：

```yaml
dns:
  use-hosts: true

hosts:
  codex-ornament-4ad4.local: 192.168.1.100
  codex-ornament-4ad4: 192.168.1.100
```

如果添加过 LAN route exclude，也可以移除：

```yaml
tun:
  route-exclude-address:
    - 192.168.0.0/16
    - 224.0.0.0/4
```

修改后重载 Clash Verge Rev 配置或重启 core。

## 验证已撤销

```powershell
Get-Process -Name codex-ornament-bridge -ErrorAction SilentlyContinue
schtasks /query /tn "Codex Ornament Bridge"
```

预期：

- 第一条命令没有输出。
- 第二条命令提示找不到该计划任务。
