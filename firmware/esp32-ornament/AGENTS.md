# Repository Guidelines

## Project Structure & Module Organization

This is an ESP-IDF firmware project for the Codex ESP32-S3 ornament. The root `CMakeLists.txt` defines the `codex_ornament` project, with persistent defaults in `sdkconfig.defaults`, current local configuration in `sdkconfig`, and flash layout in `partitions.csv`.

Application code lives in `main/`. Keep each feature split into a `.c`/`.h` pair, for example `wifi.c`, `bridge_client.c`, `display.c`, and `web_console.c`. Embedded binary assets live under `main/assets/`; `task_done.pcm` is embedded through `main/CMakeLists.txt`. Documentation previews and rendering helpers live in `docs/`. Treat `build/`, `managed_components/`, debug logs, and `release/*.bin` outputs as generated unless a release update explicitly requires them.

## Build, Test, and Development Commands

Run commands from an ESP-IDF 5.4 shell with this directory as the working directory.

```powershell
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p COM5 flash monitor
```

`set-target` initializes the target, `menuconfig` edits `main/Kconfig.projbuild` options, `build` compiles the firmware, and `flash monitor` programs a board and tails logs. To flash the packaged release image, use:

```powershell
python -m esptool --chip esp32s3 -p COM5 -b 460800 write_flash 0x0 release/codex_ornament_merged.bin
```

## Coding Style & Naming Conventions

Use C with 4-space indentation. Keep functions and variables in `snake_case`; use `ORNAMENT_` for project enums/constants and `CONFIG_ORNAMENT_` for Kconfig-backed options. Prefer small static helper functions inside the owning module, and expose only module-level APIs through headers. Add new source files to `main/CMakeLists.txt` and new user-tunable settings to `main/Kconfig.projbuild`.

## Testing Guidelines

There is no standalone unit-test suite in this firmware directory. Minimum validation for firmware changes is `idf.py build`. For behavior changes, flash hardware and smoke test serial logs, Wi-Fi provisioning, bridge polling, display rendering, and the local web console:

```powershell
Invoke-RestMethod http://<ESP32-IP>/status
```

For display-only changes, update or regenerate relevant assets in `docs/` when previews are part of the change.

## Commit & Pull Request Guidelines

Recent commits use short, imperative Title Case subjects, such as `Fix source-aware ornament task tracking` and `Add active display marquee effect`. Follow that style and keep commits focused.

Pull requests should include a concise summary, hardware tested, commands run, related issue or motivation, and screenshots or `docs/` previews for UI/display changes. Note any changed GPIO, Kconfig defaults, partition layout, or release binary.

## Security & Configuration Tips

Do not commit real Wi-Fi credentials, private bridge URLs, or local machine secrets. The web console has no authentication, so document and test it as LAN-only tooling. Prefer `sdkconfig.defaults` for safe project defaults and NVS or local `menuconfig` for device-specific values.
