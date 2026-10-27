/* App icons and short descriptions from the card: <card>/apps/<project>/icon.png and about.txt, where <project> is
   the app's esp_app_desc_t.project_name (HOME shows it under the name: "orbit_drum · v0.4.9").
   A worker reads and decodes them (LVGL's lodepng) and shrinks each icon to at most MASTER px; the LVGL task keeps
   them here and draws them into the emblems (l5_cover.c), the bootscreen tile and its chips. The LVGL task never
   waits for the card: an icon that is not loaded yet shows the procedural emblem, and the views are drawn again
   when the worker is done. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "nvs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "l5_board.h"
#include "l5_files.h"
#include "l5_slots.h"
#include "l5_ui.h"
#include "src/libs/lodepng/lodepng.h"

#define TAG "l5_icon"
#define APPS_DIR CONFIG_BSP_SD_MOUNT_POINT "/apps"
#define MASTER 256          /* kept icons fit in MASTER x MASTER: the largest drawing (HOME) is 199 px */
#define MAX_SIDE 1024       /* larger PNGs are not decoded (4 MB of pixels) */
#define MAX_FILE (2u << 20)
#define CACHE 16            /* the installed apps (8) and the .bin files the SD view showed */
#define MAX_TRIES 3         /* decodes that may run out of memory before an icon waits for its file to change */
#define RETRY_MS 10000
#define HUES 24             /* colour bins of 15 degrees: the icon's colours (icon_colors) */
#define RGB_OK 0x1000000u   /* set in accent / tint: the colour is there (0 = the icon has none) */
#define NVS_NS "l5"         /* the boot app's accent for the next start (l5_appicon_boot_accent) */

typedef struct {
    char project[33];
    uint32_t gen;                 /* which add() made it: a result for an entry dropped since is not taken */
    uint32_t icon_sig, about_sig; /* the files read (size and time), 0 = none on the card */
    uint8_t tries;                /* decodes of icon.png that ran out of memory in a row */
    bool seen;                    /* the worker looked at the card for it at least once */
    uint8_t *px;                  /* premultiplied RGBA, w x h; NULL = no icon */
    uint16_t w, h;
    uint32_t accent, tint;        /* the icon's colours, 0xRRGGBB | RGB_OK; 0 = none (icon_colors) */
    char about[L5_ABOUT_LEN];
    uint32_t used;                /* last asked for: a full cache drops the oldest one that is not installed */
} icon_t;

/* one app for the worker: what the cache holds in, what the card holds out */
typedef struct {
    char project[33];
    uint32_t gen;
    uint32_t icon_sig, about_sig;
    uint8_t tries;
    bool icon_new, about_new; /* the file changed: px / about hold what it holds now */
    uint8_t *px;
    uint16_t w, h;
    uint32_t accent, tint;
    char about[L5_ABOUT_LEN];
} item_t;

typedef struct {
    int n;
    bool retry; /* an icon ran out of memory: the card is looked at again in RETRY_MS */
    item_t it[CACHE];
} job_t;

typedef enum { DECODED, REFUSED, NO_MEMORY } decode_t;

static icon_t s_icon[CACHE]; /* LVGL task only, as everything below but the worker and what it reads */
static uint32_t s_tick, s_gen;
static volatile bool s_running; /* a job is out; the worker clears it only when it cannot hand the job back */
static bool s_again;            /* asked for while a job was out */
static TaskHandle_t s_task;     /* the worker, made by the first job and kept */
static job_t *volatile s_job;   /* the worker's next job: set before its notification, while no job is out */
static lv_timer_t *s_retry;

#ifndef ESP_PLATFORM
int l5_appicon_test_nomem; /* PC test (tools/host_test/ui/shots.c): this many decodes run out of memory */
#endif

/* ---------- pixels ---------- */

static void premultiply(uint8_t *p, size_t n)
{
    for (; n--; p += 4) {
        const unsigned a = p[3];
        if (a == 255) continue;
        p[0] = (uint8_t)((p[0] * a + 127) / 255);
        p[1] = (uint8_t)((p[1] * a + 127) / 255);
        p[2] = (uint8_t)((p[2] * a + 127) / 255);
    }
}

/* k x k blocks averaged in place: a 1024 px icon becomes 256 px before the filter below */
static void shrink_blocks(uint8_t *p, unsigned *w, unsigned *h, unsigned k)
{
    const unsigned ow = *w / k, oh = *h / k, kk = k * k;
    for (unsigned oy = 0; oy < oh; oy++) {
        for (unsigned ox = 0; ox < ow; ox++) {
            unsigned sum[4] = {0};
            for (unsigned y = 0; y < k; y++) {
                const uint8_t *s = p + ((size_t)(oy * k + y) * *w + ox * k) * 4;
                for (unsigned x = 0; x < k * 4; x++) sum[x & 3] += s[x];
            }
            uint8_t *d = p + ((size_t)oy * ow + ox) * 4; /* never past a block still to be read */
            for (int c = 0; c < 4; c++) d[c] = (uint8_t)((sum[c] + kk / 2) / kk);
        }
    }
    *w = ow;
    *h = oh;
}

/* weights along one axis: a tent as wide as an output pixel, at least one source pixel to each side (bilinear when
   enlarging); output o takes taps pixels from first[o] on, those outside the image weigh 0 */
typedef struct {
    int taps;
    int *first;
    float *w;
} axis_t;

static void axis_free(axis_t *a)
{
    free(a->first);
    free(a->w);
    a->first = NULL;
    a->w = NULL;
}

static bool axis_make(axis_t *a, int n_in, int n_out)
{
    const float scale = (float)n_in / n_out, s = scale > 1 ? scale : 1;
    a->taps = 2 * (int)ceilf(s) + 1;
    a->first = malloc(n_out * sizeof(int));
    a->w = calloc((size_t)n_out * a->taps, sizeof(float));
    if (!a->first || !a->w) {
        axis_free(a);
        return false;
    }
    for (int o = 0; o < n_out; o++) {
        const float c = (o + 0.5f) * scale - 0.5f; /* the output pixel's center in source pixels */
        const int lo = (int)floorf(c - s) + 1;
        float *w = a->w + (size_t)o * a->taps, sum = 0;
        for (int k = 0; k < a->taps; k++) {
            const int i = lo + k;
            const float d = fabsf(i - c) / s;
            if (i >= 0 && i < n_in && d < 1) {
                w[k] = 1 - d;
                sum += w[k];
            }
        }
        for (int k = 0; k < a->taps && sum > 0; k++) w[k] /= sum;
        a->first[o] = lo;
    }
    return true;
}

/* premultiplied RGBA sw x sh -> dw x dh (malloc), NULL without memory */
static uint8_t *resample(const uint8_t *src, int sw, int sh, int dw, int dh)
{
    uint8_t *dst = malloc((size_t)dw * dh * 4);
    if (!dst) return NULL;
    if (dw == sw && dh == sh) {
        memcpy(dst, src, (size_t)dw * dh * 4);
        return dst;
    }
    axis_t ax = {0}, ay = {0};
    float *mid = malloc((size_t)dw * sh * 4 * sizeof(float)); /* rows done, columns not yet */
    if (!mid || !axis_make(&ax, sw, dw) || !axis_make(&ay, sh, dh)) {
        free(mid);
        axis_free(&ax);
        free(dst);
        return NULL;
    }
    for (int y = 0; y < sh; y++) {
        const uint8_t *row = src + (size_t)y * sw * 4;
        for (int o = 0; o < dw; o++) {
            const float *w = ax.w + (size_t)o * ax.taps;
            float acc[4] = {0};
            for (int k = 0; k < ax.taps; k++) {
                if (w[k] == 0) continue;
                const uint8_t *p = row + (size_t)(ax.first[o] + k) * 4;
                for (int c = 0; c < 4; c++) acc[c] += w[k] * p[c];
            }
            memcpy(mid + ((size_t)y * dw + o) * 4, acc, sizeof(acc));
        }
    }
    for (int o = 0; o < dh; o++) {
        const float *w = ay.w + (size_t)o * ay.taps;
        for (int x = 0; x < dw; x++) {
            float acc[4] = {0};
            for (int k = 0; k < ay.taps; k++) {
                if (w[k] == 0) continue;
                const float *p = mid + ((size_t)(ay.first[o] + k) * dw + x) * 4;
                for (int c = 0; c < 4; c++) acc[c] += w[k] * p[c];
            }
            uint8_t *d = dst + ((size_t)o * dw + x) * 4;
            const float a = acc[3] < 0 ? 0 : acc[3] > 255 ? 255 : acc[3];
            for (int c = 0; c < 4; c++) { /* premultiplied: no color above its alpha */
                const float v = c == 3 ? a : acc[c] < 0 ? 0 : acc[c] > a ? a : acc[c];
                d[c] = (uint8_t)(v + 0.5f);
            }
        }
    }
    free(mid);
    axis_free(&ax);
    axis_free(&ay);
    return dst;
}

/* w x h that fits in box x box with the icon's proportions */
static void fit(int sw, int sh, int box, int *w, int *h)
{
    if (sw >= sh) {
        *w = box;
        *h = (sh * box + sw / 2) / sw;
    } else {
        *h = box;
        *w = (sw * box + sh / 2) / sh;
    }
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

/* rounded corners (radius px, at most half the short side: LV_RADIUS_CIRCLE is a circle) */
static void round_corners(uint8_t *p, int w, int h, int32_t radius)
{
    const float r = (float)(radius < (w < h ? w : h) / 2 ? radius : (w < h ? w : h) / 2);
    if (r < 1) return;
    for (int y = 0; y < h; y++) {
        const float py = y + 0.5f, cy = py < r ? r : py > h - r ? h - r : py;
        for (int x = 0; x < w; x++) {
            const float px = x + 0.5f, cx = px < r ? r : px > w - r ? w - r : px;
            if (cx == px || cy == py) continue; /* not in a corner */
            const float cover = r + 0.5f - sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
            if (cover >= 1) continue;
            uint8_t *d = p + ((size_t)y * w + x) * 4;
            const float f = cover > 0 ? cover : 0;
            for (int c = 0; c < 4; c++) d[c] = (uint8_t)(d[c] * f + 0.5f);
        }
    }
}

/* ---------- the icon's colours ---------- */

static uint32_t hsv_rgb(float h, float s, float v)
{
    const float c = v * s, hp = h / 60.0f, x = c * (1 - fabsf(fmodf(hp, 2) - 1)), m = v - c;
    float r = 0, g = 0, b = 0;
    if (hp < 1) r = c, g = x;
    else if (hp < 2) r = x, g = c;
    else if (hp < 3) g = c, b = x;
    else if (hp < 4) g = x, b = c;
    else if (hp < 5) r = x, b = c;
    else r = c, b = x;
    return (uint32_t)((r + m) * 255 + 0.5f) << 16 | (uint32_t)((g + m) * 255 + 0.5f) << 8 | (uint32_t)((b + m) * 255 + 0.5f);
}

/* hue (0..360), saturation and value (0..1) of an RGB colour */
static void rgb_hsv(float r, float g, float b, float *h, float *s, float *v)
{
    const float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    *v = mx / 255;
    *s = mx > 0 ? d / mx : 0;
    if (d <= 0) *h = 0;
    else if (mx == r) *h = 60 * fmodf((g - b) / d + 6, 6);
    else if (mx == g) *h = 60 * ((b - r) / d + 2);
    else *h = 60 * ((r - g) / d + 4);
}

/* The colours of a premultiplied RGBA icon. Its coloured pixels (not transparent, not black, not grey) go into hue
   bins of 15 degrees; a bin counts with its two neighbours, so a hue on a bin edge is not split. tint: the hue most
   of them have (each pixel weighs its saturation), for backgrounds; accent: the most vivid hue (saturation^2 *
   value^4: a small bright ring beats a large dark background), for the ring, buttons and frames. Both are evened
   out for the launcher: the accent at least 55 % saturated and 85 % bright, the tint 55 % bright. None (0) when
   less than 3 % of the icon is coloured. */
static void icon_colors(const uint8_t *px, int n, uint32_t *accent, uint32_t *tint)
{
    float wt[HUES] = {0}, wa[HUES] = {0}, ct[HUES][3] = {{0}}, ca[HUES][3] = {{0}};
    int opaque = 0;
    *accent = *tint = 0;
    for (const uint8_t *p = px; n--; p += 4) {
        const unsigned a = p[3];
        if (a < 128) continue;
        opaque++;
        const float r = p[0] * 255.0f / a, g = p[1] * 255.0f / a, b = p[2] * 255.0f / a;
        float h, sat, val;
        rgb_hsv(r, g, b, &h, &sat, &val);
        if (val < 0.12f || sat < 0.25f) continue;
        const int bin = (int)(h * HUES / 360) % HUES;
        const float w1 = sat, w2 = sat * sat * val * val * val * val;
        wt[bin] += w1, wa[bin] += w2;
        ct[bin][0] += w1 * r, ct[bin][1] += w1 * g, ct[bin][2] += w1 * b;
        ca[bin][0] += w2 * r, ca[bin][1] += w2 * g, ca[bin][2] += w2 * b;
    }
    float total = 0;
    for (int i = 0; i < HUES; i++) total += wt[i];
    if (!opaque || total < 0.03f * opaque) return;
    for (int pass = 0; pass < 2; pass++) {
        const float *w = pass ? wa : wt;
        float(*c)[3] = pass ? ca : ct;
        int best = 0;
        float best_score = -1;
        for (int i = 0; i < HUES; i++) {
            const float score = w[(i + HUES - 1) % HUES] + 2 * w[i] + w[(i + 1) % HUES];
            if (score > best_score) best_score = score, best = i;
        }
        float sum = 0, rgb[3] = {0};
        for (int d = -1; d <= 1; d++) {
            const int i = (best + d + HUES) % HUES;
            sum += w[i];
            for (int k = 0; k < 3; k++) rgb[k] += c[i][k];
        }
        if (sum <= 0) continue;
        float h, sat, val;
        rgb_hsv(rgb[0] / sum, rgb[1] / sum, rgb[2] / sum, &h, &sat, &val);
        if (pass) *accent = hsv_rgb(h, fmaxf(sat, 0.55f), fmaxf(val, 0.85f)) | RGB_OK;
        else *tint = hsv_rgb(h, fminf(fmaxf(sat, 0.35f), 0.8f), 0.55f) | RGB_OK;
    }
}

/* ---------- the worker ---------- */

static uint32_t file_sig(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    const uint32_t s = (uint32_t)st.st_size * 2654435761u ^ (uint32_t)st.st_mtime;
    return s ? s : 1;
}

/* the first line that is not empty, cut on a character boundary */
static void read_about(const char *path, char *out, size_t len)
{
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char b[256];
    size_t n = fread(b, 1, sizeof(b) - 1, f);
    fclose(f);
    b[n] = 0;
    char *p = b;
    if (n >= 3 && !memcmp(p, "\xEF\xBB\xBF", 3)) p += 3; /* UTF-8 BOM (Notepad) */
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    size_t k = strcspn(p, "\r\n");
    while (k && (p[k - 1] == ' ' || p[k - 1] == '\t')) k--;
    if (k >= len) {
        k = len - 1;
        while (k && ((uint8_t)p[k] & 0xC0) == 0x80) k--;
    }
    memcpy(out, p, k);
    out[k] = 0;
    for (char *t = out; *t; t++) {
        if (*t == '\t') *t = ' ';
    }
}

static unsigned be32(const uint8_t *p)
{
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

static decode_t decode(const char *path, item_t *it)
{
    const int64_t t0 = esp_timer_get_time();
#ifndef ESP_PLATFORM
    if (l5_appicon_test_nomem > 0) {
        l5_appicon_test_nomem--;
        return NO_MEMORY;
    }
#endif
    FILE *f = fopen(path, "rb");
    if (!f) return REFUSED;
    long n = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    if (n <= 0 || n > (long)MAX_FILE) {
        if (n > (long)MAX_FILE) ESP_LOGW(TAG, "%s: %ld bytes, at most %u", path, n, MAX_FILE);
        fclose(f);
        return REFUSED;
    }
    uint8_t *png = malloc(n);
    bool ok = png && fseek(f, 0, SEEK_SET) == 0 && fread(png, 1, n, f) == (size_t)n;
    fclose(f);
    if (!ok) {
        free(png);
        return png ? REFUSED : NO_MEMORY;
    }
    /* the IHDR chunk before anything is allocated for the pixels: signature, length, "IHDR", w, h, bit depth, colour
       type */
    unsigned w = n >= 26 && !memcmp(png, "\x89PNG\r\n\x1a\n", 8) && !memcmp(png + 12, "IHDR", 4) ? be32(png + 16) : 0;
    unsigned h = w ? be32(png + 20) : 0;
    if (!w || !h || w > MAX_SIDE || h > MAX_SIDE) {
        if (w) ESP_LOGW(TAG, "%s: %ux%u px, at most %ux%u", path, w, h, MAX_SIDE, MAX_SIDE);
        else ESP_LOGW(TAG, "%s: not a PNG file", path);
        free(png);
        return REFUSED;
    }
    /* LVGL 9.3's lodepng decodes into an ARGB8888 buffer (4 bytes a pixel) before it converts the colours, so a PNG
       of more than 32 bits a pixel (16-bit RGB or RGBA) would be written past its end. Channels of colour types
       0 gray, 2 RGB, 3 palette, 4 gray + alpha, 6 RGBA; lodepng refuses the others itself. */
    static const uint8_t CHANNELS[7] = {1, 0, 3, 1, 2, 0, 4};
    const unsigned depth = png[24], type = png[25];
    if (type < 7 && CHANNELS[type] * depth > 32) {
        ESP_LOGW(TAG, "%s: 16 bits a channel, save the PNG with 8", path);
        free(png);
        return REFUSED;
    }
    /* LVGL's lodepng hands out an lv_draw_buf_t (not a bare pixel array) whose rows of 4 * w bytes are R, G, B, A;
       lv_lodepng.c swaps R and B afterwards for LVGL, this file reads them as they come. The firmware's lodepng has
       no error texts (CMakeLists.txt): the number is lodepng's error code. */
    lv_draw_buf_t *img = NULL;
    unsigned err = lodepng_decode32((unsigned char **)&img, &w, &h, png, n);
    free(png);
    if (!err && (!img || img->header.stride != w * 4)) err = 1000; /* not the layout this file expects */
    if (err) {
        ESP_LOGW(TAG, "%s: PNG error %u", path, err);
        if (img) lv_draw_buf_destroy(img);
        return err == 83 ? NO_MEMORY : REFUSED; /* 83: lodepng's "memory allocation failed" */
    }
    uint8_t *rgba = img->data;
    const unsigned w0 = w, h0 = h;
    premultiply(rgba, (size_t)w * h);
    const unsigned k = (w < h ? w : h) / MASTER;
    if (k > 1) shrink_blocks(rgba, &w, &h, k);
    int dw = (int)w, dh = (int)h;
    if (w > MASTER || h > MASTER) fit((int)w, (int)h, MASTER, &dw, &dh);
    it->px = resample(rgba, (int)w, (int)h, dw, dh);
    lv_draw_buf_destroy(img);
    if (!it->px) return NO_MEMORY;
    it->w = (uint16_t)dw;
    it->h = (uint16_t)dh;
    icon_colors(it->px, dw * dh, &it->accent, &it->tint);
    ESP_LOGI(TAG, "%s: %ux%u px, kept %dx%d, accent #%06lx, tint #%06lx (%d ms)", it->project, w0, h0, dw, dh,
             (unsigned long)(it->accent & 0xFFFFFF), (unsigned long)(it->tint & 0xFFFFFF),
             (int)((esp_timer_get_time() - t0) / 1000));
    return DECODED;
}

static void load(job_t *j, item_t *it)
{
    char p[L5_FILES_PATH];
    snprintf(p, sizeof(p), APPS_DIR "/%s/about.txt", it->project);
    uint32_t s = file_sig(p);
    if (s != it->about_sig) {
        it->about_sig = s;
        it->about_new = true;
        if (s) read_about(p, it->about, sizeof(it->about));
    }
    snprintf(p, sizeof(p), APPS_DIR "/%s/icon.png", it->project);
    s = file_sig(p);
    if (s == it->icon_sig) return;
    /* a file that is refused keeps its signature: read again when it changes */
    const decode_t r = s ? decode(p, it) : REFUSED;
    if (r == NO_MEMORY && ++it->tries < MAX_TRIES) { /* the old icon stays and the signature too: read again */
        ESP_LOGW(TAG, "%s: no memory for the icon, try %d of %d", it->project, it->tries, MAX_TRIES);
        j->retry = true;
        return;
    }
    if (r == NO_MEMORY) ESP_LOGW(TAG, "%s: no memory for the icon, read again when it changes", it->project);
    it->tries = 0;
    it->icon_sig = s;
    it->icon_new = true;
}

static void drop(job_t *j)
{
    for (int i = 0; i < j->n; i++) free(j->it[i].px);
    free(j);
}

static void start(void);
static icon_t *find(const char *project);

static void retry_cb(lv_timer_t *t)
{
    (void)t;
    s_retry = NULL; /* one shot: LVGL deletes it */
    start();
}

/* the LVGL task takes the worker's results */
static void apply_cb(void *arg)
{
    job_t *j = arg;
    bool shown = false, stale = false; /* something a view draws changed; an entry was made again meanwhile */
    for (int i = 0; i < j->n; i++) {
        item_t *it = &j->it[i];
        icon_t *e = find(it->project);
        if (!e) continue; /* dropped from a full cache meanwhile */
        if (e->gen != it->gen) { /* dropped and asked for again: its files are looked at once more */
            stale = true;
            continue;
        }
        e->tries = it->tries;
        e->seen = true;
        if (it->about_new) {
            e->about_sig = it->about_sig;
            shown |= strcmp(e->about, it->about) != 0;
            memcpy(e->about, it->about, sizeof(e->about));
        }
        if (it->icon_new) {
            e->icon_sig = it->icon_sig;
            shown |= e->px || it->px;
            free(e->px);
            e->px = it->px;
            e->w = it->w;
            e->h = it->h;
            e->accent = it->px ? it->accent : 0;
            e->tint = it->px ? it->tint : 0;
            it->px = NULL;
        }
    }
    if (j->retry && !s_retry && (s_retry = lv_timer_create(retry_cb, RETRY_MS, NULL)) != NULL) {
        lv_timer_set_repeat_count(s_retry, 1);
    }
    drop(j);
    s_running = false;
    const bool again = s_again || stale;
    s_again = false;
    if (shown) {
        /* the same card stays in the middle; a hidden HOME is only marked stale, and its next build (for whatever
           reason) brings the boot app to the middle as usual */
        if (l5_ui_view() == L5_VIEW_HOME) l5_home_keep_place();
        /* CFG > UI color APP: the boot app's icon colour; a new accent repaints the bootscreen already */
        const bool repainted = l5_cfg_accent_follow();
        l5_ui_render_views(L5_VIEW_BIT(L5_VIEW_HOME) | L5_VIEW_BIT(L5_VIEW_SD)); /* HOME asks for a new look */
        if (!repainted) l5_boot_screen_icons();
    }
    if (again && !s_running) start();
}

/* the results to the LVGL task; the caller holds the LVGL lock */
static void post(job_t *j)
{
    if (lv_async_call(apply_cb, j) != LV_RESULT_OK) {
        drop(j);
        s_running = false;
    }
}

static void run(job_t *j)
{
    for (int i = 0; i < j->n; i++) load(j, &j->it[i]);
}

/* one task for every job: a task per HOME build fragmented the internal RAM its stack comes from */
static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        job_t *j = s_job;
        s_job = NULL;
        if (!j) continue;
        run(j);
        while (!l5_board_lock(1000)) { /* the LVGL task is busy (an install, a long build): wait */
        }
        post(j);
        l5_board_unlock();
    }
}

/* ---------- the cache (LVGL task) ---------- */

static bool valid(const char *project)
{
    if (!project || !*project || strlen(project) >= sizeof(s_icon[0].project)) return false;
    if (!strcmp(project, ".") || !strcmp(project, "..")) return false;
    return !strpbrk(project, "/\\:");
}

static icon_t *find(const char *project)
{
    for (int i = 0; i < CACHE; i++) {
        if (s_icon[i].project[0] && !strcmp(s_icon[i].project, project)) return &s_icon[i];
    }
    return NULL;
}

static bool installed(const char *project)
{
    for (int i = 0; i < g_slots.n; i++) {
        if (g_slots.slot[i].used && !strcmp(g_slots.slot[i].project, project)) return true;
    }
    return false;
}

static icon_t *add(const char *project)
{
    icon_t *e = NULL;
    for (int i = 0; i < CACHE && !e; i++) {
        if (!s_icon[i].project[0]) e = &s_icon[i];
    }
    for (int i = 0; i < CACHE && !e; i++) { /* full: the oldest that is not installed */
        icon_t *c = &s_icon[i];
        if (!installed(c->project) && (!e || c->used < e->used)) e = c;
    }
    if (!e) return NULL;
    free(e->px);
    memset(e, 0, sizeof(*e));
    snprintf(e->project, sizeof(e->project), "%s", project);
    e->gen = ++s_gen;
    e->used = ++s_tick;
    return e;
}

static void start(void)
{
    if (s_running) {
        s_again = true;
        return;
    }
    /* without a mounted card there is nothing to read (the launcher never unmounts a card it mounted, so the
       worker never reads a card that goes away under it) */
    if (!l5_board_sd_ready()) return;
    job_t *j = calloc(1, sizeof(*j));
    if (!j) return;
    for (int i = 0; i < CACHE; i++) {
        const icon_t *e = &s_icon[i];
        if (!e->project[0]) continue;
        item_t *it = &j->it[j->n++];
        memcpy(it->project, e->project, sizeof(it->project)); /* same size */
        it->gen = e->gen;
        it->icon_sig = e->icon_sig;
        it->about_sig = e->about_sig;
        it->tries = e->tries;
    }
    if (!j->n) {
        free(j);
        return;
    }
    s_running = true;
    s_again = false;
    if (!s_task && xTaskCreatePinnedToCore(worker, "l5_icons", 8192, NULL, 2, &s_task, 1) != pdPASS) s_task = NULL;
    if (s_task) {
        s_job = j;
        xTaskNotifyGive(s_task);
        return;
    }
    /* no worker (no memory for its stack; the PC snapshots refuse it): the LVGL task reads the card itself */
    run(j);
    post(j);
}

void l5_appicon_refresh(void)
{
    for (int i = 0; i < g_slots.n; i++) {
        const l5_slot_t *s = &g_slots.slot[i];
        if (s->used && valid(s->project) && !find(s->project)) add(s->project);
    }
    start();
}

/* the entry of a project; one never asked for is looked for on the card (and drawn when it is found) */
static icon_t *get(const char *project)
{
    if (!valid(project)) return NULL;
    icon_t *e = find(project);
    if (!e && (e = add(project)) != NULL) start();
    if (e) e->used = ++s_tick;
    return e;
}

bool l5_appicon_has(const char *project)
{
    const icon_t *e = get(project);
    return e && e->px;
}

const char *l5_appicon_about(const char *project)
{
    const icon_t *e = get(project);
    return e && e->about[0] ? e->about : NULL;
}

bool l5_appicon_colors(const char *project, lv_color_t *accent, lv_color_t *tint)
{
    const icon_t *e = get(project);
    if (!e || !e->px || !(e->tint & RGB_OK)) return false;
    if (accent) *accent = lv_color_hex(e->accent & 0xFFFFFF);
    if (tint) *tint = lv_color_hex(e->tint & 0xFFFFFF);
    return true;
}

/* The accent of the boot app's icon is kept in NVS ("l5": acc_prj, acc_rgb), so the next start draws its first frame
   (the bootscreen) in that colour before the worker has read the icon. */
static struct {
    bool read;
    char project[33];
    uint32_t rgb; /* 0xRRGGBB | RGB_OK, 0 = none */
} s_kept;

static void kept_read(void)
{
    if (s_kept.read) return;
    s_kept.read = true;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_kept.project);
    if (nvs_get_str(h, "acc_prj", s_kept.project, &len) != ESP_OK || nvs_get_u32(h, "acc_rgb", &s_kept.rgb) != ESP_OK) {
        s_kept.project[0] = 0;
        s_kept.rgb = 0;
    }
    nvs_close(h);
}

static void kept_write(const char *project, uint32_t rgb)
{
    if (!strcmp(s_kept.project, project) && s_kept.rgb == rgb) return;
    snprintf(s_kept.project, sizeof(s_kept.project), "%s", project);
    s_kept.rgb = rgb;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_str(h, "acc_prj", project) == ESP_OK && nvs_set_u32(h, "acc_rgb", rgb) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

uint32_t l5_appicon_boot_accent(uint32_t fallback)
{
    const l5_slot_t *s = l5_slots_boot_app();
    if (!s || !valid(s->project)) return fallback;
    kept_read();
    const icon_t *e = find(s->project); /* only what is known: no card read starts here (main.c calls it first) */
    if (!e || !e->seen || e->tries) { /* not looked at yet, or a decode that ran out of memory waits for its retry:
                                          the colour it had at the last start */
        return !strcmp(s_kept.project, s->project) && (s_kept.rgb & RGB_OK) ? s_kept.rgb & 0xFFFFFF : fallback;
    }
    const uint32_t rgb = e->px ? e->accent : 0;
    kept_write(s->project, rgb);
    return rgb & RGB_OK ? rgb & 0xFFFFFF : fallback;
}

/* the icon fitted into box x box and its corners rounded: premultiplied RGBA (malloc), NULL without it */
static uint8_t *render(const char *project, int32_t box, int32_t radius, int *w, int *h)
{
    const icon_t *e = get(project);
    if (!e || !e->px || box < 1) return NULL;
    fit(e->w, e->h, box, w, h);
    uint8_t *p = resample(e->px, e->w, e->h, *w, *h);
    if (p) round_corners(p, *w, *h, radius);
    return p;
}

bool l5_appicon_blend(lv_draw_buf_t *dst, const char *project, int32_t x, int32_t y, int32_t box, int32_t radius)
{
    int w, h;
    if (!dst || dst->header.cf != LV_COLOR_FORMAT_RGB565) return false;
    uint8_t *p = render(project, box, radius, &w, &h);
    if (!p) return false;
    x += (box - w) / 2;
    y += (box - h) / 2;
    for (int r = 0; r < h; r++) {
        if (y + r < 0 || y + r >= (int32_t)dst->header.h) continue;
        uint16_t *d = (uint16_t *)(dst->data + (size_t)(y + r) * dst->header.stride);
        const uint8_t *s = p + (size_t)r * w * 4;
        for (int c = 0; c < w; c++, s += 4) {
            const int32_t dx = x + c;
            if (dx < 0 || dx >= (int32_t)dst->header.w || !s[3]) continue;
            const unsigned v = d[dx], keep = 255 - s[3];
            const unsigned r8 = ((v >> 11) & 0x1f) * 255 / 31, g8 = ((v >> 5) & 0x3f) * 255 / 63, b8 = (v & 0x1f) * 255 / 31;
            const unsigned R = s[0] + (r8 * keep + 127) / 255, G = s[1] + (g8 * keep + 127) / 255,
                           B = s[2] + (b8 * keep + 127) / 255;
            d[dx] = (uint16_t)(((R * 31 + 127) / 255) << 11 | ((G * 63 + 127) / 255) << 5 | (B * 31 + 127) / 255);
        }
    }
    free(p);
    return true;
}

lv_obj_t *l5_appicon_create(lv_obj_t *parent, const char *project, int32_t box, int32_t radius)
{
    int w, h;
    uint8_t *p = render(project, box, radius, &w, &h);
    if (!p) return NULL;
    lv_draw_buf_t *buf = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
    if (!buf) {
        free(p);
        return NULL;
    }
    for (int r = 0; r < h; r++) { /* LVGL's ARGB8888: B, G, R, A in memory, not premultiplied */
        uint8_t *d = buf->data + (size_t)r * buf->header.stride;
        const uint8_t *s = p + (size_t)r * w * 4;
        for (int c = 0; c < w; c++, s += 4, d += 4) {
            const unsigned a = s[3];
            for (int k = 0; k < 3; k++) { /* rounded corners may leave a color one above its alpha */
                const unsigned v = a ? (s[2 - k] * 255 + a / 2) / a : 0;
                d[k] = (uint8_t)(v > 255 ? 255 : v);
            }
            d[3] = (uint8_t)a;
        }
    }
    free(p);
    lv_obj_t *cv = lv_canvas_create(parent);
    l5_canvas_own(cv, buf);
    lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE);
    return cv;
}
