# 撤销网桥相关设置

本文档记录 Codex Ornament 网桥在 Windows 上做过的系统级配置，以及对应的撤销命令。

## 停止当前网桥进程

```powershell
Get-Process -Name codex-ornament-bridge -ErrorAction SilentlyContinue | Stop-Process -Force
```

## 删除开机自启任务

当前计划任务名为 `Codex Ornament Bridge`，触发条件是当前用户登录时启动网桥。

```powershell
schtasks /delete /tn "Codex Ornament Bridge" /f
```

也可以用 PowerShell 删除：

```powershell
Unregister-ScheduledTask -TaskName "Codex Ornament Bridge" -Confirm:$false
```

## 撤销 LAN IP 覆盖

启动脚本会自动选择可用 LAN 地址，并忽略 `198.18.0.0/15` 这类代理或虚拟网卡地址。

如果手动配置过 `CODEX_ORNAMENT_LAN_IP`，可以从下面位置删除该变量：

```text
D:\Desktop\codex\codex-quota-widget\.env
D:\Desktop\codex\codex-quota-widget\.env.local
```

也检查用户或系统环境变量中是否存在同名变量：

```powershell
[Environment]::GetEnvironmentVariable("CODEX_ORNAMENT_LAN_IP", "User")
[Environment]::GetEnvironmentVariable("CODEX_ORNAMENT_LAN_IP", "Machine")
```

需要删除时：

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

如果 `%USERPROFILE%\.codex\config.toml` 中配置了 `notify` 并指向同一脚本，也一并删除该 `notify` 配置。

## 撤销 Claude hook

Claude 配置通常位于：

```text
%USERPROFILE%\.claude\settings.json
```

删除其中指向本仓库脚本的 `UserPromptSubmit` 和 `Stop` hook，尤其是带有 `-Source Claude` 的命令：

```text
D:\Desktop\codex\codex-quota-widget\scripts\codex-ornament-hook.ps1
```

`PermissionRequest` 语音播放 hook 只负责权限提示音，不是任务状态上报的必要配置。是否删除取决于是否还需要权限提示音。

## 撤销 Clash Nyanpasu 绕过配置

如果曾经为了访问本地域名修改 Clash Nyanpasu 配置，请检查：

```text
C:\Users\86147\AppData\Roaming\Clash Nyanpasu\config
```

移除与下面域名或地址相关的绕过规则：

```text
codex-ornament-4ad4.local
.local
127.0.0.1
localhost
```

修改后重启 Clash Nyanpasu，使配置生效。

## 验证已撤销

```powershell
Get-Process -Name codex-ornament-bridge -ErrorAction SilentlyContinue
schtasks /query /tn "Codex Ornament Bridge"
```

预期结果：

- 第一条命令没有输出。
- 第二条命令提示找不到该计划任务。
