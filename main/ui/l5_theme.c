#include <string.h>
#include "l5_theme.h"

l5_palette_t L5P;
l5_styles_t L5S;
l5_colors_t L5C;
lv_font_t l5_mono13_tall, l5_mono15_tall, l5_mono18_tall;

/* ---------- themes ---------- */

#define H(x) LV_COLOR_MAKE(((x) >> 16) & 0xff, ((x) >> 8) & 0xff, (x) & 0xff)

const l5_theme_info_t l5_themes[L5_THEME_COUNT] = {
    [L5_THEME_GRAPHITE] = {"graphite", "Graphite"},
    [L5_THEME_MIDNIGHT] = {"midnight", "Midnight"},
    [L5_THEME_DAYLIGHT] = {"daylight", "Daylight"},
};

/* Every field is set in every theme (tools/host_test: shots --taps checks that none is left at 0 by mistake). */
static const l5_colors_t THEMES[L5_THEME_COUNT] = {
    /* the launcher as it was designed: a light machined plate around black instrument panels */
    [L5_THEME_GRAPHITE] = {
        .plate = H(0xe3e4e7), .plate_hi = H(0xf7f7f8), .plate_lo = H(0xc5c7cc), .plate_edge = H(0xa7aab0),
        .plate_bottom = H(0xdcdde1), .shine = H(0xffffff),
        .ink = H(0x0f1012), .ink_soft = H(0x3a3d43), .muted = H(0x6f737c),
        .panel = H(0x141518), .panel_2 = H(0x1d1f23), .panel_3 = H(0x292b30), .panel_line = H(0x34373d),
        .panel_edge = H(0x000000), .on_panel = H(0xf2f3f5), .on_panel_muted = H(0x8e939c),
        .btn_top = H(0x2b2d32), .btn_bot = H(0x141518), .btn_shadow = H(0x000000), .light_top = H(0xfdfdfd),
        .slot_top = H(0x24262b), .slot_bot = H(0x16171a), .edge = H(0x3a3d43), .meter = H(0x33363c),
        .track = H(0x2b2d32), .well = H(0x0b0c0e), .well_ink = H(0xd7d9dd), .scrim = H(0x08090b), .dim = H(0x4a4d54),
        .chip = H(0x1a1b1f), .chip_edge = H(0x474a51),
        .key_top = H(0x2e3035), .key_bot = H(0x1b1c20), .keyfn_top = H(0x3a3c42), .keyfn_bot = H(0x26282c),
        .knob = H(0xd9dbdf), .ring = H(0xffffff),
        .rail = H(0x16171a), .rail_edge = H(0x000000), .boot_top = H(0x202227), .boot_bot = H(0x0b0c0e),
        .danger = H(0xff6b6b), .warn = H(0xf0a400),
    },
    /* dark everywhere: the plate and the side panes go dark too, the text on them light */
    [L5_THEME_MIDNIGHT] = {
        .plate = H(0x0a0b0d), .plate_hi = H(0x1a1c20), .plate_lo = H(0x2a2c31), .plate_edge = H(0x3a3d43),
        .plate_bottom = H(0x0e0f12), .shine = H(0x2a2c31),
        .ink = H(0xe9eaed), .ink_soft = H(0xb3b7be), .muted = H(0x7c818a),
        .panel = H(0x121316), .panel_2 = H(0x1a1c20), .panel_3 = H(0x26282d), .panel_line = H(0x2e3036),
        .panel_edge = H(0x000000), .on_panel = H(0xf2f3f5), .on_panel_muted = H(0x8a8f98),
        .btn_top = H(0x27292e), .btn_bot = H(0x121316), .btn_shadow = H(0x000000), .light_top = H(0x2e3035),
        .slot_top = H(0x1f2125), .slot_bot = H(0x131417), .edge = H(0x34373d), .meter = H(0x2e3036),
        .track = H(0x26282d), .well = H(0x08090b), .well_ink = H(0xd7d9dd), .scrim = H(0x000000), .dim = H(0x44474e),
        .chip = H(0x16171a), .chip_edge = H(0x3d4047),
        .key_top = H(0x2a2c30), .key_bot = H(0x17181b), .keyfn_top = H(0x34363c), .keyfn_bot = H(0x222428),
        .knob = H(0xd9dbdf), .ring = H(0xffffff),
        .rail = H(0x0f1013), .rail_edge = H(0x000000), .boot_top = H(0x16171b), .boot_bot = H(0x050506),
        .danger = H(0xff6b6b), .warn = H(0xf0a400),
    },
    /* light panels with dark text on a mid-grey plate; toasts and the cover art stay dark */
    [L5_THEME_DAYLIGHT] = {
        .plate = H(0xc8cbd1), .plate_hi = H(0xe6e8ec), .plate_lo = H(0xb0b4bb), .plate_edge = H(0x959aa2),
        .plate_bottom = H(0xc2c5cb), .shine = H(0xf4f5f7),
        .ink = H(0x111316), .ink_soft = H(0x3a3d43), .muted = H(0x5c6069),
        .panel = H(0xebedf0), .panel_2 = H(0xfbfbfc), .panel_3 = H(0xdde0e4), .panel_line = H(0xcfd3d8),
        .panel_edge = H(0xaeb2b9), .on_panel = H(0x15171a), .on_panel_muted = H(0x5e636c),
        .btn_top = H(0xffffff), .btn_bot = H(0xe3e6ea), .btn_shadow = H(0xa3a7af), .light_top = H(0xffffff),
        .slot_top = H(0xffffff), .slot_bot = H(0xeef0f3), .edge = H(0xbfc3ca), .meter = H(0xd3d6db),
        .track = H(0xd8dbe0), .well = H(0xf4f5f7), .well_ink = H(0x2a2d32), .scrim = H(0x1b1d21), .dim = H(0xa4a8b0),
        .chip = H(0xf4f5f7), .chip_edge = H(0xbcc0c7),
        .key_top = H(0xffffff), .key_bot = H(0xe6e8ec), .keyfn_top = H(0xd9dce1), .keyfn_bot = H(0xc8ccd2),
        .knob = H(0xffffff), .ring = H(0x15171a),
        .rail = H(0xf2f3f5), .rail_edge = H(0xb8bcc3), .boot_top = H(0xf7f8fa), .boot_bot = H(0xd9dce1),
        .danger = H(0xc62828), .warn = H(0xa86400), /* readable on the light panels */
    },
};

static int s_theme;
static uint32_t s_gen;
static uint32_t s_accent_hex;
static bool s_built; /* the styles exist: a rebuild resets them first (lv_style_init would leak their memory) */
#define STYLE_INIT(st) (s_built ? lv_style_reset(st) : lv_style_init(st))

int l5_theme_current(void)
{
    return s_theme;
}

uint32_t l5_theme_gen(void)
{
    return s_gen;
}

int l5_theme_find(const char *id)
{
    for (int i = 0; id && i < L5_THEME_COUNT; i++) {
        if (strcmp(id, l5_themes[i].id) == 0) return i;
    }
    return -1;
}

const l5_colors_t *l5_theme_colors(int theme)
{
    return &THEMES[theme >= 0 && theme < L5_THEME_COUNT ? theme : L5_THEME_GRAPHITE];
}

static void colors(int theme)
{
    s_theme = theme >= 0 && theme < L5_THEME_COUNT ? theme : L5_THEME_GRAPHITE;
    L5C = THEMES[s_theme];
}

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
    s_accent_hex = accent_hex;
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
    lv_style_set_outline_color(&L5S.focus, L5P.accent);
}

static void build_styles(void)
{
    STYLE_INIT(&L5S.bare);
    lv_style_set_pad_all(&L5S.bare, 0);
    lv_style_set_border_width(&L5S.bare, 0);
    lv_style_set_bg_opa(&L5S.bare, LV_OPA_TRANSP);
    lv_style_set_radius(&L5S.bare, 0);

    /* .plate: light machined plate */
    STYLE_INIT(&L5S.plate);
    grad(&L5S.plate, L5_PLATE_HI, L5_PLATE);
    lv_style_set_bg_grad_stop(&L5S.plate, 153); /* plate-hi -> plate at 60% */
    lv_style_set_border_side(&L5S.plate, LV_BORDER_SIDE_TOP);
    lv_style_set_border_color(&L5S.plate, L5C.shine);
    lv_style_set_border_width(&L5S.plate, 1);
    lv_style_set_text_color(&L5S.plate, L5_INK);

    /* .dark: black instrument panel */
    STYLE_INIT(&L5S.dark);
    grad(&L5S.dark, L5_PANEL_2, L5_PANEL);
    lv_style_set_border_color(&L5S.dark, L5C.panel_edge);
    lv_style_set_border_width(&L5S.dark, 1);
    lv_style_set_text_color(&L5S.dark, L5_ON_PANEL);

    /* .col-h */
    STYLE_INIT(&L5S.col_h);
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
    STYLE_INIT(&L5S.act);
    grad(&L5S.act, L5C.btn_top, L5C.btn_bot);
    lv_style_set_radius(&L5S.act, 4);
    lv_style_set_height(&L5S.act, 60);
    lv_style_set_text_color(&L5S.act, L5_ON_PANEL);
    lv_style_set_text_font(&L5S.act, &l5_mono_15);
    lv_style_set_text_letter_space(&L5S.act, 1);
    lv_style_set_shadow_color(&L5S.act, L5C.btn_shadow);
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

    STYLE_INIT(&L5S.act_pressed);
    lv_style_set_translate_y(&L5S.act_pressed, 2);
    lv_style_set_shadow_offset_y(&L5S.act_pressed, 1);

    STYLE_INIT(&L5S.act_primary);
    lv_style_set_bg_grad_dir(&L5S.act_primary, LV_GRAD_DIR_VER);
    lv_style_set_shadow_width(&L5S.act_primary, 14);
    lv_style_set_shadow_offset_y(&L5S.act_primary, 0);
    lv_style_set_shadow_opa(&L5S.act_primary, 115);
    lv_style_set_text_color(&L5S.act_primary, L5_WHITE);

    STYLE_INIT(&L5S.act_light);
    grad(&L5S.act_light, L5C.light_top, L5_PLATE);
    lv_style_set_text_color(&L5S.act_light, L5_INK);
    lv_style_set_shadow_color(&L5S.act_light, L5_PLATE_EDGE);
    lv_style_set_border_width(&L5S.act_light, 1);
    lv_style_set_border_color(&L5S.act_light, L5_PLATE_LO);

    STYLE_INIT(&L5S.act_disabled);
    lv_style_set_opa(&L5S.act_disabled, 89);

    /* .dot */
    STYLE_INIT(&L5S.dot);
    lv_style_set_size(&L5S.dot, 10, 10);
    lv_style_set_radius(&L5S.dot, LV_RADIUS_CIRCLE);
    lv_style_set_bg_opa(&L5S.dot, LV_OPA_COVER);
    lv_style_set_shadow_width(&L5S.dot, 8);
    lv_style_set_shadow_opa(&L5S.dot, 204);
    lv_style_set_border_width(&L5S.dot, 0);

    /* .slot card */
    STYLE_INIT(&L5S.slot);
    grad(&L5S.slot, L5C.slot_top, L5C.slot_bot);
    lv_style_set_border_width(&L5S.slot, 1);
    lv_style_set_border_color(&L5S.slot, L5C.edge);
    lv_style_set_radius(&L5S.slot, 4);
    lv_style_set_shadow_color(&L5S.slot, L5C.btn_shadow);
    lv_style_set_shadow_width(&L5S.slot, 2);
    lv_style_set_shadow_offset_y(&L5S.slot, 3);
    lv_style_set_shadow_opa(&L5S.slot, LV_OPA_COVER);
    lv_style_set_pad_all(&L5S.slot, 0); /* padding 40/18/16 lives in the card's inner column */
    lv_style_set_text_color(&L5S.slot, L5_ON_PANEL);

    STYLE_INIT(&L5S.slot_sel);
    lv_style_set_shadow_width(&L5S.slot_sel, 24);
    lv_style_set_shadow_offset_y(&L5S.slot_sel, 0);
    lv_style_set_shadow_spread(&L5S.slot_sel, 1);
    lv_style_set_shadow_opa(&L5S.slot_sel, 90);

    STYLE_INIT(&L5S.meter);
    lv_style_set_bg_opa(&L5S.meter, LV_OPA_COVER);
    lv_style_set_bg_color(&L5S.meter, L5C.meter);
    lv_style_set_radius(&L5S.meter, 0);
    lv_style_set_height(&L5S.meter, 8);
    STYLE_INIT(&L5S.meter_ind);
    lv_style_set_bg_opa(&L5S.meter_ind, LV_OPA_COVER);
    lv_style_set_radius(&L5S.meter_ind, 0);

    STYLE_INIT(&L5S.pick);
    lv_style_set_bg_opa(&L5S.pick, LV_OPA_COVER);
    lv_style_set_radius(&L5S.pick, 2);
    lv_style_set_pad_hor(&L5S.pick, 10);
    lv_style_set_pad_ver(&L5S.pick, 5);
    lv_style_set_text_font(&L5S.pick, &l5_mono_12);
    lv_style_set_text_letter_space(&L5S.pick, 1);

    STYLE_INIT(&L5S.scrim);
    lv_style_set_bg_opa(&L5S.scrim, 158);
    lv_style_set_bg_color(&L5S.scrim, L5C.scrim);
    lv_style_set_border_width(&L5S.scrim, 0);
    lv_style_set_radius(&L5S.scrim, 0);

    STYLE_INIT(&L5S.menu_btn);
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
    STYLE_INIT(&L5S.menu_btn_pr);
    lv_style_set_bg_opa(&L5S.menu_btn_pr, 64);

    STYLE_INIT(&L5S.toast);
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
    STYLE_INIT(&L5S.press_tint); /* row:active */
    lv_style_set_bg_opa(&L5S.press_tint, 64);
    STYLE_INIT(&L5S.press_fill); /* .step button:active */
    lv_style_set_bg_opa(&L5S.press_fill, LV_OPA_COVER);
    STYLE_INIT(&L5S.press_ring); /* swatch, card, flash map segment:active */
    lv_style_set_outline_width(&L5S.press_ring, 2);
    lv_style_set_outline_color(&L5S.press_ring, L5C.ring);
    lv_style_set_outline_opa(&L5S.press_ring, 170);
    STYLE_INIT(&L5S.tgl_on); /* .tgl.on */
    lv_style_set_shadow_width(&L5S.tgl_on, 10);
    lv_style_set_shadow_opa(&L5S.tgl_on, 128);
    STYLE_INIT(&L5S.swatch_sel); /* swatch [aria-pressed]: white ring + black ring */
    lv_style_set_outline_width(&L5S.swatch_sel, 3);
    lv_style_set_outline_color(&L5S.swatch_sel, L5_WHITE);
    lv_style_set_shadow_width(&L5S.swatch_sel, 0);
    lv_style_set_shadow_spread(&L5S.swatch_sel, 5);
    lv_style_set_shadow_color(&L5S.swatch_sel, L5_BLACK);
    lv_style_set_shadow_opa(&L5S.swatch_sel, LV_OPA_COVER);
    STYLE_INIT(&L5S.focus); /* keyboard focus: an accent ring 3 px inside the edge */
    lv_style_set_outline_width(&L5S.focus, 2);
    lv_style_set_outline_pad(&L5S.focus, -3);
    lv_style_set_outline_opa(&L5S.focus, LV_OPA_COVER);

    accent_styles();
    s_built = true;
}

void l5_theme_init(uint32_t accent_hex, int theme)
{
    tall_font(&l5_mono13_tall, &l5_mono_13);
    tall_font(&l5_mono15_tall, &l5_mono_15);
    tall_font(&l5_mono18_tall, &l5_mono_18);
    colors(theme);
    palette(accent_hex); /* accent_lo mixes with the theme's panel */
    build_styles();
}

void l5_theme_set(int theme)
{
    l5_dither_forget(); /* the old theme's gradients would fill the table */
    colors(theme);
    palette(s_accent_hex);
    build_styles();
    s_gen++;
    lv_obj_report_style_change(NULL);
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
    l5_dither_hook(b);
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
