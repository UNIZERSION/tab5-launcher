/* The rail down the left edge (x 0..112, since 26.10.2026; before, a 72 px top bar): the TAB5 logo, the five tabs
   (icon over name, 112 x 88 each, no gaps: every pixel of the rail's middle belongs to a tab) and the status at the
   bottom: clock, battery, Wi-Fi and power. The active tab has an accent tint, an accent bar on its left edge and
   accent text. A tab switch redraws the two tabs; the clock redraws only when the minute changes. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_app_desc.h"
#include "l5_board.h"
#include "l5_ui.h"
#include "l5_wifi.h"

#define LOGO_H 84  /* logo and version above the tabs */
#define TAB_H 88
#define BTN_W 48   /* Wi-Fi and power side by side at the bottom */
#define BTN_H 56
#define RAIL_BG (L5C.rail)

static const struct {
    const char *icon, *text;
} TABS[L5_VIEW_COUNT] = {
    {L5_ICON_HOME, "HOME"}, {L5_ICON_SD, "SD"}, {L5_ICON_OTA, "OTA"}, {L5_ICON_PMAN, "PMAN"}, {L5_ICON_CFG, "CFG"},
};

static lv_obj_t *s_tab[L5_VIEW_COUNT], *s_mark[L5_VIEW_COUNT], *s_logo_accent;
static lv_obj_t *s_clk_d, *s_clk_t, *s_batt_fill, *s_batt_pct, *s_wifi;
static int s_batt = -1;
static bool s_wifi_off = true; /* #i-wifi-off: the wifi glyph with a slash */

static void tab_cb(lv_event_t *e)
{
    l5_ui_go((l5_view_t)(intptr_t)lv_event_get_user_data(e));
}

static void wifi_cb(lv_event_t *e)
{
    (void)e;
    l5_wifi_menu();
}

static void power_cb(lv_event_t *e)
{
    (void)e;
    l5_power_menu();
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
    d.width = 6; /* gap in the rail's colour, then the stroke */
    d.color = RAIL_BG;
    lv_draw_line(lv_event_get_layer(e), &d);
    d.width = 2;
    d.color = lv_obj_get_style_text_color(icon, 0);
    lv_draw_line(lv_event_get_layer(e), &d);
}

static lv_obj_t *hline(lv_obj_t *rail, int32_t y)
{
    lv_obj_t *l = l5_box(rail);
    lv_obj_set_pos(l, 16, y);
    lv_obj_set_size(l, L5_RAIL_W - 32, 1);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(l, L5_PANEL_LINE, 0);
    return l;
}

static lv_obj_t *rail_btn(lv_obj_t *rail, int32_t x, int32_t y, const char *icon, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(rail);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, BTN_W, BTN_H);
    lv_obj_set_ext_click_area(b, 2); /* the 4 px between the two buttons is split between them */
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_bg_color(b, L5_PANEL_3, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_center(l5_icon(b, &l5_icon_24, L5_ON_PANEL, icon));
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void tick_cb(lv_timer_t *t)
{
    static int n;
    (void)t;
    if (n++ % 6 == 0) {
        s_batt = l5_board_battery_percent(); /* every 30 s */
    }
    l5_rail_paint_status();
}

static lv_obj_t *s_rail;

/* everything on the rail, in the colours of the current theme */
static void build(lv_obj_t *rail)
{
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rail, RAIL_BG, 0);
    lv_obj_set_style_border_side(rail, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_width(rail, 1, 0);
    lv_obj_set_style_border_color(rail, L5C.rail_edge, 0);

    /* logo: TAB5, the version under it */
    lv_obj_t *logo = l5_box(rail);
    lv_obj_set_size(logo, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(logo, LV_FLEX_FLOW_ROW);
    lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 18);
    l5_label(logo, &l5_orb_xb27, L5_ON_PANEL, "TAB");
    s_logo_accent = l5_label(logo, &l5_orb_xb27, L5P.accent, "5");
    char ver[24];
    snprintf(ver, sizeof(ver), "v%.8s", esp_app_get_description()->version);
    lv_obj_t *v = l5_label(rail, &l5_mono_12, L5_ON_PANEL_MUTED, ver);
    lv_obj_set_style_text_letter_space(v, 1, 0);
    lv_obj_align(v, LV_ALIGN_TOP_MID, 0, 52);
    hline(rail, LOGO_H - 1);

    for (int i = 0; i < L5_VIEW_COUNT; i++) {
        lv_obj_t *t = lv_button_create(rail);
        lv_obj_remove_style_all(t);
        lv_obj_set_pos(t, 0, LOGO_H + i * TAB_H);
        lv_obj_set_size(t, L5_RAIL_W - 1, TAB_H); /* the rail's right border stays */
        lv_obj_set_style_pad_row(t, 6, 0);
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_text_color(t, L5_ON_PANEL_MUTED, 0);
        lv_obj_set_style_bg_color(t, L5_PANEL_3, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(t, l5_mix(L5P.accent, RAIL_BG, 40), LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_style_text_color(t, L5P.accent, LV_STATE_CHECKED);
        lv_obj_t *mark = l5_box(t); /* the accent bar on the left edge of the active tab */
        lv_obj_add_flag(mark, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(mark, 0, 0);
        lv_obj_set_size(mark, 4, TAB_H);
        lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(mark, L5P.accent, 0);
        lv_obj_t *ic = lv_label_create(t);
        lv_obj_set_style_text_font(ic, &l5_icon_24, 0);
        lv_label_set_text(ic, TABS[i].icon);
        lv_obj_t *tx = lv_label_create(t);
        lv_obj_set_style_text_font(tx, &l5_chakra_sb16, 0);
        lv_obj_set_style_text_letter_space(tx, 1, 0);
        lv_label_set_text(tx, TABS[i].text);
        lv_obj_add_event_cb(t, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_tab[i] = t;
        s_mark[i] = mark;
    }

    /* status: clock, battery, then Wi-Fi and power */
    const int32_t st = LOGO_H + L5_VIEW_COUNT * TAB_H; /* 524 */
    hline(rail, st + 8);
    s_clk_t = l5_label(rail, &l5_orb_b22, L5_ON_PANEL, "");
    lv_obj_set_style_text_letter_space(s_clk_t, 1, 0);
    lv_obj_align(s_clk_t, LV_ALIGN_TOP_MID, 0, st + 22);
    s_clk_d = l5_label(rail, &l5_mono_11, L5_ON_PANEL_MUTED, "");
    lv_obj_set_style_text_letter_space(s_clk_d, 1, 0);
    lv_obj_align(s_clk_d, LV_ALIGN_TOP_MID, 0, st + 52);

    lv_obj_t *batt = l5_box(rail); /* .batt: cell with fill + nub, percentage */
    lv_obj_set_size(batt, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(batt, 8, 0);
    lv_obj_set_flex_flow(batt, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(batt, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(batt, LV_ALIGN_TOP_MID, 0, st + 80);
    lv_obj_t *cell = l5_box(batt);
    lv_obj_set_size(cell, 30, 16);
    lv_obj_set_style_border_width(cell, 2, 0);
    lv_obj_set_style_border_color(cell, L5_ON_PANEL, 0);
    lv_obj_set_style_radius(cell, 3, 0);
    lv_obj_set_style_pad_all(cell, 2, 0);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    s_batt_fill = l5_box(cell);
    lv_obj_set_height(s_batt_fill, lv_pct(100));
    lv_obj_set_style_bg_opa(s_batt_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_batt_fill, L5_ON_PANEL, 0);
    lv_obj_t *nub = l5_box(cell);
    lv_obj_add_flag(nub, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(nub, 3, 6);
    lv_obj_set_pos(nub, 27, 1); /* right: -6px of the padding box */
    lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(nub, L5_ON_PANEL, 0);
    s_batt_pct = l5_label(batt, &l5_chakra_sb16, L5_ON_PANEL, "");

    const int32_t by = L5_SCREEN_H - 12 - BTN_H;
    lv_obj_t *wb = rail_btn(rail, (L5_RAIL_W - 2 * BTN_W - 4) / 2, by, L5_ICON_WIFI, wifi_cb);
    s_wifi = lv_obj_get_child(wb, 0);
    lv_obj_add_event_cb(s_wifi, wifi_slash_cb, LV_EVENT_DRAW_POST, NULL);
    rail_btn(rail, (L5_RAIL_W - 2 * BTN_W - 4) / 2 + BTN_W + 4, by, L5_ICON_POWER, power_cb);

}

void l5_rail_create(lv_obj_t *screen)
{
    s_rail = l5_box(screen);
    lv_obj_set_size(s_rail, L5_RAIL_W, L5_SCREEN_H);
    build(s_rail);
    s_batt = l5_board_battery_percent();
    l5_rail_paint_status();
    lv_timer_create(tick_cb, 5000, NULL);
}

void l5_rail_rebuild(void)
{
    lv_obj_clean(s_rail);
    s_wifi_off = true; /* as at the start; l5_rail_paint_status() below sets the real state */
    build(s_rail);
    l5_rail_select(l5_ui_view());
    l5_rail_paint_status();
}

int l5_rail_battery(void)
{
    return s_batt;
}

const char *const l5_days[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

bool l5_clock_now(struct tm *t)
{
    const time_t now = time(NULL);
    localtime_r(&now, t);
    return t->tm_year + 1900 >= 2024;
}

void l5_battery_text(char *out, size_t len)
{
    if (s_batt >= 0) snprintf(out, len, "BATTERY %d%%", s_batt);
    else snprintf(out, len, "USB");
}

void l5_rail_apply_accent(void)
{
    lv_obj_set_style_text_color(s_logo_accent, L5P.accent, 0);
    for (int i = 0; i < L5_VIEW_COUNT; i++) {
        lv_obj_set_style_text_color(s_tab[i], L5P.accent, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(s_tab[i], l5_mix(L5P.accent, RAIL_BG, 40), LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(s_mark[i], L5P.accent, 0);
    }
    l5_rail_paint_wifi();
}

void l5_rail_select(l5_view_t view)
{
    for (int i = 0; i < L5_VIEW_COUNT; i++) {
        lv_obj_set_state(s_tab[i], LV_STATE_CHECKED, i == (int)view);
        lv_obj_set_flag(s_mark[i], LV_OBJ_FLAG_HIDDEN, i != (int)view);
    }
}

static void set_text(lv_obj_t *l, const char *t)
{
    if (strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

void l5_rail_paint_status(void)
{
    struct tm t;
    char d[24], h[8];
    if (l5_clock_now(&t)) {
        snprintf(d, sizeof(d), "%02d.%02d %s", t.tm_mon + 1, t.tm_mday, l5_days[t.tm_wday]);
        snprintf(h, sizeof(h), "%02d:%02d", t.tm_hour, t.tm_min);
    } else {
        snprintf(d, sizeof(d), "RTC NOT SET");
        snprintf(h, sizeof(h), "--:--");
    }
    set_text(s_clk_d, d);
    set_text(s_clk_t, h);
    int pct = s_batt < 0 ? 100 : s_batt;
    if (lv_obj_get_style_width(s_batt_fill, 0) != 22 * pct / 100) lv_obj_set_width(s_batt_fill, 22 * pct / 100);
    char p[16];
    if (s_batt < 0) {
        snprintf(p, sizeof(p), "USB");
    } else {
        snprintf(p, sizeof(p), "%d%%", s_batt);
    }
    set_text(s_batt_pct, p);
    l5_rail_paint_wifi();
}

void l5_rail_paint_wifi(void)
{
    l5_wifi_status_t st;
    l5_wifi_get_status(&st);
    bool on = st.state == L5_WIFI_CONNECTED; /* .tb-btn.on */
    bool busy = st.state == L5_WIFI_STARTING || st.state == L5_WIFI_CONNECTING;
    lv_obj_set_style_text_color(s_wifi, on ? L5P.accent : busy ? L5_MUTED : L5_ON_PANEL, 0);
    if (s_wifi_off != (!on && !busy)) {
        s_wifi_off = !on && !busy;
        lv_obj_invalidate(s_wifi);
    }
}
