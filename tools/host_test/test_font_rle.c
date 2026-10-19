/* Two draw units decompress glyphs of compressed fonts at the same time (LVGL 9.3, two SW draw units): every
   glyph from two threads is compared with the same glyph decoded by one thread. run.py links this twice:
   with LVGL's own lv_font_fmt_txt.c ("shipped": the global RLE state garbles glyphs, the test expects that)
   and with the launcher's copy from tools/patch_lvgl_rle.py ("patched": every glyph must match). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

LV_FONT_DECLARE(l5_chakra_b28)
LV_FONT_DECLARE(l5_chakra_b40)
LV_FONT_DECLARE(l5_orb_blk76)

#define MAX_GLYPHS 1024
#define PASSES 40
#define BUF_W 160
#define BUF_H 160

typedef struct {
    const lv_font_t *font;
    uint32_t cp;
    uint32_t size; /* bytes of the A8 bitmap (stride x height) */
    uint8_t *ref;  /* decoded by one thread */
} glyph_t;

static glyph_t G[MAX_GLYPHS];
static int NG;

typedef struct {
    int id;
    lv_draw_buf_t *buf;
    long decoded, garbled;
} worker_t;

/* the glyph into buf; returns the bitmap bytes or 0 */
static uint32_t decode(const glyph_t *gl, lv_draw_buf_t *buf)
{
    lv_font_glyph_dsc_t g;
    memset(&g, 0, sizeof(g));
    if (!lv_font_get_glyph_dsc(gl->font, &g, gl->cp, 0) || !g.box_w || !g.box_h || g.resolved_font != gl->font) return 0;
    if (!lv_font_get_glyph_bitmap(&g, buf)) return 0;
    return lv_draw_buf_width_to_stride(g.box_w, LV_COLOR_FORMAT_A8) * g.box_h;
}

static void add(const lv_font_t *f, uint32_t from, uint32_t to, lv_draw_buf_t *buf)
{
    for (uint32_t cp = from; cp <= to && NG < MAX_GLYPHS; cp++) {
        glyph_t *gl = &G[NG];
        gl->font = f;
        gl->cp = cp;
        gl->size = decode(gl, buf);
        if (!gl->size) continue;
        gl->ref = malloc(gl->size);
        memcpy(gl->ref, buf->data, gl->size);
        NG++;
    }
}

static void work(worker_t *w)
{
    for (int pass = 0; pass < PASSES; pass++) {
        for (int k = 0; k < NG; k++) {
            const glyph_t *gl = &G[w->id ? NG - 1 - k : k]; /* the two threads go opposite ways */
            if (decode(gl, w->buf) != gl->size) {
                w->garbled++;
            } else if (memcmp(w->buf->data, gl->ref, gl->size)) {
                w->garbled++;
            }
            w->decoded++;
        }
    }
}

#ifdef _WIN32
static DWORD WINAPI thread_fn(LPVOID arg)
{
    work(arg);
    return 0;
}
#else
static void *thread_fn(void *arg)
{
    work(arg);
    return NULL;
}
#endif

int main(int argc, char **argv)
{
    const char *variant = argc > 1 ? argv[1] : "patched";
    lv_init();
    worker_t w[2] = {{0}, {0}};
    for (int i = 0; i < 2; i++) {
        w[i].id = i;
        w[i].buf = lv_draw_buf_create(BUF_W, BUF_H, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    }
    add(&l5_chakra_b28, 0x20, 0x17F, w[0].buf); /* compressed fonts: 24 px and up */
    add(&l5_chakra_b40, 0x20, 0x17F, w[0].buf);
    add(&l5_orb_blk76, 0x41, 0x5A, w[0].buf);
#ifdef _WIN32
    HANDLE t[2];
    for (int i = 0; i < 2; i++) t[i] = CreateThread(NULL, 0, thread_fn, &w[i], 0, NULL);
    WaitForMultipleObjects(2, t, TRUE, INFINITE);
#else
    pthread_t t[2];
    for (int i = 0; i < 2; i++) pthread_create(&t[i], NULL, thread_fn, &w[i]);
    for (int i = 0; i < 2; i++) pthread_join(t[i], NULL);
#endif
    long decoded = w[0].decoded + w[1].decoded, garbled = w[0].garbled + w[1].garbled;
    printf("%s lv_font_fmt_txt.c: %d glyphs of 3 compressed fonts, %ld decodes in 2 threads, %ld garbled\n", variant, NG,
           decoded, garbled);
    if (!strcmp(variant, "shipped")) { /* the bug the patch removes: must show up, or the test proves nothing */
        if (!garbled) printf("  the shipped file decoded every glyph right: the race did not show up on this PC\n");
        return garbled ? 0 : 3;
    }
    return garbled || NG < 300 ? 1 : 0;
}
