/* One LVGL style set built from the CSS tokens in tab5-launcher/launcher.css. */
#pragma once
#include "lvgl.h"
#include "fonts/l5_fonts.h"
#include "l5_icons.h"

/* :root tokens (device screen, same in both page themes) */
#define L5_PLATE          lv_color_hex(0xe3e4e7)
#define L5_PLATE_HI       lv_color_hex(0xf7f7f8)
#define L5_PLATE_LO       lv_color_hex(0xc5c7cc)
#define L5_PLATE_EDGE     lv_color_hex(0xa7aab0)
#define L5_PLATE_BOTTOM   lv_color_hex(0xdcdde1)
#define L5_INK            lv_color_hex(0x0f1012)
#define L5_INK_SOFT       lv_color_hex(0x3a3d43)
#define L5_MUTED          lv_color_hex(0x6f737c)
#define L5_PANEL          lv_color_hex(0x141518)
#define L5_PANEL_2        lv_color_hex(0x1d1f23)
#define L5_PANEL_3        lv_color_hex(0x292b30)
#define L5_PANEL_LINE     lv_color_hex(0x34373d)
#define L5_ON_PANEL       lv_color_hex(0xf2f3f5)
#define L5_ON_PANEL_MUTED lv_color_hex(0x8e939c)
#define L5_OK             lv_color_hex(0x1fb25a)
#define L5_WARN           lv_color_hex(0xf0a400)
#define L5_DANGER         lv_color_hex(0xff6b6b)
#define L5_BLACK          lv_color_hex(0x000000)
#define L5_WHITE          lv_color_hex(0xffffff)

/* Layout constants (px on the 1280x720 landscape screen) */
#define L5_SCREEN_W   1280
#define L5_SCREEN_H   720
#define L5_TOPBAR_H   72 /* 58 before 19.10.2026: the tab keys were hard to hit on the Tab5 */
#define L5_VIEW_H     (L5_SCREEN_H - L5_TOPBAR_H) /* .views: top 72, height 648 */
#define L5_PANEL_Y    8   /* .vw-main / .vw-side / .vw-col / .vw-full: top 8, height 632 */
#define L5_PANEL_H    (L5_VIEW_H - 2 * L5_PANEL_Y)
#define L5_CHAMFER    18

typedef struct {
    lv_color_t accent;     /* --accent */
    lv_color_t accent_ink; /* --accent-ink: text on accent */
    lv_color_t accent_lo;  /* rgba(accent,.78) over the panel, end of .act.primary gradient */
} l5_palette_t;

typedef struct {
    lv_style_t plate;        /* .plate */
    lv_style_t dark;         /* .dark */
    lv_style_t col_h;        /* .col-h (dark panel header row) */
    lv_style_t act;          /* .act */
    lv_style_t act_pressed;  /* .act:active */
    lv_style_t act_primary;  /* .act.primary */
    lv_style_t act_light;    /* .act.light */
    lv_style_t act_disabled; /* .act:disabled */
    lv_style_t dot;          /* .dot */
    lv_style_t slot;         /* .slot */
    lv_style_t slot_sel;     /* .slot.sel */
    lv_style_t meter;        /* .slot .use .meter */
    lv_style_t meter_ind;    /* .meter i */
    lv_style_t pick;         /* .slot .pick */
    lv_style_t scrim;        /* .scrim */
    lv_style_t menu_btn;     /* .menu-list button */
    lv_style_t menu_btn_pr;  /* .menu-list button:active */
    lv_style_t toast;        /* .toast */
    lv_style_t bare;         /* container without any decoration */
    lv_style_t press_tint;   /* LV_STATE_PRESSED on rows: accent at 25% */
    lv_style_t press_fill;   /* LV_STATE_PRESSED on small buttons: accent */
    lv_style_t press_ring;   /* LV_STATE_PRESSED on swatches, cards, map segments: white outline */
    lv_style_t tgl_on;       /* LV_STATE_CHECKED on a toggle track: accent with glow */
    lv_style_t swatch_sel;   /* LV_STATE_CHECKED on the selected colour swatch */
} l5_styles_t;

extern l5_palette_t L5P;
extern l5_styles_t L5S;

/* The mono sizes for file names, paths and text files: same glyphs, a line tall enough for the accented
   capitals that come from their fallback font (l5_latin_15, Chakra Petch 17), so the first line of a
   label does not cut the accents off. */
extern lv_font_t l5_mono13_tall, l5_mono15_tall, l5_mono18_tall;

void l5_theme_init(uint32_t accent_hex);
/* Change the accent at runtime (CFG > UI color): restyles every object using the set. */
void l5_theme_set_accent(uint32_t accent_hex);

/* helpers */
lv_obj_t *l5_box(lv_obj_t *parent);                          /* plain container, no style */
lv_obj_t *l5_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text);
lv_obj_t *l5_icon(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *icon);
lv_obj_t *l5_dot(lv_obj_t *parent);
lv_obj_t *l5_col_header(lv_obj_t *parent, const char *title, const char *tag); /* returns the tag label */
lv_obj_t *l5_act_button(lv_obj_t *parent, const char *icon, const char *text, bool primary);
lv_obj_t *l5_hline(lv_obj_t *parent, lv_color_t color);
/* clip-path chamfer (18px top-left + bottom-right) drawn as corner triangles in `bg` */
void l5_chamfer(lv_obj_t *obj, int32_t size, lv_color_t bg);
/* dashed 1px border (.slot.empty) */
void l5_dashed_border(lv_obj_t *obj, lv_color_t color);
lv_color_t l5_mix(lv_color_t a, lv_color_t b, uint8_t a_weight);
