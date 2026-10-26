> **Project development notes** (as of 2026-10-19). The `setup\…` commands are the author's own scripts from outside the repository that only set up the ESP-IDF environment: instead of them, `idf.py build` in an ESP-IDF 5.5.2 environment is enough (README.md). Folder names such as `..\tab5-launcher` (the HTML prototype, not in the repository) and `tab5-launcher-fw` (this firmware) come from the author's machine.

# Tab5 Launcher — firmware (ESP-IDF 5.5.2 + LVGL 9.3)

A native port of the HTML prototype from `..\tab5-launcher` (index.html, launcher.css, app.js, views.js,
views2.js, ui.js, data.js) to the **M5Stack Tab5** (ESP32-P4, 1280×720 screen in landscape, GT911
or ST7123 touch). Already in place: the top bar, the HOME screen with app slots, the boot screen with a countdown,
booting the selected slot via otadata, the SD tab (browsing the card and installing a .bin to a slot),
PMAN, CFG, the on-screen keyboard, Wi-Fi over the ESP32-C6 (scan, password, saved networks, auto-connect)
and OTA: the launcherhub catalog (the same one as in Launcher 2.10), installation over HTTPS streamed into
the slot, merged images with their own partition table, and AUTO PARTITION (the "OTA: online installer"
section). OTA works on the device (2026-10-18/19: download from the CDN, AUTO PARTITION, boot-loop guard,
older-hardware warning). Build 0x1CE880 (flashed 2026-10-19 at 10:04, app-flash) adds: COPY / PASTE, RENAME, NEW FOLDER, DELETE and a preview of text files in the SD manager
(the "SD file manager" section), a fix for distorted letters in large fonts (the "Fonts" section), Avoid SPIFFS,
which installs only the app, merged images that start at address 0x0 (e.g. OrcSDR after TO SD), the new top bar
from the prototype, turning the screen fully off after inactivity (CFG → Screen off, the "Screen dimming and
screen off" section) and a large HOME card for a single slot, with a fix for the card that was not visible with
one slot (the "HOME" section). Launcher 0x1CEA90 with bootloader 0x5730 (flashed 2026-10-19 at 10:59) improves
the boot-loop guard: a clean app restart via the LP watchdog (OrcSDR) and loops through the launcher (the "How
the boot target is chosen" section); the top bar is 72 px instead of 58, with larger touch targets ("What already works" → top bar).
Build 0x1CF910 (flashed at 11:59, app-flash) draws HOME with the large card and the first frame after startup
faster (the "HOME" section → speed) and has Charge Mode with the real charge level. Build 0x1CF730 (flashed at
12:26, app-flash) adds the FULL and USB · NOT CHARGING states to Charge Mode when the current is close to zero,
and removes the HOME measurement triggered by a long press on "APPS IN FLASH". Build 0x1CFB30 (flashed at 12:46,
app-flash) enables the battery charger at startup (CHG_EN on the 0x44 expander, as in Aurora); before that, the
battery did not charge in the launcher ("What already works" → charger). WUI is still a stub.

> **On the device (2026-10-19):** launcher 0x1CFB30 with bootloader 0x5730 (SHA-256 checksums in "Building").
> Tested on the Tab5: the boot-loop guard with OrcSDR (clean restart via the LP watchdog, RESET → launcher), the
> 72 px bar, the large HOME card and its speed, dimming and screen off, Charge Mode (CHARGING, FULL, ON BATTERY)
> and the charger, DELETE on SD, installing a merged image that starts at 0x0 from SD. **Not tested on the device:**
> Avoid SPIFFS; COPY / PASTE, RENAME, NEW FOLDER and the text preview in the SD manager; BOOT AGAIN; RESET between
> the "was stopped" dialog and ERASE APP.

> The version with the on-screen keyboard and Wi-Fi **works on the device**: on 2026-10-18 the following passed:
> scan, password from the keyboard, connection with an IP address, wrong password, saving the network, auto-connect
> after RESET, installing from SD with Wi-Fi connected, BOOT of an app and Reset config. Build 0x1D3BB0 (Wi-Fi
> shutdown on restart, a fix for lost taps, faster touch response) has also been tested on the
> device. Before flashing the launcher for the first time
> onto a device that does not have it yet, read the "Flashing" section: the bootloader
> and the partition table change.

## Restored build of 2026-10-20

The sources of the launcher flashed on the Tab5 on 2026-10-20 (built 12:33:16, 1,914,912 B = 0x1D3820, app
ELF SHA-256 476e0a80…) were lost with the laptop they were on: GitHub only had v0.1.0 (built 2026-10-18 21:18:03,
0x1CFB30) and two commits that change `docs/GUIDE.md`. The changes below were rebuilt on 2026-10-21 from that binary,
read back from the Tab5 (`esptool read-flash`, the whole 16 MB, MD5-checked against the chip): every string that is
in the Tab5 build and not in v0.1.0 (and the other way round), placed in its source file by the strings next to it in
`.rodata`. The register map of the keyboard comes from M5Stack's own sources (named in `main/l5_kbd.h`). Logic that the
strings do not show was written new, so the code is not the lost code, and none of it has run on the device yet.

- **WUI tab and USB Mass Storage row removed** (both only said "not ported yet"): five tabs, menus "OTA, SD, CFG, PMan,
  APP, OFF", texts "Install it from SD or OTA".
- **M5Stack Tab5 Keyboard** (`main/l5_kbd.c`, `main/ui/l5_keys.c`, new). Rebuilt a second time on 2026-10-21 from the
  machine code of the Tab5 build: the functions of v0.1.0 were matched in it (masked RISC-V instructions, 90% of the
  code), the rest disassembled and read (`objdump`), so the keys, the driver and the dialogs' key handling follow that
  code; the key map is in the GUIDE ("M5Stack Tab5 Keyboard").
  - Driver (`l5_kbd.c`): I2C port 0 (the BSP's system bus is `CONFIG_BSP_I2C_NUM` = 1), SDA GPIO 0, SCL GPIO 1, internal
    pull-ups, address 0x6D at 100 kHz; INT on GPIO 50 (input with pull-up, read as a level). A worker (priority 2,
    core 1) looks for the keyboard every 2 s with the `i2c.master` log off only during the probe: register 0xFE
    (firmware), 0xFF must read 0x6D, then 0x10 (mode). Setup: 0x10 = 1 (HID), 0x00 = 2 (interrupt for HID only),
    0x02 = 0, 0x01 = 0. While it is there, events (0x30: HID modifier + usage ID) are read when INT is low or every
    100 ms, more than 32 waiting are flushed; every 1 s the mode is read back and set again when it is not HID; a failed
    read = "keyboard gone". The loop runs every 10 ms (100 ms without a keyboard). Keys (`l5_key_t`, 6 bytes: kind,
    character, ctrl, shift, alt, repeat): characters, Enter (also the keypad's), Esc, Backspace, Delete, Tab, arrows,
    Home / End, PgUp / PgDn. Auto-repeat after 450 ms every 60 ms, at most 10 s, not for Enter / Esc / Tab or with
    Ctrl / Alt; a release stops it. Queue of 64, flushed when full. At a restart (shutdown handler) the device and the
    bus are deleted and GPIO 0, 1, 50 reset; the mode is not changed.
  - UI (`l5_keys.c`): an LVGL timer takes up to 8 keys every 20 ms. A dimmed or dark screen only wakes; every key counts
    as activity. A dialog, menu or the text field on `#layer` gets the key as the custom LVGL event `l5_key_event()`
    (`lv_event_register_id()`, the parameter is the `l5_key_t`); a screen on `#sys` goes to `l5_sys_key()` (boot screen:
    any key = ENTER LAUNCHER, Charge Mode: Enter / Esc); otherwise Tab / Shift+Tab and Ctrl+→ / Ctrl+← switch tabs and
    the rest goes to `l5_home_key()`, `l5_sd_key()`, `l5_ota_key()` or `l5_cfg_key()`.
  - The ring is `L5S.focus` (2 px outline in the accent, 3 px inside the object) on `LV_STATE_FOCUS_KEY`: menu items,
    the modal's buttons, the CFG rows (`K`: every row, its action key, its stepper keys, the color row; the focused row
    survives a rebuild). The text field with the keyboard plugged in (`l5_keyboard.c`): the panel moves to the middle of
    the screen, the key rows are hidden and a bar shows "KEYBOARD · ENTER = OK · ESC = CANCEL · TAP THE FIELD FOR
    ON-SCREEN KEYS" with CANCEL / OK; the caret's 500 ms timer switches the layout when the keyboard comes or goes.
- **Dithered gradients** (`main/ui/l5_dither.c`, new; rebuilt the same way from 0x40041e2e..0x40042544 of that build:
  compiled here it gives the same 1814 bytes of code, apart from the addresses of four float constants). On the RGB565
  panel LVGL's 2-stop vertical gradients show bands: L5S.dark (#1d1f23 -> #141518) changes red by 9 levels over 632 px,
  so whole 60 px high stripes come out in one color. The module draws such a gradient from a 32 px wide RGB565 strip
  built once with a 4 x 4 ordered (Bayer) dither (threshold (m + 0.5) / 16 per pixel, each channel rounded to the level
  below or above in proportion) and tiles it over the area with `lv_draw_image` (`tile`, `clip_radius` = the object's
  radius up to half the shorter side).
  - `l5_dither_hook(obj)`: `LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS` + an `LV_EVENT_DRAW_TASK_ADDED` callback that takes the
    object's LV_PART_MAIN fill task when it is a vertical gradient with 2 stops and all opacities at least 253, draws
    the strip and sets the fill's opa to 0. `l5_panel(obj, dark)` = `L5S.dark` / `L5S.plate` + the hook.
    `l5_grad_key()` + `l5_grad_draw()` do the same for custom drawing (falling back to `lv_draw_rect`).
  - Where (as in that build): the panes and columns (`l5w_pane`, PMAN's pane, CFG's columns), the dialogs, the text
    field's panel, every `l5_act_button`, the keys of the on-screen keyboard, the top bar (the bar, the tab track, the
    tab keys, its graded strips), the bootscreen (background and ENTER LAUNCHER), HOME's panel (`panel_bg_cb`, now for
    every layout) and the cover emblems (background, chip body). `sys_show()` sets a gradient only when the two colors
    differ, so the black screens have none.
  - Cache: up to 64 gradients (colors + stop positions, never dropped) and 128 strips keyed by gradient and height,
    2 MB in PSRAM at most; when full, the strip drawn longest ago and not in the current frame (a frame counter on
    `LV_EVENT_REFR_START`) is freed. No strip (no PSRAM, every strip in use this frame, taller than 4096 px): LVGL's
    own gradient. A disable flag is read but nothing in that build sets it (`l5_dither_set_enabled()` is unused).
  - PC test: `shots --taps` checks that CFG's dark column is dithered (every 4 x 4 block mixes levels and averages to
    the ideal gradient within one 8-bit step; without the module: one color, 5 steps off).
- **config.conf put back** (`l5_config_replace()`, `l5_sd_ops.c`): RENAME or PASTE onto `/config.conf` while the
  launcher's own file is there asks "Replace config.conf?"; the old file steps aside as `config.conf.old` for the swap, the
  settings apply at once (`l5_cfg_reload_now()`), a file that is not a JSON object leaves them as they are.
- **Wi-Fi with another ESP-Hosted on the C6:** `c6_mismatch` in the Wi-Fi status, the dialog "ESP32-C6 needs ESP-Hosted
  2.12" (what the C6 runs, what to flash with M5Burner), WHY NO WI-FI in the OTA tab, the HOME empty-slot line about
  both install sources.
- **Display start** (`l5_display.c`, `l5_board.c`): every step has a name (`STEP()`); the board is recognised before
  `bsp_display_new()`, which stops at `assert()` on a board it does not know (a panic boots the app in otadata). Unknown
  board or a failed step: "NO DISPLAY: <step>" in the log, the backlight blinks, RESET tries again.
- **Boot choice:** without a choice in NVS, only apps the launcher installed (registry entry) become the boot app;
  HOME and the boot screen say "NO APP CHOSEN TO START" for the others, BOOT on the one-slot card chooses it.
- **Install with little internal RAM** (`l5_inst.c`): without a 32 KB block the data goes through PSRAM and the flash
  through an internal bounce buffer (16 KB down to 512 B), else 32-byte steps (`esp_flash_write`); internal RAM is
  logged at the start and the end. TO SD takes a PSRAM buffer when there is no 16 KB block.
- **OTA:** a version or file name longer than the launcher keeps is left out with a warning instead of being cut.

## Why ESP-IDF and not PlatformIO

- **ESP-IDF 5.5.2 is already installed** (`..\setup\install-esp-idf.ps1`), and Aurora Player, built with it, runs
  on the same Tab5 — the same versions of the BSP (`espressif/m5stack_tab5` 1.3.2),
  `esp_lvgl_port` 2.8.0 and LVGL 9.3.0.
- **The launcher needs its own bootloader and has to write otadata.** In ESP-IDF that means the
  `bootloader_components/` directory and the partition API. In PlatformIO with Arduino-ESP32 the bootloader is
  precompiled, and the ESP32-P4 needs a fork of the platform (pioarduino).
- **The Tab5 BSP from Espressif** supports both panel versions (ILI9881C + GT911 and ST7123/ST7121),
  hardware image rotation via the PPA, the SD card and the IO expanders. M5Unified/M5GFX would require
  Arduino 3.x and a custom bridge to LVGL.

## Building

In PowerShell (the scripts in `..\setup` set up the IDF environment):

```powershell
cd <folder above tab5-launcher-fw>
.\setup\build.ps1 -Project .\tab5-launcher-fw build
```

or in a window with IDF loaded (`. .\setup\idf-env.ps1`):

```powershell
idf.py -C tab5-launcher-fw build
```

Output: `build\tab5_launcher.bin` (the launcher, `factory` partition), `build\bootloader\bootloader.bin`
(override, at 0x2000) and `build\partition_table\partition-table.bin` (at 0x8000).

Latest build (charger, Charge Mode, faster HOME, 72 px bar, boot-loop guard version 2;
launcher flashed 2026-10-19 at 12:46, bootloader at 10:59, tested on the device; no warnings in the launcher code,
the `-Wunused-function` warnings from `freertos/atomic.h` come from `lv_freertos.c` in LVGL):

| File | Size | Headroom |
|---|---|---|
| `tab5_launcher.bin` | 1,899,312 B (0x1CFB30) | 197,840 B (193 KB, 9%) of the 2 MB partition |
| `bootloader.bin` | 22,320 B (0x5730) | 2256 B (9%) of 24 KB (0x2000–0x8000) |

SHA-256 of the flashed files: `tab5_launcher.bin` cfab97ad5b3342e3a61dfe53137ab35354c2a7496b3625d48cab0e140c803e88,
`bootloader.bin` aaadc6b7195ec4fc8f91841e05096b7d1c7f6f32556f40ba2dca90ad6a8272bb. The sources in the repository produce
exactly these files: an incremental build after the commit has nothing to do (`ninja`), and the checksums do not change.
A clean build gives a different file, if only because of the compile date in the app description (`CONFIG_APP_COMPILE_TIME_DATE`).

Previous build, from the commit and from the device (0x1C2DF0 with bootloader 0x5440): `tab5_launcher.bin`
55e4ff5ae5838dd015820fdde09c6b314f92e5293b6e5fffb99b696b22df356c, `bootloader.bin`
7bea86389141e5dea61f068c66718a6af370a16fda1395b325e8a9acbba18b79.

The bootloader changed in build 0x1C2DF0 (boot-loop guard, 0x5440) and in 0x1CE990 (version 2, 21,568 →
22,320 B, 0x5730): it is flashed with `bootloader-flash` (at 0x2000; the partition table and apps stay), while
`app-flash` alone does not change it. Without the new bootloader the launcher works as before, only the guard does not
turn on. Version 2 also counts loops through the launcher, and an LP watchdog reset right after an app starts boots
that app again. Both files must come from the same build: the old bootloader 0x5440 and a launcher from 0x1CE990 on
(or the other way round) store the guard state in a different layout, so the launcher will not see the stop, will not
show the dialog and the loop will not end (starting the app and the launcher works normally).

Before OTA the launcher was 1,915,824 B (0x1D3BB0). How OTA fit
into 2 MB without enlarging the launcher partition: "Wi-Fi" → "Size".

The first build downloads components from the Espressif registry into `managed_components/`
(versions pinned in `main/idf_component.yml` and `dependencies.lock`). Since the Wi-Fi version these also include
`espressif/esp_hosted` 2.12.6, `espressif/esp_wifi_remote` 1.5.1 and their dependencies
`espressif/wifi_remote_over_eppp` 0.3.3, `espressif/eppp_link` 1.1.6 and `espressif/esp_serial_slave_link`
1.1.2 (the "Wi-Fi" section).

## Flashing

**Warning:** flashing for the first time overwrites the bootloader, the partition table and the Aurora Player app
(which currently occupies `factory`, 15 MiB from 0x10000). First make a backup of the whole flash:

```powershell
python -m esptool --chip esp32p4 -p COM4 read_flash 0 0x1000000 tab5-backup-16MB.bin
```

Then:

```powershell
.\setup\build.ps1 -Project .\tab5-launcher-fw -p COM4 flash monitor
```

`idf.py flash` writes the bootloader, the partition table, an empty `otadata` and the launcher to `factory`.
An empty otadata means that the launcher starts right away after boot.

The SD or OTA tab flashes an app to a slot (see below). It can also be done by hand. It has to
be an **ESP32-P4 app image** (the project's `.bin` file, not a "merged" one with the bootloader; a merged image
is installed by the SD and OTA tabs, which take only the app and the matching data images from it):

```powershell
python -m esptool --chip esp32p4 -p COM4 write_flash 0x210000 my_app.bin      # app0 (ota_0)
python -m esptool --chip esp32p4 -p COM4 write_flash 0x610000 other_app.bin   # app1 in the 2 Apps scheme
```

## Partition table (default scheme: 1 slot × 8 MB)

**Every scheme contains the Aurora Player PIN partition `aurora_cfg`** (data/nvs, 0xF10000, 24 KB;
`LOCK_ADDRESS` / `LOCK_SIZE` in `..\aurora-player\main\screen_lock_storage.cpp`). Aurora looks for it
at exactly this address. If it is missing or contains anything other than erased flash or NVS,
Aurora stays locked once a PIN has been set (fail-closed). That is why the area between the launcher
and coredump is split into three parts:

| Range | Size | Contents |
|---|---|---|
| 0x210000–0xF10000 | 13312 KB | app slots, followed by FAT (`vfs` ends at 0xF10000 at the latest) |
| 0xF10000–0xF16000 | 24 KB | `aurora_cfg`: never moved, formatted or erased |
| 0xF16000–0xFF0000 | 872 KB | SPIFFS (the whole gap), a firmware's partition from AUTO PARTITION (up to 872 KB) or nothing |

`partitions.csv` is the Default scheme (`app: 8192, n: 1, spiffs: 872, fat: 5120`): one 8 MB slot,
so that large apps (Aurora at 6.2 MB, the UIFlow2 2.5 app at 7.4 MB) fit without a visit to PMAN.
The 2 Apps and 4 Apps schemes give several slots:

```csv
# Name,     Type, SubType,  Offset,   Size,     Flags
nvs,        data, nvs,      0x9000,   0x5000,
otadata,    data, ota,      0xE000,   0x2000,
launcher,   app,  factory,  0x10000,  0x200000,
app0,       app,  ota_0,    0x210000, 0x800000,
vfs,        data, fat,      0xA10000, 0x500000,
aurora_cfg, data, nvs,      0xF10000, 0x6000,
spiffs,     data, spiffs,   0xF16000, 0xDA000,
coredump,   data, coredump, 0xFF0000, 0x10000,
```

| Area | Offset | Size |
|---|---|---|
| bootloader (override) | 0x2000 | up to 24 KB |
| partition table | 0x8000 | 4 KB |
| nvs | 0x9000 | 20 KB |
| otadata | 0xE000 | 8 KB |
| launcher (`factory`) | 0x10000 | 2 MB |
| app0 (`ota_0`) | 0x210000 | 8 MB |
| vfs (FAT, e.g. for UiFlow2) | 0xA10000 | 5 MB |
| aurora_cfg (Aurora Player PIN) | 0xF10000 | 24 KB |
| spiffs | 0xF16000 | 872 KB |
| coredump | 0xFF0000 | 64 KB |

All schemes (nvs, otadata, launcher, `aurora_cfg` and coredump are fixed; SPIFFS is always at
0xF16000, 872 KB):

| Scheme | App slots | vfs (FAT) |
|---|---|---|
| Default | 1 × 8 MB: 0x210000 | 0xA10000, 5120 KB |
| 2 Apps | 2 × 4 MB: 0x210000, 0x610000 | 0xA10000, 5120 KB |
| 4 Apps | 4 × 2 MB: 0x210000, 0x410000, 0x610000, 0x810000 | 0xA10000, 5120 KB |
| Min SPIFFS | 1 × 10 MB: 0x210000 | 0xC10000, 3072 KB |
| UiFlow2 | 1 × 7552 KB: 0x210000 | 0x970000, 5760 KB |
| Doom | 1 × 4 MB: 0x210000 | 0x610000, 9216 KB |
| Max App | 1 × 12864 KB: 0x210000 | 0xEA0000, 448 KB |

Change of 2026-10-18 (OTA): large apps take priority over several slots. Default has
one 8 MB slot instead of two 4 MB ones (the old Default is now "2 Apps"), UiFlow2 has a 7552 KB slot
instead of 7 MB (it fits the UIFlow2 2.5.3 app, 7,709,696 B) and a 5760 KB FAT instead of 6144 KB.
A device keeps its table until a different scheme is selected in PMAN: the table of the old
Default is now recognized as "2 Apps", and the table of the old UiFlow2 (7 MB slot, 6 MB vfs) as Custom
(the apps in it stay, Aurora at 6.3 MB still fits). `partitions.csv` only matters when the table is flashed
over USB. Layouts from AUTO PARTITION (the "OTA" section) have the firmware's data partitions, with its labels
and sizes, between the slot and `vfs`, and its partition in the gap after `aurora_cfg` (e.g. `storage`
640 KB). PMAN shows them as Custom. The prototype (`data.js schemes`, `L5.table()`) needs to be
synced with these sizes.

One difference from the prototype: in `L5.table()` the launcher has the `test` subtype, here it has `factory`
(like M5Launcher 2.10 on the P4: `support_files/custom_16Mb_p4.csv`). Thanks to this, an erased otadata
always boots the launcher, even with the standard bootloader. The prototype should be changed
to `factory` at some point.

## How the boot target is chosen

1. **The launcher (factory) starts after power-on and after a reset.** This is handled by the bootloader
   override in `bootloader_components/main/bootloader_start.c`:

   | Reset reason | What starts |
   |---|---|
   | power-on, reset from USB (esptool; the launcher logs `reset reason 11`), watchdog, brown-out | the launcher |
   | **RESET button on the Tab5**: `rst:0x7 (HP_SYS_HP_WDT_RESET)`, reason 7 | always the launcher |
   | software reset (`esp_restart()` of the launcher or of the app itself, including a panic): `rst:0xc (SW_CPU_RESET)` | the app selected in otadata (the launcher if otadata is empty) |
   | LP watchdog (`rst:0x9 (HP_SYS_LP_WDT_RESET)`, also 0xd and 0x10) less than 15 s after the app started | the same app, counted as a quick restart (since build 0x1CE990, see below); if later, the launcher |
   | the third quick restart of an app in a row (each start less than 15 s after the previous one) or the third return to the launcher within 15 s of the app starting | the launcher, with the "… was stopped" dialog (boot-loop guard, see below) |
   | wake-up from deep sleep | the launcher; the app if `deepSleepStart` = false |

   The RESET button on the Tab5 does not give a "power-on" reset, only a system watchdog reset (reason 7,
   `HP_SYS_HP_WDT_RESET`). In the log it looks like this: ROM `rst:0x7 (HP_SYS_HP_WDT_RESET)`, bootloader
   `CPU has been reset by WDT.` and `l5boot: reset reason 7: starting launcher`, launcher
   `reset reason 7`. This is normal. The override boots the app from otadata only after a software
   reset, after an LP watchdog reset within 15 s of the app's start and after deep sleep with `L5:DS=0`, so RESET always returns
   to the launcher (tested on the
   device on 2026-10-18). A launcher restart with an empty otadata, e.g. after a scheme change, logs
   `reset reason 3` (`ESP_RST_SW`).

2. On HOME, tapping a card selects the slot (`S.boot`), which is saved in NVS (`l5/boot`).
3. After the countdown (`bootTime`), after FAST BOOT APP, BOOT or "Boot now", the launcher verifies the image
   (`esp_image_verify`: ESP32-P4 chip, segments, SHA-256), writes otadata for `ota_N`
   and calls `esp_restart()`.

   Otadata is written with the same sequence arithmetic as `esp_ota_set_boot_partition()`
   (`esp_rewrite_ota_data()` in IDF). In addition, `seq_label` gets an `L5:DS=1`/`L5:DS=0` marker
   for `deepSleepStart`. This field is not covered by the CRC, and only the launcher's bootloader reads it.
4. "Restart" in the power menu clears otadata and restarts, so the launcher's boot screen comes back.
   "Erase slot" erases the whole slot. If otadata pointed to that slot, it is cleared as well.

**Boot-loop guard** (since build 0x1C2DF0 with bootloader 0x5440; tested on the device on
2026-10-19; since build 0x1CE990 with bootloader 0x5730, on the device
since 0x1CEA90, also a clean restart via the LP watchdog and loops through the launcher). On the ESP32-P4
an app panic is also a software reset (`rst:0xc`), and that always boots the app from otadata. An app that
crashes right after starting (e.g. TAB5 Demo: `GT911 initialization failed!` after 2.9 s on a board with ST7121 + ST712x)
used to restart endlessly until RESET, and then the countdown started it again.

- The bootloader counts consecutive app starts after a software reset, each less than 15 s after the
  previous one (time from the LP timer counter, which keeps running through these resets; the RC_SLOW clock with the
  calibration left by the last app). The counter lives in the `LP_SYSTEM_REG_LP_STORE13..15` registers: on the P4,
  IDF uses registers 1–10, and the bootloader does not touch LP RAM because apps keep their data there.
  After power-on the registers are zeroed.
- The fourth such start (the third quick restart) boots the launcher with the "stopped" flag. The bootloader log
  shows, in turn, `l5boot: ota_0: quick restart 1 of 3 (reset reason 12, … ms after start)`, `… 2 of 3`
  and `l5boot: ota_0 stopped: 3 quick restarts, 0 early returns within 15 s (reset reason 12): starting launcher`
  (bootloader 0x5440 printed `ota_0 restarted 3 times within 15 s of starting`).
- **Clean restart via the LP watchdog** (since 0x1CE990). After a software reset, OrcSDR v0.3.0-beta.2 itself does
  a clean restart with the LP watchdog in `app_main` (`RTL_RESET_NORMALIZE original_reason=3 restarting_cleanly`, then
  `rst:0x9`), probably for USB and the RTL-SDR receiver. Bootloader 0x5440 treated this like any watchdog reset
  and started the launcher, and the countdown started OrcSDR again: 5 rounds about 10 s apart (log from 2026-10-19, build
  0x1CE880), until the user entered the launcher. Now an LP watchdog reset less than 15 s after the same app started
  (guard state valid, app not stopped, launcher not run in the meantime) boots it again
  and counts as a quick restart: `l5boot: ota_0: quick restart 1 of 3 (reset reason 9, … ms after start)`.
  OrcSDR then gets its clean restart and starts. An LP watchdog reset after 15 s → launcher, as before.
- **Loops through the launcher** (since 0x1CE990). A reset other than RESET, power-on, wake-up and a reset from the host
  (USB, JTAG) less than 15 s after the app started, e.g. a brown-out, a CPU watchdog or an LP watchdog that the
  app did not survive, is an "early return": the bootloader starts the launcher and records it in the guard state
  (`l5boot: reset reason 15 2000 ms after ota_0 started: starting launcher (early returns 1 of 3)`). The launcher
  does not zero this counter, and the countdown starts the app again. The third early return in a row stops it:
  the "… came back to the Launcher 3 times within 15 s of starting (brown-out) and was stopped" dialog. A reset
  while the launcher is running (e.g. its own brown-out) does not count.
- **What ends a series:** an app running for 15 s, RESET (also a reset from USB and JTAG), power-on,
  wake-up from deep sleep, a start by the user (BOOT and "Boot now" on HOME, FAST BOOT APP, BOOT AGAIN,
  RESTART NOW and START APP after an install, "APP · Boot …" in the power menu: the launcher then zeroes the registers),
  erasing or overwriting the slot, and a scheme change. The boot screen countdown and `btnOnlyStart` do not end
  a series.
- The launcher zeroes the registers at startup only after a stop (a marker in NVS carries the stop forward). Bootloader
  0x5440 and launchers up to 0x1CE880 zeroed them at every launcher start, which is why a loop through the launcher
  went on forever. After a stop, the launcher shows the "TAB5 Demo was stopped" dialog instead of the boot screen:
  "… restarted 3 times right after starting (panic) and was stopped. It may not support this Tab5." with the
  ERASE APP (FORMAT of the slot, with confirmation), BOOT AGAIN and OK buttons. The reason comes
  from `esp_reset_reason()`, that is, from what the app recorded before its last restart.
- A stopped app (marker in NVS `l5/stopped`, only for that image) does not start on its own, either from the countdown
  or with `btnOnlyStart`. After RESET the launcher shows the dialog again, this time "was stopped on an earlier start".
  The marker is removed after BOOT AGAIN, BOOT or "Boot now" on HOME, after erasing the slot and after a new install to
  that slot.
- The bootloader's decision is a single function without IDF types, `l5_guard_decide()` in `bootloader_components/main/l5_guard.h`
  (the launcher reads the same file). Test on the PC (`python tools\host_test\run.py`, the `test_guard` part; this
  test alone: `python tools\host_test\run.py --guard`): P4 reset codes, every reset reason with an empty state,
  after 2 s and after 20 s of the app running, with the launcher running, with a stop not yet shown, with a changed slot
  in otadata and with a corrupted or old state, LP counter wraparound; a device model with the launcher:
  OrcSDR (LP watchdog after 1.5 s → OrcSDR, RESET → launcher), a panic loop and an LP watchdog loop (stop after 3
  quick restarts), brown-out loops through the launcher with `bootTime` 1 and 5 s and app run times of 0.2–14 s
  (stop after 3 rounds), a mix of resets (stop after 7), end of the series after 20 s of running, after RESET and
  after a BOOT by the user, BOOT AGAIN, a brown-out in the launcher, a stop after a launcher panic, deep sleep. The test
  catches a revert of each of the three new rules (checked on modified copies of the header: 9, 15 and 10 failures).
- Unchanged: single app restarts (also several, if they are more than 15 s apart), RESET → launcher,
  wake-up from deep sleep (starts both series over), the otadata rules.

## HOME

Port of `renderHome()` and `hero()` from app.js (`main/ui/l5_home.c`): the "APPS IN FLASH" panel with the `ota_N` slots
from the real partition table. The large card and both fixes below came with build 0x1CE880 (flashed 2026-10-19); in the
device logs (0x1CEA90, 0x1CF910) the card is drawn, timings under "Speed" below.

- **One slot** (Default, Min SPIFFS, UiFlow2, Doom, Max App, Custom with a single `ota_0`): one wide card across the
  whole panel, no footer (the card itself holds the boot screen line and BOOT). Occupied slot: a 420 px cover, the
  line "APP0 · OTA_0 · ‹size›B SLOT" with BOOTS, the name, "author · vX" (the author from the OTA catalog, otherwise
  `project_name` if the registry renamed the app), the fields SOURCE / INSTALLED (with no install date: BUILT with the
  build date) / SIZE / FREE IN SLOT, SLOT USE with a bar, "STARTS AFTER THE Ns BOOTSCREEN", BOOT ‹NAME› (a long name
  is cut with "…") and ERASE (the same "Erase app0?" confirmation as in the card menu). Empty slot: a 420 px box with a
  dashed border and diagonal hatching, "NO FIRMWARE", "Empty slot", FROM SD CARD (SD tab) and FROM OTA (OTA tab);
  with no SD card, the line `SD CARD: …`.
- **2–8 slots:** cards in a grid and the footer with BOOT, as before.
- **The 420 px cover** is created once when the view is built (an RGB565 canvas in PSRAM, about 345 KB), then it is
  only shown. Typefaces follow the `U.cover` scale: initials in Orbitron ExtraBold 54 (`l5_orb_xb54`, a new font, only
  0–9 and A–Z, about 7 KB), the name in Orbitron Bold 22, the lines in Share Tech Mono 15.
- **Fix: with one slot the card was not visible** (on the device "only a thin bar at the bottom with the name", i.e.
  the footer). The card grid used the row alignment `LV_GRID_ALIGN_STRETCH`, which LVGL 9.3 does not support for grid
  tracks: `grid_align()` then leaves the position of the first row uninitialized (memory from `lv_malloc`), so
  the card landed in a random place. On the PC, successive layout recalculations gave y = 139 one time and
  y = −1,604,800,565 the next. With several slots it came out right by chance. Now `LV_GRID_ALIGN_START` (FR rows
  fill the height anyway).
- **Cover fix** (`l5_cover.c`: HOME, SD panel, OTA details): the "SLOT NN · ESP32-P4" text and the name
  shared one buffer, and draw tasks only run in `lv_canvas_finish_layer()`, so the name was drawn at the top. Now
  each text has its own buffer. The name on the bar stays on one line with "…" (a loop as in `U.cover`; before, it
  wrapped and ran into the caption or disappeared past the edge), the caption is in capital letters as in the
  prototype, and on the 132 px covers (SD, OTA) the bar grows so that both lines fit.

Screenshots on the PC (`python tools\host_test\ui_shots.py [folder]`, scenes 20–24): the device state (Custom, one
11264 KB slot, Aurora 1.3.21 from SD), an empty slot, two slots (2 Apps), the longest name from OTA with an author
and no install date, an empty slot with no SD card; 20b previews an outline instead of the glow (not in the firmware).

**Speed** (since build 0x1CF910, flashed 2026-10-19 at 11:59). Before the change (0x1CEA90, `tap->visible` lines): HOME
243–253 ms (build 57–68, render 166–174), the first frame after startup 466–481 ms (render 375–388), other tabs
with a ready view 119–168 ms (render 99–134). What slowed them down and what changed (device results after the
change at the end of the section):

- **The first frame after startup was HOME under the boot screen.** `l5_display_refresh_now()` does not draw at once, so
  the "tab HOME" frame from `main.c` was only produced after the boot screen was created, and LVGL 9.3 always draws
  the active screen under the top layer, even when a full-screen system screen covers it completely: the HOME view
  with the bar and the boot screen on top of it, the whole screen twice. Now under every full-screen system screen
  (startup, app launch, restart, OFF, Charge Mode) the view and the bar are hidden (`l5_ui_covered()`), and `main.c` only
  builds HOME (`l5_ui_prepare()`). The boot screen frame has its own measurement: `l5_display: bootscreen: tap->visible …`.
- **HOME was built twice after startup:** in `main.c` and after ENTER LAUNCHER, because `l5_ui_go()` rebuilds the active
  tab (that is meant to happen when the tab is tapped). Returning from a system screen now uses `l5_ui_show()`, which
  only shows the ready view. The other HOME rebuilds in the log came after an accent color change in CFG (then all
  tabs are rebuilt, because the accent is baked into their styles), and that stays.
- **The panel under the large card was painted for nothing:** the card (about 1218 × 542 px) has its own background, and
  the draw buffers are in PSRAM, so every pixel under it was filled twice. The panel now paints its gradient only around
  the card (a hole 4 px smaller than the card, so its rounded corners still show the panel); the image is the same down
  to the pixel (screenshots before and after compared: identical below the bar).
- **The glow and the cover shadow** stayed: LVGL 9.3 draws a shadow only at the edges and does not paint it under the
  object, and on the device they cost about 8 and 7 ms (measurement below). The glow stays (design decision, 2026-10-19);
  a cheaper equivalent (a 2 px outline in the accent color) is in screenshot 20b.
- **Measurement on the device** (removed on 2026-10-26 with the one-slot card it measured, when HOME became a carousel;
  kept here as the record of what was measured. The `L5_HOME_BENCH` flag in `main/ui/l5_home.c`, 0 since build 0x1CF730: in a normal
  build a long press on "APPS IN FLASH" does nothing and the `.bin` file has no measurement code). With `L5_HOME_BENCH 1`
  (and `L5_TOUCH_TRACE` in `l5_display.h`) a long press on the "APPS IN FLASH" label (about 1 s) draws the HOME view 3 times
  in each of 7 variants, every 400 ms, and `l5_display` logs each frame: `HOME as built`, `HOME no glow`, `HOME no emblem
  shadow`, `HOME no emblem`, `HOME no name`, `HOME panel under card` (the old double painting), `HOME panel only`
  (no card). Meanwhile the screen flickers through the variants, at the end `l5_home: bench: done` and the view as before
  (PC test: `run.py --idle`, the `shots --taps` part, builds HOME with `-DL5_HOME_BENCH=1` and checks that the screen
  is identical afterwards). A normal build keeps the lines `l5_home: emblem 420 px drawn in N ms` and `l5_home: HOME
  built in N ms` on every HOME build.

**Results on the device** (0x1CF910, 2026-10-19, `tap->visible` lines):

- startup after RESET: `emblem 420 px drawn in 24 ms`, `HOME built in 41 ms`, the boot screen frame `bootscreen:
  tap->visible 287 ms`, render 272 ms (previously 375–388 ms, because HOME with the bar was drawn underneath);
- ENTER LAUNCHER → HOME: 163 ms, build 0 (previously a second build, 57–68 ms);
- HOME ↔ SD: HOME 157–179 ms (render 148–166, build 0–2), SD 126–135 ms;
- other tabs: OTA 118–178 ms, WUI 128, PMAN 149, CFG 157–162;
- the two HOME rebuilds (223–226 ms, build 66–67) are taps on the active HOME tab, as intended.

So HOME fits within the other tabs' times (target 120–170 ms), and the first frame after startup is well below 400 ms.
Measurement of the card's parts (render in ms, two runs of 3 frames per variant; the cost is the difference of means):

| Variant | Render | Cost of the part |
|---|---|---|
| `HOME as built` (as in the build) | 143–148 | — |
| `HOME no glow` | 136–138 | glow about 8 ms |
| `HOME no emblem shadow` | 137–140 | cover shadow about 7 ms |
| `HOME no emblem` | 123–127 | the 420 px cover together with its shadow about 20 ms |
| `HOME no name` | 140–143 | name about 3 ms |
| `HOME panel under card` (as before 0x1CF910) | 166–172 | panel under the card: about 24 ms more |
| `HOME panel only` (no card) | 96–97 | the whole card about 48 ms |

## SD card: FAT32, exFAT, long names

- **FAT32 and exFAT.** IDF 5.5.2 has `FF_FS_EXFAT 0` hard-coded in FatFs and offers no Kconfig option for it.
  `cmake/exfat.cmake` copies the `fatfs` component to `build/l5_components/fatfs` with `FF_FS_EXFAT 1`
  and uses it in place of the built-in one (Aurora does the same; the global IDF is not modified). With exFAT,
  `ff.c` requires `CONFIG_FATFS_USE_LABEL=y`. Cards above 32 GB are usually formatted as exFAT.
- **Long names** are enabled: `CONFIG_FATFS_LFN_HEAP=y`, `CONFIG_FATFS_MAX_LFN=255`,
  `CONFIG_FATFS_API_ENCODING_UTF_8=y`. The BSP warning "Long filenames on SD card are disabled"
  was false: BSP 1.3.2 checks `CONFIG_FATFS_LONG_FILENAMES`, which is the name of a `choice` block,
  from which no macro is ever generated. The top-level `CMakeLists.txt` defines that macro for the BSP when LFN
  is really enabled.
- **The launcher never formats the card** (`format_if_mount_failed = false`,
  `CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL` disabled).
- **Card state** (`l5_board_sd_state()`): when mounting fails, the SD tab shows the reason
  and a TRY AGAIN button (mounts again without a restart), and empty slots on HOME show the line
  `SD CARD: …`:

  | Mount error | State | Message |
  |---|---|---|
  | `ESP_ERR_TIMEOUT`, `INVALID_RESPONSE`, `INVALID_CRC`, `NOT_FOUND` | `NO CARD` | no response from the card |
  | `ESP_FAIL` (FatFs `f_mount`, e.g. 13 = `FR_NO_FILESYSTEM`) | `NOT READABLE` | the card responds but has no FAT32/exFAT volume: format it on a computer |
  | other | `SD ERROR` | the ESP-IDF error name |

  `/config.conf` is read only at startup, so after TRY AGAIN the settings from the card take effect
  only after a restart.
- The warning `ldo: The voltage value 0 is out of the recommended range` comes from IDF
  (`sd_pwr_ctrl_new_on_chip_ldo` opens LDO channel 4 as adjustable at 0 mV), and `sdmmc_card_init`
  sets 3.3 V right after that. This is not launcher configuration and needs no changes.

## Installing from the SD card

The SD tab is a port of `L5.views.sd` from views.js and of `L5.install` / `L5.fits` / `L5.tooBig` from app.js.
Installing follows the same flow as OTA (`main/ui/l5_inst_ui.c` and the installer `main/l5_inst.c`,
described in the "OTA: online installer" section).

- Folder list in a dark panel (rows like `.frow`): folders first, then files, alphabetically.
  Tapping a folder opens it, tapping a file selects it and shows its details on the right.
  UP goes one level up and selects the folder we came out of. Names starting with a dot are skipped
  (also macOS `._*.bin`), as is `System Volume Information`. One folder shows up to 200 entries,
  the footer says when there are more.
- The ONLY BINS / ALL FILES button toggles `onlyBins` and saves `/config.conf`.
- The side panel for a `.bin` reads the file header (`esp_image_header_t` + `esp_app_desc_t`): project,
  version, build date, IDF version, whether the image fits in the slot (`fits` / `too big`) and whether the same
  image (by `app_elf_sha256`) is already in one of the slots. It also recognizes an image for another chip
  and for another ESP32-P4 revision (INSTALL disabled), a bootloader on its own (disabled) and a **merged image**
  (bootloader, partition table, app, data images) in both layouts found in practice: a file from flash address
  0x0 (only 0xFF up to 0x2000, bootloader at 0x2000, table at 0x8000: the default `esptool merge_bin`, M5Burner
  files, e.g. OrcSDR after TO SD) or a file that starts with the bootloader (table 0x6000 further on). The app and
  data images sit in the file at their table address minus the file's start address. Panel: "merged image from 0x0:
  bootloader, partition table, app, data", INSTALL enabled. Refused with an explanation in the panel (INSTALL
  disabled): a full flash dump (a file from 0x0 of 4/8/16/32 MB that covers all partitions of its
  table, e.g. from `esptool read_flash 0 0x1000000`) and an image that boots the launcher itself (an app partition
  named "launcher"). Until 2026-10-19 the launcher took the table at 0x8000 only from files starting with
  the bootloader, i.e. from a layout that real ESP32-P4 files do not have.
- INSTALL: the exact app length from the segment headers (for a merged image, from its app
  partition), then a check against the partition table. App image too big → the "App slot
  too small" dialog with a USE ‹scheme› button. A merged image whose data partitions do not match → the check
  dialog with INSTALL ANYWAY / AUTO PARTITION (as in OTA). With several slots, a selection menu: first
  the empty ones, then the occupied ones as "replace …" in red. With one occupied slot it asks "Replace …?"
  (the prototype does not ask this). Data images from the merged file that match the partitions (same
  label, type and size) go along with the app; with Ask SPIFFS on, first the question
  APP ONLY / APP + DATA. With it off (CFG: "Avoid SPIFFS") only the app is installed, without asking,
  and the data partitions stay unchanged, just as in the prototype (`withSpiffs` only with Ask SPIFFS). Avoid SPIFFS
  (since build 0x1CE880) is not yet tested on the device.
- The write runs in a separate task with a progress bar and CANCEL: the first 4 KB and an image check (chip,
  revision, app description), erasing the slot range and the data partitions, copying (a 32 KB buffer
  in internal RAM), `esp_image_verify` (chip, revision, segments, SHA-256). **The first sector
  (4 KB) with the image header is written last**, so an interrupted write never looks like
  an installed app. After an error this sector is erased and the slot is empty. If otadata
  pointed to the slot being overwritten, it is cleared before erasing.
- On success: `l5_slots_scan()`, the slot becomes the boot app (NVS `l5/boot`), a DONE dialog
  with RESTART NOW (hand-off like BOOT) or STAY IN LAUNCHER.
- **App registry in NVS** (namespace `l5`, keys `app0`…`app7`, blob): the display name
  (file name without `.bin` or the name from the catalog), source (`SD`, `OTA`, `LINK`), install date (from the RTC,
  empty if the clock is not set), image size and `app_elf_sha256`, for OTA also the author, fid and version
  from the catalog (entries of the old size can still be read). An entry applies only to the image it was written for:
  an app flashed later by hand with esptool falls back to `project_name`. "Erase slot" deletes the entry.
  The card on HOME shows the name from the registry, below it `project_name · vX` (when the name differs)
  and `SD · <install date>` or `FLASH · <build date>` for an app without an entry. The large card with one slot
  has this data in the SOURCE and INSTALLED fields (with no install date: BUILT with the build date).

## SD file manager: COPY / PASTE, RENAME, NEW FOLDER, DELETE, VIEW

Port of `sdAction()` / `renderSD()` from views.js (the SD menu of Launcher 2.10 has the same set). UI:
`main/ui/l5_sd.c` (list, side panel) and `main/ui/l5_sd_ops.c` (actions, clipboard, background task, dialogs),
file operations: `main/l5_files.c` (no LVGL, tested on the PC). Since build 0x1CE880 (flashed
2026-10-19; tested on the device: DELETE; **not tested**: COPY / PASTE, RENAME, NEW FOLDER and the text preview).

- **Where:** on the bar above the list: + FOLDER and PASTE (as in the prototype; PASTE grayed out when the clipboard
  is empty, red when it holds something). In the side panel above the footer: INSTALL (.bin file) or VIEW (text
  file) at full width, below it RENAME | COPY | DELETE for the selected item, and with nothing selected
  RENAME | DELETE for the current folder (no buttons in the card root). Tapping a folder opens it,
  so a folder gets selected as in the prototype: UP from inside it, or after NEW FOLDER, PASTE, RENAME.
- **COPY** remembers the path of the selected file or folder (clipboard, `R.sd.clip`), the footer shows
  "CLIPBOARD: name", toast "Copied … · open a folder and tap PASTE". Changes in place, without rebuilding
  the view. The clipboard stays after pasting (you can paste many times), follows a rename of the item itself or
  of a folder above it, and is cleared when the item is deleted.
- **PASTE** copies into the open folder, folders recursively. If the name is taken: `name-copy.ext`,
  `name-copy2.ext`, … (`uniq()` from the prototype: the extension starts at the last dot, the same for folders).
  Refusals: the source is gone ("Nothing to paste", the clipboard is cleared), a folder into itself or into
  its own subfolder ("Cannot paste here"). First counting (files, folders, bytes), then a space check:
  each file rounded up to whole clusters, each new folder one cluster, plus one cluster of headroom,
  against the free space from FatFs (cluster size from `f_getfree`); too little → "Not enough space" and nothing
  is copied. Copying in 64 KB blocks through a PSRAM buffer aligned to the cache line (the SD driver
  reads and writes it directly). After an error or CANCEL, whatever was already copied is removed.
  Log: `l5_sd: copy 5729872 bytes in 3456 ms (1619 KB/s), 1 files, 0 folders: /sdcard/a.bin -> /sdcard/b/a.bin`
  (when interrupted, with the suffix "(cancelled, copy removed)" or "(failed, copy removed)").
- **RENAME** (the selected item or the current folder) and **NEW FOLDER**: the on-screen keyboard with the current name
  ("RENAME FILE" / "RENAME FOLDER") or with `new_folder` ("NEW FOLDER NAME"; if that exists, `new_folder2`,
  `new_folder3`, …), up to 128 characters. Leading and trailing spaces are trimmed. Rejected, with a dialog that says
  why and CANCEL / EDIT NAME buttons (the keyboard comes back with the typed text): an empty name, the characters
  `/ \ : * ? " < > |` and control characters (FAT and exFAT do not accept them), a leading dot (the list does not
  show such names), a trailing dot (FAT strips it), "System Volume Information", more than 255 characters (UTF-16, as
  in FAT), a path longer than 319 characters, the name of another file or folder in this folder (FAT is not
  case-sensitive; changing only the case of the same item is allowed). After a rename the current folder
  stays open under the new path.
- **App registry and renaming:** the registry in NVS ties an entry to the image (`app_elf_sha256`), not to the file
  path (`S.slots[i].path` from the prototype has no counterpart), so renaming a file or folder leaves nothing
  in it inconsistent: STATUS "in flash · appN" in the panel follows the file after a rename and after a copy.
  Renaming `/config.conf` gives the toast "Renamed: the launcher starts with default settings next time".
- **DELETE:** for a file it asks right away "Delete name?" ("The file is removed from the SD card. A copy already in flash
  keeps working."). A folder is first counted in the background, then the dialog gives the number of items: "The folder
  and its 523 items (480 files, 43 folders, 1.2 GB) are removed from the SD card." (for the current folder, also that
  the list goes up one level). Deletion runs in the background from the deepest items, with progress "N of M items
  removed" and CANCEL (what is already deleted is gone; the rest stays, toast "Delete stopped: …"). Read-only items
  (e.g. with the attribute set in Windows) get it cleared (`f_chmod`) and are removed too. Never anything outside
  `/sdcard`, never the card root itself, no "." or ".." in the path (checked by `l5_files_delete`, regardless of the UI).
  **`/config.conf`:** the dialog says that the file holds the settings and the OTA favorites and that the settings
  return to defaults; after deletion (under the file lock, a pending background save is dropped) the settings return
  to defaults at once (brightness, accent, rotation…, the partition scheme stays), without writing a new file; a new
  one is created on the next change in CFG.
- **VIEW** (`.txt .conf .json .cfg .ini .md .csv .log .py`): a "VIEW" dialog with the name, text in Share Tech Mono 15,
  scrolled with a finger; CLOSE or a tap outside closes it. It shows at most the first 64 KB, the header says
  "FIRST 64 KB OF 1.2 MB", and for a whole file "3.2 KB · 120 LINES". A NUL byte in the first 64 KB → the
  "Not a text file" dialog (binary data, also UTF-16). The text is prepared for display: CR LF and a lone CR → LF,
  tabs expanded to 4 columns, terminal color codes (ESP-IDF logs) removed, other control characters and broken
  UTF-8 bytes shown as "?", a character cut at the 64 KB boundary skipped. The text goes to LVGL in chunks of
  at most 2 KB (one label per chunk, split at line ends), so only the visible ones are drawn.
- **Polish letters:** Share Tech Mono has Latin-1, but only ten letters of Latin Extended-A (it lacks
  ą ć ę ń ś ź ż). The new font `l5_latin_15` (Latin-1 from Share Tech Mono, the rest of Extended-A from Chakra Petch
  Regular 15, 11.5 KB) is the LVGL fallback for mono 13, 14 and 15, and Chakra Petch 17 for mono 16 and 18. Letters
  merged into the mono fonts themselves would raise their line height (15 px: 17 → 20) and shift all
  labels. Labels with names (the path on the bar, PATH, the clipboard, the text in VIEW) use a copy of the font with
  a taller line (`l5_mono13_tall` etc.), so that the accents of capital letters (Ś, Ż, Ó) are not cut off at the top.
- **Large folders:** the list reads FatFs directly (`f_readdir`: name, size and date in one pass);
  previously `stat()` for each file searched the folder from the start, i.e. quadratic time. Item counts of
  subfolders are computed for at most 300 ms per display (the rest is counted on selection), and rows
  share common styles instead of local ones. Each folder display logs
  `l5_sd: /sd/duzy: 230 entries (230 listed, 200 rows): read in N ms, folders counted in N ms, view built in N ms`.
- **Touch** as introduced in b1d3fe9: actions on CLICKED with the pressed style shown at once, COPY updates the footer
  and PASTE in place, after actions on the card the view is rebuilt only once the finger is lifted, during background
  work a transparent overlay catches taps, and the progress dialog with CANCEL appears only after 300 ms (small
  files do not flash a dialog). Rows are still 62 px.
- **Toasts** (the whole launcher): `lv_obj_fade_out()` in LVGL 9.3 applies its start value immediately
  and deletes a running `lv_obj_fade_in()` of the same object, so every toast stayed transparent (it was
  missing from the PC screenshots, and in the Wi-Fi test the toast "was not reported"). The fade-out is now a separate
  animation without `early_apply`: the toast is visible for about 2.2 s.

**Test on the PC** (`python tools\host_test\run.py`, the `test_files` part; this test alone:
`python tools\host_test\run.py --files`): `l5_files.c` on a folder that stands in for the card (NTFS, like FAT,
is not case-sensitive), result "PASSED":

- names: trimming spaces, empty, each of `/ \ : * ? " < > |`, control characters, a leading and a trailing dot,
  "System Volume Information", Polish letters, 255 characters (also 255 Polish letters = 510 B), 256 characters,
  broken UTF-8; case-insensitive comparison (also Ł/ł, Ó/ó, Ź/ź), "inside" with different case,
  the parent folder,
- available names: `x.txt` → `x-copy.txt` → `x-copy2.txt` → `x-copy3.txt`, different case, `dir-copy`,
  the last dot (`a.b-copy.c`), `new_folder2`, `new_folder3`, a name that does not fit,
- listing (size, date, hidden, limit, missing folder) and counting (files, folders, bytes, clusters),
- copying: a file, a tree with a Polish name byte for byte, progress rising to the total; refusals: into itself
  (also with different case), onto an existing file, onto the root, outside the card, missing source; CANCEL halfway
  (nothing is left); card full after 40 KB (the phrase "the card is full", partial copy removed, source
  untouched); half a file removed after an error,
- deleting: a tree with a read-only file, progress, a file; refusals: the root, the root with "/", "..", outside the
  card, missing; CANCEL after two items (the rest stays); rename (also case only, onto an existing
  file, outside the card, the root), new folder (twice, the root),
- preview: CR LF / CR / FF, tabs, color codes, control characters and broken bytes as "?", BOM, Polish
  text byte for byte, NUL = binary file, empty text; 4000 lines in chunks ≤ 2048 B split at line
  ends (identical when joined), a line without spaces (never split inside a two-byte character), a line with
  spaces (split at a space), a flood of tabs (truncation); files: 100 KB with a two-byte character at the
  64 KB boundary (character skipped, "cut"), empty, binary, missing.

**Screenshots on the PC** (`python tools\host_test\ui_shots.py [folder]`; about 10 s the first time,
then about 2 s): LVGL 9.3 with the firmware's options (`build/config/sdkconfig.h`, no FreeRTOS), the real fonts,
the theme, the launcher screen (`l5_ui.c`: views, top bar, layers, overlay) and all tabs, SD actions, dialogs and the
keyboard on a folder standing in for the card; stubs for the board, the display driver, the partition table (the one
from the device: Custom, one 11264 KB slot), network, NVS, settings saving and the installer (`tools/host_test/ui`,
`views_stubs.c`). Scenes 20–24 are HOME (the "HOME" section), 30–33 CFG, PMAN, OTA without Wi-Fi and WUI, 40–45
Charge Mode (78% charging, 15% on battery, 6% charging, no battery pack, 99% with no charge current: FULL, 60% with
no charge current: USB · NOT CHARGING). 16 SD scenes as PNG: the root, .bin (the longest side panel, with STATUS:
it fits with about 40 px of headroom), a text file, the clipboard, the current folder, a paste with a toast, a preview
of Polish text (accented capital letters in the first line intact), a preview of a truncated file, the delete
confirmations for a folder and `config.conf`, the RENAME keyboard with a Polish name, a rejected name, NEW FOLDER, a new
folder, pasting a folder into itself, a folder with 230 files ("FIRST 200 LISTED"). Background tasks run there at once,
so the progress dialog never shows. Scenes 50–53 come from the key test (`shots --keys`, also run by
`run.py`): the ring on CANCEL of a confirmation, on a menu item and on a CFG row, the text field with the keyboard
plugged in. The key test types on a stand-in keyboard (`views_stubs.c`: `shots_kbd_push`) and checks tab switching,
waking, the dialogs, menus, VIEW, progress, the text field, HOME, SD (on the card folder), OTA (no Wi-Fi, a failure,
search, three favorites) and CFG.

## OTA: online installer

Port of `L5.views.ota` (views.js) on top of the catalog that Launcher 2.10 uses for the Tab5
(`src/onlineLauncher.cpp`, `boards/m5stack-tab5` with `OTA_TAG "tab5"`). Code: `main/ui/l5_ota.c` (view),
`main/l5_net.c` (HTTPS), `main/l5_fw.c` (firmware description, comparison with the partition table, AUTO PARTITION),
`main/l5_inst.c` (installer) and `main/ui/l5_inst_ui.c` (install flow shared with SD).
Tested on the device 2026-10-18/19.

**Catalog source** (as in Launcher 2.10, checked live on 2026-10-18):

- list: `GET https://api.launcherhub.net/firmwares?category=tab5&order_by=name|date[&page=N][&q=…][&star=1]`
  → `{total, page_size: 100, page, items: [{fid, name, author, star}]}` (83 entries at that time, one page),
- versions: `?fid=X` → `{name, author, star, versions: [{version, file, published_at, …}]}`,
- install data: `?fid=X&version=V` → `install: {format: "merged", app: {source_offset, image_size,
  partition_size}, partitions: [{type, subtype, label, size, source_offset, copy_size, source}]}`,
- files: all of them (app, data images, TO SD, UPDATE ALL) come **straight from the M5Stack CDN**:
  `https://m5burner-cdn.m5stack.com/firmware/<file>` (relative names from the catalog), with Range requests.
  The CDN certificate chain ends at DigiCert Global Root G2 from `certs/ota_roots.pem`.
- Difference from Launcher (design decision, 2026-10-18): Launcher downloads through
  `https://api.launcherhub.net/download?fid=X&file=<CDN URL>` (fallback `/proxy?`) and sends the
  `HWID` header with the Wi-Fi MAC address. Without that header both endpoints answer HTTP 400
  `{"error":"The HWID header is required."}` (checked from the device and from a PC). By design the launcher
  sends no device identifier, so it does not use `/download` or `/proxy` and does not send `HWID`
  anywhere. launcherhub serves only the list, versions and install data; its download counter does not
  see these downloads.
- The catalog gives no description, file size, download count or cover image, so the panel does not show them
  (the prototype took them from `data.js`). The versions response also has short fields (`Fs`, `s`, `f`, `f2`,
  `ao`, `as`, e.g. for CHESS `ao` 65536, `as` 3212224, `Fs` 3277760 = `ao` + `as`), but Launcher (2.6.7, 2.9.1
  and the current one) does not read them: it takes the layout from the install data or from the partition
  table in the file itself. This launcher does the same and does not rely on those fields.

**View:**

- Without Wi-Fi: "CONNECT TO WI-FI" with the connection state and a CONNECT button (scan and connect as from the
  icon in the bar). Once connected, the view switches to the list by itself.
- ALL / STARRED / FAVORITES / DOWNLOADED chips, search from the on-screen keyboard (`q=` on the server side,
  a local filter in FAVORITES and DOWNLOADED), A–Z / LATEST (`order_by=name` / `date`), pages of 100 with
  arrows in the footer.
- Row: a badge with initials in a color derived from the name, the name, star, heart, author,
  "on SD vX". The prototype's cover image is only in the side panel: 100 covers on canvases would use
  800 KB of PSRAM and take seconds to draw.
- The side panel shows only what the catalog provides: version, publication date, author, star,
  and from the install data the app size (with a warning when it does not fit in the slot), the firmware's
  data partitions (`sys FAT 1 MB+`; the plus means the firmware carries an image of that partition) and the
  file type (merged image / app image). Plus ON SD and IN FLASH (a slot with this fid in the registry). Versions
  are chips, newest first (by `published_at`).
- Buttons: INSTALL vX, TO SD, FAVORITE / UNFAVORITE; for a custom link INSTALL, TO SD, REMOVE.
- Catalog queries run in a single background task (`l5_ota`): the latest one wins, and the result comes back
  under the LVGL lock. Longer actions (install preparation, TO SD, UPDATE ALL) have a progress dialog with CANCEL.

**INSTALL** (the same flow as in the SD tab):

1. Install data from the catalog. When there is none (e.g. a custom link from ADD LINK), the first 35 KB of the file
   (Range) tell whether it is an app image or a merged image (from 0x0 or from the bootloader, with the same refusals
   as in the SD tab). Before that, a one-byte read checks that the CDN (or the custom link's server) responds.
2. Comparison with the current partition table (`l5_fw_check`): whether the app fits in the slot, and for each
   of the firmware's data partitions OK (same label, type and size: the image will be written), LARGER (the app
   can use it, the image will not be written), SMALL, a different type or missing. An `nvs` larger than the
   launcher's is only informational.
3. If something does not match, a check dialog with the list and the buttons CANCEL / INSTALL ANYWAY / AUTO PARTITION.
   Below the list you can see right away the layout AUTO PARTITION will make and its effects (e.g. "Every
   installed app is erased", images that will not fit).
4. Slot choice (empty ones first), "Replace …?" when the only slot is occupied, and, with Ask SPIFFS on, the
   question APP ONLY / APP + DATA (data images overwrite the contents of those partitions). With it off (Avoid
   SPIFFS), only the app, without asking; DONE then says "without its data images (Avoid SPIFFS)".
5. Installation with a progress bar and CANCEL, then DONE with RESTART NOW (otadata → ota_N, the same procedure
   as BOOT, with an orderly Wi-Fi shutdown in `esp_restart()`) or STAY IN LAUNCHER. After a network error, a
   dialog with RETRY.

**AUTO PARTITION** (`l5_fw_plan`): one slot for the app (the image rounded up to 64 KB; the slot gets the rest of
the area before `aurora_cfg` as headroom for updates), the firmware's data partitions with its labels
and exact sizes (only then can their images be written: FAT with wear leveling and SPIFFS have their
geometry in the image) and the 872 KB gap after `aurora_cfg` for one of them. When it does not all fit, first
the partitions without an image shrink (but not below 448 KB), then images are dropped, largest first; the
dialog says what is affected. nvs, otadata, launcher, `aurora_cfg` and coredump stay in place. The table is
written the same way as for a scheme change (`l5_pt_apply`: backup, verification, erasing the changed
partitions). Then the installation goes straight into the new table (addresses from it, no restart in
between), and at the end there is one restart (TO LAUNCHER or START APP). Data images are written only with
Ask SPIFFS on; with Avoid SPIFFS it makes the same layout but installs only the app, and the new data
partitions stay empty (the confirmation dialog says so). The effect on firmware that needs its data images:
UIFlow2 then gets an empty `sys` (system files) and `vfs`. UIFlow2 formats `vfs` itself (checked earlier in
its sources); whether it starts with an empty `sys` has not been checked; for such firmware it is better to
turn on Ask SPIFFS and choose APP + DATA.

UIFlow2.0 v2.5.3 example (a real catalog response): app 7,709,696 B, FAT `sys` 1 MB with an image, FAT `vfs`
4864 KB with an image, SPIFFS `storage` 640 KB without an image. On Default, `sys` and `storage` are missing and `vfs`
has 5 MB (the image will not be written). AUTO PARTITION: `app0 7552 KB · sys FAT 1 MB · vfs FAT 4736 KB · storage
SPIFFS 640 KB`. The app, `sys` and `vfs` together take 13440 KB, and there are 13312 KB before `aurora_cfg`, so the
`vfs` image is not written, and `vfs` has 4736 KB and stays empty. The dialog says so plainly ("vfs gets 4.6 MB
instead of 4.8 MB and stays empty (no room for its image): the app formats it when it starts, the files in that
image are not copied.").

What this means for UIFlow2 2.5.3 (checked in the uiflow-micropython sources at tag 2.5.3, not on the device):
`_boot.py` mounts `sys` as `/system` and `vfs` as `/flash`. When `/flash` does not mount (empty partition),
`inisetup.setup()` formats `vfs` (LittleFS), writes `boot.py` with the startup menu (`boot_option` 1) and creates
the directories `apps`, `libs`, `res/font` and `res/img`. The Tab5 startup menu (`startup/tab5` modules) is built
into the firmware and reads nothing from `/flash`, and `/system` comes from the written `sys` image. Only the
sample files from the `vfs` image are lost. In the 2.5.3 sources these are `apps/helloworld.py`, images in
`res/img` (default.jpg, default.png, m5stack.pbm, uiflow.bmp, uiflow.jpg, uiflow.png), README files and an
empty `main.py`. Programs from the UIFlow IDE upload their own resources. So repacking the image files into a
smaller `vfs` is not needed; it would also require LittleFS in the launcher, since UIFlow2 uses that file
system. I did not download the M5Burner image itself (a 16 MB file), so its contents may differ from the sources.

**Installer** (`l5_inst.c`, also for SD):

1. The first 4 KB of the app: whether the source responds and has an ESP32-P4 image with an app description and
   the right revision. Nothing is erased up to this point.
2. Erasing the slot up to the end of the image (size from the catalog or from the file).
3. The app from byte 4096, streamed in 32 KB pieces (no buffer for the whole file). The image length follows from
   the segment headers read on the fly. In the same pass the launcher searches the image for the names of the
   display and touch drivers (case-insensitive): `gt911`, `ili9881` (the first Tab5 boards) and `st7123`,
   `st7121`, `st712x` (later ones). A dropped connection is resumed from where it broke off with a Range request:
   3 attempts with pauses of 2, 4 and 6 s, during which Wi-Fi can reconnect.
4. **Older-hardware warning:** when the image has only the names of the first board (GT911 / ILI9881C, no
   ST712x) and this Tab5 has ST712x touch (detected at startup at I2C 0x55, as in Aurora), the "Older Tab5 hardware"
   dialog: "This firmware seems to support only the older Tab5 display/touch (ILI9881C + GT911). This Tab5
   has ST7121 + ST712x, so it may not start." with INSTALL ANYWAY / CANCEL. It appears before the data images
   and the header are written, so CANCEL leaves the data partitions unchanged and the slot empty. With none of
   the names, or with both groups (Aurora, launcher), there is no warning.
5. Data images: each partition is erased only right before its image is written.
6. The first sector is written last, then `esp_image_verify` on flash (segments, appended SHA-256, chip). After an
   error or CANCEL this sector is erased: the slot is empty rather than holding half an app.
7. Registry in NVS: name, author, source (`OTA`, `LINK`, `SD`), fid and version from the catalog.

Erases and writes can only land in the range 0x210000–0xFF0000 outside `aurora_cfg` (checked before every
operation). The bootloader and the partition table from a merged image are never written. The log reports the
throughput of each connection (`l5_net: GET host/path: N bytes in T ms (R KB/s), HTTP 206`) and of each part
(`l5_inst: app: N bytes in T ms (R KB/s), flash writes T ms`).

**TO SD** saves the file to `/sdcard/downloads/<name>.bin` (through a `.part` file, renamed only after
success) and, like Launcher, a `fid: version` entry in `/sdcard/downloads/downloaded.json` (`[{…}]`). The log
reports the network and card write times (`l5_ota: to SD … (R KB/s), SD writes T ms`): Wi-Fi (SDIO, slot 1)
and the card (slot 0) share one SDMMC controller. A merged file from the card is then installed in the SD tab
with the same flow (with the partition table from the file).

**DOWNLOADED** shows the entries from `downloaded.json`. **[UPDATE ALL]** checks the latest catalog version of
each one and downloads a newer one to the card. It is the counterpart of "Check for Updates" in Launcher, but
through `?fid=` instead of `POST /updateList`, because I do not know the format of that response.

**FAVORITES** work as in Launcher: `"favorite": [{name, fid, link}]` in `/config.conf`. A catalog entry has a fid,
a custom link has an empty fid and the address in `link`. ADD LINK asks for a name and an `https://…` link on the keyboard.

**TLS:** esp_http_client and mbedTLS as a client only (ECDHE, curves P-256, P-384 and X25519, no renegotiation,
CCM, PKCS7, CRL or CSR). Instead of the IDF certificate bundle the launcher has 12 root certificates
in `certs/ota_roots.pem`: ISRG Root X1/X2 (api.launcherhub.net, *.githubusercontent.com), DigiCert Global
Root G2 (m5burner-cdn.m5stack.com), USERTrust ECC (github.com) and a few popular CAs for links from other
servers. Up to 5 redirects (also to another host). Connecting, including the TLS handshake and response headers,
is allowed 15 s (on the Tab5, launcherhub answers in 2–5 s), and after a timeout or a failed connection there is
one retry after 0.5 s. During a download only 30 s without data counts as an error. Every error has a sentence
for the user naming the server (no Wi-Fi, DNS, no connection, TLS with code, HTTP with status, timeout, dropped
connection), and the log gives the host, path and query: `l5_net: GET m5burner-cdn.m5stack.com/firmware/<file> failed:
m5burner-cdn.m5stack.com answered HTTP 404 (file not found).`

**Test on the PC** (`tools/host_test`: `l5_ptable.c`, `l5_fw.c` and `l5_inst.c` compiled
with IDF stubs, 16 MB of flash simulated in memory, a simulated HTTPS server with Range support; result
"PASSED: 0 failure(s)"):

- the image length parser on the real `tab5_launcher.bin` and `bootloader.bin` (chunks of 1, 7, 24 and 4096 B
  and 1 MB, plus walking the headers alone, as for a file on the card),
- the UIFlow2.0 v2.5.3 manifest: fields, comparison with Default, AUTO PARTITION plan, table write, repeated
  comparison (`sys` with an image, `vfs` smaller, as in the plan),
- a merged image in the real layout from address 0x0 (0xFF up to 0x2000, bootloader, table with an MD5 record at
  0x8000, the launcher as the app at 0x10000, two FAT images): detection, app length from the file, plan,
  table write and installation over HTTPS. The app is
  in the slot byte for byte, the `sys` and `vfs` images in place, `storage` empty, nothing else changes,
  the registry has the fid and version. APP ONLY does not touch the data partitions. Two dropped connections
  while downloading the app: resumed via Range, correct result. Four drops: an error with RETRY available, the
  slot without a header. CANCEL during the download. An ESP32-S3 image and a wrong revision: refused before
  erasing, flash untouched. Catalog size larger than the slot: refused without connecting. The same image
  starting with the bootloader (base 0x2000): app at 0xE000, `sys` image at 0x3FE000 in the file; installing
  from the card gives the same flash. SANITY CHECK of the app inside both files (at 0x10000 and 0xE000).
  Refusals: a copy of the whole 16 MB flash, an image with an app partition "launcher", the old test layout
  (bootloader at 0, table at 0x8000: does not occur in real ESP32-P4 files), while a 12 MB file passes. The
  same image from a file on the "card" and an app image from a file into the Default table,
- plans for other tables: a 12.5 MB app with a 1 MB FAT image (the image is dropped, FAT 512 KB), an app over
  13 MB (refused), `spiffs` 256 KB in the gap, LittleFS in the gap, seven partitions (two are dropped),
- driver names: any split of the text into chunks (down to 1 byte) and any letter case give the same
  names, incomplete names do not count; `tab5_launcher.bin` has both groups (no warning), `bootloader.bin`
  neither. Installing an image with only the GT911 / ILI9881C names: CANCEL at the warning leaves the data
  partition byte for byte and the slot without a header, INSTALL ANYWAY installs, an image with both groups
  triggers no prompt. The rule alone on any file: `python tools\host_test\run.py --scan file.bin …`,
- all schemes from the current Default and from the old tables, and the rejected layouts.

To run: `python tools\host_test\run.py` (C compiler: `zig cc` or `gcc`, the `CC` variable overrides
it; ESP-IDF from `IDF_PATH` for cJSON and `gen_esp32part.py`; a built firmware, because the image parser is
checked on `build\tab5_launcher.bin` and `build\bootloader\bootloader.bin`; the font test and the dimming test
compile LVGL with the options from `build\config\sdkconfig.h`, about 10 s the first time, the objects stay
in `%TEMP%\l5_ui_lvgl`).

Tested on the device (2026-10-18/19): HTTPS to api.launcherhub.net and m5burner-cdn.m5stack.com (certificates,
Range), download with writing to flash at 262–265 KB/s, the check dialog and AUTO PARTITION, the older-hardware
warning, the boot-loop guard. Not yet tested on the device: resuming after a Wi-Fi drop
and CANCEL during a download, TO SD and UPDATE ALL, FAVORITES and ADD LINK (GitHub redirects),
APP ONLY / APP + DATA.

## PMAN: partitions and scheme changes

Port of `renderPM` / `pmAction` (views2.js) and `L5.pickScheme` / `L5.applyScheme` (app.js),
on the real partition table at 0x8000.

- Flash map (segments as in `.fmap`), a LABEL / TYPE / SUBTYPE / OFFSET / SIZE / USED table,
  a "(free)" row for the free space before `aurora_cfg` (room for FAT) and "(spiffs gap)" for
  the empty gap after it (room for SPIFFS). Selected-partition panel: SYSTEM · LOCKED for nvs,
  otadata, the launcher, **`aurora_cfg`** and coredump (no button is active); for slots
  and data partitions FORMAT, RESIZE, BACKUP, RESTORE, DELETE; for free space CREATE PARTITION.
- Schemes from the table in the "Partition table" section, and Custom: number of slots, slot size
  (up to 13312 KB / number of slots), SPIFFS ("SPIFFS 872K after aurora_cfg" or "No SPIFFS"),
  FAT gets the rest of the area before `aurora_cfg` (or nothing when less than 448 KB is left).
  The same mechanism handles RESIZE (slots or the main FAT), DELETE / CREATE, AUTO PARTITION
  (for an app image a slot fitted to the file; for a merged image the layout from its own partition
  table, the same as in the INSTALL dialog), OPTIMIZE FLASH (slots shrunk to the largest app,
  the rest for FAT; **apps are erased**, because the images cannot be moved) and the USE ‹scheme› button
  in the "App slot too small" dialog. Aurora-Player-1.3.21-Application.bin (6.3 MB) fits in Default.
- Firmware partitions added by AUTO PARTITION (e.g. `sys`, `storage`) take their size from the firmware's table:
  RESIZE does not change them (message "Fixed size"), DELETE removes exactly that partition. Previously DELETE
  and RESIZE always acted on `vfs` or SPIFFS. The partition in the gap after `aurora_cfg` has a fixed size
  (SPIFFS 872 KB or the size from the firmware's table).
- FORMAT erases the partition (the app that uses it will format it its own way at
  startup: UiFlow2/MicroPython and ESP-IDF FAT have different formats). BACKUP saves a raw copy
  to `/sdcard/backups/<label>_RRRRMMDD.bin`, RESTORE picks a copy from that folder.
- SANITY CHECK reads the selected .bin from the card the way the bootloader does: header, segments, checksum,
  appended SHA-256, chip (chip id) and whether it fits in the slot. For a merged image (from 0x0 or from the
  bootloader) it checks the app inside the file: "Merged image from 0x0: the app at 0x10000 of the file was
  checked.", with an explanation when INSTALL rejects such a file (copy of the whole flash, launcher).

**Changing the partition table** (`main/l5_ptable.c`, `l5_pt_build` + `l5_pt_apply`):

1. The new table is built only in the area between the end of the launcher (0x210000) and coredump
   (0xFF0000): slots `app0…appN-1` (`ota_N`, 64 KB) from 0x210000, after them the firmware's data
   partitions from AUTO PARTITION (at most 4), `vfs` (FAT) up to 0xF10000 at the latest, then `aurora_cfg`
   at 0xF10000 and one partition at 0xF16000 (`spiffs` 872 KB or a firmware partition of up to 872 KB).
   Bootloader, 0x8000, nvs, otadata, launcher, coredump and the existing `aurora_cfg` must stay identical
   byte for byte (the `aurora_cfg` entry is copied from the old table).
2. Full validation before touching flash: labels, alignment (4 KB, apps 64 KB), no
   overlaps, flash size, at least 1 MB per slot and 448 KB (0x70000) for the main FAT (firmware
   partitions take their size from the firmware's table), system labels and `app*` reserved,
   consecutive `ota_0…`, unchanged system area, exactly one `aurora_cfg` (data/nvs,
   0xF10000, 24 KB), nothing overlapping 0xF10000–0xF16000, then on the finished bytes the same
   function the bootloader uses (`esp_partition_table_verify`, with the MD5 record as
   in gen_esp32part.py), and a read-back.
3. The confirmation dialog says plainly what will be erased: when the slots change, every installed app
   by name ("Aurora-Player-1.3.21 in app0 will be erased: install it again from SD or OTA
   afterwards."), plus the data partitions that are new, move or change size, and that the Tab5
   will restart. The same goes for the AUTO PARTITION dialog and for slot FORMAT.
4. Writing in a separate task, the screen covered by a progress dialog, the power menu locked:
   a backup of the old table in NVS (`l5/pt_old`) and on the card (`/sdcard/backups/partition-table_RRRRMMDD.bin`),
   clearing otadata, erasing sector 0x8000, writing 0xC00 bytes, reading back and comparing
   (plus `esp_partition_table_verify`). On a mismatch the old table is written back
   and checked. The window between the erase and the write is two flash operations (a few tens of ms).
5. Then erasing: the first 64 KB of every new slot (no old image stays at the
   start of a slot) and every data partition that is new, moved or changed in size
   (compared by label), the app registry in NVS and the S.boot choice.
   When the slots do not change (e.g. UiFlow2 → UiFlow2), the apps stay. The 24 KB of `aurora_cfg`
   are erased **only when that partition is created for the first time** (the old table did not
   have it, so at 0xF10000 there are bytes of the old FAT or of an app). The erase function refuses
   any other erase in that range. Finally `esp_restart()`; the empty otadata boots the
   launcher with the new table, and `cfg.scheme` is saved in `/config.conf`.

**Migrating from an old table** (without `aurora_cfg`, e.g. the old Default with SPIFFS at 0xA10000 or
the old UiFlow2 with FAT up to 0xFF0000): PMAN accepts it as usual and shows its partitions, but no
scheme carries the "current" marker. Choosing any scheme (including the same one)
writes a table with `aurora_cfg` and erases its 24 KB. After a new `partitions.csv` is flashed over USB
onto a device with the old layout, `aurora_cfg` exists right away but may contain bytes of the old
FAT. That is why at every startup the launcher (`l5_pt_aurora_sanitize()` in `main.c`) checks these
24 KB: it leaves them alone when they are fully erased or some page has a valid NVS header (state
and CRC32 as in `nvs_page`), and otherwise erases them once and logs
`aurora_cfg holds neither erased flash nor NVS (old FAT bytes?): erasing its 24 KB for Aurora`.
An old Aurora image that crosses the app0/app1 boundary after the table change does not fit
in the slot (`esp_image_get_metadata` returns an error), so HOME shows an empty slot.

**Writing at 0x8000 and the flash configuration:** `sdkconfig` has not changed, the protection stays
in the default mode (`CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS`). For the duration of those two operations
on sector 0x8000 the launcher calls `esp_flash_set_dangerous_write_protection(esp_flash_default_chip, false)`
and turns the protection back on right afterwards. `esp_ota_ops.c` in IDF does the same when updating
the partition table. With flash encryption enabled the launcher refuses to change the table.

I checked the `l5_ptable.c` logic on the PC (compiled with IDF stubs, flash simulated
in memory, the `aurora_cfg` area filled with a pattern): all schemes from the new Default
(`aurora_cfg` identical byte for byte every time, and no byte outside the allowed ranges
changes), migration from the old Default and the old UiFlow2 (`aurora_cfg` is created, its 24 KB are
erased), several Custom layouts, rejected layouts (`aurora_cfg` moved, shrunk, removed,
FAT overlapping it, SPIFFS of a different size, too many slots), a write error with the old table
restored, a restore error, a failed backup to NVS, a changed system partition, a table changed
after reading, and `l5_pt_aurora_sanitize()` (erases garbage, keeps an erased area and an NVS page).
The resulting tables also went through `gen_esp32part.py` (with the MD5 check). Since OTA the test is in the
repository (`tools/host_test`, the "OTA" section → "Test on the PC") and also covers the new schemes, layouts
with firmware partitions, gaps smaller than 872 KB and rejecting reserved labels.

## CFG

Port of `renderCFG` / `cfgAction` (views2.js), three columns DISPLAY / INSTALL & STORAGE / SYSTEM.

- Take effect immediately: brightness (5% step), dim time, screen-off time (Screen off: OFF, 1, 2, 5,
  10, 30 min), accent color (the whole UI, including the top bar), rotation 0°/180°, boot screen time. Every change goes to `/config.conf`
  (without a card it works until a restart).
- Switches: Ask SPIFFS (off: "Avoid SPIFFS", installing without data images), Only Bins,
  Wi-Fi auto-connect (the "Wi-Fi" section), Start from DeepSleep
  (turning it off asks for confirmation; the bootloader reads it at the next app start), Only with button.
- Actions: Charge Mode, Partition scheme and List of partitions (PMAN), Clear FAT, Save / Restore
  SPIFFS (`/sdcard/backups`), Wi-Fi networks (the Wi-Fi menu, subtitle "Connected: SSID" or "N saved"),
  Reset config (the scheme stays; the "dev" version also removes the saved Wi-Fi networks and the NVS namespace
  `nvs.net80211`), About (with the ESP32-C6 firmware version), Restart, Turn off.
- USB Mass Storage is a placeholder with a description for now.

## Screen dimming and screen off

Port of the idle timer from app.js (`setVeil`, `L5.DIMS`, `L5.OFFS`), `main/ui/l5_ui.c`. Both times count from
the last touch (`lv_display_get_inactive_time`); a timer checks them every second. Screen off since build
0x1CBE30 (in 0x1CE880, flashed 2026-10-19: in the device log, dimming after 20 s and `screen off after 60 s`).

- After `dim` seconds (CFG → Dim time) the backlight drops to 12% of the CFG brightness (+1), and a veil catches
  the first touch.
- After `screenOff` seconds (CFG → Screen off, default 2 min) backlight 0 and the veil. Works with Dim time OFF too,
  and with Screen off shorter than Dim time the screen goes dark without dimming. The panel gets no DISPOFF (it
  showed noise when turned back on), only the backlight goes off. Log: `l5_ui: screen off after N s without
  touch: the next tap only wakes`.
- In the dark LVGL does not draw: `lv_display_enable_invalidation()` is off (a counter, always one disable and one
  enable), so the clock every 5 s and background changes do not occupy the LVGL task. After wake-up the whole
  screen is redrawn at once, with the current time and whatever changed in the dark.
- Waking is unchanged: the first touch on a dimmed or dark screen only wakes it (the CFG brightness returns),
  the tap does not reach the UI. A new dialog that opens by itself (end of an install, an error) also wakes the
  screen and resets the idle time, so that it can be seen. Toasts do not wake it.
- Not during the boot screen or system screens (starting an app, RESTARTING, POWERED OFF, Charge
  Mode): switching to them turns the screen on if it was dark.

**Test on the PC** (`python tools\host_test\run.py`, the `shots --idle` part; this test alone:
`python tools\host_test\run.py --idle`): the real `l5_ui.c` and LVGL timers on a simulated clock, brightness 70,
dim 30 s, Screen off 60 s: after 29.5 s bright, after 31.5 s dimmed (level 9), after 61.5 s backlight 0
and drawing paused, a tap on the veil wakes (70, drawing resumes); Dim time OFF: dark after 60 s with no
dimming on the way; Screen off OFF: only dimmed for 400 s; dim 300 s and off 60 s: dark after 60 s;
a dialog that opened by itself (`l5_info`) wakes; a system screen (`l5_ui_set_booted(false)`) wakes, and outside
the launcher the screen does not go dark; after three screen-off and wake cycles drawing is still enabled.

## On-screen keyboard

Port of `U.keyboard` from ui.js (`main/ui/l5_keyboard.c`). Any view can open it and gets the text
in a callback:

```c
static void name_cb(const char *text, void *user)
{
    if (!text) return;           /* CANCEL */
    /* OK: text is valid only during this call */
}

const l5_keyboard_opts_t opts = {.title = "NEW FOLDER NAME", .value = "new_folder", .mask = false, .max_len = 64};
l5_keyboard(&opts, name_cb, NULL);
```

- `l5_keyboard()` returns right away (in the prototype it is a `Promise`). The keyboard disappears before the
  callback runs, so the callback can open another dialog or the next keyboard right away. The typed text
  is then zeroed in memory (passwords).
- Look based on the theme tokens (`.kb-wrap`, `.kb.dark`, `.key`, `.key.fn`, `.key.ok`, `.kb-field`):
  a 1280×450 panel at the bottom edge over a dimmed screen, 64 px keys with a gradient
  and a shadow, the pressed key in the accent color and shifted by 2 px, a field with a border and a blinking
  cursor in the accent color.
- Two layouts as in the prototype (abc and ?123). SHIFT is one-shot for letters; in ?123 it stays
  on. `mask`: dots instead of characters and a SHOW / HIDE key. `max_len`: character limit (default 64).
- When the text is longer than the field, the field shows its end with "…" at the start.
- Differences from the prototype:
  - in the ?123 layout there is a dot instead of "€": no launcher font has the U+20AC character (new
    fonts require Node.js). The field takes "£" and "§" from Chakra Petch (the fallback font), "°" is
    in Share Tech Mono;
  - ⌫ and the SHIFT arrow are FontAwesome symbols from LVGL's built-in Montserrat 14 font
    (the fallback font of the function keys);
  - a long press on ⌫ keeps deleting characters;
  - the M5Stack Tab5 Keyboard stands for `keydown` (see "Restored build of 2026-10-20": characters, Backspace / Delete,
    Ctrl+U = CLEAR, Enter = OK, Esc = CANCEL, the compact layout with the bar); there is no pasting.
- Today it is used by the Wi-Fi password ("PASSWORD · SSID"), the OTA search ("SEARCH FIRMWARE") and ADD LINK
  in the OTA favorites (name and link). It is ready for renaming and new folders on SD and for the WUI login.

## Wi-Fi (ESP32-C6 over esp_hosted)

### Which firmware the C6 runs and why host 2.12.6

- The Tab5 has an ESP32-C6-MINI-1U module connected to the P4 over SDIO: slot 1 of the SDMMC controller,
  CLK GPIO12, CMD 13, D0–D3 11/10/9/8, C6 reset (EN) on GPIO15. C6 power comes from the PI4IOE5V6408 0x44
  expander, pin 0 (`WLAN_PWR_EN`): it is off after power-on. The SD card uses the same SDMMC
  controller (slot 0).
- **This Tab5 (ESP32-P4 v1.3, ST7123 panel) has ESP-Hosted-MCU slave 2.12.6 on the C6.** That is what the C6
  itself reported on the device on 2026-10-18 (`l5_wifi: ESP32-C6 firmware: ESP-Hosted 2.12.6`). The first Tab5
  units came with 1.4.1 from the factory: the file `ESP32C6-WiFi-SDIO-Interface-V1.4.1-96bea3a_0x0.bin` in
  M5Tab5-UserDemo (`platforms/tab5/wifi_c6_fw`), which the M5Stack demo pairs with host `espressif/esp_hosted`
  1.4.0 and `espressif/esp_wifi_remote` 0.8.5. Issue bmorcelli/Launcher#440 also describes a Tab5 with 2.12.6.
- Espressif recommends the same version on both sides (esp-hosted-mcu `docs/troubleshooting.md`: "use the
  same version for master and slave"), and this proved true: the first launcher version, with host 1.4.1, did
  start the C6 running 2.12.6 (transport, version, `esp_wifi_init`), but during a scan it read a C6 event as a
  second `WIFI_EVENT_STA_START` and lwIP stopped at `assert failed: netif_add ... (netif already
  added)`. A panic means a software reset, so the Tab5 started the app from otadata (Aurora).
- That is why `main/idf_component.yml` has **`espressif/esp_hosted` ==2.12.6** and
  **`espressif/esp_wifi_remote` ==1.5.1** (esp_hosted from 2.11 on requires esp_wifi_remote ≥ 1.3.1; 1.5.1 is
  from the same weeks as 2.12.6 and supports IDF 5.5). The component manager also pulled in
  `espressif/wifi_remote_over_eppp` 0.3.3, `espressif/eppp_link` 1.1.6 and `espressif/esp_serial_slave_link`
  1.1.2 (esp_wifi_remote dependencies; they do not end up in the image, since the Wi-Fi library is ESP-Hosted, not EPPP).
- **Version gate:** after the transport starts, the launcher asks the C6 for its version and starts Wi-Fi only
  when major.minor matches the host (today 2.12). Otherwise Wi-Fi stays off with the message
  "ESP32-C6 not available" and both versions, instead of crashing at the first scan. A Tab5 with the factory
  1.4.1 on the C6 will therefore not connect with this build: that would take a build with host 1.4.x or a C6
  firmware update.
- Host options (the "Wi-Fi" section in `sdkconfig.defaults`): C6 as slave (`CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6`),
  SDIO slot 1, 4-bit, 40 MHz, receive in streaming mode, pins as above, reset on GPIO15. The reset polarity
  is `CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH`: with it ESP-Hosted 2.x gives a HIGH→LOW→HIGH pulse,
  and EN on the C6 is active low. 1.4.x ignored this setting (an `#ifdef` bug), so the M5Stack configuration
  with "Active Low" worked only there; in 2.x "Active Low" keeps the C6 in reset (esp-hosted-mcu#127).

### How the launcher starts the C6

`main/l5_wifi.c`: a single worker task `l5_wifi` (core 1). The UI never waits for the C6.
Nothing from ESP-Hosted runs when the launcher starts. By itself, the component starts through the constructor
`esp_hosted_host_init` (`port_esp_hosted_host_init.c`) even before `app_main`. With 2.12.6 the launcher's main
task could not be created after it (`assert failed: esp_startup_start_app ... (res == pdTRUE)`,
and the panic started Aurora), so the top-level `CMakeLists.txt` leaves this file out of the build. The
component itself stays unchanged. The C6 starts only when the launcher needs it:

1. `esp_hosted_init()`: slot 1 of the SDMMC controller, tasks, queues and buffers, no talking to the C6 yet.
   On the device this took about 124 KB of internal RAM (338 → 214 KB free, largest free block
   108 KB); log `l5_wifi: ESP-Hosted host 2.12.6 started: internal RAM … -> … bytes free`. Then the event
   loop, `esp_netif` for the station and the handlers for Wi-Fi, IP and `ESP_HOSTED_EVENT` events.
2. C6 power is switched off for 100 ms and back on. The expander keeps its state across a RESET of the
   P4, so the C6 may have been left in a state from the previous app (in bmorcelli/Launcher#440 Wi-Fi does
   not come back after other firmware). The pin is set directly through the PI4IOE registers (push-pull output),
   without resetting the chip: the BSP expander driver resets the whole chip when it is first opened, and the
   same expander carries the USB 5 V and charging pins.
3. `esp_hosted_connect_to_slave()`: reset pulse on GPIO15, 1.5 s for the C6 to boot, SDIO card initialization,
   INIT event from the C6. In 2.12.6 it ends with an error (no abort) after about 25 s at the latest; a failed
   card initialization does not release the SDMMC controller, so the SD card keeps working.
4. C6 firmware version (`esp_hosted_get_coprocessor_fwversion`) and the version gate (above).
5. `esp_wifi_init`, `WIFI_STORAGE_RAM` (the C6 writes nothing to its flash), STA mode,
   `esp_wifi_start`. On the device: transport after 1.9 s, station ready after 2.2 s (the log gives both times).

Safeguards:

- **No restarts from ESP-Hosted.** A software reset of the launcher starts the app from otadata,
  so `CONFIG_ESP_HOSTED_TRANSPORT_RESTART_ON_FAILURE=n`. ESP-Hosted then reports a transport failure with
  the `ESP_HOSTED_EVENT_TRANSPORT_FAILURE` event: the launcher turns Wi-Fi off ("ESP32-C6 not available",
  "press RESET") and keeps running. To cover any other restart path, the launcher also replaces the
  `_h_restart_host` pointer in the ESP-Hosted `g_hosted_osi_funcs` table (the component stays unchanged).
- **Version gate** before `esp_wifi_init` (section above).
- **Start marker in NVS** (`l5_wifi/starting`). If a C6 start ended in a crash (a panic starts the
  app from otadata, and the next RESET would start Wi-Fi again), the next launcher start skips
  auto-connect once and logs `the last ESP32-C6 start did not finish (crash?)`.
- Disabled because the launcher does not use them: the ESP-Hosted CLI console, the C6 memory monitor, iTWT.

**Shutdown at restart.** Every software reset (BOOT and the handover to the app, Restart, scheme
change) runs the shutdown procedure from `l5_wifi.c` (`wifi_off_at_restart`, before the screen is
turned off). The order is:

1. Shutdown flag: from then on there is no "ESP32-C6 stopped answering" message, no reconnect
   attempts and no UI refresh. A scan or connection attempt in progress ends at once (the
   `EV_ABORT` bit).
2. The `l5_wifi` task, the only one that sends requests to the C6, calls `esp_wifi_disconnect()`
   (the access point sees the station leave) and `esp_wifi_stop()`.
3. The same task calls `esp_hosted_deinit()`. The ESP-Hosted RPC and SDIO tasks end
   (read, write, processing) and the SDMMC controller is shut down, so nothing is sent any more.
4. Only then is the C6 power switched off (expander 0x44).

The procedure waits at most 400 ms for steps 2–3. A C6 that does not answer (the RPC timeout in ESP-Hosted
is 5 s) therefore does not block the restart: it is then simply switched off. The log has one line:
`Wi-Fi and ESP-Hosted stopped for the restart in N ms` or
`the ESP32-C6 did not stop within 400 ms: switched off anyway`. Without Wi-Fi (the C6 never
started) the procedure switches the C6 power off right away, as before.

Previously the procedure switched the C6 power off right away while the SDIO write task was still sending
data. The log before `rst:0xc` then showed `sdmmc_io_rw_extended: sdmmc_send_cmd returned 0x107`,
`H_SDIO_DRV: sdio_write_task: … Failed to send data`, `link lost (reason 8)`,
`Unrecoverable host sdio state` and the false message "The ESP32-C6 stopped answering on SDIO".

Result on the device (build 0x1D3BB0, 2026-10-18): three BOOT handovers with Wi-Fi connected
logged `Wi-Fi and ESP-Hosted stopped for the restart in 36–38 ms`, and Power → Restart
`… in 38 ms` (then `rst:0xc` and the launcher with `reset reason 3`). There was no `sdmmc_io`,
`H_SDIO_DRV`, "Unrecoverable" or "stopped answering" line. After RESET, auto-connect as before, after about 13.5 s.
One cosmetic issue remains: in 2 of 4 restarts `W l5_wifi: link lost (reason 8)` still appears.
The restart still goes through correctly. To be explained later: where this event comes from when
the shutdown flag is already set.

**OFF** (power menu): before the pulses that switch off the board's power, the launcher stops only
Wi-Fi (`l5_wifi_pause()`: disconnect and `esp_wifi_stop()`, also 400 ms at most). The C6 loses power
together with the whole board. ESP-Hosted stays running, because `esp_hosted_deinit()` shuts down the SDMMC
controller shared with the SD card. When power stays on (e.g. from USB) and "Power-off failed" appears,
`l5_wifi_resume()` starts Wi-Fi again and reconnects if it was connected.

Every app therefore starts with the C6 as after power-on; apps with Wi-Fi turn it on themselves,
just as on a cold boot.

### Networks, auto-connect, UI

Port of `L5.connect`, `L5.pickWifi` and `L5.wifiMenu` (app.js) in `main/ui/l5_wifi_ui.c`:

- **Top bar icon:** crossed out (`i-wifi-off`) when there is no connection; grayed out without the slash
  while the C6 starts and while connecting; in the accent color once connected (`.tb-btn.on`). A tap opens
  the Wi-Fi menu.
- **Wi-Fi menu** (top bar and CFG > Wi-Fi networks): title "Connected: SSID (IP)" or "Not connected",
  items Scan and connect or Disconnect, Saved networks (N) (list of saved networks where single ones can be
  deleted; an addition over the prototype) and Forget saved networks (with confirmation).
- **Scan:** progress dialog ("Starting the ESP32-C6…", "ESP32-C6 scan"), then the list
  "SSID · -46 dBm · WPA2 · saved" (a padlock next to networks with a password): strongest first, one entry
  per SSID, no hidden networks, up to 16 entries. The C6 sees only 2.4 GHz networks.
- **Password** is entered on the on-screen keyboard ("PASSWORD · SSID", masked) when the network has a
  password and is not saved. A WPA password shorter than 8 characters is rejected right away.
- **Connecting:** dialog "Connecting to SSID" with the stages "Associating…" and "Waiting for DHCP…", then
  the toast "Connected to SSID · IP" or the dialog "Wrong password", "Network not found", "No IP address" (20 s
  without DHCP) or "Could not connect". 802.1X (enterprise) networks are not supported.
- **Saved networks:** NVS, namespace `l5_wifi`, key `nets` (blob, up to 8 networks, most recently used
  first). As in the prototype, only networks with a password are saved. A saved password that the access
  point rejects is deleted, so the next time the network is picked the launcher asks for a new one.
- **Auto-connect** (`cfg.wifiAuto` and at least one saved network): in the background at startup, in parallel
  with the boot screen. C6 start, scan and connection to the strongest saved network in range.
  The countdown does not wait for Wi-Fi; if the app starts earlier, the restart interrupts the Wi-Fi start
  (the C6 is then switched off). Without `wifiAuto` or without saved networks the C6 stays off until the
  first scan.
- **Lost connection** (e.g. router restart): 3 attempts every 5 s, then "Not connected".
- **CFG** shows the state in the "Wi-Fi networks" row, About shows the ESP-Hosted version reported by the C6.
  The OTA tab without a connection shows "CONNECT TO WI-FI" and switches to the catalog once Wi-Fi connects.

### Size

| Build | `tab5_launcher.bin` | Free of 2 MB |
|---|---|---|
| before Wi-Fi | 1,588,512 B (0x183D20) | 497 KB |
| + esp_hosted, lwIP, keyboard, Wi-Fi UI | 1,878,656 B (0x1CAA80) | 213 KB |
| + `CONFIG_LWIP_IPV6=n` | 1,850,560 B (0x1C3CC0) | 241 KB |
| + lwIP and esp_hosted compiled with `-Os` | 1,828,912 B (0x1BE830) | 262 KB |
| host esp_hosted 2.12.6 + esp_wifi_remote 1.5.1 instead of 1.4.1 + 0.8.5 | 1,905,408 B (0x1D1300) | 187 KB |
| + ESP-Hosted started from `l5_wifi.c` instead of the constructor, RAM log (tested on the device) | 1,906,304 B (0x1D1680) | 186 KB |
| + reason for no auto-connect in the log (tested on the device) | 1,906,336 B (0x1D16A0) | 186 KB (9%) |
| + orderly Wi-Fi shutdown at restart and OFF (not flashed) | 1,909,888 B (0x1D2480) | 183 KB (9%) |
| + temporary touch trace `l5_touch` (tested on the device) | 1,913,200 B (0x1D3170) | 180 KB (9%) |
| + non-clickable `l5_box` containers, rebuild after the finger is lifted (not flashed) | 1,913,296 B (0x1D31D0) | 180 KB (9%) |
| + in-place changes (CFG, HOME, PMAN), settings saved in the background, lazy rebuilds, pressed style (tested on the device) | 1,915,824 B (0x1D3BB0) | 177 KB (9%) |
| + OTA: esp_http_client, esp-tls, mbedTLS client only, 12 certificates, unused LVGL widgets disabled, catalog, installer, AUTO PARTITION | 2,078,464 B (0x1FB700) | 18 KB (1%) |
| + fonts from 24 px up compressed, launcher code with `-Os` (display driver with `-O2`) | 1,842,816 B (0x1C1E80) | 248 KB (12%) |
| + names of the apps to be erased in the confirmation dialogs (tested on the device: download via launcherhub rejected, HTTP 400 without `HWID`) | 1,843,376 B (0x1C20B0) | 248 KB (12%) |
| + files straight from the M5Stack CDN, 15 s to connect with one retry, 30 s for data, error logs with the URL (tested on the device: download and AUTO PARTITION work) | 1,843,680 B (0x1C21E0) | 248 KB (12%) |
| + boot-loop guard (with bootloader 0x5440), warning about apps only for GT911 / ILI9881C (tested on the device 2026-10-19) | 1,846,768 B (0x1C2DF0) | 245 KB (12%) |
| + SD file manager (COPY / PASTE, RENAME, NEW FOLDER, DELETE, VIEW), `l5_latin_15` and 3 icons (about 12 KB), visible toasts (not flashed) | 1,879,296 B (0x1CAD00) | 213 KB (10%) |
| + per-call font RLE state (LVGL), Avoid SPIFFS, merged images from 0x0, new top bar (not flashed) | 1,882,896 B (0x1CBB10) | 209 KB (10%) |
| + screen off after inactivity (`screenOff`, row in CFG, drawing paused while dark, dialogs wake the screen) (not flashed) | 1,883,696 B (0x1CBE30) | 208 KB (10%) |
| + large HOME card for a single slot, 420 px cover with `l5_orb_xb54` (about 7 KB), fixes to the HOME grid and cover text (flashed 2026-10-19) | 1,894,528 B (0x1CE880) | 197 KB (10%) |
| + guard: LP watchdog right after start → app, loops through the launcher (`l5_guard.h`, bootloader 0x5730) (not flashed) | 1,894,800 B (0x1CE990) | 197 KB (10%) |
| + 72 px top bar (47 px keys, touch area up to the bar edge, 52 px buttons), 632 px panels (flashed 2026-10-19 with bootloader 0x5730) | 1,895,056 B (0x1CEA90) | 197 KB (10%) |
| + Charge Mode with the real level and charging state, tab dividers visible (not flashed) | 1,896,880 B (0x1CF1B0) | 195 KB (10%) |
| + faster HOME (no drawing under the boot screen, built only once, panel without a background under the card), measured on the device (flashed 2026-10-19) | 1,898,768 B (0x1CF910) | 193 KB (9%) |
| + Charge Mode: FULL and USB · NOT CHARGING at near-zero current, two matching readings; HOME measurement only with `L5_HOME_BENCH` (flashed 2026-10-19) | 1,898,288 B (0x1CF730) | 194 KB (9%) |
| + charger enabled at startup (CHG_EN on PI4IOE 0x44 register by register, `l5_pi4io.h`) and again after a failed OFF (flashed 2026-10-19) | 1,899,312 B (0x1CFB30) | 193 KB (9%) |
| + new look (2026-10-26, on top of the restored build of 2026-10-20 at 1,912,880 B): boot screen with the countdown ring, side menu instead of the top bar, HOME carousel (not flashed) | 1,900,016 B (0x1CFDF0) | 192 KB (9%) |

The Wi-Fi stack and OTA fit in the 2 MB partition: the launcher, `L5_POOL_START` and the fixed areas do not
change, and the launcher partition did not have to be enlarged. The largest parts: host esp_hosted 2.12.6
about 144 KB after `-Os` (1.4.1 was about 100 KB before `-Os`; 2.12 supports many more RPC requests), lwIP
about 98 KB before `-Os`, esp_netif + esp_event + esp_wifi_remote about 16 KB. `-Os` is used for lwIP,
esp_hosted, esp_http_client, esp-tls, tcp_transport, http_parser and the mbedTLS libraries (top-level
`CMakeLists.txt`) and, since OTA, for the launcher code (`main/CMakeLists.txt`, except `l5_display.c`); LVGL
and the rest of IDF stay at `-O2`. How OTA was made to fit (measured with `idf.py size-components`):

- TLS: client only, ECDHE key exchange only, three curves, no CCM, PKCS7, CMAC, CRL, CSR, PEM writing
  or error strings, 12 certificates instead of the IDF bundle, `-Os`. The HTTPS stack is about 125 KB (mbedcrypto
  72 KB, mbedtls 25 KB, mbedx509 7 KB, esp_http_client, esp-tls, tcp_transport; cJSON was already there).
- LVGL: widgets the launcher does not create are disabled (the default theme linked all of them): 290 → 218 KB.
- Fonts from 24 px up (titles, logo, large icons) compressed by lv_font_conv (RLE with prefilter,
  lossless, `CONFIG_LV_USE_FONT_COMPRESSED`): launcher `.rodata` 571 → 357 KB. The smaller faces used to draw
  lists and labels stayed uncompressed.
- Launcher code with `-Os`: `.text` 159 → 136 KB.

After OTA 248 KB were left, after the SD file manager (code about 20 KB, font and icons about 12 KB) 213 KB,
after the font fix, Avoid SPIFFS, merged images from 0x0 and the new bar 209 KB, after screen off 208 KB, after
the large HOME card 197 KB. The next reserves are compressing the remaining fonts (about 30 KB) or `-Os` for
LVGL (about 40–60 KB, at the cost of drawing speed).

**Will WUI and USB MSC still fit** (so that the launcher partition does not have to be enlarged later, which
would mean a second migration with apps being erased):

| Part | Size | Source |
|---|---|---|
| HTTP server (`esp_http_server`: start, URI handlers, chunked responses, receiving, query parameters) | 19 KB | measured 2026-10-18 on a copy of the project (the component itself 12.1 KB, the rest is socket and newlib functions) |
| WUI handling (file manager, upload and download, partitions, install via `l5_install_fw()`, settings) | 20–35 KB | estimate, code with `-Os` |
| WUI page (HTML, JS, CSS, gzip) | 15–30 KB | estimate; 0 KB if the page lives on the SD card |
| QR code with the address (`lv_qrcode` + qrcodegen) | about 8 KB | estimate |
| AP mode and DHCP server | 3–8 KB | estimate |
| USB MSC (esp_tinyusb, TinyUSB device, MSC class, SD card as a drive) | 30–45 KB | estimate; components from the Espressif registry, they have to be downloaded to measure them |
| **total** | **about 95–145 KB** | |

With 197 KB free, about 52–102 KB remain after WUI and USB MSC, and the fonts and LVGL are another 70–90 KB of reserve.
The 2 MB partition should therefore be enough, and no second migration needs to be planned.

## Configuration `/config.conf` (SD card)

A JSON object with the keys of `cfg` from app.js (example: `sdcard/config.conf`). A missing file means
default values. Saving (`l5_config_save`) keeps keys the firmware does not know yet
(e.g. `wuiUser`, `custom`), and OTA favorites save only the `favorite` key. Both save paths read the file,
change their own keys and write it back under a shared lock. DELETE on `/config.conf` in the SD manager restores
the default settings at once (the scheme stays), and a new file is created on the next change.

| Key | Default | Meaning |
|---|---|---|
| `bright` | 70 | backlight %, step 5 |
| `dim` | 30 | dimming after N s of inactivity (0, 10, 20, 30, 60, 120, 300) |
| `screenOff` | 120 | backlight off after N s without touch, 0 = never (0, 60, 120, 300, 600, 1800; other numbers round down to a step); also works with `dim` 0. File without this key: 120 |
| `color` | `"red"` | accent: red, orange, amber, green, cyan, blue, violet, magenta |
| `rot` | 0 | 0 = landscape, 180 = landscape upside down |
| `askSpiffs` | true | true: firmware with data images asks APP ONLY / APP + DATA (AUTO PARTITION writes them without asking); false ("Avoid SPIFFS"): the app only, data partitions unchanged, also with AUTO PARTITION |
| `onlyBins` | true | the SD manager shows only .bin |
| `bootTime` | 3 | boot screen countdown, 1–5 s |
| `wifiAuto` | true | connect to saved networks at startup |
| `deepSleepStart` | true | wake-up from deep sleep opens the launcher |
| `btnOnlyStart` | false | launcher only when the screen is touched at startup |
| `scheme` | `"default"` | partition scheme ID from data.js (`custom` after AUTO PARTITION) |
| `favorite` | none | OTA favorites as in Launcher: `[{"name", "fid", "link"}]`, a custom link has an empty `fid` |

## Structure

| File | Counterpart in the prototype |
|---|---|
| `main/main.c` | `L5.start()` |
| `main/l5_config.c` | `cfg`, `defaults()`, `L5.save()` |
| `main/l5_slots.c` | `S.slots`, `S.boot`, `L5.syncSlots`, `L5.table`, `L5.bootApp` (otadata), `L5.eraseSlot`, app registry in NVS, `L5.binInfo` (.bin header), `L5.bestScheme` |
| `main/l5_net.c` | HTTPS client for OTA (redirects, Range, cancel, error description) |
| `main/l5_fw.c` | firmware from the catalog or from a file: manifest, table from the merged image, comparison with the table, AUTO PARTITION, image length on the fly |
| `main/l5_inst.c` | image writing from `L5.install`: SD and HTTPS streamed into the slot, data images, verification, registry |
| `main/ui/l5_inst_ui.c` | `L5.install` / `L5.tooBig`: check dialogs, AUTO PARTITION, slot, APP ONLY / APP + DATA, progress, RETRY |
| `main/ui/l5_ota.c` | `L5.views.ota` (`#otaMain`, `#otaSide`), `otaAction` |
| `main/ui/l5_widgets.c` | shared elements of the two panels from launcher.css (`.chip`, `.kv`, `.side-head`, `.empty`) |
| `main/l5_files.c` | card file operations for `sdAction`: names, `uniq()`, listing through FatFs, counting, copying, deleting, text for VIEW |
| `main/ui/l5_sd_ops.c` | `sdAction` (copy, paste, rename, mkdir, delete, view), `R.sd.clip`, background task with progress and CANCEL |
| `certs/ota_roots.pem` | root certificates for HTTPS (not in the prototype) |
| `tools/host_test` | test on the PC: partition table, AUTO PARTITION, installer, file operations, font decompression in two threads, dimming and screen off (`ui/shots.c --idle`), the Tab5 Keyboard's keys (`ui/shots.c --keys`), boot-loop guard (`test_guard.c`), charger pins on an expander model (`test_charger.c`); `ui_shots.py`: screenshots of the SD and HOME views on the real launcher screen; `lvgl_host.py`: LVGL with the firmware options on the PC (not in the prototype) |
| `tools/patch_lvgl_rle.py` | copy of `lv_font_fmt_txt.c` from LVGL 9.3 with per-call RLE state (the "Fonts" section) |
| `main/l5_board.c` | display start, backlight, SD, RTC RX8130, INA226 battery (voltage, pack current and state: charging / battery / USB not charging), charger (CHG_EN), power-off, ESP32-C6 power |
| `main/l5_pi4io.h` | registers of the second expander (PI4IOE5V6408 0x44) and the charger enable steps, shared by the firmware and the PC test |
| `main/l5_kbd.c` | M5Stack Tab5 Keyboard on the side port (not in the prototype, which has `keydown`): own I2C bus, HID events turned into keys with auto-repeat |
| `main/ui/l5_keys.c` | the keys in the UI: waking, tab switching, the topmost dialog / menu / text field, the system screen or the visible tab (not in the prototype) |
| `main/ui/l5_dither.c` | ordered dithering of the vertical gradients for the RGB565 panel: strips cached in PSRAM (not in the prototype) |
| `main/l5_wifi.c` | `R.wifi`, `S.savedWifi`, Wi-Fi in `L5.start`: ESP32-C6 over ESP-Hosted, scan, connecting, saved networks in NVS |
| `main/l5_display.c` | panel + touch, LVGL → PPA → hidden DPI buffer, whole-frame publishing (from `..\aurora-player\components\tab5_hal\tab5_display.c`) |
| `main/ui/l5_theme.c` | `:root` tokens and classes from launcher.css (`.plate`, `.dark`, `.act`, `.slot`, `.chamfer`…) |
| `main/ui/l5_rail.c` | the side menu (112 px, since 26.10.2026; it replaced the 72 px top bar `l5_topbar.c`): logo, five 112 x 88 px tabs without gaps, clock, battery, Wi-Fi and power |
| `main/ui/l5_home.c` | HOME as a carousel (since 26.10.2026; a grid and `hero()` before): one 440 x 490 px card per slot in a strip that scrolls sideways and snaps a card to the middle (`LV_SCROLL_SNAP_CENTER`, `LV_OBJ_FLAG_SCROLL_ONE`); the cards beside it get a black shade (opa 150) instead of a transform, the boot app an accent frame instead of a glow (both cheap to redraw while the strip scrolls) |
| `main/ui/l5_boot.c` | the boot screen (since 26.10.2026; before in `l5_sys.c`): the ring is drawn with `lv_draw_arc` in a draw callback (the arc widget is off), and each 40 ms step invalidates only the box between the old and the new end of the arc |
| `main/ui/l5_sys.c` | `L5.bootScreen`, `L5.bootApp`, `L5.restart`, `L5.powerMenu`, `L5.powerOff` |
| `main/ui/l5_charge.c` | `L5.chargeMode` (`.charge`): battery with the real level, status line (CHARGING / ON BATTERY / FULL / USB · NOT CHARGING), refresh every 5 s |
| `main/ui/l5_dialogs.c` | `U.modal` (`l5_ask`), `U.menu`, `U.confirm`, `U.info`, `U.progress`, `U.toast` |
| `main/ui/l5_keyboard.c` | `U.keyboard` |
| `main/ui/l5_wifi_ui.c` | `L5.wifiMenu`, `L5.pickWifi`, `L5.connect` |
| `main/ui/l5_cover.c` | `U.cover()` (the same FNV-1a seed and mulberry32 generator) |
| `main/ui/l5_sd.c` | `L5.views.sd` (`#sdMain`, `#sdSide`), the `L5.install` / `L5.tooBig` interface (file actions: `l5_sd_ops.c`) |
| `main/l5_ptable.c` | `L5.table`, `L5.applyScheme` (writes the table at 0x8000), partition format / backup / restore, SANITY CHECK of a .bin file |
| `main/ui/l5_pman.c` | `L5.views.pman` + `pmAction`, `L5.pickScheme` |
| `main/ui/l5_cfg.c` | `L5.views.cfg` + `cfgAction` |
| `main/ui/l5_views.c` | tab switching |
| `bootloader_components/main` | "launcher after reset" logic; `l5_guard.h`: boot-loop guard, shared with the launcher and the test (not in the prototype) |
| `tools/gen_fonts.py`, `tools/fonts/*.ttf` | fonts from `index.html` (Google Fonts) |

## Fonts

`main/fonts/*.c` were generated with **lv_font_conv 1.5.3** (4 bpp) from the TTF files in `tools/fonts`
(Google Fonts, SIL OFL 1.1 license). Faces from 24 px up are compressed (lossless, RLE with prefilter,
`CONFIG_LV_USE_FONT_COMPRESSED=y`), smaller ones are not:

- Chakra Petch SemiBold 16/17/19, Regular 17, Bold 24/26/28/40 (`--f-ui`); from 17 px with Latin-1
  and Latin Extended-A, so with Polish letters in app names,
- Orbitron Bold 12/16/22, ExtraBold 27 and 54 (54 only 0–9 and A–Z, the initials on the large HOME cover), Black 76
  (`--f-display`; the face has no "·"),
- Share Tech Mono 11/12/13/14/15/16/18/22 (`--f-mono`); 13–15 px have the fallback `l5_latin_15`
  (Latin-1 from Share Tech Mono, the rest of Latin Extended-A from Chakra Petch Regular 15), 16 and 18 px have
  Chakra Petch Regular 17, so file names, paths and text in VIEW show Polish letters (the "SD file manager" section),
- icons: FontAwesome 5 from the LVGL component in place of the SVG sprite (`main/ui/l5_icons.h`), 20/22/24/54/64 px;
  heart, star, list, link, sort (OTA chips and rows) and copy, paste and pencil (SD actions)
  exist only at 20 px.

**Garbled letters in large fonts** (fixed since build 0x1CBB10, in 0x1CE880, flashed 2026-10-19). On the device
letters and symbols were sometimes drawn ragged, and correctly after the next tap or redraw. LVGL 9.3
unpacks the glyphs of compressed fonts (all from 24 px up) with one global RLE state
(`src/font/lv_font_fmt_txt.c`: `#define font_rle LV_GLOBAL_DEFAULT()->font_fmt_rle`, `rle_init()` and `rle_next()`
from `decompress()`), and since b1d3fe9 the launcher draws with two draw units (`CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2`).
Two threads unpacking at the same time shifted each other's position in the bit stream. Upstream (master branch,
the file moved to `src/font/fmt_txt/`) has the same global state (checked 2026-10-19), so there was nothing to
backport. The fix: `tools/patch_lvgl_rle.py` writes to `build/l5_components/lvgl/` a copy of that one file
with the state on the `decompress()` stack (passed to `rle_init()` / `rle_next()`), and the top-level `CMakeLists.txt`
replaces the original in the lvgl component with it (just as it drops one ESP-Hosted file); `managed_components`
and ESP-IDF stay unchanged. If LVGL changes and the snippet is not found, the script stops the build with a message.
The two draw units and the compression stay (speed and size). Other code paths for letters in LVGL 9.3 do not share
state between units: the glyph buffer is private to each draw task (`lv_draw_label_iterate_characters`),
glyph lookup and kerning only read, the circle cache has a mutex, the "hint" of a long label is written by
the task of that one label. Test on the PC (`python tools\host_test\run.py --fonts`): 656 glyphs of three
compressed fonts, 40 runs each in two threads, compared with a single thread; with the original file
46,050–50,276 of 52,480 decodes come out garbled, with the copy 0.

Regenerating them (Node.js is needed only then):

```powershell
npm install lv_font_conv@1.5.3        # in any folder outside the project
python tools\gen_fonts.py --converter <folder>\node_modules\lv_font_conv\lv_font_conv.js
python tools\gen_fonts.py --converter … --only l5_icon_20     # tylko wybrane czcionki
```

## What already works (in code)

- Tear-free 1280×720 landscape display (`main/l5_display.c`, ported from Aurora's
  "atomic landscape" path): LVGL renders 1280×160 chunks in PSRAM, PPA rotates each chunk
  into a **hidden** 720×1280 DPI buffer, and the finished frame is published once per LVGL refresh
  and picked up by the panel on VSYNC. Three DPI buffers (`CONFIG_BSP_LCD_DPI_BUFFER_NUMS=3`);
  the buffer that was on screen goes back into use after two VSYNCs, and its stale areas are
  filled in from the latest frame. Before, the BSP had one DPI buffer and every chunk
  (1280×120) went straight into the displayed buffer, so a tab change could be seen chunk by chunk.
  `rot: 180` is a 270° PPA rotation. GT911/ST7123 touch from the BSP (polled every 16 ms), with LVGL rotating
  the coordinates. The backlight turns on only once the panel shows the first full frame.
- Tab change: one LVGL refresh and one published frame. What speeds it up
  (measured on the previous version: 4 chunks, 855 kpx, PPA 54–57 ms, from touch to frame
  HOME/OTA/WUI 170–179 ms, SD 220–270, PMAN 249–291, CFG 262–312):
  - tabs are built once and after that only shown / hidden. A tab that does not exist yet
    is built by the launcher in the background after 2 s without touch, once per boot. A tab left stale by
    a data change is rebuilt only on the next visit (`l5_ui_render_views()`), because building
    in the background blocks touch reading for 50–60 ms. A settings change in CFG and a selection in HOME and PMAN
    do not rebuild the view, they only change their own widgets.
    Tapping the active tab rebuilds it (e.g. reads the SD card again);
  - the refresh starts right after the switch (`lv_timer_ready` on the refresh timer), without
    waiting for the next 16 ms period;
  - the buffer repair skips everything the current refresh will draw anyway (the LVGL area
    list `inv_areas`). Before, on a full view change, PPA copied the whole previous view
    (about 850 kpx) into the new buffer before it was painted over;
  - 1280×160 chunks instead of 1280×480: PPA rotates chunk N while LVGL draws N+1, and once rendering
    ends only a small last chunk is left;
  - LVGL draws with two threads (`CONFIG_LV_OS_FREERTOS`, `CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2`,
    synchronized with semaphores, because `esp_lvgl_port` wakes its task with notifications).
  - Not possible: 400 MHz in IDF 5.5.2 needs an ESP32-P4 of revision v3.0 or later (the Tab5 has v1.x,
    `ESP32P4_SELECTS_REV_LESS_V3`), so it stays at 360 MHz. The PPA burst is already at its maximum (128 B).
    SIMD from `esp_lvgl_port` is only for esp32/esp32s3 and LVGL 9.1, and LVGL 9.3 has no
    PPA draw unit.
- **No flicker during flash writes and on restart:**
  - The DMA that sends the frame to the panel is restarted after every frame by the DW_GDMA interrupt
    (`mipi_dsi_dma_trans_done_cb` in IDF). While flash is being erased or written, the cache is
    disabled and normal interrupts wait. The image then stopped flowing and the panel flickered
    during installs from SD and partition table changes. `CONFIG_LCD_DSI_ISR_CACHE_SAFE=y`
    moves this interrupt (and the launcher's VSYNC callback, `on_refresh`) into IRAM, so it also works
    with the cache disabled.
  - Before every `esp_restart()` (BOOT, countdown, Restart, scheme change) the shutdown
    routine in `l5_display.c` turns off the backlight and sends DISPOFF to the panel. Without this, until
    the next app initialized the screen, the panel showed noise with no DSI signal. The next
    app turns the panel back on by itself (SLPOUT + DISPON in its init sequence).
- Layout containers (`l5_box`) are not clickable, so a tap on a label or a column goes to the
  clickable row, card or button around it. The `LV_OBJ_FLAG_CLICKABLE` flag is added explicitly only
  to objects that handle or block taps, and to scrolling lists.
- One set of styles from the CSS tokens. Cut corners (`clip-path`) are drawn in with triangles,
  and the dashed border of an empty slot with lines.
- Top bar (since build 0x1CBB10, as in the prototype from 2026-10-19): one plate without cut wedges
  (gradient plate-hi → plate → #d6d8dc, white line at the top), logo with a screw and the version with a left border, two
  vertical seams, a recessed groove with six tab keys (the active one raised, icon and text in the accent
  color, dividers hidden next to it), a dark seam at the bottom with a glowing strip under the active tab (one
  object moved on a tab change), Wi-Fi icon with the connection state and the Wi-Fi menu, clock from the RTC,
  battery from the INA226, power menu (APP, Restart, Charge Mode, OFF). Only rectangles and two-stop
  gradients (`l5_skew_edge()` removed); the bar's downward shadow is left out. A tab change redraws two
  keys, the dividers next to them and the strip.
- **72 px bar** (since build 0x1CEA90, flashed 2026-10-19: 22 taps on the bar hit their targets; before, the tabs
  were hard to hit on the Tab5): seam y 69–72, screw
  (16, 26), vertical seams 2 × 42 px, groove at y 8 and 53 px high, keys 47 × 76.5 px (previously 31 px: LVGL
  counts the groove border into the spacing, so the padding is 2 px + 1 px of border), dividers from 12 px to h − 12, Wi-Fi
  and power buttons 52 × 52 px, separators by the clock 1 × 36, gap in the status group 8 px (the date always on one
  line). Views start at y = 72 (648 px), panels have y 8 and 632 px (`L5_PANEL_Y`, `L5_PANEL_H`).
  The white line at the top of the bar is a rectangle: the bar's top border shifted everything in it 1 px down and the seam
  was 2 px instead of 3 (also in the 58 px bar).
- **Bar touch areas:** a tab key takes taps from the full height of the bar above the seam (y 0–68) and from
  its half of the 3-pixel gap to its neighbor, the outermost keys another 6 px outward (`tab_hit_cb`,
  `LV_OBJ_FLAG_ADV_HITTEST`: `ext_click_area` alone would give the left key's edge to its right-hand neighbor, because LVGL
  checks that one first; the groove has a search margin, otherwise points above it would not reach the keys).
  Wi-Fi and power: 6 px around (`lv_obj_set_ext_click_area`). The look is unchanged. Test on the PC
  (`python tools\host_test\run.py --idle`, the `shots --taps` part): center, edges and gaps of every key,
  the top of the bar and the row above the seam, the seam (nothing), 6/7 px past the outermost keys, 6/7 px around Wi-Fi and power.
- **Tab dividers** (fixed in 0x1CF1B0): the lines between inactive keys had not been drawn since the bar was
  rebuilt (0x1CBB10). They are children of the keys, 2 px to their left, and LVGL 9.3 clips a protruding child to the parent's "ext draw
  size", which is 0 without the active key's shadow. The key now has an ext draw of 2 px.
- **Charge Mode** (since build 0x1CF1B0, on the device since 0x1CF910; port of `L5.chargeMode` and `.charge`,
  `main/ui/l5_charge.c`): the battery fill is as wide as the real percentage (`l5_board_battery_percent()`)
  and does not move; color green from 20%, amber at 10–19%, accent below 10%. Without a pack (power from USB),
  "USB" instead of the percentage and no fill. Below the percentage (Orbitron 54, "%" added to `l5_orb_xb54`) a status
  line based on the pack current (since build 0x1CF730; CHARGING, FULL and ON BATTERY tested on the device):
  - over 20 mA into the pack: lightning bolt and "CHARGING" in green, the end of the fill pulses (white 18 px bar,
    opacity 0 → 60% → 0 in 1.6 s);
  - over 20 mA out of the pack: "ON BATTERY" in gray, no pulse;
  - up to 20 mA in either direction (power from USB, the charger is not charging): from 95% a check mark and "FULL" in green, below
    95% a USB icon and "USB · NOT CHARGING" in amber, no pulse;
  - no reading: no such line.

  Percentage and state every 5 s (an `lv_timer` deleted together with the screen), "CHARGE MODE · TAP TO EXIT" at the bottom, a tap
  goes back to the launcher. The battery has a 16 × 50 px tip as in the CSS. In 0x1CF910 a full pack on USB (99%, −1 mA)
  showed "ON BATTERY": hence the FULL and USB · NOT CHARGING states.
- **Pack current** (`l5_board_battery_state()`): 5 mΩ shunt, INA226 0x41, register 0x01 (2.5 µV per unit,
  i.e. 0.5 mA), negative for current into the pack, so the launcher computes `mA = −raw / 2` (positive into the pack). The sign
  matches Aurora Player (`tab5_hal`): in the same log from 2026-10-19, while charging, `current=+485mA
  charging=1`. Threshold 20 mA; so that the label does not jump at the threshold, a new state needs two matching readings in a row (every
  5 s, so a change shows after about 10 s at the latest). These switch at once: over 100 mA in either direction (plugging
  in or pulling out USB) and the first reading after Charge Mode opens (the state from the previous visit can be hours old).
  The logic is in `l5_charge_next()` (`l5_board.h`); the PC test `run.py --idle` checks 18 readings, including
  that a reading jumping 19 / 21 mA does not switch the state. The log gets the first reading at each opening and each
  change, with the current: `l5_board: battery: N mA into the pack: charging` / `on battery` / `external power, not
  charging`, and a missing reading as `battery: no pack or no current reading`. The CHG_STAT pin on the expander is not used:
  Aurora saw it high at −202 mA after USB was unplugged.
- **Charger** (since build 0x1CFB30, flashed 2026-10-19 and tested; `l5_board_charger_on()`, steps
  in `main/l5_pi4io.h`): at boot,
  even before the screen (about 1.5 s after power-on; after the screen start it would be about 3.8 s), the launcher sets, on the
  PI4IOE5V6408 0x44 expander, pin 7 CHG_EN high and pin 6 CHG_STAT as an input without pull-up, just like Aurora
  (`tab5_charger_init()`, "Same CHG_EN=HIGH as M5Unified"). Before, the launcher did not do this and the charger did
  not charge under it: on 2026-10-19 Aurora charged at +411…+485 mA, and 14 s after RESET into the launcher the pack current
  was −1 mA, also after unplugging and replugging USB twice. Writes go register by register (read, change the bit, write), as
  in `l5_board_c6_power()`: no expander reset, so WLAN_PWR_EN, USB 5 V and the power-off pin stay as they are. CHG_EN gets
  its level first, then the push-pull output, and the direction last, so it is never low along the way; CHG_STAT
  becomes an input first. Test on the PC (`run.py --charger`, part of the full `run.py`): 1792 start states
  of both pins with various states of the remaining ones (after power-on, after a BSP driver reset, in use, patterns):
  the other pins do not move, CHG_EN is never driven low, CHG_STAT never starts being driven.
  When the I2C bus does not respond at boot, the launcher retries the write after the screen starts (which unblocks the bus).
  After that nothing turns CHG_EN off: handing over to an app and restarts do not touch the expander (`l5_board_c6_power()`
  changes only pin 0). OFF opens the expander driver from the BSP, which resets the whole chip, so when power stays on
  (USB), the launcher turns the charger back on. Boot log: `l5_board: charger: CHG_EN high (CHG_STAT n); it was
  high|off: output|input, high-Z n, level n`. The second part tells how the launcher found the pin: after RESET from Aurora, "was high"
  means the expander kept it, and "was off" that it lost it. On the device, after RESET from Aurora (which was charging at +400 mA),
  the result was `it was off: input, high-Z 1, level 0`, i.e. the power-on state: RESET clears the settings of the
  0x44 expander. Charge Mode 16.8 s after boot: `battery: 371 mA into the pack: charging`, CHARGING on the screen.
- HOME: "APPS IN FLASH" panel with a card for every `ota_N` slot from the real partition table
  (cover, name from the NVS registry, version, source and date, slot usage), BOOTS marker, card menu (Boot now,
  Boot this app after the bootscreen, Erase appN), footer with the BOOT button. With one slot, one wide
  card with BOOT and ERASE (since build 0x1CE880, flashed 2026-10-19; the "HOME" section). The scheme is
  recognized by the partition sizes (Default, 4 Apps, Min SPIFFS, UiFlow2, Doom, Max App, Custom).
- Boot screen with countdown, ENTER LAUNCHER, FAST BOOT APP, automatic entry after 1.5 s
  with no apps, `btnOnlyStart`.
- Dimming after `dim` seconds and backlight off after `screenOff` seconds without touch (the first
  touch only wakes the screen; screen off since build 0x1CBE30, on the device since 0x1CE880; the "Screen dimming and screen
  off" section).
- SD: folder browsing, Only Bins filter, file and image details, installing a .bin into a slot
  with slot choice, size check, verification and the app registry in NVS (the
  "Installing from the SD card" section). "App slot too small" has a USE ‹scheme› button. COPY / PASTE, RENAME,
  NEW FOLDER, DELETE and VIEW (the "SD file manager" section; DELETE tested on the device).
- PMAN and CFG: the "PMAN: partitions and scheme changes" and "CFG" sections.
- On-screen keyboard and Wi-Fi: the "On-screen keyboard" and "Wi-Fi (ESP32-C6 over esp_hosted)" sections.
- OTA (tested on the device 2026-10-18/19): launcherhub catalog, search, versions, favorites,
  custom links, TO SD, UPDATE ALL, install over HTTPS into a slot, merged images, AUTO PARTITION
  (the "OTA: online installer" section).
- Placeholder: WUI.

## Still to port from the prototype

- **SD** (views.js `renderSD`, the rest): USB MASS STORAGE button in the panel footer, cover number counted over
  all .bin files on the card (today: in the current folder). Cut (move) is in neither the prototype
  nor the brief.
- **Install** (`L5.install`, `L5.tooBig`), the rest: resuming the install after a scheme change from the
  "App slot too small" dialog (after the restart the file has to be picked again; AUTO PARTITION from the check dialog
  installs without a restart in between). Installing from WUI will use the same `l5_install_fw()`.
- **OTA**, the rest: TO SD for firmware with separate data files (`install.sources`) saves only the main
  file; a network list in the OTA panel itself (today the CONNECT button opens the scan dialog).
- **Wi-Fi**, the rest: a badge (`.badge`) by the icon while WUI is running, AP mode for WUI, an internal / external
  antenna switch (MMCX), C6 firmware update (then also a newer host version).
- **WUI** (views2.js `renderWUI`): web server, STA/AP mode, login, QR code, request log.
- **PMAN**, the rest: OPTIMIZE FLASH that moves the app images (today it erases the apps
  and changes the size, with a warning), SPIFFS/FAT usage percentage (today "EMPTY" / "IN USE"
  based on the first sector), tables outside the `partitions.csv` rules (PMAN shows them but does not change them).
- **CFG**, the rest: USB Mass Storage (placeholder).
- USB Mass Storage mode (`L5.usbMode`).
- The boot screen's radial gradient, double shadows and inner glows from the CSS
  (LVGL has one shadow per object).
