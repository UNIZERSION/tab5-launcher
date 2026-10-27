/* Launcher UI (LVGL 9): screen layout, views, dialogs and system screens.
   Every function here must be called with the LVGL lock held (LVGL task or l5_board_lock). */
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"
#include "l5_fw.h"
#include "l5_kbd.h"
#include "l5_theme.h"

typedef enum {
    L5_VIEW_HOME, L5_VIEW_SD, L5_VIEW_OTA, L5_VIEW_PMAN, L5_VIEW_CFG, L5_VIEW_COUNT,
} l5_view_t;

/* ---------- screen (l5_ui.c) ---------- */
void l5_ui_init(void);
#define L5_VIEW_BIT(v) (1u << (v))
#define L5_VIEWS_ALL ((1u << L5_VIEW_COUNT) - 1)

void l5_ui_go(l5_view_t view);   /* L5.go: a tab tap (tapping the active tab rebuilds it), drawn at once */
void l5_ui_show(l5_view_t view); /* back from a system screen: a built view is only shown */
void l5_ui_prepare(l5_view_t view); /* at start: built and selected, drawn with whatever frame comes next */
void l5_ui_covered(bool covered); /* a full-screen system screen is up: the launcher underneath is not drawn */
/* CFG > Theme: the colours change without a restart, right after the current event: shared styles restyle in place,
   the rail and the visible view are built again, the other views when they are shown next. */
void l5_ui_retheme(int theme);
/* Data changed: the visible view, if it is in `views`, is rebuilt now (after the release while a
   finger is down); the other views in `views` are rebuilt when they are shown next. Views that only
   change a value update their widgets in place instead (CFG, HOME and PMAN selection). */
void l5_ui_render_views(uint32_t views);
void l5_ui_render_views_async(uint32_t views); /* same, after the current event finished */
void l5_ui_render(void);         /* L5.render: all views */
void l5_ui_render_async(void);   /* same, after the current event finished */
l5_view_t l5_ui_view(void);
lv_obj_t *l5_ui_layer(void);     /* #layer: dialogs */
lv_obj_t *l5_ui_sys(void);       /* #sys: boot / app / off / charge screens */
lv_obj_t *l5_ui_toasts(void);    /* .toast container, above #sys */
void l5_ui_set_booted(bool booted); /* R.booted: idle dimming and screen off only run inside the launcher */
void l5_ui_wake(void);           /* undo idle dimming / screen off (dialogs call it when they open) */
bool l5_ui_asleep(void);         /* dimmed or dark: the next tap or key only wakes the screen */
bool l5_ui_idle_clock(void);     /* the dimmed screen shows the clock (CFG > Idle clock) */
int l5_ui_dim_level(int bright, bool clock); /* the dimmed backlight %: with the clock a quarter, at least 5 */

/* ---------- keyboard keys (l5_keys.c): the M5Stack Tab5 Keyboard in the whole UI ---------- */
void l5_keys_init(void);         /* after l5_ui_init: a 20 ms timer takes the keys and sends them on */
uint32_t l5_key_event(void);     /* LVGL event code of a key sent to a dialog; lv_event_get_param() is the l5_key_t */
void l5_keys_help(void);         /* F1 / ?: the shortcuts of the visible tab, text fields and the bootscreen */
/* the visible tab's keys; true when the key did something */
bool l5_home_key(const l5_key_t *k);
bool l5_sd_key(const l5_key_t *k);
bool l5_ota_key(const l5_key_t *k);
bool l5_pman_key(const l5_key_t *k);
bool l5_cfg_key(const l5_key_t *k);
bool l5_sys_key(const l5_key_t *k); /* a system screen is up (l5_sys.c) */

/* ---------- the rail of tabs down the left edge (l5_rail.c; the top bar until 26.10.2026) ---------- */
void l5_rail_create(lv_obj_t *screen);
void l5_rail_select(l5_view_t view);
void l5_rail_paint_status(void); /* L5.paintStatus + clock tick */
void l5_rail_paint_wifi(void);   /* Wi-Fi icon: wifi-off / connecting / .tb-btn.on */
void l5_rail_apply_accent(void); /* after l5_theme_set_accent */
void l5_rail_rebuild(void);      /* after l5_theme_set: built again in the new colours */
int l5_rail_battery(void);       /* the battery percent it shows (read every 30 s), -1 when unknown */
/* for the rail, the bootscreen and the idle clock: the local time (false while the RTC is not set, before 2024),
   the days "SUN".."SAT", "BATTERY 76%" or "USB" from the rail's reading */
struct tm;
bool l5_clock_now(struct tm *t);
extern const char *const l5_days[7];
void l5_battery_text(char *out, size_t len);

/* ---------- on-screen keyboard (l5_keyboard.c), port of U.keyboard ---------- */
typedef void (*l5_keyboard_cb_t)(const char *text, void *user); /* text == NULL: CANCEL */
typedef struct {
    const char *title; /* .kb-top .q, "INPUT" when NULL */
    const char *value; /* initial text */
    bool mask;         /* bullets instead of the text, SHOW / HIDE key */
    uint16_t max_len;  /* characters, 64 when 0 */
} l5_keyboard_opts_t;
/* Opens over everything in #layer and returns at once. The callback gets the text (valid during
   the call) after the keyboard is gone, so it may open the next dialog. */
void l5_keyboard(const l5_keyboard_opts_t *opts, l5_keyboard_cb_t cb, void *user);

/* ---------- Wi-Fi dialogs (l5_wifi_ui.c), L5.wifiMenu / L5.pickWifi / L5.connect ---------- */
void l5_wifi_ui_init(void);  /* status listener for the top bar, CFG and OTA */
void l5_wifi_menu(void);     /* top bar icon, CFG > Wi-Fi networks */
void l5_wifi_pick(void);     /* scan, pick, password on the keyboard, connect */
void l5_wifi_why(void);      /* Wi-Fi FAILED: why (another ESP-Hosted on the C6: what to flash) */
const char *l5_wifi_summary(char *buf, size_t len); /* "Connected: X" / "N saved" */

/* ---------- views ---------- */
void l5_home_render(lv_obj_t *view);                  /* l5_home.c */
void l5_home_keep_place(void); /* the next HOME build keeps the card in the middle (new icons, not new slots) */
void l5_sd_render(lv_obj_t *view);                    /* l5_sd.c: file browser + install */
void l5_cfg_render(lv_obj_t *view);                   /* l5_cfg.c: settings, saved to /config.conf */
void l5_cfg_paint_wifi(void);                         /* CFG > Wi-Fi networks subtitle */
void l5_cfg_paint(void);       /* CFG widgets follow g_cfg in place (values, toggles, swatches) */
void l5_cfg_save(void);        /* /config.conf 300 ms after the last change, in a background task */
esp_err_t l5_cfg_flush(void);  /* write a pending change now (restart, power-off, hand-over) */
esp_err_t l5_cfg_save_now(void); /* write g_cfg now (from any task) */
void l5_cfg_discard_pending(void); /* /config.conf is about to be deleted: a pending change is not written */
/* the launcher's accent: CFG > UI color, with APP the boot app's icon colour (red without one) */
uint32_t l5_cfg_accent_hex(void);
/* with APP: the accent follows the boot app (another app chosen, its icon loaded); every view and the bootscreen
   follow. True when the accent changed (the bootscreen was repainted). */
bool l5_cfg_accent_follow(void);
void l5_cfg_defaults_now(void);    /* /config.conf was deleted: defaults applied (scheme kept), not saved */
esp_err_t l5_cfg_reload_now(void); /* /config.conf was replaced: its settings apply now (scheme kept); not JSON: kept */
void l5_views_render(l5_view_t v, lv_obj_t *view);    /* l5_views.c: routing */

/* ---------- PMAN (l5_pman.c) ---------- */
void l5_pman_render(lv_obj_t *view);
void l5_pman_pick_scheme(void);               /* L5.pickScheme */
void l5_pman_use_scheme(const char *id);      /* id or label from data.js, e.g. "uiflow2" */
void l5_pman_format(const char *label);       /* app slot or data partition, asks first */
void l5_pman_backup(const char *label);       /* data partition -> /sdcard/backups */
void l5_pman_restore(const char *label);      /* pick a backup, asks first */
bool l5_pman_busy(void);                      /* partition table being rewritten */

/* ---------- install flow shared by SD and OTA (l5_inst_ui.c), L5.install / L5.tooBig ---------- */
/* Takes fw (heap_caps_malloc'd, freed when the flow ends). Compares it with the partition table:
   missing or different data partitions get INSTALL ANYWAY / AUTO PARTITION; then the flash slot,
   APP ONLY / APP + DATA (Ask SPIFFS), the install with progress + CANCEL, RESTART NOW or RETRY.
   name / author go to the registry (empty: from fw), src is "SD", "OTA" or "LINK". */
void l5_install_fw(l5_fw_t *fw, const char *name, const char *author, const char *src);
bool l5_install_busy(void);
void l5_ota_render(lv_obj_t *view);   /* l5_ota.c: online installer */
void l5_ota_paint_wifi(void);         /* OTA follows the Wi-Fi state */

/* ---------- bootscreen (l5_boot.c) ---------- */
void l5_boot_screen(void);   /* L5.bootScreen: the app in a ring that counts down, LAUNCHER, START NOW, other apps */
void l5_boot_screen_stop(void);  /* its countdown stops: another system screen replaces it (l5_sys.c) */
bool l5_boot_screen_key(const l5_key_t *k); /* the bootscreen is up: any key enters the launcher (true) */
void l5_boot_screen_icons(void); /* app icons were loaded (l5_appicon.c): the tile and chips show them */
void l5_enter_launcher(void);    /* from the bootscreen into HOME */

/* ---------- system screens (l5_sys.c) ---------- */
void l5_boot_app(void);      /* L5.bootApp: the user hands over to the selected slot (the boot-loop guard starts over) */
void l5_boot_app_auto(void); /* the bootscreen countdown and btnOnlyStart: the guard keeps counting early returns */
/* Launcher instead of the bootscreen: the app in `slot` was stopped by the boot-loop guard just now after
   `restarts` quick restarts or `returns` early returns to the launcher (both -1: on an earlier start).
   BOOT AGAIN / ERASE APP / OK. */
void l5_stopped_dialog(int slot, int restarts, int returns);
/* hand over to an app installed into a table written since boot (AUTO PARTITION) */
void l5_boot_at(uint32_t ota_index, uint32_t ota_count, uint32_t addr, uint32_t size, const char *name);
void l5_restart(void);       /* L5.restart: back to the bootscreen */
void l5_power_menu(void);    /* L5.powerMenu */
void l5_power_off(void);     /* L5.powerOff */
void l5_charge_mode(void);   /* L5.chargeMode: l5_charge.c */
lv_obj_t *l5_sys_show(lv_color_t top, lv_color_t bottom); /* a full-screen system screen on #sys */
void l5_sys_hide(void);

/* ---------- dialogs (l5_dialogs.c) ---------- */
typedef struct {
    const char *id;     /* NULL entries are skipped, like items.filter(Boolean) */
    const char *icon;   /* L5_ICON_*, NULL = chevron */
    const char *label;
    bool danger;
} l5_menu_item_t;
typedef void (*l5_menu_cb_t)(const char *id, void *user);   /* id == NULL: dismissed */
typedef void (*l5_confirm_cb_t)(bool ok, void *user);
typedef void (*l5_choose_cb_t)(int index, void *user);
typedef struct l5_progress l5_progress_t;

void l5_menu(const char *kind, const char *title, const l5_menu_item_t *items, int n, l5_menu_cb_t cb, void *user);
void l5_confirm(const char *kind, const char *title, const char *body, const char *ok, bool danger,
                l5_confirm_cb_t cb, void *user);
void l5_info(const char *kind, const char *title, const char *body);
/* U.modal with two actions and no dismiss: cancel -> cb(false), primary ok -> cb(true) */
void l5_ask(const char *kind, const char *title, const char *body, const char *cancel, const char *ok,
            const char *ok_icon, l5_confirm_cb_t cb, void *user);
/* U.modal with n actions (the last one primary, with icon), no dismiss; long bodies scroll */
void l5_choose(const char *kind, const char *title, const char *body, const char *const *labels, int n,
               const char *icon, l5_choose_cb_t cb, void *user);
/* U.modal kind VIEW: read-only text in the mono font that scrolls, CLOSE (a tap beside it closes too).
   text: `pieces` NUL-terminated label texts one after the other, given to free_fn when it closes. */
void l5_text_dialog(const char *title, const char *note, char *text, uint32_t pieces, void (*free_fn)(void *));
l5_progress_t *l5_progress_open(const char *kind, const char *title);
/* adds CANCEL under the bar: it sets *flag, the worker stops and closes the dialog */
void l5_progress_cancelable(l5_progress_t *p, volatile bool *flag);
void l5_progress_set(l5_progress_t *p, int pct, const char *label);
void l5_progress_close(l5_progress_t *p);
void l5_toast(const char *fmt, ...);
void l5_dialogs_close_all(void);

/* ---------- two-pane view building blocks (l5_widgets.c), launcher.css ---------- */
lv_obj_t *l5w_pane(lv_obj_t *view, bool dark, int32_t x, int32_t w); /* .vw-main dark / .vw-side plate */
lv_obj_t *l5w_line_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text);
void l5w_empty(lv_obj_t *parent, const char *title, const char *body);  /* .empty on a dark pane */
lv_obj_t *l5w_chip(lv_obj_t *parent, const char *icon, const char *text, bool on, lv_event_cb_t cb);
lv_obj_t *l5w_kv(lv_obj_t *grid, const char *k, const char *v, bool accent);
lv_obj_t *l5w_kv_grid(lv_obj_t *side);
void l5w_row_sel_draw(lv_event_t *e);     /* LV_EVENT_DRAW_POST of a row: accent bar while CHECKED */
lv_obj_t *l5w_side_head(lv_obj_t *side);
void l5w_big_icon(lv_obj_t *head, const char *icon);
void l5w_head_text(lv_obj_t *head, const char *kind, const char *title, const char *sub);
/* "Aurora-Player-1.3.21 in app0 will be erased: install it again from SD or OTA afterwards." */
void l5w_apps_erased(char *out, size_t len);

/* ---------- procedural firmware emblem (l5_cover.c), port of U.cover ---------- */
/* project: esp_app_desc_t.project_name (NULL: none); with an icon on the card (l5_appicon.c) the emblem shows it
   instead of the chip, and the first line of about.txt instead of sub */
lv_obj_t *l5_cover_create(lv_obj_t *parent, int32_t size, const char *seed, const char *title, const char *sub,
                          int num, bool text, const char *project);

/* ---------- app icons from the card (l5_appicon.c) ---------- */
/* <card>/apps/<project>/icon.png (PNG up to 1024 x 1024 px and 2 MB, square looks best) and about.txt (its first
   line). A worker loads them; the views are drawn again when something changed. */
#define L5_ABOUT_LEN 64
void l5_appicon_refresh(void); /* every app's files on the card again (HOME and the SD tab's .bin panel call it) */
bool l5_appicon_has(const char *project);  /* its icon is loaded; one never asked for is looked for */
const char *l5_appicon_about(const char *project); /* about.txt's first line, NULL when none */
/* the icon fitted into box x box, corners rounded to radius (LV_RADIUS_CIRCLE: round), blended into an RGB565
   buffer at x, y; false without an icon */
bool l5_appicon_blend(lv_draw_buf_t *dst, const char *project, int32_t x, int32_t y, int32_t box, int32_t radius);
/* the same as a canvas with alpha; NULL without an icon */
lv_obj_t *l5_appicon_create(lv_obj_t *parent, const char *project, int32_t box, int32_t radius);
/* the icon's colours, evened out for the launcher: accent (vivid: ring, buttons, frames) and tint (the hue most of the
   icon has, for backgrounds); false without an icon or when it is grey. Either pointer may be NULL. */
bool l5_appicon_colors(const char *project, lv_color_t *accent, lv_color_t *tint);
/* the boot app's accent (0xRRGGBB): from its icon, before the icon is read the one kept from the last start (NVS),
   else fallback */
uint32_t l5_appicon_boot_accent(uint32_t fallback);
/* the canvas shows buf and destroys it with itself (the emblems, the app icons) */
void l5_canvas_own(lv_obj_t *canvas, lv_draw_buf_t *buf);
/* the emblem's two letters: the first letters of the first two words, upper case ("FW" without any) */
void l5_cover_initials(const char *title, char out[3]);

/* ---------- formatting (ui.js) ---------- */
void l5_fmt_bytes(char *out, size_t len, uint64_t bytes); /* U.fmtBytes */
void l5_fmt_kb(char *out, size_t len, uint32_t kb);       /* U.fmtKB */
