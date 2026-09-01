#!/usr/bin/env python3
"""Build the TamaPoke shop icon set, firmware table, and round-screen previews."""

from __future__ import annotations

import io
import math
import re
import urllib.request
from collections import Counter
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
ASSET_ROOT = ROOT / "assets" / "shop_items_v3"
SOURCE_ROOT = ASSET_ROOT / "source"
PIXEL_ROOT = ASSET_ROOT / "firmware_32"
CACHE_ROOT = ROOT / "tools" / "shop_art_cache"
PREVIEW_ROOT = ROOT / "previews" / "shop_items_v3"
HEADER_PATH = ROOT / "shop_icons.h"

CANVAS = 192
SOURCE_SIZE = 96
ICON_SIZE = 32
INK = "#20232c"
WHITE = "#fffdf5"
OPENMOJI_BASE = "https://raw.githubusercontent.com/hfg-gmuend/openmoji/master/color/72x72"

CATEGORIES = [
    ("食品", [
        "苹果", "香蕉", "橙子", "葡萄", "西瓜", "汉堡", "三明治", "面包", "蛋糕", "布丁",
        "草莓", "桃子", "樱桃", "菠萝", "椰子", "饼干", "甜甜圈", "冰淇淋", "热牛奶", "能量棒",
    ]),
    ("玩具", [
        "橡皮球", "毛绒玩偶", "拉绳", "摇铃", "积木", "风筝", "小鼓", "玩具火车", "泡泡机", "激光逗猫",
        "飞盘", "跳绳", "纸飞机", "魔术方块", "音乐盒", "水枪", "沙滩桶", "望远镜", "机器人", "星星投影",
    ]),
    ("药品", [
        "恢复药", "绷带", "维生素", "解毒药", "肠胃药", "退烧药", "安睡药", "能量药", "消毒液", "万能药",
        "止痛药", "护心丸", "清凉油", "驱虫药", "润肤膏", "眼药水", "营养液", "活力针", "幸运药", "全效药",
    ]),
    ("装备", [
        "训练剑", "防护盾", "安全帽", "护甲", "护身符", "疾风靴", "力量手套", "勇者徽章", "守护披风", "能量晶体",
        "木制法杖", "铁护腕", "火焰宝石", "冰霜宝石", "雷电宝石", "龙鳞甲", "圣盾", "隐身斗篷", "生命核心", "冠军奖杯",
    ]),
    ("道具", [
        "可爱小屋", "大树", "花坛", "喷泉", "小池塘", "沙发", "小床", "餐桌", "落地灯", "彩色地毯",
        "秋千", "滑梯", "秋日树", "雪人", "帐篷", "篝火", "风车", "邮筒", "小桥", "彩虹门",
    ]),
    ("旅游", [
        "城市公园", "海边沙滩", "森林小径", "雪山小屋", "长城", "金字塔", "空中花园", "宙斯神像", "阿尔忒弥斯神庙", "摩索拉斯陵墓",
        "太阳神巨像", "亚历山大灯塔", "埃菲尔铁塔", "比萨斜塔", "西班牙角斗场", "凯旋门", "悉尼歌剧院", "泰姬陵", "双子塔", "富士山",
    ]),
]

OPENMOJI_CODES = [
    ["1F34E", "1F34C", "1F34A", "1F347", "1F349", "1F354", "1F96A", "1F35E", "1F370", "1F36E",
     "1F353", "1F351", "1F352", "1F34D", "1F965", "1F36A", "1F369", "1F368", "1F95B", "1F36B"],
    ["1F3BE", "1F9F8", "1FA80", "1F514", "1F9F1", "1FA81", "1F941", "1F682", "1FAE7", "1F526",
     "1F94F", "1F9F5", "1F6E9", "1F9E9", "1F3B6", "1F52B", "1FAA3", "1F52D", "1F916", "1F4A1"],
    ["1F48A", "1FA79", "1F9F4", "1F9EA", "1F48A", "1F321", "1F48A", "1F9EA", "1F9F4", "1F489",
     "1F48A", "2764", "1F9F4", "1F41B", "1F9F4", "1F4A7", "1F9EA", "1F489", "1F340", "2695"],
    ["1F5E1", "1F6E1", "26D1", "1F9BA", "1F4FF", "1F97E", "1F9E4", "1F3C5", "1F9E3", "1F48E",
     "1FA84", "1F9E4", "1F48E", "1F48E", "1F48E", "1F9BA", "1F6E1", "1F9E3", "2764", "1F3C6"],
    ["1F3E0", "1F333", "1F33A", "26F2", "1F3DE", "1F6CB", "1F6CF", "1FA91", "1F4A1", "1F9F6",
     "1F3A0", "1F6DD", "1F342", "2603", "26FA", "1F525", "1F3A1", "1F4EE", "1F309", "1F308"],
    ["1F3DE"] * 20,
]

CATEGORY_COLORS = ["#f47c61", "#719fe8", "#59bd8f", "#9b7adb", "#e1a34f", "#53b9d1"]
FONT_PATH = Path(r"C:\Windows\Fonts\NotoSansSC-VF.ttf")


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(FONT_PATH), size=size, index=0)


def ensure_dirs() -> None:
    for path in (SOURCE_ROOT, PIXEL_ROOT, CACHE_ROOT, PREVIEW_ROOT):
        path.mkdir(parents=True, exist_ok=True)


def fetch_openmoji(code: str) -> Image.Image:
    cache_path = CACHE_ROOT / f"{code}.png"
    if not cache_path.exists():
        req = urllib.request.Request(f"{OPENMOJI_BASE}/{code}.png", headers={"User-Agent": "TamaPoke-art-builder"})
        try:
            with urllib.request.urlopen(req, timeout=25) as response:
                cache_path.write_bytes(response.read())
        except Exception:
            fallback = "1F381"
            req = urllib.request.Request(f"{OPENMOJI_BASE}/{fallback}.png", headers={"User-Agent": "TamaPoke-art-builder"})
            with urllib.request.urlopen(req, timeout=25) as response:
                cache_path.write_bytes(response.read())
    return Image.open(io.BytesIO(cache_path.read_bytes())).convert("RGBA")


def outlined_polygon(draw: ImageDraw.ImageDraw, points, fill, width=8):
    draw.polygon(points, fill=fill)
    draw.line(list(points) + [points[0]], fill=INK, width=width, joint="curve")


def draw_symbol(draw: ImageDraw.ImageDraw, kind: str, cx: int, cy: int, size: int, color: str) -> None:
    w = max(4, size // 5)
    if kind == "plus":
        draw.rounded_rectangle((cx-w, cy-size, cx+w, cy+size), radius=w, fill=color)
        draw.rounded_rectangle((cx-size, cy-w, cx+size, cy+w), radius=w, fill=color)
    elif kind == "heart":
        pts = [(cx, cy+size), (cx-size, cy), (cx-size, cy-size//2), (cx-size//2, cy-size),
               (cx, cy-size//2), (cx+size//2, cy-size), (cx+size, cy-size//2), (cx+size, cy)]
        draw.polygon(pts, fill=color)
    elif kind == "bolt":
        draw.polygon([(cx+2, cy-size), (cx-size, cy+2), (cx-2, cy+2), (cx-6, cy+size),
                      (cx+size, cy-3), (cx+3, cy-3)], fill=color)
    elif kind == "drop":
        draw.polygon([(cx, cy-size), (cx-size, cy+5), (cx, cy+size), (cx+size, cy+5)], fill=color)
    elif kind == "moon":
        draw.ellipse((cx-size, cy-size, cx+size, cy+size), fill=color)
        draw.ellipse((cx-size//3, cy-size, cx+size, cy+size//2), fill=WHITE)
    elif kind == "star":
        pts = []
        for i in range(10):
            a = -math.pi / 2 + i * math.pi / 5
            r = size if i % 2 == 0 else size * 0.42
            pts.append((cx + math.cos(a) * r, cy + math.sin(a) * r))
        draw.polygon(pts, fill=color)
    elif kind == "bug":
        draw.ellipse((cx-size, cy-size//2, cx+size, cy+size), fill=color)
        draw.line((cx, cy-size, cx, cy+size), fill=WHITE, width=max(2, w//2))
    elif kind == "snow":
        for a in (0, math.pi/3, 2*math.pi/3):
            dx, dy = math.cos(a)*size, math.sin(a)*size
            draw.line((cx-dx, cy-dy, cx+dx, cy+dy), fill=color, width=max(3, w//2))


def add_badge(image: Image.Image, slot: int, category: int) -> None:
    draw = ImageDraw.Draw(image)
    badge_colors = ["#ef476f", "#ffd166", "#58c7d8", "#73c174", "#8b78db"]
    kinds = ["plus", "heart", "drop", "bolt", "moon", "star", "bug", "snow"]
    cx, cy, radius = 155, 154, 26
    draw.ellipse((cx-radius, cy-radius, cx+radius, cy+radius), fill=WHITE, outline=INK, width=7)
    draw_symbol(draw, kinds[slot % len(kinds)], cx, cy, 14, badge_colors[(slot + category) % len(badge_colors)])


def custom_toy(slot: int) -> Image.Image | None:
    if slot not in (11, 13, 14, 19):
        return None
    im = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    if slot == 11:
        d.arc((34, 20, 158, 168), 205, 335, fill=INK, width=10)
        d.line((39, 126, 62, 155), fill="#e85d75", width=13)
        d.line((153, 126, 130, 155), fill="#e85d75", width=13)
        d.rounded_rectangle((48, 145, 72, 166), radius=8, fill="#ffd166", outline=INK, width=6)
        d.rounded_rectangle((120, 145, 144, 166), radius=8, fill="#ffd166", outline=INK, width=6)
    elif slot == 13:
        d.rounded_rectangle((37, 37, 155, 155), radius=17, fill=WHITE, outline=INK, width=9)
        cols = ["#ef476f", "#ffd166", "#58c7d8", "#73c174", "#8b78db", "#ff8c42", "#ef476f", "#58c7d8", "#ffd166"]
        for y in range(3):
            for x in range(3):
                d.rounded_rectangle((48+x*34, 48+y*34, 74+x*34, 74+y*34), radius=5, fill=cols[y*3+x], outline=INK, width=4)
    elif slot == 14:
        d.rounded_rectangle((32, 82, 158, 151), radius=14, fill="#e7a95b", outline=INK, width=9)
        d.ellipse((58, 95, 105, 142), fill="#fff5c2", outline=INK, width=7)
        d.line((144, 89, 171, 62, 171, 43), fill=INK, width=8, joint="curve")
        d.ellipse((160, 31, 181, 52), fill="#f47c61", outline=INK, width=5)
        d.arc((88, 22, 129, 65), 210, 45, fill="#719fe8", width=8)
        d.ellipse((77, 53, 97, 68), fill="#719fe8", outline=INK, width=4)
    else:
        d.rounded_rectangle((42, 92, 150, 156), radius=18, fill="#719fe8", outline=INK, width=9)
        d.ellipse((73, 106, 119, 148), fill="#d9f5ff", outline=INK, width=6)
        d.polygon([(96, 18), (106, 52), (142, 52), (113, 72), (124, 108), (96, 87), (68, 108), (79, 72), (50, 52), (86, 52)], fill="#ffd166", outline=INK)
    return im


def custom_prop(slot: int) -> Image.Image | None:
    if slot not in (7, 9, 10, 11, 16, 19):
        return None
    im = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    if slot == 7:
        d.ellipse((28, 46, 164, 105), fill="#e1a34f", outline=INK, width=9)
        for x in (50, 136): d.line((x, 96, x-6, 164), fill=INK, width=11)
        d.ellipse((62, 59, 91, 84), fill="#fff5c2", outline=INK, width=5)
        d.ellipse((104, 56, 138, 87), fill="#f47c61", outline=INK, width=5)
    elif slot == 9:
        d.rounded_rectangle((24, 55, 168, 145), radius=20, fill="#8b78db", outline=INK, width=9)
        for x, c in ((45,"#ffd166"),(74,"#58c7d8"),(103,"#ef476f"),(132,"#73c174")):
            d.rounded_rectangle((x, 69, x+18, 132), radius=7, fill=c)
    elif slot == 10:
        d.line((42, 35, 150, 35), fill=INK, width=11)
        d.line((51, 35, 31, 169), fill=INK, width=11); d.line((141, 35, 161, 169), fill=INK, width=11)
        d.line((75, 39, 75, 126), fill="#719fe8", width=7); d.line((117, 39, 117, 126), fill="#719fe8", width=7)
        d.rounded_rectangle((61, 121, 131, 151), radius=8, fill="#f47c61", outline=INK, width=7)
    elif slot == 11:
        outlined_polygon(d, [(52,27),(136,27),(159,151),(82,151)], "#58c7d8")
        d.line((60, 45, 128, 45), fill=WHITE, width=8); d.line((77, 140, 34, 169), fill=INK, width=12)
    elif slot == 16:
        d.rounded_rectangle((88, 79, 104, 172), radius=6, fill="#e1a34f", outline=INK, width=5)
        d.ellipse((83, 68, 109, 94), fill="#f47c61", outline=INK, width=6)
        for pts, c in [([(93,75),(36,34),(79,68)],"#ffd166"), ([(99,75),(156,34),(113,68)],"#58c7d8"),
                       ([(99,87),(156,129),(113,94)],"#73c174"), ([(93,87),(36,129),(79,94)],"#ef476f")]:
            outlined_polygon(d, pts, c, 5)
    else:
        d.line((52, 166, 52, 82, 96, 43, 140, 82, 140, 166), fill=INK, width=12, joint="curve")
        for box, c in [((48,78,78,110),"#ef476f"),((78,48,112,80),"#ffd166"),((112,78,144,110),"#58c7d8")]:
            d.arc(box, 180, 360, fill=c, width=12)
    return im


def travel_icon(slot: int) -> Image.Image:
    im = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    sky, green, stone, gold, red, blue = "#bde9f5", "#69b96b", "#e7d8b5", "#efbd4f", "#df645b", "#4f9fd4"
    d.ellipse((16, 16, 176, 176), fill=sky, outline=INK, width=8)
    if slot == 0:
        d.ellipse((24,111,168,169), fill=green, outline=INK, width=6); d.polygon([(74,164),(97,86),(121,164)], fill="#f4d69b")
        for x in (47,145): d.rectangle((x-5,66,x+5,137), fill="#7c5537"); d.ellipse((x-28,35,x+28,91), fill=green, outline=INK, width=5)
    elif slot == 1:
        d.rectangle((20,111,172,170), fill="#f6d078"); d.rectangle((20,91,172,118), fill=blue)
        d.line((98,67,98,153), fill=INK, width=7); outlined_polygon(d, [(47,78),(99,38),(151,78)], red, 6)
    elif slot == 2:
        d.polygon([(20,170),(75,93),(117,93),(172,170)], fill="#ddb87b")
        for x in (43,70,126,151): d.rectangle((x-4,53,x+4,150), fill="#76523b"); d.ellipse((x-23,30,x+23,78), fill=green, outline=INK, width=5)
    elif slot == 3:
        outlined_polygon(d, [(24,104),(72,45),(107,104)], "#eef7ff", 6); outlined_polygon(d, [(84,104),(132,34),(171,104)], "#eef7ff", 6)
        d.rectangle((56,105,140,164), fill="#e9a45f", outline=INK, width=7); outlined_polygon(d, [(48,108),(98,70),(148,108)], red, 7)
    elif slot == 4:
        d.polygon([(17,152),(48,103),(81,124),(116,79),(175,123),(175,171),(17,171)], fill="#78a56e")
        pts=[(20,138),(53,119),(84,135),(117,99),(174,123)]; d.line(pts, fill=INK, width=16, joint="curve"); d.line(pts, fill=stone, width=9, joint="curve")
        for x,y in pts[1:-1]: d.rectangle((x-8,y-24,x+8,y+3), fill=stone, outline=INK, width=4)
    elif slot == 5:
        outlined_polygon(d, [(24,159),(83,51),(139,159)], gold, 8); outlined_polygon(d, [(94,159),(133,89),(171,159)], "#e3a94b", 7); d.ellipse((31,30,59,58), fill="#ffd75e", outline=INK, width=5)
    elif slot == 6:
        for i,(x,y,w) in enumerate([(35,128,122),(49,98,94),(65,68,62)]):
            d.rounded_rectangle((x,y,x+w,y+31), radius=6, fill=stone, outline=INK, width=6)
            for px in range(x+8,x+w-5,20): d.ellipse((px,y-18,px+20,y+9), fill=green, outline=INK, width=3)
    elif slot == 7:
        d.rounded_rectangle((60,74,132,155), radius=25, fill=stone, outline=INK, width=7); d.ellipse((75,35,117,81), fill=stone, outline=INK, width=7)
        d.line((64,101,29,130), fill=INK, width=11); d.line((128,101,160,66), fill=INK, width=11); draw_symbol(d,"bolt",165,60,18,gold)
    elif slot in (8, 9):
        d.polygon([(42,75),(150,75),(132,50),(60,50)], fill=gold, outline=INK)
        for x in range(51,151,20): d.rectangle((x,76,x+10,146), fill=WHITE, outline=INK, width=4)
        d.rectangle((36,145,156,162), fill=stone, outline=INK, width=6)
        if slot == 9: d.polygon([(54,49),(96,25),(138,49)], fill=red, outline=INK)
    elif slot == 10:
        d.line((54,160,75,61,117,61,139,160), fill=INK, width=13, joint="curve"); d.ellipse((76,27,116,70), fill=gold, outline=INK, width=7)
        d.line((74,91,35,69), fill=INK, width=9); d.line((118,91,157,69), fill=INK, width=9); d.rectangle((20,154,172,170), fill=blue)
    elif slot == 11:
        d.polygon([(76,150),(116,150),(109,45),(83,45)], fill=WHITE, outline=INK); d.polygon([(83,45),(96,23),(109,45)], fill=red, outline=INK)
        d.rectangle((59,149,133,166), fill=stone, outline=INK, width=6); d.polygon([(118,61),(174,45),(122,78)], fill="#fff3a8")
    elif slot == 12:
        d.line((96,25,51,165), fill=INK, width=12); d.line((96,25,141,165), fill=INK, width=12); d.line((58,143,134,143), fill=INK, width=9)
        d.line((72,103,120,103), fill=INK, width=8); d.line((83,66,109,66), fill=INK, width=7); d.line((96,25,96,166), fill=INK, width=6)
    elif slot == 13:
        pts=[(74,25),(124,32),(112,164),(62,158)]; outlined_polygon(d,pts,WHITE,7)
        for y in (53,80,107,134): d.arc((72,y,118,y+26),0,180,fill=INK,width=5)
    elif slot == 14:
        d.ellipse((28,44,164,158), fill="#d6a36c", outline=INK, width=8); d.rectangle((28,91,164,158), fill="#d6a36c")
        for y in (71,103,132):
            for x in range(43,153,29): d.arc((x,y,x+20,y+22),180,360,fill=INK,width=5)
        d.line((28,158,164,158),fill=INK,width=8)
    elif slot == 15:
        d.rectangle((44,84,148,160), fill=stone, outline=INK, width=8); d.arc((65,78,127,145),180,360,fill=INK,width=9)
        d.rectangle((57,48,135,85), fill=stone, outline=INK, width=7); d.rectangle((73,29,119,49), fill=stone, outline=INK, width=6)
    elif slot == 16:
        for pts,c in [([(31,154),(72,63),(96,154)],WHITE), ([(67,154),(108,43),(132,154)],WHITE), ([(102,154),(148,74),(168,154)],WHITE)]: outlined_polygon(d,pts,c,7)
        d.rectangle((24,153,174,169),fill=blue)
    elif slot == 17:
        d.rectangle((38,102,154,160), fill=WHITE, outline=INK, width=7); d.ellipse((65,42,127,111), fill=WHITE, outline=INK, width=7)
        d.rectangle((89,35,103,50), fill=gold, outline=INK, width=4)
        for x in (31,161): d.rectangle((x-7,71,x+7,159), fill=WHITE, outline=INK, width=5); d.ellipse((x-13,50,x+13,77), fill=WHITE, outline=INK, width=5)
    elif slot == 18:
        for x in (55,118):
            d.polygon([(x,37),(x+24,37),(x+31,158),(x-7,158)], fill="#b9d8e8", outline=INK)
            for y in range(55,143,18): d.line((x+2,y,x+23,y), fill=blue, width=4)
        d.line((79,82,118,82), fill=INK, width=8); d.line((79,96,118,96), fill=gold, width=5)
    else:
        outlined_polygon(d, [(22,149),(75,72),(106,112),(137,55),(174,149)], "#91b2c7", 7)
        d.polygon([(75,72),(92,96),(106,112),(120,91),(137,55),(144,76),(151,95),(167,149),(22,149)], fill="#f4f7fb")
        d.ellipse((122,24,166,68), fill=red, outline=INK, width=5)
        d.line((31,159,164,159), fill=green, width=13)
    return im


def build_item(category: int, slot: int) -> Image.Image:
    if category == 5:
        return travel_icon(slot)
    custom = custom_toy(slot) if category == 1 else custom_prop(slot) if category == 4 else None
    if custom is not None:
        return custom
    source = fetch_openmoji(OPENMOJI_CODES[category][slot])
    bbox = source.getchannel("A").getbbox()
    if bbox:
        source = source.crop(bbox)
    scale = min(170 / source.width, 170 / source.height)
    source = source.resize((max(1, round(source.width * scale)), max(1, round(source.height * scale))), Image.Resampling.LANCZOS)
    im = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    im.alpha_composite(source, ((CANVAS-source.width)//2, (CANVAS-source.height)//2))
    if category in (2, 3):
        add_badge(im, slot, category)
    return im


def pixelize(source: Image.Image) -> Image.Image:
    icon = source.resize((ICON_SIZE, ICON_SIZE), Image.Resampling.LANCZOS)
    alpha = icon.getchannel("A")
    quantized = icon.convert("RGB").quantize(colors=15, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE).convert("RGBA")
    quantized.putalpha(alpha.point(lambda value: 255 if value >= 72 else 0))
    return quantized


def rgb565(rgb) -> int:
    r, g, b = rgb
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def build_header(pixel_paths: list[Path]) -> None:
    palettes, packed_items = [], []
    for path in pixel_paths:
        im = Image.open(path).convert("RGBA")
        pixels = list(im.get_flattened_data())
        colors = Counter((r, g, b) for r, g, b, a in pixels if a >= 72)
        chosen = [rgb for rgb, _ in colors.most_common(15)]
        while len(chosen) < 15:
            chosen.append((0, 0, 0))
        palette = [0] + [rgb565(color) for color in chosen]
        indices = []
        for r, g, b, a in pixels:
            if a < 72:
                indices.append(0); continue
            best = min(range(15), key=lambda i: (chosen[i][0]-r)**2 + (chosen[i][1]-g)**2 + (chosen[i][2]-b)**2)
            indices.append(best + 1)
        packed = [(indices[i] << 4) | indices[i+1] for i in range(0, len(indices), 2)]
        palettes.append(palette); packed_items.append(packed)
    lines = [
        "#pragma once", "#include <Arduino.h>", "", "// Generated by tools/build_shop_item_art.py.",
        f"#define SHOP_ICON_SIZE {ICON_SIZE}", "#define SHOP_ICON_PALETTE_SIZE 16", "",
        "static const uint16_t SHOP_ICON_PALETTES[SHOP_TOTAL_ITEMS][SHOP_ICON_PALETTE_SIZE] PROGMEM = {",
    ]
    for palette in palettes:
        lines.append("  {" + ", ".join(f"0x{value:04X}" for value in palette) + "},")
    lines += ["};", "", f"static const uint8_t SHOP_ICON_PIXELS[SHOP_TOTAL_ITEMS][{ICON_SIZE*ICON_SIZE//2}] PROGMEM = {{"]
    for packed in packed_items:
        lines.append("  {")
        for i in range(0, len(packed), 24):
            lines.append("    " + ", ".join(f"0x{value:02X}" for value in packed[i:i+24]) + ",")
        lines.append("  },")
    lines += ["};", ""]
    HEADER_PATH.write_text("\n".join(lines), encoding="utf-8")


def load_prices() -> list[list[int]]:
    text = (ROOT / "shop.h").read_text(encoding="utf-8")
    products = re.findall(r'\{"([^"]+)",\s*(\d+),\s*\d+\}', text)
    expected = [name for _, names in CATEGORIES for name in names]
    found = [name for name, _ in products]
    if found != expected:
        raise RuntimeError("shop.h product order does not match the art table")
    values = [int(price) for _, price in products]
    return [values[i*20:(i+1)*20] for i in range(6)]


def text_center(draw, xy, text, face, fill):
    box = draw.textbbox((0, 0), text, font=face)
    draw.text((xy[0]-(box[2]-box[0])/2, xy[1]-(box[3]-box[1])/2-box[1]), text, font=face, fill=fill)


def round_screen(category: int, prices: list[list[int]], pixel_paths: list[Path], detail_slot: int | None = None) -> Image.Image:
    size = 466
    im = Image.new("RGB", (size, size), "#05070b"); d = ImageDraw.Draw(im)
    d.ellipse((2, 2, 464, 464), fill="#24364f", outline="#fffdf5", width=2)
    title = CATEGORIES[category][0]
    text_center(d, (233, 29), title, font(25), WHITE)
    d.ellipse((350, 18, 368, 36), fill="#ffd166", outline=INK, width=2)
    d.text((374, 12), "00580", font=font(22), fill=WHITE)
    if detail_slot is not None:
        name = CATEGORIES[category][1][detail_slot]
        d.rounded_rectangle((38, 52, 428, 382), radius=18, fill=WHITE, outline=INK, width=2)
        text_center(d, (233, 78), name, font(25), INK)
        icon = Image.open(pixel_paths[category*20+detail_slot]).convert("RGBA").resize((96, 96), Image.Resampling.NEAREST)
        im.paste(icon, (185, 94), icon)
        value_text = "需要燃料30" if category == 5 else ("饥饿感 +42" if category == 0 else "喜悦感 +16")
        text_center(d, (233, 213), value_text, font(23), INK)
        info = f"价格 {prices[category][detail_slot]}G   库存 2"
        text_center(d, (233, 257), info, font(24), INK)
        d.rounded_rectangle((176, 274, 290, 312), radius=7, outline=INK, width=2)
        d.polygon([(146,293),(158,284),(158,302)], fill=INK); d.polygon([(320,293),(308,284),(308,302)], fill=INK)
        text_center(d, (233, 293), "x1", font(23), INK)
        d.rounded_rectangle((112, 326, 204, 366), radius=8, fill="#e0e7ee")
        d.rounded_rectangle((262, 326, 354, 366), radius=8, fill="#55b383")
        d.line((145,333,171,359), fill=INK, width=6); d.line((171,333,145,359), fill=INK, width=6)
        d.line((291,347,303,358,326,334), fill=WHITE, width=6, joint="curve")
        for i in range(20):
            x = 143 + i * 10
            if i == detail_slot: d.ellipse((x-3,389,x+3,395), fill=WHITE)
            else: d.ellipse((x-2,390,x+2,394), outline=WHITE, width=1)
    else:
        for row in range(5):
            slot = row
            y = 64 + row * 56
            d.rounded_rectangle((85, y, 381, y+46), radius=9, fill="#314763" if row & 1 else "#263751", outline=WHITE, width=2)
            icon = Image.open(pixel_paths[category*20+slot]).convert("RGBA")
            im.paste(icon, (92, y+7), icon)
            d.text((130, y+4), CATEGORIES[category][1][slot], font=font(23), fill=WHITE)
            price = f"{prices[category][slot]}G"
            box = d.textbbox((0,0), price, font=font(23)); d.text((365-(box[2]-box[0]), y+4), price, font=font(23), fill=WHITE)
        text_center(d, (233, 373), "1-5 / 20", font(18), WHITE)
    mask = Image.new("L", (size, size), 0); ImageDraw.Draw(mask).ellipse((1,1,465,465), fill=255)
    out = Image.new("RGB", (size,size), "#10151e"); out.paste(im, (0,0), mask)
    return out


def build_previews(prices: list[list[int]], pixel_paths: list[Path]) -> None:
    previews = [
        (round_screen(0, prices, pixel_paths), "食品列表"),
        (round_screen(1, prices, pixel_paths), "玩具列表"),
        (round_screen(5, prices, pixel_paths), "旅游列表"),
        (round_screen(0, prices, pixel_paths, 5), "汉堡详情"),
        (round_screen(3, prices, pixel_paths, 12), "火焰宝石详情"),
        (round_screen(5, prices, pixel_paths, 12), "埃菲尔铁塔详情"),
    ]
    sheet = Image.new("RGB", (1510, 1080), "#111722"); d = ImageDraw.Draw(sheet)
    text_center(d, (755, 40), "TamaPoke 商店物品新画风 · 圆屏实机比例预览", font(32), WHITE)
    for i, (screen, label) in enumerate(previews):
        x = 24 + (i % 3) * 495; y = 82 + (i // 3) * 495
        sheet.paste(screen, (x+14, y))
        text_center(d, (x+247, y+474), label, font(22), "#dce5ef")
    sheet.save(PREVIEW_ROOT / "shop-game-preview.png")

    catalog = Image.new("RGB", (1840, 1320), "#111722"); cd = ImageDraw.Draw(catalog)
    text_center(cd, (920, 38), "TamaPoke 商店 120 项卡通物品总览", font(34), WHITE)
    for category, (category_name, names) in enumerate(CATEGORIES):
        ox = 20 + (category % 3) * 610; oy = 78 + (category // 3) * 612
        cd.rounded_rectangle((ox, oy, ox+590, oy+590), radius=12, fill="#202b3b", outline=CATEGORY_COLORS[category], width=3)
        text_center(cd, (ox+295, oy+28), category_name, font(27), CATEGORY_COLORS[category])
        for slot, name in enumerate(names):
            col, row = slot % 5, slot // 5
            x, y = ox+16+col*114, oy+58+row*130
            cd.rounded_rectangle((x,y,x+104,y+120), radius=8, fill="#f6f3e8")
            icon = Image.open(pixel_paths[category*20+slot]).convert("RGBA").resize((64,64), Image.Resampling.NEAREST)
            catalog.paste(icon, (x+20,y+8), icon)
            text_center(cd, (x+52,y+88), name, font(16), INK)
            text_center(cd, (x+52,y+108), f"{prices[category][slot]}G", font(15), "#566173")
    catalog.save(PREVIEW_ROOT / "shop-all-120-items.png")


def main() -> None:
    ensure_dirs()
    prices = load_prices()
    pixel_paths: list[Path] = []
    for category, (category_name, names) in enumerate(CATEGORIES):
        source_dir = SOURCE_ROOT / f"{category+1:02d}-{category_name}"
        pixel_dir = PIXEL_ROOT / f"{category+1:02d}-{category_name}"
        source_dir.mkdir(parents=True, exist_ok=True); pixel_dir.mkdir(parents=True, exist_ok=True)
        for slot, name in enumerate(names):
            art = build_item(category, slot)
            source = art.resize((SOURCE_SIZE, SOURCE_SIZE), Image.Resampling.LANCZOS)
            source_path = source_dir / f"{slot+1:02d}-{name}.png"
            pixel_path = pixel_dir / f"{slot+1:02d}-{name}.png"
            source.save(source_path)
            pixelize(source).save(pixel_path)
            pixel_paths.append(pixel_path)
    build_header(pixel_paths)
    build_previews(prices, pixel_paths)
    license_url = "https://raw.githubusercontent.com/hfg-gmuend/openmoji/master/LICENSE.txt"
    req = urllib.request.Request(license_url, headers={"User-Agent": "TamaPoke-art-builder"})
    with urllib.request.urlopen(req, timeout=25) as response:
        (ASSET_ROOT / "OPENMOJI_LICENSE.txt").write_bytes(response.read())
    print(f"Built {len(pixel_paths)} icons")
    print(HEADER_PATH)
    print(PREVIEW_ROOT / "shop-game-preview.png")
    print(PREVIEW_ROOT / "shop-all-120-items.png")


if __name__ == "__main__":
    main()
