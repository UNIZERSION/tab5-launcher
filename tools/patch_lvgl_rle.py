#!/usr/bin/env python3
"""Writes a patched copy of LVGL 9.3's src/font/lv_font_fmt_txt.c:

  python tools/patch_lvgl_rle.py <lvgl>/src/font/lv_font_fmt_txt.c <output .c>

LVGL 9.3 decompresses glyphs of compressed fonts (here every font from 24 px, CONFIG_LV_USE_FONT_COMPRESSED)
with one global RLE state, LV_GLOBAL_DEFAULT()->font_fmt_rle. The launcher draws with two software draw
units (CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2), so two draw threads can decompress at the same time and overwrite
each other's position in the bit stream: such glyphs come out garbled until the next redraw. The copy keeps
the state on decompress()'s stack and hands it to rle_init() / rle_next(); nothing else changes. Upstream
master still has the global state (checked 2026-10-19).

The main CMakeLists.txt builds the copy instead of the original (managed_components and ESP-IDF stay as they
are); tools/host_test uses the same copy. A snippet that is not found exactly stops with an error: a newer
LVGL needs a look at this patch first. The relative #includes become absolute, because the copy lives in the
build folder.
"""
import os
import re
import sys

PATCHES = [
    ('#if LV_USE_FONT_COMPRESSED\n    #define font_rle LV_GLOBAL_DEFAULT()->font_fmt_rle\n#endif /*LV_USE_FONT_COMPRESSED*/\n',
     '/* Tab5 launcher patch (tools/patch_lvgl_rle.py): the RLE state lives on decompress()\'s stack instead of\n'
     ' * LV_GLOBAL_DEFAULT()->font_fmt_rle, so two draw units can decompress glyphs at the same time. */\n', 1),
    ('    static inline void decompress_line(uint8_t * out, int32_t w);\n',
     '    static inline void decompress_line(lv_font_fmt_rle_t * rle, uint8_t * out, int32_t w);\n', 1),
    ('    static inline void rle_init(const uint8_t * in,  uint8_t bpp);\n    static inline uint8_t rle_next(void);\n',
     '    static inline void rle_init(lv_font_fmt_rle_t * rle, const uint8_t * in,  uint8_t bpp);\n'
     '    static inline uint8_t rle_next(lv_font_fmt_rle_t * rle);\n', 1),
    ('    rle_init(in, bpp);\n', '    lv_font_fmt_rle_t rle;\n    rle_init(&rle, in, bpp);\n', 1),
    ('decompress_line(line_buf', 'decompress_line(&rle, line_buf', 3),
    ('static inline void decompress_line(uint8_t * out, int32_t w)\n{',
     'static inline void decompress_line(lv_font_fmt_rle_t * rle, uint8_t * out, int32_t w)\n{', 1),
    ('        out[i] = rle_next();\n', '        out[i] = rle_next(rle);\n', 1),
    ('static inline void rle_init(const uint8_t * in,  uint8_t bpp)\n{\n    lv_font_fmt_rle_t * rle = &font_rle;\n',
     'static inline void rle_init(lv_font_fmt_rle_t * rle, const uint8_t * in,  uint8_t bpp)\n{\n', 1),
    ('static inline uint8_t rle_next(void)\n{\n    uint8_t v = 0;\n    uint8_t ret = 0;\n'
     '    lv_font_fmt_rle_t * rle = &font_rle;\n',
     'static inline uint8_t rle_next(lv_font_fmt_rle_t * rle)\n{\n    uint8_t v = 0;\n    uint8_t ret = 0;\n', 1),
]


def patch(text, src_dir):
    for old, new, count in PATCHES:
        found = text.count(old)
        if found != count:
            raise SystemExit(f'patch_lvgl_rle.py: expected {count} x {old.splitlines()[0]!r}, found {found}: '
                             'this LVGL differs from 9.3.0, check whether the patch is still needed')
        text = text.replace(old, new)
    if 'font_rle' in text:
        raise SystemExit('patch_lvgl_rle.py: font_rle is still used somewhere')
    base = src_dir.replace('\\', '/')
    return re.sub(r'#include "([^"]+)"', lambda m: f'#include "{os.path.normpath(os.path.join(base, m.group(1))).replace(os.sep, "/")}"', text)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, encoding='utf-8', newline='') as f:
        text = f.read().replace('\r\n', '\n')
    out = patch(text, os.path.dirname(os.path.abspath(src)))
    if os.path.exists(dst) and open(dst, encoding='utf-8', newline='').read() == out:
        return  # unchanged: no rebuild
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    with open(dst, 'w', encoding='utf-8', newline='') as f:
        f.write(out)


if __name__ == '__main__':
    main()
