#!/usr/bin/env python3
"""Generate the LVGL fonts of the launcher with lv_font_conv 1.5.3.

The faces and sizes follow tab5-launcher/launcher.css:
  --f-ui      Chakra Petch      (tabs, card names, menus, dialogs)
  --f-display Orbitron          (logo, clock, bootscreen, numbers)
  --f-mono    Share Tech Mono   (labels, tags, buttons)
Icons replace the prototype's SVG sprite with FontAwesome 5 glyphs from the LVGL
component (scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff).

Usage (Node.js and lv_font_conv are only needed when fonts change):
  npm install lv_font_conv@1.5.3          (in any folder outside the firmware)
  python tools/gen_fonts.py --converter <that folder>/node_modules/lv_font_conv/lv_font_conv.js [--node <node.exe>]
"""
import argparse
import json
import re
import subprocess
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
TTF = PROJECT / "tools" / "fonts"
OUT = PROJECT / "main" / "fonts"

ASCII = ["0x20-0x7E"]
# middle dot, multiplication sign, degree, dashes, bullet, ellipsis: used by the prototype strings
EXTRA = ["0xB0", "0xB7", "0xD7", "0x2013", "0x2014", "0x2022", "0x2026"]
# app names come from flash/SD and may use Latin-1 + Latin Extended-A (Polish, German, ...)
LATIN = ["0x20-0x7E", "0xA0-0x17F", "0x2013", "0x2014", "0x2022", "0x2026"]
# File names, paths and text files in the mono face (SD path, info rows, clipboard, text viewer) need
# Latin-1 + Latin Extended-A too. Share Tech Mono has Latin-1 but only ten Extended-A letters (no
# a/c/e/n/s/z with Polish accents), so l5_latin_15 takes the rest from Chakra Petch Regular, and the
# mono sizes use it (or Chakra Petch 17) as LVGL fallback for the letters they lack. Merging the
# letters into the mono fonts themselves would raise their line height (15 px: 17 -> 20) and move
# every label that uses them.
MONO_LATIN1 = ["0xA0-0xFF", "0x131", "0x141-0x142", "0x152-0x153", "0x160-0x161", "0x178", "0x17D-0x17E"]
MONO_EXT_A = ["0x100-0x130", "0x132-0x140", "0x143-0x151", "0x154-0x15F", "0x162-0x177", "0x179-0x17C"]

# FontAwesome codepoints, see main/ui/l5_icons.h
ICONS = [
    0xF002, 0xF00C, 0xF00D, 0xF011, 0xF013, 0xF015, 0xF019, 0xF021, 0xF023, 0xF04B,
    0xF053, 0xF054, 0xF067, 0xF071, 0xF07B, 0xF0AC, 0xF0DB, 0xF0E7, 0xF142, 0xF15B,
    0xF186, 0xF1EB, 0xF240, 0xF287, 0xF2DB, 0xF2ED, 0xF35D, 0xF7C2,
]

FONTS = [
    # name,              ttf,                          size, ranges [, options]
    # options: "second" = (ttf, ranges) merged in, "fallback" = font LVGL asks for missing letters,
    # "kerning" = False drops the kerning pairs
    ("l5_chakra_sb16",  "ChakraPetch-SemiBold.ttf",    16, ASCII + EXTRA),
    ("l5_chakra_sb17",  "ChakraPetch-SemiBold.ttf",    17, LATIN),
    ("l5_chakra_sb19",  "ChakraPetch-SemiBold.ttf",    19, LATIN),
    ("l5_chakra_r17",   "ChakraPetch-Regular.ttf",     17, LATIN),
    ("l5_chakra_b24",   "ChakraPetch-Bold.ttf",        24, LATIN),
    ("l5_chakra_b26",   "ChakraPetch-Bold.ttf",        26, LATIN),
    ("l5_chakra_b28",   "ChakraPetch-Bold.ttf",        28, LATIN),
    ("l5_chakra_b40",   "ChakraPetch-Bold.ttf",        40, LATIN),
    ("l5_orb_b12",      "Orbitron-Bold.ttf",           12, ASCII + EXTRA),
    ("l5_orb_b16",      "Orbitron-Bold.ttf",           16, ASCII + EXTRA),
    ("l5_orb_b22",      "Orbitron-Bold.ttf",           22, ASCII + EXTRA),
    ("l5_orb_xb27",     "Orbitron-ExtraBold.ttf",      27, ASCII + EXTRA),
    # initials on the 420 px HOME emblem (one-slot card, U.cover: cs * 0.36 = 54 px) and Charge Mode's "78%" / "USB"
    ("l5_orb_xb54",     "Orbitron-ExtraBold.ttf",      54, ["0x25", "0x30-0x39", "0x41-0x5A"]),
    ("l5_orb_blk76",    "Orbitron-Black.ttf",          76, ["0x41-0x5A"]),
    ("l5_mono_11",      "ShareTechMono-Regular.ttf",   11, ASCII + EXTRA),
    ("l5_mono_12",      "ShareTechMono-Regular.ttf",   12, ASCII + EXTRA),
    ("l5_mono_13",      "ShareTechMono-Regular.ttf",   13, ASCII + EXTRA, {"fallback": "l5_latin_15"}),
    ("l5_mono_14",      "ShareTechMono-Regular.ttf",   14, ASCII + EXTRA, {"fallback": "l5_latin_15"}),
    ("l5_mono_15",      "ShareTechMono-Regular.ttf",   15, ASCII + EXTRA, {"fallback": "l5_latin_15"}),
    ("l5_mono_16",      "ShareTechMono-Regular.ttf",   16, ASCII + EXTRA, {"fallback": "l5_chakra_r17"}),
    ("l5_mono_18",      "ShareTechMono-Regular.ttf",   18, ASCII + EXTRA, {"fallback": "l5_chakra_r17"}),
    ("l5_mono_22",      "ShareTechMono-Regular.ttf",   22, ASCII + EXTRA),
    ("l5_latin_15",     "ShareTechMono-Regular.ttf",   15, MONO_LATIN1,
     {"second": ("ChakraPetch-Regular.ttf", MONO_EXT_A), "kerning": False}),
]
ICON_SIZES = (20, 22, 24, 54, 64)
# Faces of 24 px and more (titles, logo, big icons) are stored compressed (lv_font_conv RLE +
# prefilter, lossless, CONFIG_LV_USE_FONT_COMPRESSED): about half the flash, and they are drawn far
# less often than the list and label sizes, which stay uncompressed for drawing speed.
COMPRESSED_MIN_SIZE = 24
# Chips, rows and buttons only use the 20 px icons: heart, star, list, link, sort (OTA),
# copy, paste, pen (SD file actions)
ICONS_20 = [0xF004, 0xF005, 0xF03A, 0xF0C1, 0xF0DC, 0xF0C5, 0xF0EA, 0xF304]


def find_fontawesome() -> Path:
    for root in ("managed_components/lvgl__lvgl", "../aurora-player/managed_components/lvgl__lvgl"):
        path = PROJECT / root / "scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff"
        if path.exists():
            return path
    raise SystemExit("FontAwesome woff not found: run idf.py reconfigure once so managed_components exists")


def run(node, converter, name, sources, size, options=None):
    options = options or {}
    ranges = {ttf: list(rs) for ttf, rs in sources}
    while True:
        packing = [] if size >= COMPRESSED_MIN_SIZE else ["--no-compress", "--no-prefilter"]
        if options.get("fallback"):
            packing += ["--lv-fallback", options["fallback"]]
        if options.get("kerning") is False:
            packing += ["--no-kerning"]
        cmd = [node, str(converter), *packing, "--bpp", "4", "--size", str(size),
               "--format", "lvgl", "--lv-include", "lvgl.h", "--lv-font-name", name, "-o", str(OUT / f"{name}.c")]
        for ttf, rs in ranges.items():
            cmd += ["--font", str(ttf)]
            for r in rs:
                cmd += ["-r", r]
        result = subprocess.run(cmd, cwd=PROJECT, capture_output=True, text=True)
        if result.returncode == 0:
            print(f"{name}.c")
            return
        # lv_font_conv stops when a face lacks every glyph of one range: drop that range and retry
        missing = re.search(r"doesn't have any characters included in range (0x[0-9a-f]+)-(0x[0-9a-f]+)",
                            result.stdout + result.stderr)
        if not missing:
            raise SystemExit(result.stdout + result.stderr)
        lo = int(missing.group(1), 16)
        for rs in ranges.values():
            for r in list(rs):
                if int(r.split("-")[0], 16) == lo:
                    rs.remove(r)
                    print(f"  {name}: face has no U+{lo:04X}, skipped")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--converter", type=Path, required=True)
    parser.add_argument("--node", default="node")
    parser.add_argument("--only", nargs="*", help="regenerate only these fonts (e.g. l5_icon_20)")
    args = parser.parse_args()
    version = json.loads((args.converter.resolve().parent / "package.json").read_text())["version"]
    if version != "1.5.3":
        raise SystemExit(f"Expected lv_font_conv 1.5.3, found {version}")
    OUT.mkdir(parents=True, exist_ok=True)
    names = []
    wanted = lambda n: not args.only or n in args.only
    for name, ttf, size, ranges, *opts in FONTS:
        options = opts[0] if opts else {}
        sources = [(TTF / ttf, ranges)]
        if "second" in options:
            sources.append((TTF / options["second"][0], options["second"][1]))
        if wanted(name):
            run(args.node, args.converter, name, sources, size, options)
        names.append(name)
    awesome = find_fontawesome()
    for size in ICON_SIZES:
        name = f"l5_icon_{size}"
        icons = sorted(ICONS + (ICONS_20 if size == 20 else []))
        if wanted(name):
            run(args.node, args.converter, name, [(awesome, [hex(c) for c in icons])], size)
        names.append(name)
    header = ["/* Generated by tools/gen_fonts.py - do not edit */", "#pragma once", '#include "lvgl.h"', ""]
    header += [f"LV_FONT_DECLARE({n})" for n in names]
    (OUT / "l5_fonts.h").write_text("\n".join(header) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
