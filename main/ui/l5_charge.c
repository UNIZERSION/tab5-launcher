/* Charge Mode (app.js L5.chargeMode, launcher.css .charge): the battery with its real charge instead of a sweep, the
   percentage, the state under it (CHARGING / ON BATTERY / FULL / USB · NOT CHARGING), re-read every 5 s. While charging
   only the tip of the fill pulses. A tap goes back to the launcher. */
#include <stdio.h>
#include <string.h>
#include "l5_board.h"
#include "l5_ui.h"

#define BODY_W 320 /* .bt: 320 x 150, border 8, padding 10 */
#define BODY_H 150
#define FILL_W (BODY_W - 2 * 8 - 2 * 10)
#define TIP_W 18
#define NUB_W 18 /* 2 px gap + 16 px */

static struct {
    lv_obj_t *fill, *tip, *pct, *state, *icon, *state_text;
    lv_timer_t *timer;
} C;

static void tip_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_bg_opa(obj, (lv_opa_t)v, 0);
}

/* .bt.on i::after: rgba(255,255,255,.6), opacity 0 -> 1 -> 0 in 1.6 s */
static void tip_pulse(bool on)
{
    if (on == (lv_anim_get(C.tip, tip_opa_cb) != NULL)) return;
    if (!on) {
        lv_anim_delete(C.tip, tip_opa_cb);
        lv_obj_set_style_bg_opa(C.tip, LV_OPA_TRANSP, 0);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, C.tip);
    lv_anim_set_exec_cb(&a, tip_opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, 153);
    lv_anim_set_duration(&a, 800);
    lv_anim_set_playback_duration(&a, 800);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static void paint(bool fresh)
{
    int pct = l5_board_battery_percent();
    l5_charge_t st = pct < 0 ? L5_CHARGE_UNKNOWN : l5_board_battery_state(fresh);
    int32_t w = pct < 0 ? 0 : FILL_W * pct / 100;
    lv_obj_set_width(C.fill, w);
    lv_obj_set_style_bg_color(C.fill, pct < 10 ? L5P.accent : pct < 20 ? L5_WARN : L5_OK, 0); /* i.crit / i.low */
    lv_obj_set_width(C.tip, w < TIP_W ? w : TIP_W);
    tip_pulse(st == L5_CHARGE_CHARGING && w > 0);
    if (pct < 0) {
        lv_label_set_text(C.pct, "USB"); /* no pack: the Tab5 runs from USB */
    } else {
        lv_label_set_text_fmt(C.pct, "%d%%", pct);
    }
    /* .st: CHARGING as in the prototype; ON BATTERY like .st.idle; on external power without charge current FULL from
       95 %, below that USB · NOT CHARGING (the charger stopped); no line without a reading */
    const char *text = NULL, *icon = NULL;
    lv_color_t color = L5_ON_PANEL_MUTED;
    switch (st) {
    case L5_CHARGE_CHARGING:
        text = "CHARGING";
        icon = L5_ICON_BOLT;
        color = L5_OK;
        break;
    case L5_CHARGE_ON_BATTERY:
        text = "ON BATTERY";
        break;
    case L5_CHARGE_EXTERNAL:
        text = pct >= 95 ? "FULL" : "USB \xC2\xB7 NOT CHARGING";
        icon = pct >= 95 ? L5_ICON_CHECK : L5_ICON_USB;
        color = pct >= 95 ? L5_OK : L5_WARN;
        break;
    default:
        break;
    }
    lv_obj_set_flag(C.state, LV_OBJ_FLAG_HIDDEN, text == NULL);
    lv_obj_set_flag(C.icon, LV_OBJ_FLAG_HIDDEN, icon == NULL);
    if (icon) lv_label_set_text(C.icon, icon);
    lv_label_set_text(C.state_text, text ? text : "");
    lv_obj_set_style_text_color(C.state, color, 0);
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    paint(false);
}

static void deleted_cb(lv_event_t *e)
{
    (void)e;
    if (C.timer) lv_timer_delete(C.timer);
    memset(&C, 0, sizeof(C));
}

static void exit_cb(lv_event_t *e)
{
    (void)e;
    l5_sys_hide();
    l5_ui_set_booted(true);
}

static lv_obj_t *rect(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius)
{
    lv_obj_t *r = l5_box(parent);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, w, h);
    lv_obj_set_style_radius(r, radius, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, L5_WHITE, 0);
    return r;
}

void l5_charge_mode(void)
{
    l5_ui_set_booted(false);
    lv_obj_t *el = l5_sys_show(L5_BLACK, L5_BLACK); /* .charge */
    lv_obj_add_event_cb(el, exit_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(el, deleted_cb, LV_EVENT_DELETE, NULL);
    lv_obj_t *c = l5_box(el);
    lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(c);

    /* the body is centred and the nub (.bt::after: 16 x 50, 2 px off the body, right corners round) hangs off it: the
       box around them is as wide on the left, so the column keeps the body in the middle (LVGL lets children overflow
       a parent only as far as its ext draw size) */
    lv_obj_t *batt = l5_box(c);
    lv_obj_set_size(batt, BODY_W + 2 * NUB_W, BODY_H);
    lv_obj_t *bt = l5_box(batt);
    lv_obj_set_pos(bt, NUB_W, 0);
    lv_obj_set_size(bt, BODY_W, BODY_H);
    lv_obj_set_style_border_width(bt, 8, 0);
    lv_obj_set_style_border_color(bt, L5_WHITE, 0);
    lv_obj_set_style_radius(bt, 14, 0);
    lv_obj_set_style_pad_all(bt, 10, 0);
    rect(batt, NUB_W + BODY_W + 2, (BODY_H - 50) / 2, 10, 50, 0);
    rect(batt, NUB_W + BODY_W + 6, (BODY_H - 50) / 2, 12, 50, 6);
    C.fill = l5_box(bt);
    lv_obj_set_height(C.fill, lv_pct(100));
    lv_obj_set_style_bg_opa(C.fill, LV_OPA_COVER, 0);
    C.tip = l5_box(C.fill);
    lv_obj_set_height(C.tip, lv_pct(100));
    lv_obj_align(C.tip, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(C.tip, L5_WHITE, 0);

    /* b: 700 60px Orbitron, margin-top 30; the font has digits and capitals only, so the browser's line box above and
       below them (about 15 px each) is padding here */
    C.pct = l5_label(c, &l5_orb_xb54, L5_WHITE, "");
    lv_obj_set_style_pad_top(C.pct, 30 + 15, 0);
    lv_obj_set_style_pad_bottom(C.pct, 15, 0);
    C.state = l5_box(c); /* .st: icon + state, margin 8px 0 14px */
    lv_obj_set_size(C.state, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(C.state, 8, 0);
    lv_obj_set_style_pad_bottom(C.state, 14, 0);
    lv_obj_set_style_pad_column(C.state, 10, 0);
    lv_obj_set_flex_flow(C.state, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(C.state, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    C.icon = lv_label_create(C.state);
    lv_obj_set_style_text_font(C.icon, &l5_icon_22, 0);
    C.state_text = lv_label_create(C.state);
    lv_obj_set_style_text_font(C.state_text, &l5_mono_18, 0);
    lv_obj_set_style_text_letter_space(C.state_text, 3, 0);
    lv_obj_t *s = l5_label(c, &l5_mono_16, L5_ON_PANEL_MUTED, "CHARGE MODE \xC2\xB7 TAP TO EXIT");
    lv_obj_set_style_text_letter_space(s, 3, 0);

    paint(true); /* a new visit: the first reading sets the state line */
    C.timer = lv_timer_create(timer_cb, 5000, NULL);
}
