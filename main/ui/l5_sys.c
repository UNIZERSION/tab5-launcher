/* #sys screens of app.js: bootscreen (M5Launcher flow), app hand-over, restart, power. */
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "l5_board.h"
#include "l5_config.h"
#include "l5_display.h"
#include "l5_slots.h"
#include "l5_ui.h"
#include "l5_wifi.h"

static const char *TAG = "l5_sys";

static lv_timer_t *s_boot_timer;
static lv_obj_t *s_cd_bar, *s_cd_num;
static uint32_t s_cd_start;
static bool s_bootscreen; /* the bootscreen is up: any key of the Tab5 Keyboard enters the launcher */

static void stop_boot_timer(void)
{
    if (s_boot_timer) {
        lv_timer_delete(s_boot_timer);
        s_boot_timer = NULL;
    }
}

/* L5.sysShow: clears #layer and fills #sys */
static lv_obj_t *sys_show(lv_color_t top, lv_color_t bottom)
{
    stop_boot_timer();
    s_bootscreen = false;
    l5_dialogs_close_all();
    lv_obj_t *sys = l5_ui_sys();
    lv_obj_clean(sys);
    l5_ui_covered(true);
    lv_obj_t *el = l5_box(sys);
    lv_obj_set_size(el, L5_SCREEN_W, L5_SCREEN_H);
    lv_obj_add_flag(el, LV_OBJ_FLAG_CLICKABLE); /* blocks the launcher underneath */
    lv_obj_set_style_bg_opa(el, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(el, top, 0);
    if (!lv_color_eq(top, bottom)) { /* the bootscreen; the black screens have no gradient */
        lv_obj_set_style_bg_grad_color(el, bottom, 0);
        lv_obj_set_style_bg_grad_dir(el, LV_GRAD_DIR_VER, 0);
        l5_dither_hook(el);
    }
    return el;
}

lv_obj_t *l5_sys_show(lv_color_t top, lv_color_t bottom)
{
    return sys_show(top, bottom);
}

void l5_sys_hide(void)
{
    stop_boot_timer();
    s_bootscreen = false;
    lv_obj_clean(l5_ui_sys());
    l5_ui_covered(false);
    lv_display_trigger_activity(NULL);
}

static lv_obj_t *sys_text_screen(const char *text)
{
    lv_obj_t *el = sys_show(L5_BLACK, L5_BLACK); /* .off */
    lv_obj_t *l = l5_label(el, &l5_mono_15, lv_color_hex(0x3a3c41), text);
    lv_obj_set_style_text_letter_space(l, 3, 0);
    lv_obj_center(l);
    return el;
}

/* ---------- bootscreen ---------- */

static void enter_launcher(void)
{
    l5_sys_hide();
    l5_ui_set_booted(true);
    l5_ui_show(L5_VIEW_HOME); /* built at start: shown, not built a second time */
}

static void enter_cb(lv_event_t *e)
{
    (void)e;
    enter_launcher();
}

static void fast_cb(lv_event_t *e)
{
    (void)e;
    l5_boot_app();
}

static void auto_enter_cb(lv_timer_t *t)
{
    (void)t;
    s_boot_timer = NULL; /* one-shot timers delete themselves */
    enter_launcher();
}

static void countdown_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t dur = g_cfg.bootTime * 1000u;
    uint32_t el = lv_tick_elaps(s_cd_start);
    if (el > dur) {
        el = dur;
    }
    lv_obj_set_width(s_cd_bar, (int32_t)(460 * (dur - el) / dur)); /* scaleX(1 - k) from the left */
    lv_label_set_text_fmt(s_cd_num, "%us", (unsigned)((dur - el + 999) / 1000));
    if (el >= dur) {
        stop_boot_timer();
        l5_boot_app_auto();
    }
}

/* .hatch: accent stripes at 115 degrees, 34 x 14 */
static void hatch_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = L5P.accent;
    d.width = 3;
    for (int32_t x = a.x1 - 8; x < a.x2 + 8; x += 8) {
        d.p1.x = x + 7; d.p1.y = a.y1;
        d.p2.x = x;     d.p2.y = a.y2;
        lv_draw_line(layer, &d);
    }
}

static lv_obj_t *col_box(lv_obj_t *parent)
{
    lv_obj_t *b = l5_box(parent);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    return b;
}

void l5_boot_screen(void)
{
    l5_display_mark("bootscreen"); /* the first frame after start is timed like a tab switch */
    l5_ui_set_booted(false);
    const l5_slot_t *app = l5_slots_boot_app();
    lv_obj_t *el = sys_show(lv_color_hex(0x202227), lv_color_hex(0x0b0c0e)); /* .boot */
    s_bootscreen = true;

    /* .l: logo, tag, app, countdown */
    lv_obj_t *l = col_box(el);
    lv_obj_set_pos(l, 80, 70);
    lv_obj_t *logo = l5_box(l);
    lv_obj_set_size(logo, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(logo, LV_FLEX_FLOW_ROW);
    l5_label(logo, &l5_orb_blk76, L5_WHITE, "LAUN");
    l5_label(logo, &l5_orb_blk76, L5P.accent, "CHER");
    char t[96];
    snprintf(t, sizeof(t), "M5STACK TAB5 \xC2\xB7 v%.16s", esp_app_get_description()->version);
    lv_obj_t *tag = l5_label(l, &l5_mono_18, L5_ON_PANEL_MUTED, t);
    lv_obj_set_style_text_letter_space(tag, 5, 0);
    lv_obj_set_style_pad_top(tag, 16, 0);

    lv_obj_t *ab = col_box(l);
    lv_obj_set_style_pad_top(ab, 70, 0);
    lv_obj_set_style_pad_row(ab, 8, 0);
    /* apps in flash but none chosen (flashed by hand, l5_slots_scan): the launcher opens, as with no app */
    const bool unchosen = !app && l5_slots_used() > 0;
    if (app) {
        snprintf(t, sizeof(t), "APP%d \xC2\xB7 %d OF %d SLOTS USED", g_slots.boot, l5_slots_used(), g_slots.n);
    } else {
        snprintf(t, sizeof(t), unchosen ? "NO APP CHOSEN TO START" : "NO APP INSTALLED");
    }
    lv_obj_t *k = l5_label(ab, &l5_mono_14, L5_ON_PANEL_MUTED, t);
    lv_obj_set_style_text_letter_space(k, 3, 0);
    lv_obj_t *nm = l5_label(ab, &l5_chakra_b40, L5_WHITE,
                            app ? app->name : unchosen ? "Choose one in HOME" : "Install one from SD or OTA");
    lv_obj_set_style_max_width(nm, 560, 0);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);

    if (app) { /* .cd */
        lv_obj_t *cd = col_box(l);
        lv_obj_set_width(cd, 460);
        lv_obj_set_style_pad_top(cd, 30, 0);
        lv_obj_set_style_pad_row(cd, 10, 0);
        lv_obj_t *row = l5_box(cd);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *s = l5_label(row, &l5_mono_16, L5_ON_PANEL_MUTED, "STARTING APP IN");
        lv_obj_set_style_text_letter_space(s, 2, 0);
        snprintf(t, sizeof(t), "%us", g_cfg.bootTime);
        s_cd_num = l5_label(row, &l5_orb_b16, L5_WHITE, t);
        lv_obj_t *track = l5_box(cd);
        lv_obj_set_size(track, 460, 8);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(track, lv_color_hex(0x2b2d32), 0);
        lv_obj_add_flag(track, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        s_cd_bar = l5_box(track);
        lv_obj_set_size(s_cd_bar, 460, 8);
        lv_obj_set_style_bg_opa(s_cd_bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_cd_bar, L5P.accent, 0);
        lv_obj_set_style_shadow_color(s_cd_bar, L5P.accent, 0);
        lv_obj_set_style_shadow_width(s_cd_bar, 12, 0);
        lv_obj_set_style_shadow_opa(s_cd_bar, 178, 0);
    }

    /* .hz */
    lv_obj_t *hz = l5_box(el);
    lv_obj_set_size(hz, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(hz, 16, 0);
    lv_obj_set_flex_flow(hz, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hz, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(hz, LV_ALIGN_BOTTOM_LEFT, 80, -50);
    lv_obj_t *hatch = l5_box(hz);
    lv_obj_set_size(hatch, 34, 14);
    lv_obj_add_event_cb(hatch, hatch_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_t *hl = l5_label(hz, &l5_mono_13, L5_MUTED, "TAP THE BUTTON TO ENTER LAUNCHER");
    lv_obj_set_style_text_letter_space(hl, 2, 0);

    /* .r: ENTER LAUNCHER disc (box-shadow rings: 10px #18191c, 3px accent, glow) + FAST BOOT */
    lv_obj_t *r = l5_box(el);
    lv_obj_set_pos(r, 640, 0);
    lv_obj_set_size(r, 640, L5_SCREEN_H);
    lv_obj_set_style_pad_row(r, 24, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *ring = l5_box(r);
    lv_obj_set_size(ring, 326, 326);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ring, L5P.accent, 0);
    lv_obj_set_style_shadow_color(ring, L5P.accent, 0);
    lv_obj_set_style_shadow_width(ring, 60, 0);
    lv_obj_set_style_shadow_spread(ring, 10, 0);
    lv_obj_set_style_shadow_opa(ring, 115, 0);
    lv_obj_set_style_pad_all(ring, 3, 0);
    lv_obj_t *gap = l5_box(ring);
    lv_obj_set_size(gap, 320, 320);
    lv_obj_center(gap);
    lv_obj_set_style_radius(gap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(gap, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(gap, lv_color_hex(0x18191c), 0);
    lv_obj_t *enter = lv_button_create(gap);
    lv_obj_remove_style_all(enter);
    lv_obj_set_size(enter, 300, 300);
    lv_obj_center(enter);
    lv_obj_set_style_radius(enter, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(enter, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(enter, L5P.accent, 0);
    lv_obj_set_style_bg_grad_color(enter, l5_mix(L5P.accent, lv_color_hex(0x2a0405), 110), 0);
    lv_obj_set_style_bg_grad_dir(enter, LV_GRAD_DIR_VER, 0);
    l5_dither_hook(enter);
    lv_obj_set_style_transform_scale(enter, 245, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(enter, 150, 0);
    lv_obj_set_style_transform_pivot_y(enter, 150, 0);
    lv_obj_set_flex_flow(enter, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(enter, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(enter, 12, 0);
    l5_icon(enter, &l5_icon_54, L5_WHITE, L5_ICON_HOME);
    lv_obj_t *et = l5_label(enter, &l5_mono_22, L5_WHITE, "ENTER\nLAUNCHER");
    lv_obj_set_style_text_align(et, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(et, 3, 0);
    lv_obj_add_event_cb(enter, enter_cb, LV_EVENT_CLICKED, NULL);

    if (!app) { /* nothing to boot: Launcher opens */
        s_boot_timer = lv_timer_create(auto_enter_cb, 1500, NULL);
        lv_timer_set_repeat_count(s_boot_timer, 1);
        return;
    }
    lv_obj_t *fast = lv_button_create(r);
    lv_obj_remove_style_all(fast);
    lv_obj_set_size(fast, LV_SIZE_CONTENT, 64);
    lv_obj_set_style_pad_hor(fast, 28, 0);
    lv_obj_set_style_pad_column(fast, 12, 0);
    lv_obj_set_style_border_width(fast, 1, 0);
    lv_obj_set_style_border_color(fast, lv_color_hex(0x4a4d54), 0);
    lv_obj_set_style_radius(fast, 4, 0);
    lv_obj_set_style_bg_opa(fast, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(fast, lv_color_hex(0x16171a), 0);
    lv_obj_set_style_bg_color(fast, L5_PANEL_3, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(fast, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fast, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    l5_icon(fast, &l5_icon_20, L5_WHITE, L5_ICON_LAUNCH);
    lv_obj_t *ft = l5_label(fast, &l5_mono_16, L5_WHITE, "FAST BOOT APP");
    lv_obj_set_style_text_letter_space(ft, 2, 0);
    lv_obj_add_event_cb(fast, fast_cb, LV_EVENT_CLICKED, NULL);

    s_cd_start = lv_tick_get();
    s_boot_timer = lv_timer_create(countdown_cb, 50, NULL);
    l5_display_mark_built();
    l5_display_refresh_now();
}

/* a key of the Tab5 Keyboard while #sys shows a screen (l5_keys.c): on the bootscreen any key is ENTER LAUNCHER,
   another screen (Charge Mode) gets it as l5_key_event() */
bool l5_sys_key(const l5_key_t *k)
{
    if (s_bootscreen) {
        enter_launcher();
        return true;
    }
    lv_obj_t *el = lv_obj_get_child(l5_ui_sys(), 0);
    if (el) lv_obj_send_event(el, l5_key_event(), (void *)k);
    return el != NULL;
}

/* ---------- hand-over to the selected app (L5.bootApp) ---------- */

static void handoff_task(void *arg)
{
    int slot = (int)(intptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(120)); /* let the "starting" frame reach the panel */
    l5_cfg_flush();                 /* a setting changed in the last 300 ms */
    esp_err_t err = l5_slots_handoff(slot, g_cfg.deepSleepStart); /* restarts on success */
    ESP_LOGE(TAG, "app%d hand-over failed: %s", slot, esp_err_to_name(err));
    l5_board_lock(0);
    l5_sys_hide();
    l5_ui_set_booted(true);
    l5_ui_show(L5_VIEW_HOME);
    char body[120];
    snprintf(body, sizeof(body), "app%d did not pass the image check (%s). Reinstall it from SD or OTA.",
             slot, esp_err_to_name(err));
    l5_info("BOOT", "Cannot start the app", body);
    l5_board_unlock();
    vTaskDelete(NULL);
}

/* .app-run: "APPn · OTA_n · STARTING" card while the hand-over task runs */
static void starting_screen(int slot, const char *name)
{
    lv_obj_t *el = sys_show(L5_BLACK, L5_BLACK); /* .app-run */
    lv_obj_t *card = l5_box(el);
    lv_obj_set_size(card, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(card, 40, 0);
    lv_obj_set_style_pad_hor(card, 56, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_style_bg_opa(card, 153, 0);
    lv_obj_set_style_bg_color(card, L5_BLACK, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(card);
    char t[160];
    snprintf(t, sizeof(t), "APP%d \xC2\xB7 OTA_%d \xC2\xB7 STARTING", slot, slot);
    lv_obj_t *k = l5_label(card, &l5_mono_14, lv_color_hex(0xb9bcc2), t);
    lv_obj_set_style_text_letter_space(k, 3, 0);
    l5_label(card, &l5_chakra_b40, L5_WHITE, name);
    snprintf(t, sizeof(t), "Launcher selects app%d in otadata and restarts into it.\nPress RESET on the side to come back to the "
             "Launcher boot screen.", slot);
    lv_obj_t *p = l5_label(card, &l5_chakra_r17, lv_color_hex(0xc9ccd2), t);
    lv_obj_set_style_text_align(p, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(p, 6, 0);
}

static void boot_app(bool by_user)
{
    const l5_slot_t *a = l5_slots_boot_app();
    if (!a) {
        l5_toast("No app installed");
        return;
    }
    if (by_user) { /* the user starts it on purpose: no stop mark, the boot-loop guard starts over */
        if (l5_slots_stopped() == g_slots.boot) l5_slots_clear_stopped();
        l5_slots_guard_clear();
    }
    l5_ui_set_booted(false);
    starting_screen(g_slots.boot, a->name);
    xTaskCreatePinnedToCore(handoff_task, "l5_handoff", 4096, (void *)(intptr_t)g_slots.boot, 3, NULL, 1);
}

void l5_boot_app(void)
{
    boot_app(true);
}

void l5_boot_app_auto(void)
{
    boot_app(false);
}

static struct {
    uint32_t index, count, addr, size;
} s_at;

static void handoff_at_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(120)); /* let the "starting" frame reach the panel */
    l5_cfg_flush();
    esp_err_t err = l5_slots_handoff_at(s_at.index, s_at.count, s_at.addr, s_at.size, g_cfg.deepSleepStart);
    ESP_LOGE(TAG, "ota_%lu hand-over failed: %s", (unsigned long)s_at.index, esp_err_to_name(err));
    l5_restart_to_launcher(); /* the table changed since boot: the launcher starts over with it */
    vTaskDelete(NULL);
}

void l5_boot_at(uint32_t ota_index, uint32_t ota_count, uint32_t addr, uint32_t size, const char *name)
{
    s_at.index = ota_index;
    s_at.count = ota_count;
    s_at.addr = addr;
    s_at.size = size;
    l5_slots_guard_clear(); /* START APP after AUTO PARTITION: the user's choice */
    l5_ui_set_booted(false);
    starting_screen((int)ota_index, name);
    xTaskCreatePinnedToCore(handoff_at_task, "l5_handoff", 4096, NULL, 3, NULL, 1);
}

/* ---------- an app the boot-loop guard stopped ---------- */

static int s_stopped_slot;

static void stopped_cb(int index, void *user)
{
    (void)user;
    char label[8];
    snprintf(label, sizeof(label), "app%d", s_stopped_slot);
    if (index == 0) { /* ERASE APP: PMAN's FORMAT asks first */
        l5_pman_format(label);
    } else if (index == 1) { /* BOOT AGAIN */
        l5_slots_clear_stopped();
        l5_slots_select(s_stopped_slot);
        l5_boot_app();
    } /* OK: it stays stopped until it is booted from HOME */
}

void l5_stopped_dialog(int slot, int restarts, int returns)
{
    enter_launcher();
    if (slot < 0 || slot >= g_slots.n || !g_slots.slot[slot].used) return;
    s_stopped_slot = slot;
    const char *name = g_slots.slot[slot].name;
    char reason[40], title[96], body[400];
    switch (esp_reset_reason()) { /* the app's last reset, as its esp_restart / panic handler noted it */
    case ESP_RST_PANIC: snprintf(reason, sizeof(reason), "panic"); break;
    case ESP_RST_INT_WDT: snprintf(reason, sizeof(reason), "interrupt watchdog"); break;
    case ESP_RST_TASK_WDT: snprintf(reason, sizeof(reason), "task watchdog"); break;
    case ESP_RST_WDT: snprintf(reason, sizeof(reason), "watchdog"); break;
    case ESP_RST_SW: snprintf(reason, sizeof(reason), "esp_restart"); break;
    case ESP_RST_BROWNOUT: snprintf(reason, sizeof(reason), "brown-out"); break;
    default: snprintf(reason, sizeof(reason), "reset reason %d", (int)esp_reset_reason()); break;
    }
    snprintf(title, sizeof(title), "%.60s was stopped", name);
    if (restarts > 0) {
        snprintf(body, sizeof(body), "%.60s restarted %d times right after starting (%s) and was stopped. It may not "
                 "support this Tab5. The bootscreen does not start it until you boot it again.", name, restarts, reason);
    } else if (returns > 0) {
        snprintf(body, sizeof(body), "%.60s came back to the Launcher %d times within 15 s of starting (%s) and was "
                 "stopped. It may not support this Tab5. The bootscreen does not start it until you boot it again.",
                 name, returns, reason);
    } else {
        snprintf(body, sizeof(body), "%.60s was stopped on an earlier start: it restarted again and again right after "
                 "starting. It may not support this Tab5. The bootscreen does not start it until you boot it again.", name);
    }
    static const char *const labels[] = {"ERASE APP", "BOOT AGAIN", "OK"};
    l5_choose("BOOT", title, body, labels, 3, L5_ICON_CHECK, stopped_cb, NULL);
}

/* ---------- restart / power ---------- */

static void restart_cb(lv_timer_t *t)
{
    (void)t;
    l5_restart_to_launcher();
}

void l5_restart(void)
{
    l5_cfg_flush(); /* a setting changed in the last 300 ms */
    l5_ui_set_booted(false);
    sys_text_screen("RESTARTING\xE2\x80\xA6");
    lv_timer_t *t = lv_timer_create(restart_cb, 300, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void power_off_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(150));
    l5_board_backlight(0);
    l5_cfg_flush();  /* a setting changed in the last 300 ms */
    l5_wifi_pause(); /* the access point sees the Tab5 leave; the C6 loses power with the board */
    esp_err_t err = l5_board_power_off(); /* returns only if power stayed on */
    l5_board_charger_on(); /* the power-off reset the expander 0x44 (BSP driver): on USB the pack charges again */
    l5_wifi_resume(); /* still powered (USB): Wi-Fi comes back and rejoins */
    l5_board_lock(0);
    l5_board_backlight(g_cfg.bright);
    l5_sys_hide();
    l5_ui_set_booted(true);
    l5_info("POWER", "Power-off failed", esp_err_to_name(err));
    l5_board_unlock();
    vTaskDelete(NULL);
}

void l5_power_off(void)
{
    l5_ui_set_booted(false);
    sys_text_screen("POWERED OFF \xC2\xB7 PRESS POWER");
    xTaskCreatePinnedToCore(power_off_task, "l5_poweroff", 4096, NULL, 3, NULL, 1);
}

static void power_menu_cb(const char *id, void *user)
{
    (void)user;
    if (!id) {
        return;
    }
    if (strcmp(id, "app") == 0) {
        l5_boot_app();
    } else if (strcmp(id, "restart") == 0) {
        l5_restart();
    } else if (strcmp(id, "charge") == 0) {
        l5_charge_mode();
    } else if (strcmp(id, "off") == 0) {
        l5_power_off();
    }
}

void l5_power_menu(void)
{
    if (l5_pman_busy()) { /* the partition table is being rewritten */
        l5_toast("Wait: the partition table is being written");
        return;
    }
    if (l5_install_busy()) {
        l5_toast("Wait: an install is running");
        return;
    }
    const l5_slot_t *a = l5_slots_boot_app();
    char app[64];
    snprintf(app, sizeof(app), "APP \xC2\xB7 Boot %s", a ? a->name : "");
    const l5_menu_item_t items[] = {
        {a ? "app" : NULL, L5_ICON_LAUNCH, app, false},
        {"restart", L5_ICON_RESTART, "Restart", false},
        {"charge", L5_ICON_BOLT, "Charge Mode", false},
        {"off", L5_ICON_POWER, "OFF \xC2\xB7 Turn off", true},
    };
    l5_menu("POWER", "Power", items, 4, power_menu_cb, NULL);
}
