#!/usr/bin/env python3
"""Host test for the partition table (l5_ptable.c), the firmware check and AUTO PARTITION (l5_fw.c),
the installer (l5_inst.c), the SD manager's file operations (l5_files.c), LVGL's font decompression with
two draw units (tools/patch_lvgl_rle.py), the idle dimming / screen off of the launcher's screen (l5_ui.c) and
the bootloader's boot-loop guard (bootloader_components/main/l5_guard.h).

test_all.c runs them with the stubs in stub/ against a simulated 16 MB flash (NOR semantics, the IDF
write protection of bootloader / table / running app) and a simulated HTTPS server with Range and
connection drops. The image length parser is checked on the real build outputs. test_files.c runs
l5_files.c on a folder of the work directory that stands in for the SD card. test_font_rle.c decodes the
glyphs of the compressed fonts from two threads with LVGL's own lv_font_fmt_txt.c (garbled glyphs expected)
and with the launcher's patched copy (all must match). ui/shots.c --idle runs l5_ui.c's idle timer on the PC:
dim, screen off (backlight 0, drawing paused), wake by a tap, by a dialog and by a system screen; ui/shots.c
--taps checks what taps at the edges of the top bar's keys, in the gaps and around Wi-Fi / power hit, then Charge
Mode's state line on a run of current readings (l5_board.h l5_charge_next) and that HOME's redraw bench (compiled
here with L5_HOME_BENCH=1, off in the firmware) puts the view back. test_guard.c
runs the guard's decision for every reset reason and state and the boot loops through it (OrcSDR's clean restart
through the LP watchdog, crash loops, brown-out loops through the launcher). test_charger.c runs the charger steps
of main/l5_pi4io.h (CHG_EN high, CHG_STAT an input) on a model of the 0x44 IO expander from every state of the two
pins: no other pin moves and CHG_EN is never driven low on the way.

Needs a C compiler (zig cc or gcc; CC overrides), ESP-IDF (IDF_PATH: cJSON and gen_esp32part.py) and a
build of the firmware (build/tab5_launcher.bin, build/bootloader/bootloader.bin, build/config/sdkconfig.h):

  python tools/host_test/run.py
  python tools/host_test/run.py --scan firmware.bin ...   (only the old-hardware rule on these images)
  python tools/host_test/run.py --files                    (only the file operations)
  python tools/host_test/run.py --fonts                    (only the font decompression with two threads)
  python tools/host_test/run.py --idle                     (only dim / screen off / wake, top bar taps, charge state)
  python tools/host_test/run.py --guard                    (only the boot-loop guard)
  python tools/host_test/run.py --charger                  (only the charger pins of the IO expander)
"""
import os
import shutil
import subprocess
import sys
import tempfile

import lvgl_host
import ui_shots

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.abspath(os.path.join(HERE, '..', '..'))
SOURCES = ['l5_ptable.c', 'l5_ptable.h', 'l5_fw.c', 'l5_fw.h', 'l5_inst.c', 'l5_inst.h', 'l5_net.h', 'l5_files.c',
           'l5_files.h']


def font_rle_test(cc, work):
    """0 when the shipped file garbles glyphs from two threads and the patched copy does not"""
    flags, objs, shipped, patched = lvgl_host.build(cc)
    own = []
    srcs = [os.path.join(HERE, 'test_font_rle.c')]
    srcs += [os.path.join(PROJECT, 'main', 'fonts', f + '.c') for f in ('l5_chakra_b28', 'l5_chakra_b40', 'l5_orb_blk76')]
    jobs = []
    for src in srcs:
        obj = os.path.join(work, 'rle_' + os.path.basename(src)[:-2] + '.o')
        own.append(obj)
        jobs.append(flags + ['-c', src, '-o', obj])
    lvgl_host.run_all(jobs)
    r = 0
    for variant, font_txt in (('shipped', shipped), ('patched', patched)):
        exe = os.path.join(work, f'test_font_rle_{variant}' + ('.exe' if os.name == 'nt' else ''))
        lvgl_host.link(cc, exe, own + objs + [font_txt], work)
        rv = subprocess.call([exe, variant], cwd=work)
        if variant == 'patched' or rv != 3:
            r = r or rv
    print('PASSED' if r == 0 else 'FAILED', flush=True)
    return r


def guard_test(cc, work):
    """0 when the bootloader's boot-loop guard decides as test_guard.c expects"""
    exe = os.path.join(work, 'test_guard.exe' if os.name == 'nt' else 'test_guard')
    subprocess.check_call(cc + ['-O1', '-std=gnu11', '-Wall', '-Wextra', '-I' + os.path.join(PROJECT, 'bootloader_components', 'main'),
                                '-o', exe, os.path.join(HERE, 'test_guard.c')], cwd=work)
    return subprocess.call([exe], cwd=work)


def charger_test(cc, work):
    """0 when the charger steps of main/l5_pi4io.h keep the other pins and never drive CHG_EN low (test_charger.c)"""
    exe = os.path.join(work, 'test_charger.exe' if os.name == 'nt' else 'test_charger')
    subprocess.check_call(cc + ['-O1', '-std=gnu11', '-Wall', '-Wextra', '-I' + os.path.join(PROJECT, 'main'), '-o', exe,
                                os.path.join(HERE, 'test_charger.c')], cwd=work)
    return subprocess.call([exe], cwd=work)


def idle_test(cc, work):
    """0 when dim, screen off and wake behave as l5_ui.c should and taps reach the top bar's keys and buttons"""
    exe = ui_shots.build(cc, work)
    r = subprocess.call([exe, '--idle'], cwd=work)
    return subprocess.call([exe, '--taps'], cwd=work) or r


def main():
    idf = os.environ.get('IDF_PATH') or os.path.expanduser(os.path.join('~', 'esp', 'esp-idf-v5.5.2'))
    cc = lvgl_host.compiler()
    work = tempfile.mkdtemp(prefix='l5_host_test_')
    if len(sys.argv) > 1 and sys.argv[1] in ('--fonts', '--idle', '--guard', '--charger'):
        r = {'--fonts': font_rle_test, '--idle': idle_test, '--guard': guard_test,
             '--charger': charger_test}[sys.argv[1]](cc, work)
        print('work folder:', work)
        sys.exit(r)
    # the sources sit next to the stubs, so their #include "l5_board.h" etc. find stub/ and not main/
    for f in SOURCES:
        shutil.copy(os.path.join(PROJECT, 'main', f), work)
    shutil.copytree(os.path.join(HERE, 'stub'), os.path.join(work, 'stub'))
    shutil.copy(os.path.join(HERE, 'test_all.c'), work)
    shutil.copy(os.path.join(HERE, 'test_files.c'), work)
    shutil.copy(os.path.join(HERE, 'data', 'uiflow2_manifest.json'), work)
    gen = [sys.executable, os.path.join(idf, 'components', 'partition_table', 'gen_esp32part.py'), '--quiet',
           '--flash-size', '16MB']
    subprocess.check_call(gen + [os.path.join(PROJECT, 'partitions.csv'), os.path.join(work, 'cur_default.bin')])
    for t in ('old_default', 'old_uiflow2'):
        subprocess.check_call(gen + [os.path.join(HERE, 'data', t + '.csv'), os.path.join(work, t + '.bin')])
    files_exe = os.path.join(work, 'test_files.exe' if os.name == 'nt' else 'test_files')
    subprocess.check_call(cc + ['-O1', '-std=gnu11', '-Wall', '-Istub', '-o', files_exe, 'test_files.c', 'l5_files.c'],
                          cwd=work)
    files_dir = os.path.join(work, 'files')
    os.makedirs(files_dir)
    if len(sys.argv) > 1 and sys.argv[1] == '--files':
        r = subprocess.call([files_exe, files_dir.replace('\\', '/')], cwd=work)
        print('work folder:', work)
        sys.exit(r)
    cjson = os.path.join(idf, 'components', 'json', 'cJSON')
    exe = os.path.join(work, 'test_all.exe' if os.name == 'nt' else 'test_all')
    subprocess.check_call(cc + ['-O1', '-std=gnu11', '-Istub', '-I' + cjson, '-o', exe, 'test_all.c', 'l5_ptable.c',
                                'l5_fw.c', 'l5_inst.c', os.path.join(cjson, 'cJSON.c'), '-lm'], cwd=work)
    if len(sys.argv) > 2 and sys.argv[1] == '--scan':
        r = subprocess.call([exe, '--scan'] + [os.path.abspath(f) for f in sys.argv[2:]], cwd=work)
        shutil.rmtree(work, ignore_errors=True)
        sys.exit(r)
    out = os.path.join(work, 'out')
    os.makedirs(out)
    r = subprocess.call([exe, 'cur_default.bin', 'old_default.bin', 'old_uiflow2.bin', out.replace('\\', '/'),
                         os.path.join(PROJECT, 'build').replace('\\', '/')], cwd=work)
    print('file operations (test_files):', flush=True)
    r2 = subprocess.call([files_exe, files_dir.replace('\\', '/')], cwd=work)
    print('font decompression with two threads (test_font_rle):', flush=True)
    r3 = font_rle_test(cc, work)
    print('idle dimming and screen off, top bar taps (shots --idle, --taps):', flush=True)
    r4 = idle_test(cc, work)
    print('boot-loop guard (test_guard):', flush=True)
    r5 = guard_test(cc, work)
    print('battery charger pins on the 0x44 expander (test_charger):', flush=True)
    r6 = charger_test(cc, work)
    print('work folder:', work)
    sys.exit(r or r2 or r3 or r4 or r5 or r6)


if __name__ == '__main__':
    main()
