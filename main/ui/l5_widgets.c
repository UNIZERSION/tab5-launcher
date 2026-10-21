/* Building blocks of the two-pane views (launcher.css): SD and OTA share them. */
#include <stdio.h>
#include <string.h>
#include "l5_slots.h"
#include "l5_ui.h"

/* the sentence for a layout change that moves the app slots: which apps go */
void l5w_apps_erased(char *out, size_t len)
{
    char apps[200];
    int n = l5_slots_list(apps, sizeof(apps));
    if (!n) {
        snprintf(out, len, "The app slots change (no app is installed).");
    } else {
        snprintf(out, len, "%s will be erased: install %s again from SD or OTA afterwards.", apps, n == 1 ? "it" : "them");
    }
}

lv_obj_t *l5w_pane(lv_obj_t *view, bool dark, int32_t x, int32_t w)
{
    lv_obj_t *p = l5_box(view);
    l5_panel(p, dark);
    lv_obj_set_pos(p, x, L5_PANEL_Y);
    lv_obj_set_size(p, w, L5_PANEL_H);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    l5_chamfer(p, L5_CHAMFER, L5_PLATE);
    return p;
}

/* single-line label cut with dots */
lv_obj_t *l5w_line_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = l5_label(parent, font, color, text);
    lv_obj_set_height(l, lv_font_get_line_height(font));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    return l;
}

/* .empty */
void l5w_empty(lv_obj_t *parent, const char *title, const char *body)
{
    lv_obj_t *box = l5_box(parent);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(box, 60, 0);
    lv_obj_set_style_pad_hor(box, 30, 0);
    lv_obj_set_style_pad_row(box, 6, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    l5_label(box, &l5_chakra_b24, L5_ON_PANEL, title);
    lv_obj_t *b = l5_label(box, &l5_chakra_r17, L5_ON_PANEL_MUTED, body);
    lv_obj_set_width(b, lv_pct(100));
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(b, 6, 0);
}

/* .chip, aria-pressed = on */
lv_obj_t *l5w_chip(lv_obj_t *parent, const char *icon, const char *text, bool on, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 46);
    lv_obj_set_style_pad_hor(b, 14, 0);
    lv_obj_set_style_pad_column(b, 8, 0);
    lv_obj_set_style_radius(b, 3, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, on ? L5P.accent : lv_color_hex(0x474a51), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, on ? l5_mix(L5P.accent, lv_color_hex(0x1a1b1f), 89) : lv_color_hex(0x1a1b1f), 0);
    lv_obj_set_style_bg_color(b, L5_PANEL_3, LV_STATE_PRESSED);
    lv_obj_set_style_opa(b, 102, LV_STATE_DISABLED);
    lv_obj_set_style_text_font(b, &l5_mono_14, 0);
    lv_obj_set_style_text_letter_space(b, 1, 0);
    lv_obj_set_style_text_color(b, on ? L5_WHITE : L5_ON_PANEL, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (icon) {
        l5_icon(b, &l5_icon_20, on ? L5_WHITE : L5_ON_PANEL, icon);
    }
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

/* .kv: dt 110 px, dd wraps */
lv_obj_t *l5w_kv(lv_obj_t *grid, const char *k, const char *v, bool accent)
{
    lv_obj_t *r = l5_box(grid);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(r, 12, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_t *dt = l5_label(r, &l5_mono_15, L5_MUTED, k);
    lv_obj_set_width(dt, 110);
    lv_obj_set_style_text_letter_space(dt, 1, 0);
    lv_obj_t *dd = l5_label(r, &l5_mono_15, accent ? L5P.accent : L5_INK, v);
    lv_obj_set_flex_grow(dd, 1);
    lv_label_set_long_mode(dd, LV_LABEL_LONG_WRAP);
    return dd;
}

/* .frow.sel: inset 4px accent bar on the left */
void l5w_row_sel_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    if (!lv_obj_has_state(o, LV_STATE_CHECKED)) {
        return;
    }
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    a.x2 = a.x1 + 3;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = L5P.accent;
    d.bg_opa = LV_OPA_COVER;
    lv_draw_rect(lv_event_get_layer(e), &d, &a);
}

lv_obj_t *l5w_side_head(lv_obj_t *s)
{
    lv_obj_t *h = l5_box(s); /* .side-head */
    lv_obj_set_size(h, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(h, 22, 0);
    lv_obj_set_style_pad_hor(h, 22, 0);
    lv_obj_set_style_pad_column(h, 16, 0);
    lv_obj_set_flex_flow(h, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    return h;
}

void l5w_big_icon(lv_obj_t *h, const char *icon)
{
    lv_obj_t *b = l5_box(h); /* .bigic */
    lv_obj_set_size(b, 132, 132);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, L5_PANEL_2, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, L5_PANEL_LINE, 0);
    lv_obj_set_style_outline_width(b, 1, 0);
    lv_obj_set_style_outline_color(b, L5_BLACK, 0);
    lv_obj_set_style_shadow_width(b, 10, 0);
    lv_obj_set_style_shadow_offset_y(b, 4, 0);
    lv_obj_set_style_shadow_opa(b, 77, 0);
    lv_obj_set_style_shadow_color(b, L5_BLACK, 0);
    lv_obj_center(l5_icon(b, &l5_icon_64, L5_ON_PANEL_MUTED, icon));
}

void l5w_head_text(lv_obj_t *h, const char *kind, const char *title, const char *sub)
{
    lv_obj_t *tt = l5_box(h); /* .tt */
    lv_obj_set_height(tt, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(tt, 1);
    lv_obj_set_style_pad_row(tt, 6, 0);
    lv_obj_set_flex_flow(tt, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *k = l5_box(tt);
    lv_obj_set_size(k, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(k, 8, 0);
    lv_obj_set_flex_flow(k, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(k, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    l5_dot(k);
    lv_obj_t *kl = l5_label(k, &l5_mono_13, L5_MUTED, kind);
    lv_obj_set_style_text_letter_space(kl, 2, 0);
    lv_obj_t *b = l5_label(tt, &l5_chakra_b26, L5_INK, title);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_set_style_pad_top(b, 2, 0);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    if (sub && *sub) {
        lv_obj_t *sp = l5_label(tt, &l5_chakra_sb17, L5_MUTED, sub);
        lv_obj_set_width(sp, lv_pct(100));
        lv_label_set_long_mode(sp, LV_LABEL_LONG_WRAP);
    }
}

lv_obj_t *l5w_kv_grid(lv_obj_t *s)
{
    lv_obj_t *g = l5_box(s); /* .kv margin 16px 22px 0 */
    lv_obj_set_size(g, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(g, 16, 0);
    lv_obj_set_style_pad_hor(g, 22, 0);
    lv_obj_set_style_pad_row(g, 6, 0);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
    return g;
}
