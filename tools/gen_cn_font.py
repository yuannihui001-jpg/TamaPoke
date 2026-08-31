#!/usr/bin/env python3
"""Generate the compact 25x25 Chinese glyph table used by the firmware."""

from pathlib import Path
import argparse
from typing import Optional

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]


def find_font(explicit: Optional[str]) -> Path:
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    candidates.extend(
        [
            Path("C:/Windows/Fonts/SourceHanSansCN-Bold.otf"),
            Path("C:/Windows/Fonts/SourceHanSansCN-Regular.otf"),
            Path("C:/Windows/Fonts/NotoSansSC-VF.ttf"),
            Path("C:/Windows/Fonts/NotoSansCJKsc-Regular.otf"),
            Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"),
        ]
    )
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise SystemExit("No Chinese TTF/TTC font found; pass --font explicitly")


def load_chars() -> list[str]:
    texts = [
        (ROOT / "i18n.cpp").read_text(encoding="utf-8"),
        (ROOT / "dex.h").read_text(encoding="utf-8"),
        (ROOT / "shop.h").read_text(encoding="utf-8"),
        (ROOT / "TamaPoke.ino").read_text(encoding="utf-8"),
    ]
    return sorted({c for text in texts for c in text if "\u4e00" <= c <= "\u9fff"})


def glyph_rows(font: ImageFont.FreeTypeFont, ch: str) -> list[int]:
    # Render larger, then threshold down to a crisp 25x25 bitmap.
    image = Image.new("L", (50, 50), 0)
    draw = ImageDraw.Draw(image)
    left, top, right, bottom = draw.textbbox((0, 0), ch, font=font)
    w, h = right - left, bottom - top
    draw.text(((50 - w) // 2 - left, (50 - h) // 2 - top), ch, fill=255, font=font)
    image = image.resize((25, 25), Image.Resampling.LANCZOS)
    return [
        sum((1 << (24 - x)) for x in range(25) if image.getpixel((x, y)) >= 64)
        for y in range(25)
    ]


def emit(font_path: Path, preview: bool) -> None:
    chars = load_chars()
    font = ImageFont.truetype(str(font_path), 42)
    # Noto Sans SC's variable font has a dedicated Bold master. Selecting it
    # before rasterization keeps the 25x25 bitmap strokes readable on AMOLED.
    if hasattr(font, "set_variation_by_name"):
        try:
            font.set_variation_by_name("Bold")
        except (AttributeError, OSError):
            pass
    rows = {ch: glyph_rows(font, ch) for ch in chars}

    lines = [
        "#pragma once",
        "#include <Arduino.h>",
        "",
        "#define CN_GLYPH_WIDTH 25",
        "#define CN_GLYPH_HEIGHT 25",
        "struct CnGlyph { uint16_t codepoint; uint32_t rows[CN_GLYPH_HEIGHT]; };",
        "",
        "static const CnGlyph CN_GLYPHS[] PROGMEM = {",
    ]
    for ch in chars:
        values = ", ".join(f"0x{row:07X}" for row in rows[ch])
        lines.append(f"  {{ 0x{ord(ch):04X}, {{ {values} }} }}, // {ch}")
    lines.extend(
        [
            "};",
            "#define CN_GLYPH_COUNT (sizeof(CN_GLYPHS) / sizeof(CN_GLYPHS[0]))",
            "",
        ]
    )
    (ROOT / "cn_font.h").write_text("\n".join(lines), encoding="utf-8")

    if preview:
        scale = 4
        cell_w = 25 * scale
        cell_h = 32 * scale
        sheet = Image.new("RGB", (cell_w * 10, ((len(chars) + 9) // 10) * cell_h), "white")
        draw = ImageDraw.Draw(sheet)
        for i, ch in enumerate(chars):
            x = (i % 10) * cell_w
            y = (i // 10) * cell_h
            for row, bits in enumerate(rows[ch]):
                for col in range(25):
                    if bits & (1 << (24 - col)):
                        draw.rectangle(
                            (x + col * scale, y + row * scale,
                             x + (col + 1) * scale - 1, y + (row + 1) * scale - 1),
                            fill="black")
            draw.text((x, y + 26 * scale), ch, fill="black", font=font)
        sheet.save(ROOT / "tools" / "cn_font_preview.png")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--font", help="path to a Chinese TTF/TTC font")
    parser.add_argument("--preview", action="store_true")
    args = parser.parse_args()
    emit(find_font(args.font), args.preview)
