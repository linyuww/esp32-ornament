from __future__ import annotations

from math import cos, pi, sin
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


W = 360
H = 360
SCALE = 3
OUT = Path(__file__).resolve().parent / "ui-variants"


def rgb565_palette(color: tuple[int, int, int]) -> tuple[int, int, int]:
    r, g, b = color
    r5 = r & 0xF8
    g6 = g & 0xFC
    b5 = b & 0xF8
    return r5, g6, b5


BLACK = rgb565_palette((0, 0, 0))
WHITE = rgb565_palette((240, 248, 255))
MUTED = rgb565_palette((112, 148, 172))
BLUE = rgb565_palette((16, 152, 255))
CYAN = rgb565_palette((24, 208, 255))
GREEN = rgb565_palette((56, 255, 28))
AMBER = rgb565_palette((255, 184, 48))
RED = rgb565_palette((255, 72, 72))
DIM_BLUE = rgb565_palette((0, 32, 72))
DIM_GREEN = rgb565_palette((8, 68, 20))
VIOLET = rgb565_palette((174, 92, 255))
MAGENTA = rgb565_palette((255, 66, 180))
ORANGE = rgb565_palette((255, 128, 48))
TEAL = rgb565_palette((44, 232, 196))


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    candidates = [
        "C:/Windows/Fonts/consolab.ttf" if bold else "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/segoeuib.ttf" if bold else "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arialbd.ttf" if bold else "C:/Windows/Fonts/arial.ttf",
    ]
    for path in candidates:
        try:
            return ImageFont.truetype(path, size * SCALE)
        except OSError:
            continue
    return ImageFont.load_default()


def canvas() -> tuple[Image.Image, ImageDraw.ImageDraw]:
    image = Image.new("RGB", (W * SCALE, H * SCALE), BLACK)
    draw = ImageDraw.Draw(image)
    return image, draw


def sc(value: int | float) -> int:
    return int(round(value * SCALE))


def box(x0: int, y0: int, x1: int, y1: int) -> tuple[int, int, int, int]:
    return sc(x0), sc(y0), sc(x1), sc(y1)


def draw_center_text(
    draw: ImageDraw.ImageDraw,
    xy: tuple[int, int],
    text: str,
    fill: tuple[int, int, int],
    size: int,
    bold: bool = False,
) -> None:
    f = font(size, bold)
    bbox = draw.textbbox((0, 0), text, font=f)
    x = sc(xy[0]) - (bbox[2] - bbox[0]) // 2
    y = sc(xy[1]) - (bbox[3] - bbox[1]) // 2
    draw.text((x, y), text, font=f, fill=fill)


def draw_text(
    draw: ImageDraw.ImageDraw,
    xy: tuple[int, int],
    text: str,
    fill: tuple[int, int, int],
    size: int,
    bold: bool = False,
    anchor: str | None = None,
) -> None:
    draw.text((sc(xy[0]), sc(xy[1])), text, font=font(size, bold), fill=fill, anchor=anchor)


def mask_round(image: Image.Image) -> Image.Image:
    mask = Image.new("L", image.size, 0)
    md = ImageDraw.Draw(mask)
    md.ellipse(box(4, 4, W - 4, H - 4), fill=255)
    black = Image.new("RGB", image.size, BLACK)
    return Image.composite(image, black, mask)


def save(name: str, image: Image.Image) -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    image = mask_round(image)
    small = image.resize((W, H), Image.Resampling.LANCZOS)
    large = small.resize((W * 2, H * 2), Image.Resampling.NEAREST)
    small.save(OUT / f"{name}.png")
    large.save(OUT / f"{name}-large.png")


def draw_dotted_ring(draw: ImageDraw.ImageDraw, color: tuple[int, int, int], radius: int = 154, count: int = 120) -> None:
    for i in range(count):
        angle = 2 * pi * i / count
        x = 180 + cos(angle) * radius
        y = 180 + sin(angle) * radius
        draw.rectangle(box(x - 1, y - 1, x + 1, y + 1), fill=color)


def draw_arc(
    draw: ImageDraw.ImageDraw,
    rect: tuple[int, int, int, int],
    start: int,
    end: int,
    color: tuple[int, int, int],
    width: int,
) -> None:
    draw.arc(box(*rect), start=start, end=end, fill=color, width=sc(width))


def segmented_bar(
    draw: ImageDraw.ImageDraw,
    x: int,
    y: int,
    w: int,
    h: int,
    percent: int,
    active: tuple[int, int, int],
    inactive: tuple[int, int, int],
    segments: int = 24,
) -> None:
    gap = 4
    seg_w = max(2, (w - gap * (segments - 1)) // segments)
    active_count = percent * segments // 100
    for i in range(segments):
        fill = active if i < active_count else inactive
        draw.rounded_rectangle(
            box(x + i * (seg_w + gap), y, x + i * (seg_w + gap) + seg_w, y + h),
            radius=sc(2),
            fill=fill,
        )


def variant_01_neon_hud() -> None:
    image, draw = canvas()
    draw_dotted_ring(draw, BLUE)
    draw.line(box(42, 94, 318, 94), fill=DIM_BLUE, width=sc(1))
    draw.line(box(42, 199, 318, 199), fill=DIM_BLUE, width=sc(1))
    draw_center_text(draw, (180, 55), "CODEX", CYAN, 21, True)
    draw_text(draw, (54, 112), "CURRENT", CYAN, 19, True)
    draw_text(draw, (306, 106), "27%", CYAN, 29, True, "ra")
    segmented_bar(draw, 52, 145, 256, 8, 27, CYAN, DIM_BLUE, 24)
    draw_text(draw, (80, 174), "RESET 21:59", WHITE, 14, False)
    draw_text(draw, (54, 222), "WEEKLY", GREEN, 19, True)
    draw_text(draw, (306, 216), "73%", GREEN, 29, True, "ra")
    segmented_bar(draw, 52, 250, 256, 8, 73, GREEN, DIM_GREEN, 24)
    draw_text(draw, (80, 274), "RESET MON 16:14", WHITE, 14, False)
    draw.ellipse(box(108, 304, 118, 314), fill=GREEN)
    draw_center_text(draw, (185, 309), "TASK DONE", GREEN, 14, True)
    save("01-neon-hud", image)


def variant_02_dual_arc() -> None:
    image, draw = canvas()
    draw.ellipse(box(23, 23, 337, 337), outline=DIM_BLUE, width=sc(2))
    draw_arc(draw, (28, 28, 332, 332), 210, 210 + 92, CYAN, 10)
    draw_arc(draw, (45, 45, 315, 315), 210, 210 + 246, GREEN, 10)
    for r in (93, 116, 139):
        draw.ellipse(box(180 - r, 180 - r, 180 + r, 180 + r), outline=rgb565_palette((0, 34, 46)), width=sc(1))
    draw_center_text(draw, (180, 64), "CODEX", WHITE, 18, True)
    draw_center_text(draw, (180, 139), "CURRENT", MUTED, 13, True)
    draw_center_text(draw, (180, 169), "27%", CYAN, 38, True)
    draw_center_text(draw, (180, 215), "WEEKLY 73%", GREEN, 20, True)
    draw_text(draw, (92, 252), "NEXT 21:59", WHITE, 13)
    draw_text(draw, (92, 276), "WEEK MON", MUTED, 13)
    draw.ellipse(box(135, 298, 145, 308), fill=GREEN)
    draw_center_text(draw, (190, 304), "TASK DONE", GREEN, 11, True)
    save("02-dual-arc", image)


def variant_03_clean_dashboard() -> None:
    image, draw = canvas()
    draw.ellipse(box(18, 18, 342, 342), outline=rgb565_palette((32, 52, 62)), width=sc(2))
    draw.arc(box(21, 21, 339, 339), 300, 60, fill=TEAL, width=sc(3))
    draw_center_text(draw, (180, 50), "CODEX QUOTA", WHITE, 16, True)
    rows = [
        (70, "CURRENT", "27%", CYAN, 27, "21:59"),
        (184, "WEEKLY", "73%", GREEN, 73, "MON 16:14"),
    ]
    for y, label, value, color, percent, reset in rows:
        draw.rounded_rectangle(box(46, y, 314, y + 90), radius=sc(6), outline=rgb565_palette((14, 44, 58)), width=sc(1))
        draw.rectangle(box(46, y + 14, 50, y + 76), fill=color)
        draw_text(draw, (66, y + 15), label, MUTED, 13, True)
        draw_text(draw, (292, y + 8), value, color, 28, True, "ra")
        segmented_bar(draw, 66, y + 52, 212, 7, percent, color, rgb565_palette((14, 32, 40)), 20)
        draw_text(draw, (66, y + 68), f"RESET {reset}", WHITE, 12)
    draw.ellipse(box(116, 304, 126, 314), fill=GREEN)
    draw_center_text(draw, (184, 309), "READY", GREEN, 13, True)
    save("03-clean-dashboard", image)


def variant_04_radar_pulse() -> None:
    image, draw = canvas()
    for r, color in [(154, BLUE), (120, rgb565_palette((0, 74, 92))), (84, rgb565_palette((0, 46, 62)))]:
        draw.ellipse(box(180 - r, 180 - r, 180 + r, 180 + r), outline=color, width=sc(1))
    for angle in range(0, 360, 30):
        x = 180 + cos(angle * pi / 180) * 154
        y = 180 + sin(angle * pi / 180) * 154
        draw.line(box(180, 180, x, y), fill=rgb565_palette((0, 24, 34)), width=sc(1))
    draw.pieslice(box(31, 31, 329, 329), start=318, end=344, fill=rgb565_palette((0, 52, 56)))
    draw.line(box(180, 180, 180 + 154 * cos(331 * pi / 180), 180 + 154 * sin(331 * pi / 180)), fill=TEAL, width=sc(2))
    draw_center_text(draw, (180, 55), "CODEX", TEAL, 20, True)
    draw_center_text(draw, (180, 127), "AGENT ACTIVE", CYAN, 15, True)
    draw_center_text(draw, (119, 189), "27%", CYAN, 32, True)
    draw_center_text(draw, (241, 189), "73%", GREEN, 32, True)
    draw_center_text(draw, (119, 224), "CURRENT", MUTED, 12, True)
    draw_center_text(draw, (241, 224), "WEEKLY", MUTED, 12, True)
    draw.rounded_rectangle(box(73, 262, 287, 285), radius=sc(4), outline=rgb565_palette((0, 84, 100)), width=sc(1))
    draw_center_text(draw, (180, 275), "NEXT RESET 21:59", WHITE, 13)
    save("04-radar-pulse", image)


def variant_05_terminal_grid() -> None:
    image, draw = canvas()
    for x in range(44, 317, 18):
        draw.line(box(x, 42, x, 318), fill=rgb565_palette((0, 20, 28)), width=sc(1))
    for y in range(42, 319, 18):
        draw.line(box(44, y, 316, y), fill=rgb565_palette((0, 20, 28)), width=sc(1))
    draw.rectangle(box(50, 48, 310, 74), outline=CYAN, width=sc(1))
    draw_text(draw, (62, 51), "> CODEX ORNAMENT", CYAN, 15, True)
    draw_text(draw, (58, 103), "CURRENT", WHITE, 14, True)
    draw_text(draw, (248, 94), "27%", CYAN, 34, True)
    segmented_bar(draw, 58, 139, 244, 7, 27, CYAN, DIM_BLUE, 18)
    draw_text(draw, (58, 163), "RESET 21:59", MUTED, 13)
    draw.line(box(58, 194, 302, 194), fill=rgb565_palette((0, 82, 92)), width=sc(1))
    draw_text(draw, (58, 218), "WEEKLY", WHITE, 14, True)
    draw_text(draw, (248, 209), "73%", GREEN, 34, True)
    segmented_bar(draw, 58, 253, 244, 7, 73, GREEN, DIM_GREEN, 18)
    draw_text(draw, (58, 276), "RESET MON 16:14", MUTED, 13)
    draw_text(draw, (106, 307), "[ TASK DONE ]", GREEN, 14, True)
    draw.ellipse(box(21, 21, 339, 339), outline=rgb565_palette((0, 100, 116)), width=sc(2))
    save("05-terminal-grid", image)


def variant_06_orbit_cards() -> None:
    image, draw = canvas()
    draw_arc(draw, (25, 25, 335, 335), 160, 332, VIOLET, 4)
    draw_arc(draw, (37, 37, 323, 323), 344, 520, TEAL, 4)
    for angle, color in [(160, VIOLET), (332, VIOLET), (344, TEAL), (160, TEAL)]:
        x = 180 + cos(angle * pi / 180) * 155
        y = 180 + sin(angle * pi / 180) * 155
        draw.ellipse(box(x - 5, y - 5, x + 5, y + 5), fill=color)
    draw_center_text(draw, (180, 49), "CODEX", WHITE, 18, True)
    draw.rounded_rectangle(box(63, 83, 297, 158), radius=sc(8), outline=rgb565_palette((48, 28, 86)), width=sc(1))
    draw_text(draw, (82, 94), "CURRENT", MUTED, 13, True)
    draw_text(draw, (268, 93), "27%", VIOLET, 29, True, "ra")
    segmented_bar(draw, 82, 132, 196, 7, 27, VIOLET, rgb565_palette((38, 22, 64)), 16)
    draw.rounded_rectangle(box(63, 185, 297, 260), radius=sc(8), outline=rgb565_palette((12, 72, 66)), width=sc(1))
    draw_text(draw, (82, 196), "WEEKLY", MUTED, 13, True)
    draw_text(draw, (268, 195), "73%", TEAL, 29, True, "ra")
    segmented_bar(draw, 82, 234, 196, 7, 73, TEAL, rgb565_palette((10, 50, 46)), 16)
    draw_center_text(draw, (180, 284), "RESET 21:59 / MON 16:14", WHITE, 10)
    draw_center_text(draw, (180, 306), "HOOK OK", GREEN, 12, True)
    save("06-orbit-cards", image)


def contact_sheet() -> None:
    names = [
        "01-neon-hud",
        "02-dual-arc",
        "03-clean-dashboard",
        "04-radar-pulse",
        "05-terminal-grid",
        "06-orbit-cards",
    ]
    thumbs = [Image.open(OUT / f"{name}.png").convert("RGB") for name in names]
    sheet = Image.new("RGB", (W * 3, H * 2), BLACK)
    for index, thumb in enumerate(thumbs):
        x = (index % 3) * W
        y = (index // 3) * H
        sheet.paste(thumb, (x, y))
    sheet.save(OUT / "ui-variants-contact-sheet.png")


def main() -> None:
    variant_01_neon_hud()
    variant_02_dual_arc()
    variant_03_clean_dashboard()
    variant_04_radar_pulse()
    variant_05_terminal_grid()
    variant_06_orbit_cards()
    contact_sheet()
    print(f"UI variants written to {OUT}")


if __name__ == "__main__":
    main()
