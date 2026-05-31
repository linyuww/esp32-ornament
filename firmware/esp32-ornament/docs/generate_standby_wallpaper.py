from __future__ import annotations

from pathlib import Path

from PIL import Image

SOURCE_IMAGE = Path("D:/AssaultLilyViewer_v0.5/Card010018002.jpg")
PROJECT_ROOT = Path(__file__).resolve().parents[1]
OUTPUT_H = PROJECT_ROOT / "main" / "standby_wallpaper.h"
OUTPUT_C = PROJECT_ROOT / "main" / "assets" / "standby_wallpaper_rgb565.c"
WIDTH = 240
HEIGHT = 240


def center_crop_square(image: Image.Image) -> Image.Image:
    width, height = image.size
    side = min(width, height)
    left = (width - side) // 2
    top = (height - side) // 2
    return image.crop((left, top, left + side, top + side))


def rgb565(red: int, green: int, blue: int) -> int:
    return ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)


def format_rows(values: list[int]) -> str:
    rows = []
    for offset in range(0, len(values), 12):
        chunk = values[offset : offset + 12]
        rows.append("    " + ", ".join(f"0x{value:04X}" for value in chunk) + ",")
    return "\n".join(rows)


def main() -> None:
    image = Image.open(SOURCE_IMAGE).convert("RGB")
    image = center_crop_square(image).resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
    pixels = [rgb565(red, green, blue) for red, green, blue in image.getdata()]

    OUTPUT_H.write_text(
        "\n".join(
            [
                "#pragma once",
                "",
                "#include <stdint.h>",
                "",
                f"#define STANDBY_WALLPAPER_WIDTH {WIDTH}",
                f"#define STANDBY_WALLPAPER_HEIGHT {HEIGHT}",
                "",
                "extern const uint16_t standby_wallpaper_rgb565[STANDBY_WALLPAPER_WIDTH * STANDBY_WALLPAPER_HEIGHT];",
                "",
            ]
        ),
        encoding="utf-8",
    )
    OUTPUT_C.write_text(
        "\n".join(
            [
                '#include "standby_wallpaper.h"',
                "",
                "const uint16_t standby_wallpaper_rgb565[STANDBY_WALLPAPER_WIDTH * STANDBY_WALLPAPER_HEIGHT] = {",
                format_rows(pixels),
                "};",
                "",
            ]
        ),
        encoding="utf-8",
    )
    print(f"generated {OUTPUT_C}")


if __name__ == "__main__":
    main()
