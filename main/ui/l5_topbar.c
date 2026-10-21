/* .topbar (y 0..72, 58 before 19.10.2026), launcher.css: one machined rail with the logo, a recessed track with
   the five tab keys (47 px high; six with WUI until 20.10.2026) and the status (Wi-Fi, clock, battery, power; 52 px buttons), a dark seam along
   the bottom whose lit part sits under the active key. Rectangles and two-stop gradients only: no triangles, no
   masks. A tab switch redraws the two keys, the dividers next to them and the lit seam. The keys and the two
   buttons take taps a little outside their edges (tab_hit_cb, ext click area). */
#include <stdio.h>
#include <time.h>
#include "esp_app_desc.h"
#include "l5_board.h"
#include "l5_ui.h"
#include "l5_wifi.h"
#include "src/core/lv_obj_event_private.h" /* lv_hit_test_info_t: point and result of LV_EVENT_HIT_TEST */

static const struct {
    const char *icon, *text;
} TABS[L5_VIEW_COUNT] = {
    {L5_ICON_HOME, "HOME"}, {L5_ICON_SD, "SD"}, {L5_ICON_OTA, "OTA"}, {L5_ICON_PMAN, "PMAN"}, {L5_ICON_CFG, "CFG"},
};

static lv_obj_t *s_tab[L5_VIEW_COUNT], *s_div[L5_VIEW_COUNT], *s_glow, *s_logo_accent;
static lv_obj_t *s_clk_d, *s_clk_t, *s_batt_fill, *s_batt_pct, *s_wifi;
static int s_batt = -1;
static bool s_wifi_off = true; /* #i-wifi-off: the wifi glyph with a slash */

/* .screw: 16px dark knurled head with a slot */
static void screw(lv_obj_t *parent, int32_t x, int32_t y)
{
    lv_obj_t *s = l5_box(parent);
    lv_obj_add_flag(s, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(s, x, y);
    lv_obj_set_size(s, 16, 16);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x4a4d53), 0);
    lv_obj_set_style_bg_grad_color(s, lv_color_hex(0x121315), 0);
    lv_obj_set_style_bg_grad_dir(s, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(s, 1, 0);
    lv_obj_set_style_border_color(s, L5_BLACK, 0);
    lv_obj_t *slot = l5_box(s);
    lv_obj_set_size(slot, 10, 2);
    lv_obj_center(slot);
    lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(slot, lv_color_hex(0x050506), 0);
    lv_obj_set_style_transform_rotation(slot, 350, 0);
    lv_obj_set_style_transform_pivot_x(slot, 5, 0);
    lv_obj_set_style_transform_pivot_y(slot, 1, 0);
}

/* a plain or vertically graded rectangle at a fixed place of the bar */
static lv_obj_t *rect(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, lv_color_t top, lv_color_t bottom)
{
    lv_obj_t *r = l5_box(parent);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, w, h);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, top, 0);
    if (!lv_color_eq(top, bottom)) {
        lv_obj_set_style_bg_grad_color(r, bottom, 0);
        lv_obj_set_style_bg_grad_dir(r, LV_GRAD_DIR_VER, 0);
        l5_dither_hook(r);
    }
    return r;
}

/* .tb-seam: 2 x 42 px in the middle of the 69 px above the seam, plate-edge on the left half, white on the
   right, 80 % */
static void vseam(lv_obj_t *bar, int32_t x)
{
    const int32_t y = (L5_TOPBAR_H - 3 - 42 + 1) / 2;
    lv_obj_set_style_bg_opa(rect(bar, x, y, 1, 42, L5_PLATE_EDGE, L5_PLATE_EDGE), 204, 0);
    lv_obj_set_style_bg_opa(rect(bar, x + 1, y, 1, 42, L5_WHITE, L5_WHITE), 204, 0);
}

static lv_obj_t *sep(lv_obj_t *parent)
{
    lv_obj_t *s = l5_box(parent);
    lv_obj_set_size(s, 1, 36);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s, L5_PLATE_LO, 0);
    return s;
}

static lv_obj_t *tb_btn(lv_obj_t *parent, const char *icon, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 52, 52);
    lv_obj_set_ext_click_area(b, 6); /* no other button within 6 px */
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_bg_color(b, L5_PLATE_LO, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_t *i = l5_icon(b, &l5_icon_24, L5_INK, icon);
    lv_obj_center(i);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void tab_cb(lv_event_t *e)
{
    l5_ui_go((l5_view_t)(intptr_t)lv_event_get_user_data(e));
}

/* A key takes taps from the whole height of the bar above the seam (TAB_EXT_Y above and below it) and its half of
   the 3 px gap to each neighbour; the outer keys 6 px more to the side. LVGL looks at the right-hand key first and
   its ext click area alone would take the left one's edge. */
#define TAB_EXT_Y 11 /* key y 11..58 -> 0..69 */
static void tab_hit_cb(lv_event_t *e)
{
    lv_hit_test_info_t *info = lv_event_get_hit_test_info(e);
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_current_target(e), &a);
    a.x1 -= i == 0 ? 6 : 1;
    a.x2 += i == L5_VIEW_COUNT - 1 ? 6 : 2;
    a.y1 -= TAB_EXT_Y;
    a.y2 += TAB_EXT_Y;
    const lv_point_t *p = info->point;
    info->res = p->x >= a.x1 && p->x <= a.x2 && p->y >= a.y1 && p->y <= a.y2;
}

/* the track's children are searched only for taps inside it: it reaches as far as the keys' extra area */
static void grp_ext_cb(lv_event_t *e)
{
    lv_event_set_ext_draw_size(e, TAB_EXT_Y - 3);
}

/* a key draws its divider 2 px left of itself: LVGL clips an overflowing child at the parent's ext draw size, which
   is 0 for a key without its CHECKED shadow, so the dividers were never drawn */
static void tab_ext_cb(lv_event_t *e)
{
    lv_event_set_ext_draw_size(e, 2);
}

static void wifi_cb(lv_event_t *e)
{
    (void)e;
    l5_wifi_menu();
}

/* the sprite's i-wifi-off is i-wifi plus "M3 3l18 18": FontAwesome 5 has no such glyph here */
static void wifi_slash_cb(lv_event_t *e)
{
    if (!s_wifi_off) return;
    lv_obj_t *icon = lv_event_get_current_target(e);
    lv_area_t a;
    lv_obj_get_coords(icon, &a);
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = a.x1 + 3;
    d.p1.y = a.y1 + 1;
    d.p2.x = a.x2 - 3;
    d.p2.y = a.y2 - 1;
    d.round_start = d.round_end = 1;
    d.width = 6; /* gap in the plate colour, then the stroke */
    d.color = L5_PLATE;
    lv_draw_line(lv_event_get_layer(e), &d);
    d.width = 2;
    d.color = lv_obj_get_style_text_color(icon, 0);
    lv_draw_line(lv_event_get_layer(e), &d);
}

static void power_cb(lv_event_t *e)
{
    (void)e;
    l5_power_menu();
}

static void tick_cb(lv_timer_t *t)
{
    static int n;
    (void)t;
    if (n++ % 6 == 0) {
        s_batt = l5_board_battery_percent(); /* every 30 s */
    }
    l5_topbar_paint_status();
}

void l5_topbar_create(lv_obj_t *screen)
{
    /* .topbar: plate-hi -> plate at 62 % (LVGL takes two gradient stops), then -> #d6d8dc in its own strip; 1 px
       white on top. The bar's shadow down (0 3px 8px) is left out: it would redraw with every view under it. */
    const int32_t h = L5_TOPBAR_H - 3; /* above the seam */
    lv_obj_t *bar = l5_box(screen);
    lv_obj_set_size(bar, L5_SCREEN_W, L5_TOPBAR_H);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_OVERFLOW_VISIBLE); /* the glow of the lit seam reaches into the view below */
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, L5_PLATE_HI, 0);
    lv_obj_set_style_bg_grad_color(bar, L5_PLATE, 0);
    lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_grad_stop(bar, 158, 0); /* 62 % of 255 */
    l5_dither_hook(bar);
    /* inset 0 1px 0 #fff as a rectangle: a top border would move every child 1 px down (LVGL counts it into the
       padding) and push the last row of the seam out of the bar */
    rect(bar, 0, 0, L5_SCREEN_W, 1, L5_WHITE, L5_WHITE);
    rect(bar, 0, L5_TOPBAR_H * 62 / 100, L5_SCREEN_W, h - L5_TOPBAR_H * 62 / 100, L5_PLATE, lv_color_hex(0xd7d9dd));
    rect(bar, 0, h, L5_SCREEN_W, 3, lv_color_hex(0x1b1c1f), lv_color_hex(0x2e3035)); /* .topbar::after: the seam */

    /* .tb-logo: x 0..420 */
    lv_obj_t *logo = l5_box(bar);
    lv_obj_set_size(logo, 420, h);
    lv_obj_set_style_pad_left(logo, 50, 0);
    lv_obj_set_style_pad_column(logo, 10, 0); /* 14 in CSS: "v0.1.0 · LVGL" is longer than the prototype's "v2.10 · UI" */
    lv_obj_set_flex_flow(logo, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(logo, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    screw(logo, 16 - 50, 26);
    lv_obj_t *name = l5_box(logo);
    lv_obj_set_size(name, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(name, LV_FLEX_FLOW_ROW);
    l5_label(name, &l5_orb_xb27, L5_INK, "M5STACK ");
    s_logo_accent = l5_label(name, &l5_orb_xb27, L5P.accent, "TAB5");
    char ver[40];
    snprintf(ver, sizeof(ver), "LAUNCHER\nv%.8s \xC2\xB7 LVGL", esp_app_get_description()->version);
    lv_obj_t *v = l5_label(logo, &l5_mono_12, L5_INK_SOFT, ver); /* .tb-ver, ends before the seam at x 420 */
    lv_obj_set_style_text_line_space(v, 2, 0);
    lv_obj_set_style_pad_left(v, 12, 0);
    lv_obj_set_style_border_side(v, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(v, 2, 0);
    lv_obj_set_style_border_color(v, L5_PLATE_LO, 0);
    vseam(bar, 420);
    vseam(bar, 922);

    /* .tb-tabs .grp: the recessed track (53 px, keys 47 px); its darker top stands in for the inset shadow */
    lv_obj_t *grp = l5_box(bar);
    lv_obj_set_pos(grp, 432, 8);
    lv_obj_set_size(grp, 480, 53);
    lv_obj_add_flag(grp, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_event_cb(grp, grp_ext_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    lv_obj_set_style_radius(grp, 8, 0);
    lv_obj_set_style_pad_all(grp, 2, 0); /* + the 1 px border: padding 3 (LVGL counts the border in) */
    lv_obj_set_style_pad_column(grp, 3, 0);
    lv_obj_set_style_bg_opa(grp, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(grp, lv_color_hex(0xc2c4c9), 0);
    lv_obj_set_style_bg_grad_color(grp, lv_color_hex(0xdcdee2), 0);
    lv_obj_set_style_bg_grad_dir(grp, LV_GRAD_DIR_VER, 0);
    l5_dither_hook(grp);
    lv_obj_set_style_border_width(grp, 1, 0);
    lv_obj_set_style_border_color(grp, L5_BLACK, 0);
    lv_obj_set_style_border_opa(grp, 26, 0);
    lv_obj_set_flex_flow(grp, LV_FLEX_FLOW_ROW);
    rect(bar, 440, 8 + 53, 464, 1, L5_WHITE, L5_WHITE); /* 0 1px 0 #fff under the track */
    for (int i = 0; i < L5_VIEW_COUNT; i++) {
        lv_obj_t *t = lv_button_create(grp); /* .tab */
        lv_obj_remove_style_all(t);
        lv_obj_add_flag(t, LV_OBJ_FLAG_OVERFLOW_VISIBLE); /* its divider sits in the gap left of it */
        lv_obj_set_height(t, lv_pct(100));
        lv_obj_set_flex_grow(t, 1);
        lv_obj_set_style_radius(t, 6, 0);
        lv_obj_set_style_pad_column(t, 7, 0);
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_text_color(t, L5_INK_SOFT, 0);
        lv_obj_set_style_bg_color(t, L5_BLACK, LV_STATE_PRESSED); /* .tab:active, seen at touch-down */
        lv_obj_set_style_bg_opa(t, 20, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(t, L5_WHITE, LV_STATE_CHECKED); /* [aria-selected]: a raised key */
        lv_obj_set_style_bg_grad_color(t, lv_color_hex(0xeceef0), LV_STATE_CHECKED);
        lv_obj_set_style_bg_grad_dir(t, LV_GRAD_DIR_VER, LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_style_shadow_width(t, 3, LV_STATE_CHECKED);
        lv_obj_set_style_shadow_offset_y(t, 1, LV_STATE_CHECKED);
        lv_obj_set_style_shadow_color(t, L5_BLACK, LV_STATE_CHECKED);
        lv_obj_set_style_shadow_opa(t, 77, LV_STATE_CHECKED);
        lv_obj_set_style_outline_width(t, 1, LV_STATE_CHECKED);
        lv_obj_set_style_outline_color(t, L5_BLACK, LV_STATE_CHECKED);
        lv_obj_set_style_outline_opa(t, 20, LV_STATE_CHECKED);
        lv_obj_set_style_text_color(t, L5P.accent, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(t, lv_color_hex(0xe2e4e7), LV_STATE_CHECKED | LV_STATE_PRESSED); /* the active key, pressed */
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, LV_STATE_CHECKED | LV_STATE_PRESSED);
        lv_obj_t *ic = lv_label_create(t);
        lv_obj_set_style_text_font(ic, &l5_icon_20, 0);
        lv_label_set_text(ic, TABS[i].icon);
        lv_obj_t *tx = lv_label_create(t);
        lv_obj_set_style_text_font(tx, &l5_chakra_sb16, 0);
        lv_obj_set_style_text_letter_space(tx, 1, 0);
        lv_label_set_text(tx, TABS[i].text);
        s_div[i] = NULL;
        if (i > 0) { /* .tab + .tab::before: 1 px in the middle of the 3 px gap, hidden next to the active key */
            lv_obj_t *s = l5_box(t);
            lv_obj_add_flag(s, LV_OBJ_FLAG_FLOATING);
            lv_obj_set_pos(s, -2, 12);
            lv_obj_set_size(s, 1, 47 - 24); /* top 12px, bottom 12px */
            lv_obj_set_style_bg_opa(s, 153, 0);
            lv_obj_set_style_bg_color(s, L5_PLATE_EDGE, 0);
            s_div[i] = s;
        }
        lv_obj_add_event_cb(t, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_set_ext_click_area(t, TAB_EXT_Y); /* tab_hit_cb narrows it to the key's share */
        lv_obj_add_flag(t, LV_OBJ_FLAG_ADV_HITTEST);
        lv_obj_add_event_cb(t, tab_hit_cb, LV_EVENT_HIT_TEST, (void *)(intptr_t)i);
        lv_obj_add_event_cb(t, tab_ext_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
        l5_dither_hook(t); /* the gradient of the selected key */
        lv_obj_refresh_ext_draw_size(t);
        s_tab[i] = t;
    }
    /* [aria-selected]::after: the lit part of the seam, one object that l5_topbar_select() moves */
    s_glow = l5_box(bar);
    lv_obj_set_size(s_glow, 10, 3);
    lv_obj_set_pos(s_glow, 0, h);
    lv_obj_set_style_radius(s_glow, 2, 0);
    lv_obj_set_style_bg_opa(s_glow, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_glow, L5P.accent, 0);
    lv_obj_set_style_shadow_color(s_glow, L5P.accent, 0);
    lv_obj_set_style_shadow_width(s_glow, 10, 0);
    lv_obj_set_style_shadow_spread(s_glow, 1, 0);
    lv_obj_set_style_shadow_opa(s_glow, 217, 0);

    /* .tb-status: x 924..1280, right aligned */
    lv_obj_t *st = l5_box(bar);
    lv_obj_set_pos(st, 924, 0);
    lv_obj_set_size(st, L5_SCREEN_W - 924, h);
    lv_obj_set_style_pad_left(st, 14, 0);
    lv_obj_set_style_pad_right(st, 12, 0);
    lv_obj_set_style_pad_column(st, 8, 0); /* 10 before the 52 px buttons */
    lv_obj_set_flex_flow(st, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *wb = tb_btn(st, L5_ICON_WIFI, wifi_cb);
    s_wifi = lv_obj_get_child(wb, 0);
    lv_obj_add_event_cb(s_wifi, wifi_slash_cb, LV_EVENT_DRAW_POST, NULL);
    sep(st);
    lv_obj_t *clk = l5_box(st);
    lv_obj_set_size(clk, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(clk, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(clk, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    s_clk_d = l5_label(clk, &l5_mono_11, L5_INK_SOFT, "");
    lv_obj_set_style_text_letter_space(s_clk_d, 1, 0);
    s_clk_t = l5_label(clk, &l5_orb_b22, L5_INK, "");
    lv_obj_set_style_text_letter_space(s_clk_t, 1, 0);
    sep(st);

    /* .batt: cell with fill + nub, percentage */
    lv_obj_t *batt = l5_box(st);
    lv_obj_set_size(batt, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(batt, 9, 0);
    lv_obj_set_flex_flow(batt, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(batt, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *cell = l5_box(batt);
    lv_obj_set_size(cell, 34, 17);
    lv_obj_set_style_border_width(cell, 2, 0);
    lv_obj_set_style_border_color(cell, L5_INK, 0);
    lv_obj_set_style_radius(cell, 3, 0);
    lv_obj_set_style_pad_all(cell, 2, 0);
    s_batt_fill = l5_box(cell);
    lv_obj_set_height(s_batt_fill, lv_pct(100));
    lv_obj_set_style_bg_opa(s_batt_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_batt_fill, L5_INK, 0);
    lv_obj_t *nub = l5_box(cell);
    lv_obj_add_flag(nub, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(nub, 3, 6);
    lv_obj_set_pos(nub, 31, 2); /* right: -6px of the padding box */
    lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(nub, L5_INK, 0);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    s_batt_pct = l5_label(batt, &l5_chakra_sb17, L5_INK, "");
    tb_btn(st, L5_ICON_POWER, power_cb);

    s_batt = l5_board_battery_percent();
    l5_topbar_paint_status();
    lv_timer_create(tick_cb, 5000, NULL);
}

void l5_topbar_apply_accent(void)
{
    lv_obj_set_style_text_color(s_logo_accent, L5P.accent, 0);
    for (int i = 0; i < L5_VIEW_COUNT; i++) {
        lv_obj_set_style_text_color(s_tab[i], L5P.accent, LV_STATE_CHECKED);
    }
    lv_obj_set_style_bg_color(s_glow, L5P.accent, 0);
    lv_obj_set_style_shadow_color(s_glow, L5P.accent, 0);
}

void l5_topbar_select(l5_view_t view)
{
    for (int i = 0; i < L5_VIEW_COUNT; i++) {
        lv_obj_set_state(s_tab[i], LV_STATE_CHECKED, i == (int)view);
        if (s_div[i]) lv_obj_set_flag(s_div[i], LV_OBJ_FLAG_HIDDEN, i == (int)view || i - 1 == (int)view);
    }
    lv_obj_update_layout(s_tab[view]); /* the first call comes before the screen was laid out */
    lv_area_t a;
    lv_obj_get_coords(s_tab[view], &a); /* the bar is at 0,0 of the screen */
    lv_obj_set_pos(s_glow, a.x1 + 4, L5_TOPBAR_H - 3);
    lv_obj_set_width(s_glow, lv_area_get_width(&a) - 8);
}

void l5_topbar_paint_status(void)
{
    static const char *DAYS[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year + 1900 >= 2024) {
        lv_label_set_text_fmt(s_clk_d, "%04d.%02d.%02d %s", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, DAYS[t.tm_wday]);
        lv_label_set_text_fmt(s_clk_t, "%02d:%02d", t.tm_hour, t.tm_min);
    } else {
        lv_label_set_text(s_clk_d, "RTC NOT SET");
        lv_label_set_text(s_clk_t, "--:--");
    }
    int pct = s_batt < 0 ? 100 : s_batt;
    lv_obj_set_width(s_batt_fill, 26 * pct / 100);
    if (s_batt < 0) {
        lv_label_set_text(s_batt_pct, "USB");
    } else {
        lv_label_set_text_fmt(s_batt_pct, "%d%%", s_batt);
    }
    l5_topbar_paint_wifi();
}

void l5_topbar_paint_wifi(void)
{
    l5_wifi_status_t st;
    l5_wifi_get_status(&st);
    bool on = st.state == L5_WIFI_CONNECTED; /* .tb-btn.on */
    bool busy = st.state == L5_WIFI_STARTING || st.state == L5_WIFI_CONNECTING;
    lv_obj_set_style_text_color(s_wifi, on ? L5P.accent : busy ? L5_MUTED : L5_INK, 0);
    if (s_wifi_off != (!on && !busy)) {
        s_wifi_off = !on && !busy;
        lv_obj_invalidate(s_wifi);
    }
}
