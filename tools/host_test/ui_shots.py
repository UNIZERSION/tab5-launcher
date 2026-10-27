#!/usr/bin/env python3
"""Snapshots of the SD manager and HOME rendered on the PC: LVGL 9.3 with the firmware's own LVGL options
(build/config/sdkconfig.h) and font patch, fonts, theme, the launcher's screen (l5_ui.c: views, top bar,
layers, veil), SD view, file actions, dialogs and keyboard, on a folder that stands in for the card, HOME with
slot states set in the program, CFG, PMAN (the device's partition table), OTA without Wi-Fi and Charge Mode
(ui/shots.c lists the scenes). Shows the layout of the side panel and the Polish letters in names, paths, the
clipboard line and the text viewer without flashing. run.py uses the same program for the idle test
(dim, screen off, wake: shots --idle).

  python tools/host_test/ui_shots.py [output folder]

Needs zig cc or gcc (CC overrides) and one firmware build. LVGL objects are kept in %TEMP%/l5_ui_lvgl
(lvgl_host.py), so later runs only compile the launcher's files. Writes NN_scene.png into the output folder.
"""
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

import lvgl_host

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = os.path.join(lvgl_host.PROJECT, 'main')
UI_FILES = ['ui/l5_theme.c', 'ui/l5_widgets.c', 'ui/l5_dialogs.c', 'ui/l5_cover.c', 'ui/l5_keyboard.c', 'ui/l5_sd.c',
            'ui/l5_sd_ops.c', 'ui/l5_rail.c', 'ui/l5_ui.c', 'ui/l5_home.c', 'ui/l5_views.c', 'ui/l5_cfg.c',
            'ui/l5_pman.c', 'ui/l5_ota.c', 'ui/l5_charge.c', 'ui/l5_keys.c', 'ui/l5_dither.c', 'ui/l5_boot.c',
            'ui/l5_appicon.c', 'l5_files.c', 'l5_kbd_map.c']


def ppm_to_png(ppm, png):
    data = open(ppm, 'rb').read()
    _, size, _, px = data.split(b'\n', 3)
    w, h = map(int, size.split())
    raw = b''.join(b'\x00' + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)

    with open(png, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def png_file(path, w, h, pixel, mode='rgba8', interlace=False):
    """writes a PNG with zlib (deflate as real cards hold it): mode rgba8, rgb8, rgba16, ga16 (16-bit gray + alpha,
    gray = Rec. 601 luma) or palette (with tRNS); pixel(x, y) gives (r, g, b, a)"""
    px = [[pixel(x, y) for x in range(w)] for y in range(h)]
    palette = []
    if mode == 'palette':
        palette = sorted({p for row in px for p in row})
        index = {p: i for i, p in enumerate(palette)}
    enc = {'rgba8': lambda p: bytes(p), 'rgb8': lambda p: bytes(p[:3]),
           'rgba16': lambda p: b''.join(struct.pack('>H', v * 257) for v in p),
           'ga16': lambda p: struct.pack('>HH', int(0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2] + 0.5) * 257, p[3] * 257),
           'palette': lambda p: bytes([index[p]])}[mode]
    ctype, depth = {'rgba8': (6, 8), 'rgb8': (2, 8), 'rgba16': (6, 16), 'ga16': (4, 16), 'palette': (3, 8)}[mode]
    # Adam7: x start, y start, x step, y step of the seven passes
    passes = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)] \
        if interlace else [(0, 0, 1, 1)]
    raw = bytearray()
    for x0, y0, dx, dy in passes:
        xs, ys = range(x0, w, dx), range(y0, h, dy)
        if not xs or not ys:
            continue
        for y in ys:
            raw += b'\0' + b''.join(enc(px[y][x]) for x in xs)

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)

    data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, depth, ctype, 0, 0, int(interlace)))
    if palette:
        data += chunk(b'PLTE', b''.join(bytes(p[:3]) for p in palette)) + chunk(b'tRNS', bytes(p[3] for p in palette))
    data += chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(data)


def orbit(w, h):
    """an opaque square: blue, an orange orbit with eight steps, a white center"""
    def pixel(x, y):
        u, v = (x + 0.5) / w - 0.5, (y + 0.5) / h - 0.5
        r = math.hypot(u, v)
        a = math.atan2(v, u) * 4 / math.pi
        if r < 0.12:
            return (245, 245, 245, 255)
        if abs(r - 0.33) < 0.025 or (abs(r - 0.33) < 0.06 and abs(a - round(a)) < 0.12):
            return (255, 138, 28, 255)
        return (int(18 + 40 * (0.7 - r)), int(28 + 50 * (0.7 - r)), int(70 + 110 * (0.7 - r)), 255)
    return pixel


def badge(w, h):
    """a teal badge with a white bar on a transparent background"""
    def pixel(x, y):
        dx, dy = abs(x + 0.5 - w / 2) - (w / 2 - 30), abs(y + 0.5 - h / 2) - (h / 2 - 30)
        if math.hypot(max(dx, 0), max(dy, 0)) >= 20:
            return (0, 0, 0, 0)
        u, v = (x + 0.5) / w - 0.5, (y + 0.5) / h - 0.5
        return (255, 255, 255, 255) if abs(v) < 0.08 and abs(u) < 0.3 else (0, 150, 136, 255)
    return pixel


def disc(w, h):
    """a green disc with a dark arc on a transparent background"""
    def pixel(x, y):
        u, v = (x + 0.5) / w - 0.5, (y + 0.5) / h - 0.5
        r = math.hypot(u, v)
        if r >= 0.45:
            return (0, 0, 0, 0)
        return (20, 20, 20, 255) if v < 0.1 and abs(r - 0.25) < 0.03 else (30, 215, 96, 255)
    return pixel


def test_pngs(work):
    """the icons ui/shots.c copies onto its card (work/png): every PNG kind LVGL's lodepng has to convert"""
    folder = os.path.join(work, 'png')
    os.makedirs(folder, exist_ok=True)
    for name, w, h, art, mode, interlace in (
            ('orbit512', 512, 512, orbit, 'rgb8', False),      # RGB without alpha, averaged in 2 x 2 blocks
            ('orbit300i', 300, 300, orbit, 'rgb8', True),      # interlaced (Adam7)
            ('orbit96_16', 96, 96, orbit, 'rgba16', False),    # 16-bit RGBA: 64 bits a pixel, refused
            ('disc64ga16', 64, 64, disc, 'ga16', False),       # 16-bit gray + alpha: 32 bits a pixel, converted
            ('badge', 200, 120, badge, 'rgba8', False),        # wider than high, transparent
            ('disc64', 64, 64, disc, 'rgba8', False),          # enlarged
            ('disc120p', 120, 120, disc, 'palette', False),    # palette with transparency
            ('wide1100', 1100, 8, disc, 'rgba8', False)):      # wider than 1024 px: refused
        png_file(os.path.join(folder, name + '.png'), w, h, art(w, h), mode, interlace)


def build(cc, work):
    """compiles the snapshot program into work (its card is work/card) and writes its test PNG files;
    returns the executable"""
    test_pngs(work)
    base, objs, _, font_txt = lvgl_host.build(cc)
    idf = os.environ.get('IDF_PATH') or os.path.expanduser(os.path.join('~', 'esp', 'esp-idf-v5.5.2'))
    cjson = os.path.join(idf, 'components', 'json', 'cJSON')
    card = os.path.join(work, 'card').replace('\\', '/')
    stub = os.path.join(HERE, 'ui', 'stub')
    flags = base + ['-I' + stub, '-I' + MAIN, '-I' + os.path.join(MAIN, 'ui'), '-I' + cjson, '-include', 'compat.h',
                    f'-DCONFIG_BSP_SD_MOUNT_POINT="{card}"', f'-DL5_CONFIG_PATH="{card}/config.conf"']
    srcs = [os.path.join(MAIN, f) for f in UI_FILES]
    srcs += [os.path.join(MAIN, 'fonts', f) for f in sorted(os.listdir(os.path.join(MAIN, 'fonts'))) if f.endswith('.c')]
    srcs += [os.path.join(HERE, 'ui', f) for f in ('ui_stubs.c', 'views_stubs.c', 'shots.c')]
    srcs.append(os.path.join(cjson, 'cJSON.c'))
    jobs, own = [], []
    for src in srcs:
        obj = os.path.join(work, 'ui_' + os.path.basename(src)[:-2] + '.o')
        own.append(obj)
        jobs.append(flags + ['-c', src, '-o', obj])
    lvgl_host.run_all(jobs)
    exe = os.path.join(work, 'shots.exe' if os.name == 'nt' else 'shots')
    lvgl_host.link(cc, exe, own + objs + [font_txt], work)
    return exe


def main():
    cc = lvgl_host.compiler()
    out = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else tempfile.mkdtemp(prefix='l5_ui_shots_')
    os.makedirs(out, exist_ok=True)
    work = tempfile.mkdtemp(prefix='l5_ui_')
    exe = build(cc, work)
    r = subprocess.call([exe, out.replace('\\', '/')], cwd=work)
    for f in sorted(os.listdir(out)):
        if f.endswith('.ppm'):
            ppm_to_png(os.path.join(out, f), os.path.join(out, f[:-4] + '.png'))
            os.remove(os.path.join(out, f))
    shutil.rmtree(work, ignore_errors=True)
    print('snapshots in', out)
    sys.exit(r)


if __name__ == '__main__':
    main()
