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
            'ui/l5_sd_ops.c', 'ui/l5_topbar.c', 'ui/l5_ui.c', 'ui/l5_home.c', 'ui/l5_views.c', 'ui/l5_cfg.c',
            'ui/l5_pman.c', 'ui/l5_ota.c', 'ui/l5_charge.c', 'ui/l5_keys.c', 'ui/l5_dither.c', 'l5_files.c']


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


def build(cc, work):
    """compiles the snapshot program into work (its card is work/card); returns the executable"""
    base, objs, _, font_txt = lvgl_host.build(cc)
    idf = os.environ.get('IDF_PATH') or os.path.expanduser(os.path.join('~', 'esp', 'esp-idf-v5.5.2'))
    cjson = os.path.join(idf, 'components', 'json', 'cJSON')
    card = os.path.join(work, 'card').replace('\\', '/')
    stub = os.path.join(HERE, 'ui', 'stub')
    flags = base + ['-I' + stub, '-I' + MAIN, '-I' + os.path.join(MAIN, 'ui'), '-I' + cjson, '-include', 'compat.h',
                    '-DL5_HOME_BENCH=1',  # the bench is off in the firmware; shots --taps checks it still puts HOME back
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
