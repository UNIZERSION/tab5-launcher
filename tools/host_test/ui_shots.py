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


# baseline JPEG (4:4:4, quantization 1, the standard Huffman tables) of an image made of flat 8 x 8 blocks: every
# block is its DC value and an end of block, so no DCT is needed (the test wallpapers are flat rectangles)
DC_BITS = [[0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0], [0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0]]
DC_VALS = [list(range(12)), list(range(12))]
AC_BITS = [[0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d], [0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77]]
AC_VALS = [bytes.fromhex(
    '01020300041105122131410613516107227114328191a1082342b1c11552d1f02433627282090a161718191a25262728292a3435363738'
    '393a434445464748494a535455565758595a636465666768696a737475767778797a838485868788898a92939495969798999aa2a3a4a5'
    'a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9fa'),
    bytes.fromhex(
    '000102031104052131061241510761711322328108144291a1b1c109233352f0156272d10a162434e125f11718191a262728292a3536'
    '3738393a434445464748494a535455565758595a636465666768696a737475767778797a82838485868788898a92939495969798999a'
    'a2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae2e3e4e5e6e7e8e9eaf2f3f4f5f6f7f8f9fa')]


def huff_codes(bits, vals):
    codes, code, k = {}, 0, 0
    for length in range(1, 17):
        for _ in range(bits[length - 1]):
            codes[vals[k]] = (code, length)
            code += 1
            k += 1
        code <<= 1
    return codes


def jpeg_file(path, w, h, color, progressive=False):
    """color(x, y) of the 8 x 8 block at x, y (multiples of 8) as (r, g, b)"""
    dc = [huff_codes(DC_BITS[i], DC_VALS[i]) for i in range(2)]
    ac = [huff_codes(AC_BITS[i], list(AC_VALS[i])) for i in range(2)]
    out, acc, nbits = bytearray(), 0, 0

    def put(code, length):
        nonlocal acc, nbits
        acc = acc << length | code
        nbits += length
        while nbits >= 8:
            nbits -= 8
            byte = acc >> nbits & 0xFF
            out.append(byte)
            if byte == 0xFF:
                out.append(0)

    prev = [0, 0, 0]
    for by in range(0, h, 8):
        for bx in range(0, w, 8):
            r, g, b = color(bx, by)
            ycc = (0.299 * r + 0.587 * g + 0.114 * b, 128 - 0.168736 * r - 0.331264 * g + 0.5 * b,
                   128 + 0.5 * r - 0.418688 * g - 0.081312 * b)
            for c in range(3):
                v = int(round(8 * (ycc[c] - 128)))
                diff, prev[c] = v - prev[c], v
                size = abs(diff).bit_length()
                put(*dc[c > 0][size])
                if size:
                    put(diff if diff > 0 else diff + (1 << size) - 1, size)
                put(*ac[c > 0][0])  # end of block
    if nbits:
        put((1 << (8 - nbits)) - 1, 8 - nbits)

    def seg(marker, body):
        return bytes([0xFF, marker]) + struct.pack('>H', len(body) + 2) + body

    data = b'\xff\xd8'
    data += seg(0xDB, b'\x00' + bytes([1] * 64)) + seg(0xDB, b'\x01' + bytes([1] * 64))
    data += seg(0xC2 if progressive else 0xC0,
                struct.pack('>BHHB', 8, h, w, 3) + bytes([1, 0x11, 0, 2, 0x11, 1, 3, 0x11, 1]))
    for cls in range(2):
        for i in range(2):
            bits, vals = (DC_BITS, DC_VALS) if cls == 0 else (AC_BITS, AC_VALS)
            data += seg(0xC4, bytes([cls << 4 | i]) + bytes(bits[i]) + bytes(vals[i]))
    data += seg(0xDA, bytes([3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0])) + bytes(out) + b'\xff\xd9'
    with open(path, 'wb') as f:
        f.write(data)


def wall_art(w, h, flip=False):
    """red, green and blue thirds (flip: blue, green, red) and a white square in the middle (edges on the 8 x 8 grid)"""
    thirds = [(200, 40, 40), (40, 180, 60), (40, 60, 200)]
    if flip:
        thirds.reverse()

    def color(x, y):
        if w * 3 // 8 <= x < w * 5 // 8 and h * 2 // 8 <= y < h * 6 // 8:
            return (250, 250, 250)
        return thirds[min(2, x * 3 // w)]
    return color


def sky_art(w, h):
    """for the snapshots: an evening sky, a sun and hills"""
    def color(x, y):
        t, u = x / w, y / h
        sun = math.hypot(x - w * 0.7, y - h * 0.42) < h * 0.11
        hill = y > h * (0.68 + 0.08 * math.sin(x / w * 7.0)) or y > h * (0.78 + 0.05 * math.sin(x / w * 13.0 + 1))
        if hill:
            return (int(20 + 30 * t), int(25 + 20 * (1 - u)), int(45 + 30 * (1 - t)))
        if sun:
            return (255, 196, 120)
        return (int(40 + 200 * u * (0.6 + 0.4 * t)), int(30 + 90 * u * u), int(110 + 60 * (1 - u)))
    return color


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
    # wallpapers: a 1600 x 900 JPEG (shrunk to 1280 x 720), the same as PNG, a progressive JPEG header (refused),
    # a picture too small to fill the screen (refused)
    jpeg_file(os.path.join(folder, 'wall1600.jpg'), 1600, 900, wall_art(1600, 900))
    jpeg_file(os.path.join(folder, 'wallprog.jpg'), 1600, 896, wall_art(1600, 896), progressive=True)
    png_file(os.path.join(folder, 'wall1280.png'), 1280, 720, wall_art(1280, 720, flip=True), 'rgb8')
    jpeg_file(os.path.join(folder, 'wall320.jpg'), 320, 176, wall_art(320, 176))
    jpeg_file(os.path.join(folder, 'wall1024.jpg'), 1024, 576, wall_art(1024, 576))  # enlarged 1.25x
    png_file(os.path.join(folder, 'wallsky.png'), 1280, 720, sky_art(1280, 720), 'rgb8')  # smooth, for the snapshots


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
