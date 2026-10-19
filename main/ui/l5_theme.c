#include "l5_theme.h"

l5_palette_t L5P;
l5_styles_t L5S;
lv_font_t l5_mono13_tall, l5_mono15_tall, l5_mono18_tall;

/* src with a line that holds its fallback font's tallest letters too */
static void tall_font(lv_font_t *f, const lv_font_t *src)
{
    *f = *src;
    const lv_font_t *fb = src->fallback;
    if (fb) {
        int32_t above = LV_MAX(src->line_height - src->base_line, fb->line_height - fb->base_line);
        f->base_line = LV_MAX(src->base_line, fb->base_line);
        f->line_height = above + f->base_line;
    }
}

lv_color_t l5_mix(lv_color_t a, lv_color_t b, uint8_t a_weight)
{
    return lv_color_mix(a, b, a_weight);
}

static void grad(lv_style_t *s, lv_color_t top, lv_color_t bottom)
{
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_bg_color(s, top);
    lv_style_set_bg_grad_color(s, bottom);
    lv_style_set_bg_grad_dir(s, LV_GRAD_DIR_VER);
}

static void palette(uint32_t accent_hex)
{
    L5P.accent = lv_color_hex(accent_hex);
    uint8_t r = (accent_hex >> 16) & 0xff, g = (accent_hex >> 8) & 0xff, b = accent_hex & 0xff;
    /* app.js applyLook: --accent-ink */
    L5P.accent_ink = (r * 30 + g * 59 + b * 11) / 100 > 165 ? lv_color_hex(0x111111) : L5_WHITE;
    L5P.accent_lo = l5_mix(L5P.accent, L5_PANEL, 199);
}

static void accent_styles(void)
{
    lv_style_set_bg_color(&L5S.act_primary, L5P.accent);
    lv_style_set_bg_grad_color(&L5S.act_primary, L5P.accent_lo);
    lv_style_set_shadow_color(&L5S.act_primary, L5P.accent);
    lv_style_set_bg_color(&L5S.dot, L5P.accent);
    lv_style_set_shadow_color(&L5S.dot, L5P.accent);
    lv_style_set_border_color(&L5S.slot_sel, L5P.accent);
    lv_style_set_shadow_color(&L5S.slot_sel, L5P.accent);
    lv_style_set_bg_color(&L5S.meter_ind, L5P.accent);
    lv_style_set_bg_color(&L5S.pick, L5P.accent);
    lv_style_set_text_color(&L5S.pick, L5P.accent_ink);
    lv_style_set_bg_color(&L5S.menu_btn_pr, L5P.accent);
    lv_style_set_border_color(&L5S.toast, L5P.accent);
    lv_style_set_bg_color(&L5S.press_tint, L5P.accent);
    lv_style_set_bg_color(&L5S.press_fill, L5P.accent);
    lv_style_set_bg_color(&L5S.tgl_on, L5P.accent);
    lv_style_set_shadow_color(&L5S.tgl_on, L5P.accent);
}

void l5_theme_init(uint32_t accent_hex)
{
    palette(accent_hex);
    tall_font(&l5_mono13_tall, &l5_mono_13);
    tall_font(&l5_mono15_tall, &l5_mono_15);
    tall_font(&l5_mono18_tall, &l5_mono_18);

    lv_style_init(&L5S.bare);
    lv_style_set_pad_all(&L5S.bare, 0);
    lv_style_set_border_width(&L5S.bare, 0);
    lv_style_set_bg_opa(&L5S.bare, LV_OPA_TRANSP);
    lv_style_set_radius(&L5S.bare, 0);

    /* .plate: light machined plate */
    lv_style_init(&L5S.plate);
    grad(&L5S.plate, L5_PLATE_HI, L5_PLATE);
    lv_style_set_bg_grad_stop(&L5S.plate, 153); /* plate-hi -> plate at 60% */
    lv_style_set_border_side(&L5S.plate, LV_BORDER_SIDE_TOP);
    lv_style_set_border_color(&L5S.plate, L5_WHITE);
    lv_style_set_border_width(&L5S.plate, 1);
    lv_style_set_text_color(&L5S.plate, L5_INK);

    /* .dark: black instrument panel */
    lv_style_init(&L5S.dark);
    grad(&L5S.dark, L5_PANEL_2, L5_PANEL);
    lv_style_set_border_color(&L5S.dark, L5_BLACK);
    lv_style_set_border_width(&L5S.dark, 1);
    lv_style_set_text_color(&L5S.dark, L5_ON_PANEL);

    /* .col-h */
    lv_style_init(&L5S.col_h);
    lv_style_set_height(&L5S.col_h, 52);
    lv_style_set_width(&L5S.col_h, lv_pct(100));
    lv_style_set_pad_left(&L5S.col_h, 18);
    lv_style_set_pad_right(&L5S.col_h, 12);
    lv_style_set_pad_column(&L5S.col_h, 10);
    lv_style_set_border_side(&L5S.col_h, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_width(&L5S.col_h, 1);
    lv_style_set_border_color(&L5S.col_h, L5_PANEL_LINE);
    lv_style_set_layout(&L5S.col_h, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&L5S.col_h, LV_FLEX_FLOW_ROW);
    lv_style_set_flex_cross_place(&L5S.col_h, LV_FLEX_ALIGN_CENTER);
    lv_style_set_text_font(&L5S.col_h, &l5_mono_15);
    lv_style_set_text_letter_space(&L5S.col_h, 2);
    lv_style_set_text_color(&L5S.col_h, L5_ON_PANEL);

    /* .act and variants */
    lv_style_init(&L5S.act);
    grad(&L5S.act, lv_color_hex(0x2b2d32), lv_color_hex(0x141518));
    lv_style_set_radius(&L5S.act, 4);
    lv_style_set_height(&L5S.act, 60);
    lv_style_set_text_color(&L5S.act, L5_ON_PANEL);
    lv_style_set_text_font(&L5S.act, &l5_mono_15);
    lv_style_set_text_letter_space(&L5S.act, 1);
    lv_style_set_shadow_color(&L5S.act, L5_BLACK);
    lv_style_set_shadow_width(&L5S.act, 2);
    lv_style_set_shadow_offset_y(&L5S.act, 3);
    lv_style_set_shadow_opa(&L5S.act, LV_OPA_COVER);
    lv_style_set_pad_hor(&L5S.act, 20);
    lv_style_set_pad_column(&L5S.act, 10);
    lv_style_set_layout(&L5S.act, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&L5S.act, LV_FLEX_FLOW_ROW);
    lv_style_set_flex_main_place(&L5S.act, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_cross_place(&L5S.act, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_track_place(&L5S.act, LV_FLEX_ALIGN_CENTER);

    lv_style_init(&L5S.act_pressed);
    lv_style_set_translate_y(&L5S.act_pressed, 2);
    lv_style_set_shadow_offset_y(&L5S.act_pressed, 1);

    lv_style_init(&L5S.act_primary);
    lv_style_set_bg_grad_dir(&L5S.act_primary, LV_GRAD_DIR_VER);
    lv_style_set_shadow_width(&L5S.act_primary, 14);
    lv_style_set_shadow_offset_y(&L5S.act_primary, 0);
    lv_style_set_shadow_opa(&L5S.act_primary, 115);
    lv_style_set_text_color(&L5S.act_primary, L5_WHITE);

    lv_style_init(&L5S.act_light);
    grad(&L5S.act_light, lv_color_hex(0xfdfdfd), L5_PLATE);
    lv_style_set_text_color(&L5S.act_light, L5_INK);
    lv_style_set_shadow_color(&L5S.act_light, L5_PLATE_EDGE);
    lv_style_set_border_width(&L5S.act_light, 1);
    lv_style_set_border_color(&L5S.act_light, L5_PLATE_LO);

    lv_style_init(&L5S.act_disabled);
    lv_style_set_opa(&L5S.act_disabled, 89);

    /* .dot */
    lv_style_init(&L5S.dot);
    lv_style_set_size(&L5S.dot, 10, 10);
    lv_style_set_radius(&L5S.dot, LV_RADIUS_CIRCLE);
    lv_style_set_bg_opa(&L5S.dot, LV_OPA_COVER);
    lv_style_set_shadow_width(&L5S.dot, 8);
    lv_style_set_shadow_opa(&L5S.dot, 204);
    lv_style_set_border_width(&L5S.dot, 0);

    /* .slot card */
    lv_style_init(&L5S.slot);
    grad(&L5S.slot, lv_color_hex(0x24262b), lv_color_hex(0x16171a));
    lv_style_set_border_width(&L5S.slot, 1);
    lv_style_set_border_color(&L5S.slot, lv_color_hex(0x3a3d43));
    lv_style_set_radius(&L5S.slot, 4);
    lv_style_set_shadow_color(&L5S.slot, L5_BLACK);
    lv_style_set_shadow_width(&L5S.slot, 2);
    lv_style_set_shadow_offset_y(&L5S.slot, 3);
    lv_style_set_shadow_opa(&L5S.slot, LV_OPA_COVER);
    lv_style_set_pad_all(&L5S.slot, 0); /* padding 40/18/16 lives in the card's inner column */
    lv_style_set_text_color(&L5S.slot, L5_ON_PANEL);

    lv_style_init(&L5S.slot_sel);
    lv_style_set_shadow_width(&L5S.slot_sel, 24);
    lv_style_set_shadow_offset_y(&L5S.slot_sel, 0);
    lv_style_set_shadow_spread(&L5S.slot_sel, 1);
    lv_style_set_shadow_opa(&L5S.slot_sel, 90);

    lv_style_init(&L5S.meter);
    lv_style_set_bg_opa(&L5S.meter, LV_OPA_COVER);
    lv_style_set_bg_color(&L5S.meter, lv_color_hex(0x33363c));
    lv_style_set_radius(&L5S.meter, 0);
    lv_style_set_height(&L5S.meter, 8);
    lv_style_init(&L5S.meter_ind);
    lv_style_set_bg_opa(&L5S.meter_ind, LV_OPA_COVER);
    lv_style_set_radius(&L5S.meter_ind, 0);

    lv_style_init(&L5S.pick);
    lv_style_set_bg_opa(&L5S.pick, LV_OPA_COVER);
    lv_style_set_radius(&L5S.pick, 2);
    lv_style_set_pad_hor(&L5S.pick, 10);
    lv_style_set_pad_ver(&L5S.pick, 5);
    lv_style_set_text_font(&L5S.pick, &l5_mono_12);
    lv_style_set_text_letter_space(&L5S.pick, 1);

    lv_style_init(&L5S.scrim);
    lv_style_set_bg_opa(&L5S.scrim, 158);
    lv_style_set_bg_color(&L5S.scrim, lv_color_hex(0x08090b));
    lv_style_set_border_width(&L5S.scrim, 0);
    lv_style_set_radius(&L5S.scrim, 0);

    lv_style_init(&L5S.menu_btn);
    lv_style_set_bg_opa(&L5S.menu_btn, LV_OPA_TRANSP);
    lv_style_set_height(&L5S.menu_btn, 62);
    lv_style_set_width(&L5S.menu_btn, lv_pct(100));
    lv_style_set_pad_hor(&L5S.menu_btn, 28);
    lv_style_set_pad_column(&L5S.menu_btn, 16);
    lv_style_set_border_side(&L5S.menu_btn, LV_BORDER_SIDE_TOP);
    lv_style_set_border_width(&L5S.menu_btn, 1);
    lv_style_set_border_color(&L5S.menu_btn, L5_PANEL_LINE);
    lv_style_set_text_font(&L5S.menu_btn, &l5_chakra_sb19);
    lv_style_set_text_color(&L5S.menu_btn, L5_ON_PANEL);
    lv_style_set_layout(&L5S.menu_btn, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&L5S.menu_btn, LV_FLEX_FLOW_ROW);
    lv_style_set_flex_cross_place(&L5S.menu_btn, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_track_place(&L5S.menu_btn, LV_FLEX_ALIGN_CENTER);
    lv_style_init(&L5S.menu_btn_pr);
    lv_style_set_bg_opa(&L5S.menu_btn_pr, 64);

    lv_style_init(&L5S.toast);
    lv_style_set_bg_opa(&L5S.toast, LV_OPA_COVER);
    lv_style_set_bg_color(&L5S.toast, lv_color_hex(0x0e0f11));
    lv_style_set_text_color(&L5S.toast, L5_WHITE);
    lv_style_set_text_font(&L5S.toast, &l5_chakra_sb17);
    lv_style_set_radius(&L5S.toast, 4);
    lv_style_set_pad_ver(&L5S.toast, 14);
    lv_style_set_pad_hor(&L5S.toast, 24);
    lv_style_set_border_side(&L5S.toast, LV_BORDER_SIDE_LEFT);
    lv_style_set_border_width(&L5S.toast, 4);
    lv_style_set_shadow_color(&L5S.toast, L5_BLACK);
    lv_style_set_shadow_width(&L5S.toast, 24);
    lv_style_set_shadow_offset_y(&L5S.toast, 8);
    lv_style_set_shadow_opa(&L5S.toast, 102);

    /* feedback and state styles shared by the views: an accent change restyles them in place */
    lv_style_init(&L5S.press_tint); /* row:active */
    lv_style_set_bg_opa(&L5S.press_tint, 64);
    lv_style_init(&L5S.press_fill); /* .step button:active */
    lv_style_set_bg_opa(&L5S.press_fill, LV_OPA_COVER);
    lv_style_init(&L5S.press_ring); /* swatch, card, flash map segment:active */
    lv_style_set_outline_width(&L5S.press_ring, 2);
    lv_style_set_outline_color(&L5S.press_ring, L5_WHITE);
    lv_style_set_outline_opa(&L5S.press_ring, 170);
    lv_style_init(&L5S.tgl_on); /* .tgl.on */
    lv_style_set_shadow_width(&L5S.tgl_on, 10);
    lv_style_set_shadow_opa(&L5S.tgl_on, 128);
    lv_style_init(&L5S.swatch_sel); /* swatch [aria-pressed]: white ring + black ring */
    lv_style_set_outline_width(&L5S.swatch_sel, 3);
    lv_style_set_outline_color(&L5S.swatch_sel, L5_WHITE);
    lv_style_set_shadow_width(&L5S.swatch_sel, 0);
    lv_style_set_shadow_spread(&L5S.swatch_sel, 5);
    lv_style_set_shadow_color(&L5S.swatch_sel, L5_BLACK);
    lv_style_set_shadow_opa(&L5S.swatch_sel, LV_OPA_COVER);

    accent_styles();
}

void l5_theme_set_accent(uint32_t accent_hex)
{
    palette(accent_hex);
    accent_styles();
    lv_obj_report_style_change(NULL);
}

/* ---------- helpers ---------- */

/* A layout box: no style, no scrolling, and not clickable, so a tap on it reaches the clickable
   row, card or button around it (in the prototype the click bubbles up to the row's handler).
   A clickable lv_obj without a handler swallowed taps: the text column of a CFG row took most taps
   on its row. Boxes that handle taps, block them (scrims, modals) or scroll add the flag themselves. */
lv_obj_t *l5_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *l5_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

lv_obj_t *l5_icon(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *icon)
{
    lv_obj_t *l = l5_label(parent, font, color, icon);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

lv_obj_t *l5_dot(lv_obj_t *parent)
{
    lv_obj_t *d = l5_box(parent);
    lv_obj_add_style(d, &L5S.dot, 0);
    return d;
}

lv_obj_t *l5_col_header(lv_obj_t *parent, const char *title, const char *tag)
{
    lv_obj_t *h = l5_box(parent);
    lv_obj_add_style(h, &L5S.col_h, 0);
    l5_dot(h);
    lv_obj_t *t = lv_label_create(h);
    lv_label_set_text(t, title);
    lv_obj_t *sp = l5_box(h);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_set_height(sp, 1);
    lv_obj_t *tg = l5_label(h, &l5_mono_12, L5_ON_PANEL_MUTED, tag);
    lv_obj_set_style_text_letter_space(tg, 1, 0);
    return tg;
}

lv_obj_t *l5_act_button(lv_obj_t *parent, const char *icon, const char *text, bool primary)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_add_style(b, &L5S.act, 0);
    if (primary) {
        lv_obj_add_style(b, &L5S.act_primary, 0);
    }
    lv_obj_add_style(b, &L5S.act_pressed, LV_STATE_PRESSED);
    lv_obj_add_style(b, &L5S.act_disabled, LV_STATE_DISABLED);
    if (icon) {
        l5_icon(b, &l5_icon_20, primary ? L5_WHITE : L5_ON_PANEL, icon);
    }
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(l, 300, 0);
    return b;
}

lv_obj_t *l5_hline(lv_obj_t *parent, lv_color_t color)
{
    lv_obj_t *l = l5_box(parent);
    lv_obj_set_size(l, lv_pct(100), 1);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(l, color, 0);
    return l;
}

/* ---------- clip-path emulation ----------
   The prototype clips panels with CSS polygons. LVGL has no polygon clip, so the cut
   corners are painted over in the colour behind the panel after the panel is drawn.
   The user data packs the size (bits 0-6), a flag (bit 7) and the RGB colour (bits 8-31). */

static uintptr_t pack(int32_t size, bool flag, lv_color_t c)
{
    return (uintptr_t)(size & 0x7f) | (flag ? 0x80u : 0u) | ((uintptr_t)(lv_color_to_u32(c) & 0xffffff) << 8);
}

static void tri(lv_layer_t *layer, lv_color_t c, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.color = c;
    d.opa = LV_OPA_COVER;
    d.p[0].x = x0; d.p[0].y = y0;
    d.p[1].x = x1; d.p[1].y = y1;
    d.p[2].x = x2; d.p[2].y = y2;
    lv_draw_triangle(layer, &d);
}

static void edge(lv_layer_t *layer, lv_color_t c, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = c;
    d.width = 1;
    d.p1.x = x0; d.p1.y = y0;
    d.p2.x = x1; d.p2.y = y1;
    lv_draw_line(layer, &d);
}

static void chamfer_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    uintptr_t u = (uintptr_t)lv_event_get_user_data(e);
    int32_t s = u & 0x7f;
    lv_color_t bg = lv_color_hex((uint32_t)(u >> 8));
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    tri(layer, bg, a.x1, a.y1, a.x1 + s, a.y1, a.x1, a.y1 + s);
    tri(layer, bg, a.x2 + 1, a.y2 + 1 - s, a.x2 + 1, a.y2 + 1, a.x2 + 1 - s, a.y2 + 1);
    if (lv_obj_get_style_border_width(obj, 0) > 0) {
        lv_color_t bc = lv_obj_get_style_border_color(obj, 0);
        edge(layer, bc, a.x1, a.y1 + s, a.x1 + s, a.y1);
        edge(layer, bc, a.x2 - s, a.y2, a.x2, a.y2 - s);
    }
}

void l5_chamfer(lv_obj_t *obj, int32_t size, lv_color_t bg)
{
    lv_obj_add_event_cb(obj, chamfer_cb, LV_EVENT_DRAW_POST, (void *)pack(size, false, bg));
}


static void dashed_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_color_t c = lv_color_hex((uint32_t)(uintptr_t)lv_event_get_user_data(e));
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = c;
    d.width = 1;
    d.dash_width = 6;
    d.dash_gap = 5;
    const int32_t pts[4][4] = {
        {a.x1, a.y1, a.x2, a.y1}, {a.x2, a.y1, a.x2, a.y2}, {a.x1, a.y2, a.x2, a.y2}, {a.x1, a.y1, a.x1, a.y2},
    };
    for (int i = 0; i < 4; i++) {
        d.p1.x = pts[i][0]; d.p1.y = pts[i][1];
        d.p2.x = pts[i][2]; d.p2.y = pts[i][3];
        lv_draw_line(layer, &d);
    }
}

void l5_dashed_border(lv_obj_t *obj, lv_color_t color)
{
    lv_obj_add_event_cb(obj, dashed_cb, LV_EVENT_DRAW_POST, (void *)(uintptr_t)(lv_color_to_u32(color) & 0xffffff));
}
