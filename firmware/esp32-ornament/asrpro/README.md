# ASRPRO Voice Control First Version

This folder is a local working copy of the Tianwen ASRPRO project. The original
`D:\天问Block\asrpro` folder was not modified.

## Wiring

- ESP32 GPIO17 TX -> ASRPRO Serial1 RX
- ESP32 GPIO18 RX <- ASRPRO Serial1 TX
- UART baud rate: 9600
- Common GND is required

## Voice Commands

| Spoken phrase | ASRPRO ID | UART token | ESP32 action |
| --- | ---: | --- | --- |
| 当前状态 | 2001 | `voice_status` | Show bridge, voice, quota, task, and Wi-Fi status |
| 显示额度 | 2002 | `show_quota` | Show the normal quota/status page |
| 显示任务 | 2003 | `show_tasks` | Show Codex/Claude task counts and status |
| 显示时钟 | 2004 | `show_clock` | Show the standby clock page |
| 刷新一下 | 2005 | `refresh_state` | Fetch bridge state immediately |
| 重新匹配 | 2006 | `bridge_match` | Run bridge auto-match |
| 安静模式 | 2007 | `quiet_on` | Suppress local and ASRPRO done reminders |
| 恢复提醒 | 2008 | `quiet_off` | Re-enable done reminders |

ASRPRO still listens for `codex_done` from ESP32 and plays audio ID `10500`
when a task-done event arrives.

## Files Kept For Review

- `asr.cpp` / `_asr.cpp`: ASRPRO callback code that maps `snid` to UART tokens.
- `user_file/cmd_info/[60000]{cmd_info}.xlsx`: command table source.
- `user_file/cmd_info/[60000]{cmd_info}.xlsx.bin`: generated command table.
- `user_file/[60000]{cmd_info}.xlsx.bin`: generated command table copied to `user_file`.
- `user_code/[0]code.bin`: generated ASRPRO user code binary from the verified build.
- `asr_pro_sdk/projects/offline_asr_sample/project_file/makefile`: local toolchain fix for `riscv-nuclei-elf-gcc-ar`.

## Validation

From this copied project:

```bat
cd user_file\cmd_info
..\..\asr_pro_sdk\tools\ci-tool-kit.exe "cmd-info" "-V2"
```

The command table generated `[60000]{cmd_info}.xlsx.bin` successfully.

```bat
cd asr_pro_sdk\projects\offline_asr_sample\project_file
set path=%CD%\..\..\..\..\gcc\bin;%CD%\..\..\..\..\asr_pro_sdk\tools\build-tools\bin;%path%
make
```

The ASRPRO build completed successfully and generated `user_code/[0]code.bin`.
