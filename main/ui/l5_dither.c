/* Ordered dithering of the vertical gradients.
 *
 * On the RGB565 panel LVGL's 2-stop vertical gradients (.dark #1d1f23 -> #141518, .plate #f7f7f8 -> #e3e4e7 at 60%,
 * the buttons, the top bar, the keys) show bands. Such a gradient is drawn instead from a 32 px wide RGB565 strip,
 * built once with a 4x4 ordered (Bayer) dither and tiled over the area by lv_draw_image (clip radius = the object's
 * radius). l5_dither_hook() does it for an object's LV_PART_MAIN background (LV_EVENT_DRAW_TASK_ADDED); l5_grad_key()
 * and l5_grad_draw() for custom drawing (HOME's panel, the covers). The strips live in PSRAM: at most 128 strips and
 * 2 MB; when full, the strip drawn longest ago (and not in the current frame) is dropped. Without a strip (no
 * memory, table full, area taller than 4096 px) LVGL's own gradient is drawn.
 *
 * Rebuilt from the machine code of the build of 2026-10-20 (0x40041e2e..0x40042544): compiled with the launcher's
 * flags it gives the same instructions, apart from the addresses of four float constants. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "l5_theme.h"
#include "l5_dither.h"

struct l5_grad_key {
    lv_color_t c1; /* top (stop 0) */
    lv_color_t c2; /* bottom (stop 1) */
    uint8_t f1;    /* stop 0 position, 0..255 */
    uint8_t f2;    /* stop 1 position, 0..255 */
};

typedef struct {
    const l5_grad_key_t *key;
    int32_t h;          /* strip height = height of the area drawn */
    uint32_t used;      /* s_frame when it was last drawn */
    lv_image_dsc_t img; /* RGB565, STRIP_W x h, data in PSRAM */
} strip_t;

#define KEYS_MAX   64
#define STRIPS_MAX 128
#define STRIP_W    32
#define BYTES_MAX  (2 * 1024 * 1024)

static const uint8_t bayer[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

/* Defined in the reverse order of their addresses in that build: GCC emits the variables last-defined first. */
static bool s_off;
static int s_key_n;
static int s_strip_n;
static uint32_t s_bytes;
static lv_display_t *s_disp;
static l5_grad_key_t s_keys[KEYS_MAX];
static strip_t s_strips[STRIPS_MAX];
static uint32_t s_frame = 1;

void l5_dither_set_enabled(bool on)
{
    s_off = !on;
}

void l5_dither_forget(void)
{
    for (int i = 0; i < s_strip_n; i++) heap_caps_free((void *)s_strips[i].img.data);
    s_strip_n = 0;
    s_bytes = 0;
    s_key_n = 0;
}

static void refr_start_cb(lv_event_t *e)
{
    (void)e;
    s_frame++;
}

/* 5 or 6 bit channel value -> the 8 bit value the panel shows for it */
static int expand(int v, int bits)
{
    return bits == 5 ? (v << 3) | (v >> 2) : (v << 2) | (v >> 4);
}

/* 8 bit value v -> the 5 / 6 bit level just below it, or the next one when v is more than `thr` of the way there */
static int quantize(int bits, float v, float thr)
{
    int max = (1 << bits) - 1;
    int q = (int)(max * v / 255.0f);
    if (q < 0) q = 0;
    else if (q > max) q = max;
    while (q > 0 && expand(q, bits) > v) q--;
    while (q < max && expand(q + 1, bits) <= v) q++;
    if (q == max) return max;
    float lo = expand(q, bits);
    float hi = expand(q + 1, bits);
    return v - lo > (hi - lo) * thr ? q + 1 : q;
}

static const l5_grad_key_t *key_get(lv_color_t c1, lv_color_t c2, uint8_t f1, uint8_t f2)
{
    for (int i = 0; i < s_key_n; i++) {
        const l5_grad_key_t *k = &s_keys[i];
        if (lv_color_eq(k->c1, c1) && lv_color_eq(k->c2, c2) && k->f1 == f1 && k->f2 == f2) return k;
    }
    if (s_key_n == KEYS_MAX) return NULL;
    s_keys[s_key_n] = (l5_grad_key_t){c1, c2, f1, f2};
    return &s_keys[s_key_n++];
}

/* Count the frames of the default display: a strip drawn in the current frame is never dropped. */
static void watch_frames(void)
{
    lv_display_t *d = lv_display_get_default();
    if (d && d != s_disp) {
        s_disp = d;
        lv_display_add_event_cb(d, refr_start_cb, LV_EVENT_REFR_START, NULL);
    }
}

static strip_t *strip_find(const l5_grad_key_t *key, int32_t h)
{
    for (int i = 0; i < s_strip_n; i++)
        if (s_strips[i].key == key && s_strips[i].h == h) return &s_strips[i];
    return NULL;
}

static const lv_image_dsc_t *strip_make(const l5_grad_key_t *key, int32_t h)
{
    uint32_t bytes = h * STRIP_W * 2;
    while (s_strip_n != 0 && (s_strip_n == STRIPS_MAX || s_bytes + bytes > BYTES_MAX)) {
        int old = -1;
        for (int i = 0; i < s_strip_n; i++) {
            if (s_strips[i].used == s_frame) continue;
            if (old == -1 || s_strips[i].used < s_strips[old].used) old = i;
        }
        if (old == -1) return NULL;
        strip_t *o = &s_strips[old];
        s_bytes -= o->img.data_size;
        heap_caps_free((void *)o->img.data);
        s_strip_n--;
        *o = s_strips[s_strip_n];
    }
    uint16_t *buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (buf == NULL) return NULL;

    float r1 = key->c1.red, g1 = key->c1.green, b1 = key->c1.blue;
    float r2 = key->c2.red, g2 = key->c2.green, b2 = key->c2.blue;
    int32_t y0 = key->f1 * h >> 8;
    int32_t y1 = key->f2 * h >> 8;
    for (int y = 0; y < h; y++) {
        float t;
        if (y <= y0) t = 0.0f;
        else if (y >= y1) t = 1.0f;
        else t = (float)(y - y0) / (float)(y1 - y0);
        float r = r1 + (r2 - r1) * t, g = g1 + (g2 - g1) * t, b = b1 + (b2 - b1) * t;
        uint16_t *row = buf + y * STRIP_W;
        for (int x = 0; x < STRIP_W; x++) {
            int m = bayer[y & 3][x & 3];
            float thr = (m + 0.5f) / 16.0f;
            row[x] = (uint16_t)(quantize(5, r, thr) << 11 | quantize(6, g, thr) << 5 | quantize(5, b, thr));
        }
    }

    strip_t *e = &s_strips[s_strip_n++];
    memset(e, 0, sizeof(*e));
    e->key = key;
    e->h = h;
    e->used = s_frame;
    e->img.header.magic = LV_IMAGE_HEADER_MAGIC;
    e->img.header.cf = LV_COLOR_FORMAT_RGB565;
    e->img.header.w = STRIP_W;
    e->img.header.h = h;
    e->img.header.stride = STRIP_W * 2;
    e->img.data_size = bytes;
    e->img.data = (const uint8_t *)buf;
    s_bytes += bytes;
    return &e->img;
}

static bool draw_strip(lv_layer_t *layer, const l5_grad_key_t *key, const lv_area_t *area, int32_t radius)
{
    int32_t h = lv_area_get_height(area);
    int32_t w = lv_area_get_width(area);
    if (key == NULL || s_off || h <= 0 || h > 4096) return false;

    watch_frames();

    const lv_image_dsc_t *img;
    strip_t *f = strip_find(key, h);
    if (f) {
        f->used = s_frame;
        img = &f->img;
    } else {
        img = strip_make(key, h);
        if (img == NULL) return false;
    }

    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.src = img;
    dsc.tile = 1;
    int32_t r = LV_MIN(w, h) / 2;
    dsc.clip_radius = LV_MIN(radius, r);
    lv_draw_image(layer, &dsc, area);
    return true;
}

static void draw_task_cb(lv_event_t *e)
{
    lv_draw_task_t *t = lv_event_get_draw_task(e);
    if (s_off) return;
    if (lv_draw_task_get_type(t) != LV_DRAW_TASK_TYPE_FILL) return;
    lv_draw_fill_dsc_t *d = lv_draw_task_get_fill_dsc(t);
    if (d == NULL || d->base.part != LV_PART_MAIN) return;
    if (d->grad.dir != LV_GRAD_DIR_VER || d->grad.stops_count != 2) return;
    if (d->opa < LV_OPA_MAX || d->grad.stops[0].opa < LV_OPA_MAX || d->grad.stops[1].opa < LV_OPA_MAX) return;
    lv_area_t a;
    lv_draw_task_get_area(t, &a);
    const l5_grad_key_t *k = key_get(d->grad.stops[0].color, d->grad.stops[1].color, d->grad.stops[0].frac,
                                     d->grad.stops[1].frac);
    if (draw_strip(d->base.layer, k, &a, d->radius)) d->opa = LV_OPA_TRANSP; /* the strip replaces the fill */
}

const l5_grad_key_t *l5_grad_key(lv_color_t top, lv_color_t bottom, uint8_t stop)
{
    return key_get(top, bottom, 0, stop);
}

void l5_grad_draw(lv_layer_t *layer, const l5_grad_key_t *key, const lv_area_t *area)
{
    if (draw_strip(layer, key, area, 0)) return;
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    if (key) {
        dsc.bg_color = key->c1;
        dsc.bg_grad.dir = LV_GRAD_DIR_VER;
        dsc.bg_grad.stops_count = 2;
        dsc.bg_grad.stops[0] = (lv_grad_stop_t){.color = key->c1, .opa = LV_OPA_COVER, .frac = key->f1};
        dsc.bg_grad.stops[1] = (lv_grad_stop_t){.color = key->c2, .opa = LV_OPA_COVER, .frac = key->f2};
    } else {
        dsc.bg_color = lv_color_black();
    }
    lv_draw_rect(layer, &dsc, area);
}

void l5_dither_hook(lv_obj_t *obj)
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(obj, draw_task_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
}

void l5_panel(lv_obj_t *obj, bool dark)
{
    lv_obj_add_style(obj, dark ? &L5S.dark : &L5S.plate, 0);
    l5_dither_hook(obj);
}
