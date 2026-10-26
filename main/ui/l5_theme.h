/* One LVGL style set built from the CSS tokens in tab5-launcher/launcher.css. */
#pragma once
#include "lvgl.h"
#include "fonts/l5_fonts.h"
#include "l5_dither.h"
#include "l5_icons.h"

/* Colours of the active theme (l5_theme_set): Graphite (the original light plate + black panels), Midnight (dark
   plate too) or Daylight (light panels). Read through the L5_* names below; they are runtime values since
   26.10.2026, so never use them in a static initializer. */
typedef struct {
    lv_color_t plate, plate_hi, plate_lo, plate_edge, plate_bottom; /* the frame around the panels, the side panes */
    lv_color_t shine;              /* 1 px top edge of the side panes */
    lv_color_t ink, ink_soft, muted; /* text on the plate */
    lv_color_t panel, panel_2, panel_3, panel_line, panel_edge; /* panels: bottom, top, raised, divider, border */
    lv_color_t on_panel, on_panel_muted;                          /* text on the panels */
    lv_color_t btn_top, btn_bot, btn_shadow, light_top;           /* .act gradient and shadow, .act.light top */
    lv_color_t slot_top, slot_bot; /* HOME cards */
    lv_color_t edge;               /* card, chip and switch-track borders */
    lv_color_t meter, track;       /* empty part of meters, progress bars and the boot ring */
    lv_color_t well, well_ink;     /* deep backgrounds (text viewer, keyboard field) and their text */
    lv_color_t scrim, dim;         /* the veil under dialogs; dim icons on empty cards */
    lv_color_t chip, chip_edge;    /* chips and small framed buttons */
    lv_color_t key_top, key_bot, keyfn_top, keyfn_bot, knob; /* on-screen keyboard keys, switch knob */
    lv_color_t ring;               /* pressed outline of cards, swatches and map segments */
    lv_color_t rail, rail_edge;    /* the side menu */
    lv_color_t boot_top, boot_bot; /* the bootscreen gradient */
    lv_color_t danger, warn;       /* destructive actions, warnings: darker in Daylight */
} l5_colors_t;
extern l5_colors_t L5C;

typedef enum { L5_THEME_GRAPHITE, L5_THEME_MIDNIGHT, L5_THEME_DAYLIGHT, L5_THEME_COUNT } l5_theme_id_t;
typedef struct {
    const char *id;    /* config.conf "theme" */
    const char *label; /* CFG */
} l5_theme_info_t;
extern const l5_theme_info_t l5_themes[L5_THEME_COUNT];

#define L5_PLATE          (L5C.plate)
#define L5_PLATE_HI       (L5C.plate_hi)
#define L5_PLATE_LO       (L5C.plate_lo)
#define L5_PLATE_EDGE     (L5C.plate_edge)
#define L5_PLATE_BOTTOM   (L5C.plate_bottom)
#define L5_INK            (L5C.ink)
#define L5_INK_SOFT       (L5C.ink_soft)
#define L5_MUTED          (L5C.muted)
#define L5_PANEL          (L5C.panel)
#define L5_PANEL_2        (L5C.panel_2)
#define L5_PANEL_3        (L5C.panel_3)
#define L5_PANEL_LINE     (L5C.panel_line)
#define L5_ON_PANEL       (L5C.on_panel)
#define L5_ON_PANEL_MUTED (L5C.on_panel_muted)
#define L5_WARN           (L5C.warn)
#define L5_DANGER         (L5C.danger)
/* the same in every theme */
#define L5_OK             lv_color_hex(0x1fb25a)
#define L5_BLACK          lv_color_hex(0x000000)
#define L5_WHITE          lv_color_hex(0xffffff)

/* Layout constants (px on the 1280x720 landscape screen) */
#define L5_SCREEN_W   1280
#define L5_SCREEN_H   720
/* The tabs are a rail down the left edge (l5_rail.c; a 72 px top bar until 26.10.2026), the views fill the rest. */
#define L5_RAIL_W     112
#define L5_VIEW_W     (L5_SCREEN_W - L5_RAIL_W) /* 1168 */
#define L5_VIEW_H     L5_SCREEN_H
#define L5_GAP        12  /* between the rail, the panels and the screen's edges */
#define L5_PANEL_Y    L5_GAP /* .vw-main / .vw-side / .vw-col / .vw-full: top 12, height 696 */
#define L5_PANEL_H    (L5_VIEW_H - 2 * L5_PANEL_Y)
#define L5_FULL_W     (L5_VIEW_W - 2 * L5_GAP) /* one panel across the view (HOME, PMAN without a table) */
#define L5_SIDE_W     428 /* the light side pane of SD, OTA and PMAN */
#define L5_MAIN_W     (L5_VIEW_W - 3 * L5_GAP - L5_SIDE_W) /* the dark pane left of it: 704 */
#define L5_SIDE_X     (2 * L5_GAP + L5_MAIN_W)
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
    lv_style_t focus;        /* LV_STATE_FOCUS_KEY: what the keyboard points at (button, menu item, CFG row) */
} l5_styles_t;

extern l5_palette_t L5P;
extern l5_styles_t L5S;

/* The mono sizes for file names, paths and text files: same glyphs, a line tall enough for the accented
   capitals that come from their fallback font (l5_latin_15, Chakra Petch 17), so the first line of a
   label does not cut the accents off. */
extern lv_font_t l5_mono13_tall, l5_mono15_tall, l5_mono18_tall;

void l5_theme_init(uint32_t accent_hex, int theme);
/* Change the accent at runtime (CFG > UI color): restyles every object using the set. */
void l5_theme_set_accent(uint32_t accent_hex);
/* Switch the colours (CFG > Theme): the shared styles are rebuilt and every object using them restyled. Objects
   with their own colours (views, the rail) are rebuilt by l5_ui_retheme(). */
void l5_theme_set(int theme);
int l5_theme_current(void);
int l5_theme_find(const char *id);  /* index in l5_themes, -1 when unknown */
const l5_colors_t *l5_theme_colors(int theme); /* any theme's colours (CFG's previews) */
uint32_t l5_theme_gen(void);        /* changes with every l5_theme_set: cached styles elsewhere rebuild then */

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
