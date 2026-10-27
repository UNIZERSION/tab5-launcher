/* The bootscreen (M5Launcher flow): the app that starts after the countdown in the middle, its icon from the card
   (l5_appicon.c) or its initials on an accent tile inside a ring that empties as the seconds run out, LAUNCHER and START NOW under it, the other installed apps as
   chips at the bottom. A tap anywhere else stops the countdown. Only LVGL, the slots and the config: the PC snapshots
   (tools/host_test/ui_shots.py) draw it too. l5_sys.c shows the other system screens. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_app_desc.h"
#include "l5_board.h"
#include "l5_config.h"
#include "l5_display.h"
#include "l5_slots.h"
#include "l5_ui.h"

#define RING_D 280                /* the ring's box */
#define RING_R (RING_D / 2 - 2)   /* outer radius: 2 px inside the box, which clips the drawing */
#define RING_W 12
#define RING_Y 76                 /* the ring's top: its center sits at y 216 */
#define TILE 152                  /* the accent tile inside the ring */
#define TICK_MS 40                /* 25 steps a second: the ring's end moves 3-5 degrees a step */
#define TRACK (L5C.track)
#define MAX_CHIPS 4
#define TINT_OPA 90               /* the icon's tint over the top of the gradient: 35 % */

static struct {
    lv_timer_t *timer;
    lv_obj_t *el, *ring, *secs, *line, *tile;
    const l5_slot_t *app;          /* in the tile */
    lv_obj_t *dot[MAX_CHIPS];      /* the chips' round tiles */
    const l5_slot_t *dot_app[MAX_CHIPS];
    uint32_t start;
    int32_t angle; /* where the remaining arc starts (it ends at the top, 270); 270 + 360 = nothing left */
    unsigned shown; /* seconds in the label */
    bool up;       /* the bootscreen is on #sys: any key of the Tab5 Keyboard enters the launcher */
    bool stopped;  /* a tap stopped the countdown */
} B;

void l5_boot_screen_stop(void)
{
    if (B.timer) {
        lv_timer_delete(B.timer);
        B.timer = NULL;
    }
    B.up = false;
}

void l5_enter_launcher(void)
{
    l5_sys_hide();
    l5_ui_set_booted(true);
    l5_ui_show(L5_VIEW_HOME); /* built at start: shown, not built a second time */
}

bool l5_boot_screen_key(const l5_key_t *k)
{
    (void)k;
    if (!B.up) return false;
    l5_enter_launcher();
    return true;
}

/* ---------- the ring ---------- */

/* LVGL angles: 0 = 3 o'clock, clockwise. The remaining time is the arc from B.angle to the top. */
static void ring_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center.x = (a.x1 + a.x2) / 2;
    d.center.y = (a.y1 + a.y2) / 2;
    d.radius = RING_R;
    d.width = RING_W;
    d.start_angle = 0;
    d.end_angle = 360;
    d.color = TRACK;
    lv_draw_arc(layer, &d);
    if (B.angle >= 270 + 360) return; /* nothing left, or no app */
    d.color = B.stopped ? L5_MUTED : L5P.accent;
    d.rounded = 1;
    d.start_angle = B.angle <= 270 ? 0 : B.angle % 360;
    d.end_angle = B.angle <= 270 ? 360 : 270;
    lv_draw_arc(layer, &d);
}

/* the point on the ring's middle line at `angle`, in screen coordinates */
static lv_point_t ring_point(int32_t angle)
{
    lv_area_t a;
    lv_obj_get_coords(B.ring, &a);
    const int32_t r = RING_R - RING_W / 2;
    lv_point_t p = {
        .x = (a.x1 + a.x2) / 2 + (int32_t)(lv_trigo_cos((int16_t)(angle % 360)) * r / LV_TRIGO_SIN_MAX),
        .y = (a.y1 + a.y2) / 2 + (int32_t)(lv_trigo_sin((int16_t)(angle % 360)) * r / LV_TRIGO_SIN_MAX),
    };
    return p;
}

/* Moves the arc's start. Only the part between the old and the new end is drawn again: a step of a few degrees
   stays inside the box of its two ends (plus the stroke and its round cap), not the whole 280 px ring. */
static void ring_set(int32_t angle)
{
    if (angle == B.angle) return;
    const int32_t old = B.angle;
    B.angle = angle;
    if (old <= 270 || angle - old > 20 || old >= 270 + 360) {
        lv_obj_invalidate(B.ring);
        return;
    }
    lv_point_t p = ring_point(old), q = ring_point(angle);
    const int32_t m = RING_W / 2 + 3;
    lv_area_t a = {LV_MIN(p.x, q.x) - m, LV_MIN(p.y, q.y) - m, LV_MAX(p.x, q.x) + m, LV_MAX(p.y, q.y) + m};
    lv_obj_invalidate_area(B.ring, &a);
}

/* ---------- countdown ---------- */

static void countdown_cb(lv_timer_t *t)
{
    (void)t;
    const uint32_t dur = g_cfg.bootTime * 1000u;
    uint32_t el = lv_tick_elaps(B.start);
    if (el > dur) el = dur;
    ring_set(270 + (int32_t)(360 * (uint64_t)el / dur));
    const unsigned left = (unsigned)((dur - el + 999) / 1000);
    if (left != B.shown) {
        B.shown = left;
        lv_label_set_text_fmt(B.secs, "%u", left);
    }
    if (el >= dur) {
        l5_boot_screen_stop();
        l5_boot_app_auto();
    }
}

static void auto_enter_cb(lv_timer_t *t)
{
    (void)t;
    B.timer = NULL; /* one-shot timers delete themselves */
    l5_enter_launcher();
}

/* a tap beside the buttons: the countdown stops, the ring turns gray where it was */
static void stop_cb(lv_event_t *e)
{
    (void)e;
    if (!B.timer || B.stopped || !B.secs) return;
    lv_timer_delete(B.timer);
    B.timer = NULL;
    B.stopped = true;
    lv_obj_invalidate(B.ring);
    lv_obj_t *row = lv_obj_get_parent(B.line);
    lv_obj_clean(row);
    B.secs = NULL;
    lv_obj_t *l = l5_label(row, &l5_mono_16, L5_ON_PANEL_MUTED, "COUNTDOWN STOPPED \xC2\xB7 CHOOSE BELOW");
    lv_obj_set_style_text_letter_space(l, 2, 0);
    B.line = l;
}

static void enter_cb(lv_event_t *e)
{
    (void)e;
    l5_enter_launcher();
}

static void start_cb(lv_event_t *e)
{
    (void)e;
    l5_boot_app();
}

/* a chip: that app starts now and becomes the one the bootscreen starts (as HOME's "Boot now") */
static void chip_cb(lv_event_t *e)
{
    l5_slots_select((int)(intptr_t)lv_event_get_user_data(e));
    l5_boot_app();
}

/* ---------- building blocks ---------- */

static lv_obj_t *muted(lv_obj_t *parent, const lv_font_t *font, const char *text)
{
    lv_obj_t *l = l5_label(parent, font, L5_ON_PANEL_MUTED, text);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    return l;
}

static lv_obj_t *row_box(lv_obj_t *parent, int32_t gap)
{
    lv_obj_t *r = l5_box(parent);
    lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(r, gap, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return r;
}

/* "TAB5 LAUNCHER · v0.1.0" on the left, the clock and the battery on the right */
static void corners(lv_obj_t *el)
{
    char t[64];
    snprintf(t, sizeof(t), "TAB5 LAUNCHER \xC2\xB7 v%.16s", esp_app_get_description()->version);
    lv_obj_align(muted(el, &l5_mono_14, t), LV_ALIGN_TOP_LEFT, 40, 30);
    char bt[24];
    l5_battery_text(bt, sizeof(bt)); /* read by the rail a moment ago: no second I2C read before the first frame */
    struct tm tm;
    if (l5_clock_now(&tm)) snprintf(t, sizeof(t), "%02d:%02d \xC2\xB7 %s", tm.tm_hour, tm.tm_min, bt);
    else snprintf(t, sizeof(t), "%s", bt);
    lv_obj_align(muted(el, &l5_mono_14, t), LV_ALIGN_TOP_RIGHT, -40, 30);
}

/* the app's icon on a dark tile, else the accent tile with its initials, a plus without an app */
static lv_obj_t *tile(lv_obj_t *ring, const l5_slot_t *app)
{
    lv_obj_t *t = l5_box(ring);
    lv_obj_set_size(t, TILE, TILE);
    lv_obj_center(t);
    lv_obj_set_style_radius(t, 34, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_t *icon = app ? l5_appicon_create(t, app->project, TILE, 34) : NULL;
    if (icon) {
        lv_obj_set_style_bg_color(t, L5_PANEL_3, 0); /* under a transparent icon */
        lv_obj_center(icon);
    } else if (app) {
        lv_obj_set_style_bg_color(t, L5P.accent, 0);
        lv_obj_set_style_bg_grad_color(t, L5P.accent_lo, 0);
        lv_obj_set_style_bg_grad_dir(t, LV_GRAD_DIR_VER, 0);
        l5_dither_hook(t);
        char ini[3];
        l5_cover_initials(app->name, ini);
        lv_obj_center(l5_label(t, &l5_orb_xb54, L5P.accent_ink, ini));
    } else {
        lv_obj_set_style_bg_color(t, L5_PANEL_3, 0);
        lv_obj_center(l5_icon(t, &l5_icon_54, L5_ON_PANEL_MUTED, L5_ICON_PLUS));
    }
    return t;
}

/* a chip's round tile: the icon, else the initials */
static void dot_fill(lv_obj_t *dot, const l5_slot_t *s)
{
    lv_obj_clean(dot);
    lv_obj_t *icon = l5_appicon_create(dot, s->project, 44, LV_RADIUS_CIRCLE);
    if (icon) {
        lv_obj_center(icon);
        return;
    }
    char ini[3];
    l5_cover_initials(s->name, ini);
    lv_obj_center(l5_label(dot, &l5_orb_b16, L5_ON_PANEL, ini));
}

/* the top of the bootscreen's gradient: its theme colour with the app icon's tint (l5_appicon.c) over it */
static lv_color_t top_color(const l5_slot_t *app)
{
    lv_color_t tint;
    return app && l5_appicon_colors(app->project, NULL, &tint) ? l5_mix(tint, L5C.boot_top, TINT_OPA) : L5C.boot_top;
}

void l5_boot_screen_icons(void)
{
    if (!B.up || !B.ring) return;
    lv_obj_delete(B.tile);
    B.tile = tile(B.ring, B.app);
    for (int k = 0; k < MAX_CHIPS && B.dot[k]; k++) dot_fill(B.dot[k], B.dot_app[k]);
    lv_obj_set_style_bg_color(B.el, top_color(B.app), 0); /* the icon's tint, or none since it went */
    if (B.secs) lv_obj_set_style_text_color(B.secs, L5P.accent, 0); /* CFG > UI color APP: a new accent */
    lv_obj_invalidate(B.ring);
}

static lv_obj_t *big_button(lv_obj_t *parent, const char *icon, const char *text, bool primary, lv_event_cb_t cb)
{
    lv_obj_t *b = l5_act_button(parent, icon, text, primary);
    lv_obj_set_size(b, 400, 84);
    lv_obj_set_style_text_font(b, &l5_mono_18, 0);
    lv_obj_set_style_text_letter_space(b, 3, 0);
    lv_obj_set_style_pad_column(b, 14, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &l5_icon_24, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

/* the other installed apps, at most MAX_CHIPS: a tap starts one now */
static void chips(lv_obj_t *el)
{
    int idx[MAX_CHIPS], n = 0;
    for (int i = 0; i < g_slots.n && n < MAX_CHIPS; i++) {
        if (g_slots.slot[i].used && i != g_slots.boot) idx[n++] = i;
    }
    if (!n) return;
    lv_obj_t *box = l5_box(el);
    lv_obj_set_size(box, L5_SCREEN_W, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(box, 12, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(box, LV_ALIGN_BOTTOM_MID, 0, -26);
    muted(box, &l5_mono_13, n == 1 ? "OR START THE OTHER APP" : "OR START ANOTHER APP");
    lv_obj_t *row = row_box(box, 12);
    for (int k = 0; k < n; k++) {
        const l5_slot_t *s = &g_slots.slot[idx[k]];
        lv_obj_t *c = lv_button_create(row);
        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, LV_SIZE_CONTENT, 60);
        lv_obj_set_style_pad_left(c, 8, 0);
        lv_obj_set_style_pad_right(c, 22, 0);
        lv_obj_set_style_pad_column(c, 12, 0);
        lv_obj_set_style_radius(c, 30, 0);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_border_color(c, L5C.edge, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(c, L5C.slot_bot, 0);
        lv_obj_add_style(c, &L5S.press_fill, LV_STATE_PRESSED);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *dot = l5_box(c); /* the icon or the initials on a small round tile */
        lv_obj_set_size(dot, 44, 44);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, L5_PANEL_3, 0);
        B.dot[k] = dot;
        B.dot_app[k] = s;
        dot_fill(dot, s);
        lv_obj_t *nm = l5_label(c, &l5_chakra_sb17, L5_ON_PANEL, s->name);
        lv_obj_set_style_max_width(nm, 220, 0);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_add_event_cb(c, chip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx[k]);
    }
}

void l5_boot_screen(void)
{
    l5_display_mark("bootscreen"); /* the first frame after start is timed like a tab switch */
    l5_ui_set_booted(false);
    const l5_slot_t *app = l5_slots_boot_app();
    lv_obj_t *el = l5_sys_show(top_color(app), L5C.boot_bot); /* .boot, tinted by the app's icon */
    memset(&B, 0, sizeof(B));
    B.el = el;
    B.up = true;
    B.angle = 270 + 360; /* empty until the countdown starts */
    corners(el);

    B.ring = l5_box(el);
    lv_obj_set_size(B.ring, RING_D, RING_D);
    lv_obj_align(B.ring, LV_ALIGN_TOP_MID, 0, RING_Y);
    lv_obj_add_event_cb(B.ring, ring_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    B.app = app;
    B.tile = tile(B.ring, app);

    /* apps in flash but none chosen (flashed by hand, l5_slots_scan): the launcher opens, as with no app */
    const bool unchosen = !app && l5_slots_used() > 0;
    lv_obj_t *nm = l5_label(el, &l5_chakra_b40, L5_ON_PANEL,
                            app ? app->name : unchosen ? "Choose one in HOME" : "Install one from SD or OTA");
    lv_obj_set_style_max_width(nm, 1100, 0);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_align(nm, LV_ALIGN_TOP_MID, 0, RING_Y + RING_D + 26);

    lv_obj_t *line = row_box(el, 0);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, RING_Y + RING_D + 88);
    char t[64];
    if (app) { /* "APP0 · STARTS IN 3 s · TAP ANYWHERE TO STOP", the seconds in the accent color */
        snprintf(t, sizeof(t), "APP%d \xC2\xB7 STARTS IN ", g_slots.boot);
        muted(line, &l5_mono_16, t);
        B.shown = g_cfg.bootTime;
        snprintf(t, sizeof(t), "%u", B.shown);
        B.secs = l5_label(line, &l5_mono_16, L5P.accent, t);
        B.line = muted(line, &l5_mono_16, " s \xC2\xB7 TAP ANYWHERE TO STOP");
    } else {
        B.line = muted(line, &l5_mono_16, unchosen ? "NO APP CHOSEN TO START \xC2\xB7 OPENING THE LAUNCHER"
                                                   : "NO APP INSTALLED \xC2\xB7 OPENING THE LAUNCHER");
    }

    lv_obj_t *btns = row_box(el, 40);
    lv_obj_align(btns, LV_ALIGN_TOP_MID, 0, RING_Y + RING_D + 140);
    big_button(btns, L5_ICON_HOME, "LAUNCHER", !app, enter_cb);
    if (!app) { /* nothing to boot: Launcher opens */
        B.timer = lv_timer_create(auto_enter_cb, 1500, NULL);
        lv_timer_set_repeat_count(B.timer, 1);
        return;
    }
    big_button(btns, L5_ICON_PLAY, "START NOW", true, start_cb);
    chips(el);
    lv_obj_add_event_cb(el, stop_cb, LV_EVENT_CLICKED, NULL); /* l5_sys_show made it clickable */

    B.angle = 270;
    B.start = lv_tick_get();
    B.timer = lv_timer_create(countdown_cb, TICK_MS, NULL);
    l5_display_mark_built();
    l5_display_refresh_now();
}
