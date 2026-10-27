/* Procedural firmware emblem: PCB traces around a chip with the app's initials.
   Port of U.cover() in tab5-launcher/ui.js, same FNV-1a seed and mulberry32 sequence.
   An app with an icon on the card (l5_appicon.c) gets the same background and band with its icon in the middle. */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "l5_files.h"
#include "l5_ui.h"

/* the emblem is dark artwork in every theme (l5_theme.c): its text stays light */
#define COVER_INK lv_color_hex(0xf2f3f5)

typedef struct {
    uint32_t a;
} rng_t;

static uint32_t fnv1a(const char *s)
{
    uint32_t x = 2166136261u;
    for (; *s; s++) {
        x ^= (uint8_t)*s;
        x *= 16777619u;
    }
    return x;
}

static float rnd(rng_t *r) /* mulberry32 */
{
    r->a += 0x6d2b79f5u;
    uint32_t t = (r->a ^ (r->a >> 15)) * (1u | r->a);
    t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
    return (float)(t ^ (t >> 14)) / 4294967296.0f;
}

void l5_cover_initials(const char *title, char out[3])
{
    int n = 0;
    bool in_word = false;
    for (const char *p = title; *p && n < 2; p++) {
        bool alnum = isalnum((unsigned char)*p);
        if (alnum && !in_word) {
            out[n++] = (char)toupper((unsigned char)*p);
        }
        in_word = alnum;
    }
    out[n] = 0;
    if (!n) {
        strcpy(out, "FW");
    }
}

typedef struct {
    lv_layer_t *layer;
    rng_t rng;
    float f;
    lv_color_t accent;
} ctx_t;

/* U.cover scales its text with the canvas (f = W / 636): initials 800 cs * 0.36, name 700 38f, labels 22f */
typedef struct {
    const lv_font_t *initials, *title, *small;
} cover_fonts_t;

static cover_fonts_t cover_fonts(int32_t W)
{
    if (W >= 300) return (cover_fonts_t){&l5_orb_xb54, &l5_orb_b22, &l5_mono_15}; /* 420 px: 54, 25, 14.5 px */
    return (cover_fonts_t){&l5_orb_xb27, &l5_orb_b12, &l5_mono_11};               /* 200 px: 26, 12, 7 px */
}

/* one line of at most max_w px, cut like the JS loop: two characters off and "…" until it fits */
static void fit_line(char *t, size_t size, const lv_font_t *font, int32_t max_w)
{
    size_t n = strlen(t);
    if (lv_text_get_width(t, n, font, 0) <= max_w) return;
    while (n > 4) {
        n -= 2;
        while (n && ((uint8_t)t[n] & 0xC0) == 0x80) n--; /* not inside a UTF-8 sequence */
        if (n + 4 > size) continue;
        memcpy(t + n, "\xE2\x80\xA6", 4); /* "…" and the terminator */
        if (lv_text_get_width(t, n + 3, font, 0) <= max_w) return;
    }
}

/* top of a label whose baseline sits at y (canvas textBaseline 'alphabetic') */
static int32_t top_at_baseline(const lv_font_t *font, float y)
{
    return (int32_t)y - (lv_font_get_line_height(font) - font->base_line);
}

static void rect(ctx_t *c, float x, float y, float w, float h, lv_color_t col, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = col;
    d.bg_opa = opa;
    lv_area_t a = {(int32_t)x, (int32_t)y, (int32_t)(x + w) - 1, (int32_t)(y + h) - 1};
    if (a.x2 < a.x1) a.x2 = a.x1;
    if (a.y2 < a.y1) a.y2 = a.y1;
    lv_draw_rect(c->layer, &d, &a);
}

static void circle(ctx_t *c, float x, float y, float r, lv_color_t col)
{
    if (r < 1) r = 1;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = col;
    d.radius = LV_RADIUS_CIRCLE;
    lv_area_t a = {(int32_t)(x - r), (int32_t)(y - r), (int32_t)(x + r), (int32_t)(y + r)};
    lv_draw_rect(c->layer, &d, &a);
}

static void line(ctx_t *c, float x0, float y0, float x1, float y1, float w, lv_color_t col, lv_opa_t opa)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = col;
    d.opa = opa;
    d.width = w < 1 ? 1 : (int32_t)(w + 0.5f);
    d.round_start = d.round_end = 1;
    d.p1.x = (int32_t)x0; d.p1.y = (int32_t)y0;
    d.p2.x = (int32_t)x1; d.p2.y = (int32_t)y1;
    lv_draw_line(c->layer, &d);
}

static void text(ctx_t *c, const char *s, const lv_font_t *font, lv_color_t col, int32_t x1, int32_t y1, int32_t x2,
                 lv_text_align_t align)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = s;
    d.font = font;
    d.color = col;
    d.align = align;
    d.flag = LV_TEXT_FLAG_NONE;
    lv_area_t a = {x1, y1, x2, y1 + lv_font_get_line_height(font)};
    lv_draw_label(c->layer, &d, &a);
}

/* orthogonal run from a chip pin to the edge, ending in a pad */
static void trace(ctx_t *c, float x0, float y0, int dx, int dy)
{
    const float f = c->f;
    float x = x0, y = y0;
    bool hot = rnd(&c->rng) < 0.35f;
    lv_color_t col = hot ? c->accent : lv_color_hex(0xe1e3e7);
    lv_opa_t opa = hot ? LV_OPA_COVER : 140;
    float w = (hot ? 5 : 3) * f;
    float len1 = (20 + rnd(&c->rng) * 60) * f;
    float x1 = x + dx * len1, y1 = y + dy * len1;
    float sign = rnd(&c->rng) < 0.5f ? -1 : 1;
    float turn = sign * (20 + rnd(&c->rng) * 50) * f;
    float x2 = x1 + (dy ? turn : 0), y2 = y1 + (dx ? turn : 0);
    float len2 = (30 + rnd(&c->rng) * 120) * f;
    float x3 = x2 + dx * len2, y3 = y2 + dy * len2;
    line(c, x, y, x1, y1, w, col, opa);
    line(c, x1, y1, x2, y2, w, col, opa);
    line(c, x2, y2, x3, y3, w, col, opa);
    circle(c, x3, y3, (hot ? 9 : 7) * f, hot ? c->accent : lv_color_hex(0xd9dbdf));
    circle(c, x3, y3, 3 * f, lv_color_hex(0x0b0c0e));
}

static void draw(lv_obj_t *canvas, int32_t W, const char *seed, const char *title, const char *sub, int num, bool with_text,
                 const char *project)
{
    const int32_t H = W;
    ctx_t c = {.rng = {fnv1a(seed)}, .f = W / 636.0f, .accent = L5P.accent};
    const float f = c.f;
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    c.layer = &layer;

    lv_area_t all = {0, 0, W - 1, H - 1}; /* background: #1d1f23 -> #0b0c0e, dithered (l5_dither.c) */
    l5_grad_draw(&layer, l5_grad_key(lv_color_hex(0x1d1f23), lv_color_hex(0x0b0c0e), 255), &all);

    /* fine grid */
    const float g = W / 18.0f;
    for (float x = g; x < W; x += g) {
        rect(&c, x, 0, 1, H, L5_WHITE, 13);
    }
    for (float y = g; y < H; y += g) {
        rect(&c, 0, y, W, 1, L5_WHITE, 13);
    }

    const cover_fonts_t fn = cover_fonts(W);
    const lv_color_t pin = lv_color_hex(0x9ea2a9);
    /* the app's icon from the card (l5_appicon.c) takes the place of the chip: between the slot line and the band */
    const bool icon = project && l5_appicon_has(project);
    const char *about = project ? l5_appicon_about(project) : NULL;
    if (about) sub = about;
    char ini[3]; /* a label's text: alive until lv_canvas_finish_layer(), as the ones below */
    if (!icon) {
        const float cs = W * 0.36f, cx = W * 0.5f - cs / 2, cy = H * 0.44f - cs / 2;
        const int pins = 6;
        const float step = cs / (pins + 1);
        for (int i = 1; i <= pins; i++) {
            if (rnd(&c.rng) < 0.8f) trace(&c, cx + i * step, cy, 0, -1);
            if (rnd(&c.rng) < 0.8f) trace(&c, cx + i * step, cy + cs, 0, 1);
            if (rnd(&c.rng) < 0.8f) trace(&c, cx, cy + i * step, -1, 0);
            if (rnd(&c.rng) < 0.8f) trace(&c, cx + cs, cy + i * step, 1, 0);
        }
        for (int i = 1; i <= pins; i++) {
            rect(&c, cx + i * step - 4 * f, cy - 14 * f, 8 * f, 14 * f, pin, LV_OPA_COVER);
            rect(&c, cx + i * step - 4 * f, cy + cs, 8 * f, 14 * f, pin, LV_OPA_COVER);
            rect(&c, cx - 14 * f, cy + i * step - 4 * f, 14 * f, 8 * f, pin, LV_OPA_COVER);
            rect(&c, cx + cs, cy + i * step - 4 * f, 14 * f, 8 * f, pin, LV_OPA_COVER);
        }

        lv_draw_rect_dsc_t chip;
        lv_draw_rect_dsc_init(&chip);
        chip.bg_opa = LV_OPA_TRANSP; /* the body #34373d -> #16171a is drawn dithered under it: this is the border */
        chip.border_color = L5_WHITE;
        chip.border_opa = 46;
        chip.border_width = (int32_t)(2 * f) < 1 ? 1 : (int32_t)(2 * f);
        lv_area_t ca = {(int32_t)cx, (int32_t)cy, (int32_t)(cx + cs) - 1, (int32_t)(cy + cs) - 1};
        l5_grad_draw(&layer, l5_grad_key(lv_color_hex(0x34373d), lv_color_hex(0x16171a), 255), &ca);
        lv_draw_rect(&layer, &chip, &ca);
        circle(&c, cx + 18 * f, cy + 18 * f, 6 * f, c.accent);

        l5_cover_initials(title && *title ? title : seed, ini);
        int32_t ty = (int32_t)(cy + cs / 2 + 2 * f) - lv_font_get_line_height(fn.initials) / 2;
        text(&c, ini, fn.initials, COVER_INK, (int32_t)cx, ty, (int32_t)(cx + cs), LV_TEXT_ALIGN_CENTER);
    }
    float band_top = H; /* where the icon ends */

    /* every label keeps its own buffer until lv_canvas_finish_layer(): the draw tasks only keep the pointer,
       so one buffer reused for the slot line and the name drew the name twice */
    char slot[24], name[48], by[L5_ABOUT_LEN + 8];
    if (with_text) { /* slot index + chip family, name + author band */
        /* U.cover's places (baselines H - 66f and H - 30f, band 128f); on 132 px emblems (SD, OTA) the fonts
           are larger than that scale, so the name moves above the line under it and the band grows to hold it */
        /* about.txt may be Polish: l5_mono_11 has ASCII only, l5_mono_13 takes the other letters from l5_latin_15 */
        const lv_font_t *sub_font = about && W < 300 ? &l5_mono_13 : fn.small;
        const int32_t by_y = top_at_baseline(sub_font, H - 30 * f);
        float base = H - 66 * f;
        if (base > by_y - 1) base = by_y - 1;
        const int32_t y0 = top_at_baseline(fn.title, base);
        float band = 128 * f;
        if (band < H - y0 + 2) band = H - y0 + 2;
        band_top = H - band;
        rect(&c, 0, H - band, W, band, lv_color_hex(0x0b0c0e), 209);
        rect(&c, 0, H - band, W, 4 * f < 1 ? 1 : 4 * f, c.accent, LV_OPA_COVER);
        snprintf(slot, sizeof(slot), "SLOT %02d \xC2\xB7 ESP32-P4", num);
        text(&c, slot, fn.small, lv_color_hex(0xc9ccd2), (int32_t)(26 * f), (int32_t)(22 * f), W, LV_TEXT_ALIGN_LEFT);
        l5_files_upper(name, sizeof(name), title ? title : ""); /* capitals of Polish letters too */
        const int32_t room = W - (int32_t)(52 * f);
        fit_line(name, sizeof(name), fn.title, room);
        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.text = name;
        d.font = fn.title;
        d.color = COVER_INK;
        d.flag = LV_TEXT_FLAG_EXPAND; /* one line: a wrapped name ran into the line under it */
        lv_area_t ta = {(int32_t)(26 * f), y0, W - (int32_t)(26 * f), y0 + lv_font_get_line_height(fn.title)};
        lv_draw_label(&layer, &d, &ta);
        l5_files_upper(by, sizeof(by), sub ? sub : "");
        fit_line(by, sizeof(by), sub_font, room);
        text(&c, by, sub_font, pin, (int32_t)(26 * f), by_y, W, LV_TEXT_ALIGN_LEFT);
    }
    lv_canvas_finish_layer(canvas, &layer);
    if (icon) { /* drawn into the finished pixels: the draw tasks above are done */
        const float top = 22 * f + lv_font_get_line_height(fn.small) + 14 * f, bottom = band_top - 14 * f;
        int32_t box = (int32_t)(bottom - top);
        if (box > W - (int32_t)(52 * f)) box = W - (int32_t)(52 * f);
        l5_appicon_blend(lv_canvas_get_draw_buf(canvas), project, (W - box) / 2,
                         (int32_t)(top + (bottom - top - box) / 2), box, box * 22 / 100);
    }
}

static void canvas_delete_cb(lv_event_t *e)
{
    lv_draw_buf_destroy(lv_event_get_user_data(e));
}

void l5_canvas_own(lv_obj_t *canvas, lv_draw_buf_t *buf)
{
    lv_canvas_set_draw_buf(canvas, buf);
    lv_obj_add_event_cb(canvas, canvas_delete_cb, LV_EVENT_DELETE, buf);
}

lv_obj_t *l5_cover_create(lv_obj_t *parent, int32_t size, const char *seed, const char *title, const char *sub,
                          int num, bool with_text, const char *project)
{
    lv_obj_t *cv = lv_canvas_create(parent);
    lv_draw_buf_t *buf = lv_draw_buf_create(size, size, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    if (!buf) {
        lv_obj_set_size(cv, size, size);
        return cv;
    }
    l5_canvas_own(cv, buf);
    /* box-shadow: 0 4px 10px rgba(0,0,0,.45), 0 0 0 1px #000 (.hero canvas: 0 6px 16px rgba(0,0,0,.5)) */
    bool big = size >= 300;
    lv_obj_set_style_outline_width(cv, 1, 0);
    lv_obj_set_style_outline_color(cv, L5_BLACK, 0);
    lv_obj_set_style_shadow_width(cv, big ? 16 : 10, 0);
    lv_obj_set_style_shadow_offset_y(cv, big ? 6 : 4, 0);
    lv_obj_set_style_shadow_opa(cv, big ? 128 : 115, 0);
    lv_obj_set_style_shadow_color(cv, L5_BLACK, 0);
    draw(cv, size, seed, title, sub, num, with_text, project);
    return cv;
}
