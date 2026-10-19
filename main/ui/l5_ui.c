#include "l5_ui.h"
#include <stdio.h>
#include "l5_board.h"
#include "l5_config.h"
#include "esp_log.h"
#include "l5_display.h"
#if L5_TOUCH_TRACE
#include "esp_timer.h"
static const char *const TRACE_VIEW[L5_VIEW_COUNT] = {"HOME", "SD", "OTA", "WUI", "PMAN", "CFG"};
#endif

/* .screen > .topbar + .views, then #layer (z20), #sys (z30), toasts (z40), #veil (z50) */
static lv_obj_t *s_views[L5_VIEW_COUNT];
static lv_obj_t *s_layer, *s_sys, *s_toasts, *s_veil, *s_bar;
static l5_view_t s_view = L5_VIEW_HOME;
static bool s_booted, s_dimmed;
static bool s_off;    /* backlight off after cfg.screenOff s without touch (s_dimmed is set too) */
static bool s_frozen; /* invalidation paused while the screen is dark: nothing to see, nothing to render */
static uint32_t s_render_pending; /* views whose rebuild came during a press: done at the release */

static void prebuild_cb(lv_timer_t *t);

static lv_obj_t *overlay(void)
{
    lv_obj_t *o = l5_box(lv_layer_top());
    lv_obj_set_size(o, L5_SCREEN_W, L5_SCREEN_H);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* ---------- idle dimming and screen off (app.js setVeil / idle timer) ---------- */

/* lv_display_enable_invalidation() counts: one pause, one resume */
static void freeze(bool on)
{
    if (on == s_frozen) return;
    s_frozen = on;
    lv_display_enable_invalidation(NULL, !on);
    if (!on) { /* whatever changed while dark (clock, dialogs, views) is drawn now */
        lv_obj_invalidate(lv_screen_active());
        lv_obj_invalidate(lv_layer_top());
    }
}

static void set_veil(void)
{
    if (s_off) { /* the veil still swallows the waking tap; no DISPOFF: the panel showed noise when switched back */
        l5_board_backlight(0);
        lv_obj_remove_flag(s_veil, LV_OBJ_FLAG_HIDDEN);
        freeze(true);
    } else if (s_dimmed) {
        l5_board_backlight(g_cfg.bright * 12 / 100 + 1); /* veil opacity .88 */
        lv_obj_remove_flag(s_veil, LV_OBJ_FLAG_HIDDEN);
    } else {
        l5_board_backlight(g_cfg.bright);
        lv_obj_add_flag(s_veil, LV_OBJ_FLAG_HIDDEN);
    }
}

void l5_ui_wake(void)
{
    if (s_dimmed || s_off) {
        s_dimmed = s_off = false;
        freeze(false);
        set_veil();
    }
    lv_display_trigger_activity(NULL);
}

static void veil_cb(lv_event_t *e)
{
    (void)e;
#if L5_TOUCH_TRACE
    ESP_LOGI("l5_touch", "veil: this tap only wakes the dimmed screen");
#endif
    l5_ui_wake(); /* the waking tap is swallowed, like #veil.dimmed */
}

static void idle_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t idle = lv_display_get_inactive_time(NULL);
    if (g_cfg.screenOff > 0 && !s_off && s_booted && idle > g_cfg.screenOff * 1000u) { /* also with Dim time OFF */
        ESP_LOGI("l5_ui", "screen off after %u s without touch: the next tap only wakes", (unsigned)g_cfg.screenOff);
        s_off = s_dimmed = true;
        set_veil();
        return;
    }
    if (g_cfg.dim > 0 && !s_dimmed && s_booted && idle > g_cfg.dim * 1000u) {
#if L5_TOUCH_TRACE
        ESP_LOGI("l5_touch", "dimmed after %u s without touch: the next tap only wakes", (unsigned)g_cfg.dim);
#endif
        s_dimmed = true;
        set_veil();
    }
}

void l5_ui_set_booted(bool booted)
{
    s_booted = booted;
    if (!booted) { /* a system screen (bootscreen, starting, restart, off, charge) is drawn and seen */
        bool dark = s_dimmed || s_off;
        s_dimmed = s_off = false;
        freeze(false);
        lv_obj_add_flag(s_veil, LV_OBJ_FLAG_HIDDEN);
        if (dark) l5_board_backlight(g_cfg.bright);
    }
}

/* ---------- views ---------- */

void l5_ui_init(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scr, L5_PLATE, 0);
    lv_obj_set_style_text_font(scr, &l5_chakra_r17, 0);
    lv_obj_set_style_text_color(scr, L5_INK, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < L5_VIEW_COUNT; i++) {
        s_views[i] = l5_box(scr);
        lv_obj_set_pos(s_views[i], 0, L5_TOPBAR_H);
        lv_obj_set_size(s_views[i], L5_SCREEN_W, L5_VIEW_H);
        lv_obj_add_flag(s_views[i], LV_OBJ_FLAG_HIDDEN);
    }
    l5_topbar_create(scr);
    s_bar = lv_obj_get_child(scr, -1);

    s_layer = overlay();
    s_sys = overlay();
    s_toasts = overlay();
    s_veil = overlay();
    lv_obj_add_flag(s_veil, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_veil, veil_cb, LV_EVENT_PRESSED, NULL);
    lv_timer_create(idle_cb, 1000, NULL);
    lv_timer_create(prebuild_cb, 200, NULL);
}

lv_obj_t *l5_ui_layer(void)
{
    return s_layer;
}

lv_obj_t *l5_ui_sys(void)
{
    return s_sys;
}

lv_obj_t *l5_ui_toasts(void)
{
    return s_toasts;
}

l5_view_t l5_ui_view(void)
{
    return s_view;
}

/* Views are built once and then only shown and hidden: building CFG / PMAN / SD costs
   50-140 ms of object creation, flash and SD reads. s_built is false while a view's objects do
   not exist yet or show old data; s_ever tells a view that was never built from a stale one. */
static bool s_built[L5_VIEW_COUNT], s_ever[L5_VIEW_COUNT];

static void build(l5_view_t v)
{
    if (v == L5_VIEW_HOME) {
        l5_home_render(s_views[v]);
    } else {
        l5_views_render(v, s_views[v]);
    }
    s_built[v] = s_ever[v] = true;
}

void l5_ui_render_views(uint32_t views)
{
    /* Deleting the object under a finger cancels its tap (LVGL resets the touch input), so a rebuild
       that background work asks for during a press waits for the release (prebuild_cb). Click
       handlers run at the release and rebuild right away. */
    lv_indev_t *touch = l5_display_touch();
    if (touch && lv_indev_get_state(touch) == LV_INDEV_STATE_PRESSED) {
        s_render_pending |= views;
        return;
    }
    views |= s_render_pending;
    s_render_pending = 0;
    /* hidden views are rebuilt when they are shown next (l5_ui_go): a rebuild in the background
       blocks the touch reads for 50-60 ms */
    for (int i = 0; i < L5_VIEW_COUNT; i++) {
        if ((views & L5_VIEW_BIT(i)) && i != (int)s_view) s_built[i] = false;
    }
    if (!(views & L5_VIEW_BIT(s_view))) return;
#if L5_TOUCH_TRACE
    int64_t t0 = esp_timer_get_time();
    build(s_view);
    char what[32];
    snprintf(what, sizeof(what), "rebuild %s (data changed)", TRACE_VIEW[s_view]);
    l5_touch_trace_busy(what, t0);
#else
    build(s_view);
#endif
}

void l5_ui_render(void)
{
    l5_ui_render_views(L5_VIEWS_ALL); /* data changed (install, erase, card, ...) */
}

static void render_async_cb(void *arg)
{
    l5_ui_render_views((uint32_t)(uintptr_t)arg);
}

void l5_ui_render_views_async(uint32_t views)
{
    lv_async_call(render_async_cb, (void *)(uintptr_t)views);
}

void l5_ui_render_async(void)
{
    l5_ui_render_views_async(L5_VIEWS_ALL);
}

/* Build views that were never built while nobody touches the screen, one per tick, so the first
   switch to them only flips visibility. A build blocks the touch reads (50-60 ms for SD and PMAN),
   so it waits for 2 s without touch and happens once per view; stale views wait for their next
   visit. Never during the bootscreen or with a dialog open. */
static void prebuild_cb(lv_timer_t *t)
{
    (void)t;
    if (s_render_pending) { /* asked for during a press; stays pending while the finger is down */
        l5_ui_render_views(0);
        return;
    }
    if (!s_booted || lv_obj_get_child_count(s_layer) || lv_obj_get_child_count(s_sys) ||
        lv_display_get_inactive_time(NULL) < 2000) {
        return;
    }
    for (int i = 0; i < L5_VIEW_COUNT; i++) {
        if (!s_ever[i] && i != (int)s_view) {
#if L5_TOUCH_TRACE
            int64_t t0 = esp_timer_get_time();
            build((l5_view_t)i);
            char what[32];
            snprintf(what, sizeof(what), "prebuild %s (idle)", TRACE_VIEW[i]);
            l5_touch_trace_busy(what, t0);
#else
            build((l5_view_t)i); /* hidden: nothing on screen changes */
#endif
            return;
        }
    }
}

/* tap: the user tapped the view's tab (the active tab is rebuilt); render: the frame is published now and timed */
static void go(l5_view_t view, bool tap, bool render)
{
    static const char *const NAMES[L5_VIEW_COUNT] = {"tab HOME", "tab SD", "tab OTA", "tab WUI", "tab PMAN", "tab CFG"};
    if (render) l5_display_mark(NAMES[view]); /* logs the timing of the frame this switch publishes */
    /* Tapping the active tab rebuilds it (e.g. re-reads the SD card). A missing or stale view is
       built while still hidden. Then everything flips inside this one LVGL event: tab bar, old
       view, new view change in a single refresh, which l5_display publishes as one frame. */
    bool again = tap && view == s_view && !lv_obj_has_flag(s_views[view], LV_OBJ_FLAG_HIDDEN);
    s_view = view;
    if (!s_built[view] || again) {
        lv_obj_add_flag(s_views[view], LV_OBJ_FLAG_HIDDEN);
        build(view);
    }
    l5_topbar_select(view);
    for (int i = 0; i < L5_VIEW_COUNT; i++) {
        if (i == (int)view) {
            lv_obj_remove_flag(s_views[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_views[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (render) {
        l5_display_mark_built();
        l5_display_refresh_now(); /* render right away, not at the next 16 ms refresh period */
    }
}

void l5_ui_go(l5_view_t view)
{
    go(view, true, true);
}

void l5_ui_show(l5_view_t view)
{
    go(view, false, true);
}

void l5_ui_prepare(l5_view_t view)
{
    go(view, false, false);
}

/* LVGL draws the active screen under the top layer even when an opaque system screen covers all of it: the
   bootscreen's first frame drew the whole HOME view and the top bar underneath (378 ms of the boot). While one is
   up, the view and the top bar are hidden. */
void l5_ui_covered(bool covered)
{
    lv_obj_set_flag(s_bar, LV_OBJ_FLAG_HIDDEN, covered);
    lv_obj_set_flag(s_views[s_view], LV_OBJ_FLAG_HIDDEN, covered);
}

/* ---------- ui.js formatting ---------- */

void l5_fmt_bytes(char *out, size_t len, uint64_t b)
{
    if (b >= 1024ull * 1024 * 1024) {
        snprintf(out, len, "%.1f GB", b / 1073741824.0);
    } else if (b >= 1024 * 1024) {
        snprintf(out, len, "%.1f MB", b / 1048576.0);
    } else if (b >= 1024) {
        snprintf(out, len, "%u KB", (unsigned)((b + 512) / 1024));
    } else {
        snprintf(out, len, "%u B", (unsigned)b);
    }
}

void l5_fmt_kb(char *out, size_t len, uint32_t kb)
{
    if (kb >= 1024) {
        if (kb % 1024) {
            snprintf(out, len, "%.1fM", kb / 1024.0);
        } else {
            snprintf(out, len, "%uM", (unsigned)(kb / 1024));
        }
    } else {
        snprintf(out, len, "%uK", (unsigned)kb);
    }
}
