from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

from PIL import Image

PREVIEW_NAMES = [
    "normal",
    "running-bright",
    "running-dim",
    "warn",
    "critical",
    "done-flash",
    "done-flash-running",
    "unsynced",
    "clock",
    "music",
    "xiaozhi-idle",
    "xiaozhi-listening",
    "xiaozhi-speaking",
    "xiaozhi-connecting",
    "xiaozhi-error",
    "xiaozhi-done",
]


def run(cmd: list[str], cwd: Path) -> None:
    print(" ".join(cmd))
    subprocess.run(cmd, cwd=cwd, check=True)


def build_preview_tool(project_root: Path) -> Path:
    gcc = shutil.which("gcc")
    if gcc is None:
        raise SystemExit("gcc was not found. Add MinGW gcc to PATH or run this from an ESP-IDF shell.")

    docs_dir = project_root / "docs"
    main_dir = project_root / "main"
    exe = docs_dir / "preview_native.exe"
    cmd = [
        gcc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-DCONFIG_ORNAMENT_QUOTA_CRITICAL_PERCENT=10",
        "-DCONFIG_ORNAMENT_QUOTA_WARN_PERCENT=25",
        "-DCONFIG_ORNAMENT_MUSIC_COVER_ENABLED=1",
        "-DCONFIG_ORNAMENT_STANDBY_WALLPAPER_ENABLED=1",
        "-I",
        str(docs_dir),
        "-I",
        str(main_dir),
        "-o",
        str(exe),
        str(docs_dir / "preview_native.c"),
        str(main_dir / "display_core.c"),
        str(main_dir / "ornament_state.c"),
        str(main_dir / "assets" / "standby_wallpaper_rgb565.c"),
        "-lm",
    ]
    run(cmd, project_root)
    return exe


def convert_ppm(docs_dir: Path, name: str) -> None:
    ppm = docs_dir / f"ui-preview-{name}.ppm"
    png = docs_dir / f"ui-preview-{name}.png"
    large_png = docs_dir / f"large-ui-preview-{name}.png"
    image = Image.open(ppm).convert("RGB")
    image.save(png)
    image.resize((720, 720), Image.Resampling.NEAREST).save(large_png)


def main() -> None:
    project_root = Path(__file__).resolve().parents[1]
    docs_dir = project_root / "docs"
    exe = build_preview_tool(project_root)
    run([str(exe), str(docs_dir)], project_root)
    for name in PREVIEW_NAMES:
        convert_ppm(docs_dir, name)
        print(f"generated {docs_dir / f'large-ui-preview-{name}.png'}")


if __name__ == "__main__":
    main()
