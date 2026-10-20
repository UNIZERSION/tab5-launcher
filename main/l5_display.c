/* Atomic landscape presentation for the Tab5 panel.
 *
 * Ported from ../aurora-player/components/tab5_hal/tab5_display.c, which runs on this Tab5.
 * Changes: LV_DISPLAY_ROTATION_270 for cfg.rot = 180; the stale-area repair skips everything the
 * current LVGL refresh redraws anyway (LVGL's invalid areas, not only the first piece); smaller
 * pieces so the PPA rotation of piece N overlaps the rendering of piece N+1; per-switch timing
 * for l5_display_mark(); no sleep / snapshot API.
 *
 * Why: the BSP's stock path (bsp_display_start + esp_lvgl_port) has one DPI frame buffer and
 * copies every rotated LVGL piece straight into the buffer the panel is scanning, so a tab
 * switch shows up chunk by chunk. Here every piece of one LVGL refresh is rotated into a
 * hidden buffer and the buffer is published once, after the last piece. Three frame buffers:
 * render into a free one while the previous frame waits for scan-out; areas a buffer missed
 * while it was not the back buffer are repaired from the newest frame before reuse. */
#include "l5_display.h"
#include <stdlib.h>
#include <string.h>
#include "bsp/m5stack_tab5.h"
#include "bsp/touch.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ppa.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_lvgl_port_touch.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/axi_icm_ll.h"
#include "src/display/lv_display_private.h" /* inv_areas: what the running refresh redraws */
#if L5_TOUCH_TRACE
#include "src/indev/lv_indev_private.h" /* scroll_limit and long_press_time for the trace header line */
#endif

#define NATIVE_W 720
#define NATIVE_H 1280
#define FRAME_BYTES (NATIVE_W * NATIVE_H * 2)
/* 160 rows: a 1280x662 view is 5 pieces, so the PPA works on piece N while LVGL renders N+1
   and only a small last piece is rotated after rendering ends */
#define DRAW_PIXELS (1280 * 160)
#define MAX_DAMAGE 64
#define MAX_COVER 8
#define MAX_REST 24

static const char *TAG = "l5_display";
static esp_lcd_panel_handle_t panel;
static lv_display_t *screen;
static lv_indev_t *touch_input;

static ppa_client_handle_t ppa;
static uint8_t *frames[3];
static unsigned latest = 0;
static uint32_t retired_until[3];
static bool retired[3];
static lv_area_t stale[3][MAX_DAMAGE];
static unsigned stale_count[3];
static unsigned back = 1; /* IDF starts scan-out with fb0 */
static SemaphoreHandle_t vsync;
static uint32_t vsync_sequence;
static lv_area_t damage[MAX_DAMAGE];
static unsigned damage_count;
static bool failed, pending;
static uint32_t published_sequence;
static int64_t published_at;
static uint32_t presents;

typedef struct {
    lv_area_t area;
    const uint8_t *pixels;
    lv_display_rotation_t rot;
    bool last;
    bool traced;       /* belongs to the refresh that follows l5_display_mark() */
    uint8_t ncover;    /* first piece of a refresh: native areas this refresh redraws */
    lv_area_t cover[MAX_COVER];
} present_job_t;
static QueueHandle_t presentation_queue;
static SemaphoreHandle_t presentation_done;
static bool transfer_pending; /* LVGL task only */
static bool frame_first;      /* LVGL task only: next flush is the first of a refresh */

/* l5_display_mark(): one tap -> visible timeline. LVGL-task fields and worker fields are
   written by one side each; the report reads them 60 ms after the publish. */
static struct {
    char what[24];
    int64_t mark, built, refr_start, refr_ready, stall_us; /* LVGL task */
    bool armed, in_refresh;                               /* LVGL task */
    int64_t present;                                      /* worker */
    volatile int64_t vsync;                               /* VSYNC ISR */
    volatile bool want_vsync;
    uint64_t rotate_us, repair_us, pixels;                /* worker, traced refresh */
    uint32_t pieces, extra;
} T;
static esp_timer_handle_t report_timer;

static bool IRAM_ATTR on_refresh(esp_lcd_panel_handle_t handle, esp_lcd_dpi_panel_event_data_t *event, void *ctx)
{
    (void)handle;
    (void)event;
    (void)ctx;
    __atomic_add_fetch(&vsync_sequence, 1, __ATOMIC_RELEASE);
    if (T.want_vsync) {
        T.vsync = esp_timer_get_time();
        T.want_vsync = false;
    }
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(vsync, &wake);
    return wake == pdTRUE;
}

/* esp_restart() runs this first. Without DSI video the panel shows noise until the next app
   initialises it (its init sequence sends SLPOUT + DISPON again), so blank it and turn the
   backlight off before the reset. */
static void blank_on_restart(void)
{
    bsp_display_brightness_set(0);
    esp_lcd_panel_disp_on_off(panel, false);
    esp_rom_delay_us(40 * 1000); /* DISPOFF takes effect with the next frame (60 Hz) */
}

/* logical 1280x720 area -> native 720x1280 area, same transforms as esp_lvgl_port's PPA path */
static lv_area_t native_area(const lv_area_t *a, lv_display_rotation_t rot)
{
    if (rot == LV_DISPLAY_ROTATION_270) {
        return (lv_area_t){NATIVE_W - 1 - a->y2, a->x1, NATIVE_W - 1 - a->y1, a->x2};
    }
    return (lv_area_t){a->y1, NATIVE_H - 1 - a->x2, a->y2, NATIVE_H - 1 - a->x1};
}

static void add_damage(lv_area_t *list, unsigned *count, lv_area_t rect)
{
    for (unsigned i = 0; i < *count; i++) {
        lv_area_t *old = &list[i];
        bool adjacent = rect.x1 <= old->x2 + 1 && rect.x2 >= old->x1 - 1 && rect.y1 <= old->y2 + 1 && rect.y2 >= old->y1 - 1;
        if (adjacent) {
            if (rect.x1 < old->x1) old->x1 = rect.x1;
            if (rect.y1 < old->y1) old->y1 = rect.y1;
            if (rect.x2 > old->x2) old->x2 = rect.x2;
            if (rect.y2 > old->y2) old->y2 = rect.y2;
            return;
        }
    }
    if (*count == MAX_DAMAGE) {
        list[0] = (lv_area_t){0, 0, NATIVE_W - 1, NATIVE_H - 1};
        *count = 1;
    } else {
        list[(*count)++] = rect;
    }
}

/* r minus all rectangles in cov; -1 when the result would need more than `max` pieces */
static int subtract(lv_area_t r, const lv_area_t *cov, int ncov, lv_area_t *out, int max)
{
    lv_area_t a[MAX_REST], b[MAX_REST];
    int n = 1;
    a[0] = r;
    for (int c = 0; c < ncov && n; c++) {
        const lv_area_t *k = &cov[c];
        int m = 0;
        for (int i = 0; i < n; i++) {
            const lv_area_t *w = &a[i];
            if (k->x2 < w->x1 || k->x1 > w->x2 || k->y2 < w->y1 || k->y1 > w->y2) {
                if (m >= max) return -1;
                b[m++] = *w;
                continue;
            }
            int top = k->y1 > w->y1 ? k->y1 : w->y1, bottom = k->y2 < w->y2 ? k->y2 : w->y2;
            int left = k->x1 > w->x1 ? k->x1 : w->x1, right = k->x2 < w->x2 ? k->x2 : w->x2;
            if (m + 4 > max) return -1;
            if (w->y1 < top) b[m++] = (lv_area_t){w->x1, w->y1, w->x2, top - 1};
            if (bottom < w->y2) b[m++] = (lv_area_t){w->x1, bottom + 1, w->x2, w->y2};
            if (w->x1 < left) b[m++] = (lv_area_t){w->x1, top, left - 1, bottom};
            if (right < w->x2) b[m++] = (lv_area_t){right + 1, top, w->x2, bottom};
        }
        memcpy(a, b, m * sizeof(lv_area_t));
        n = m;
    }
    memcpy(out, a, n * sizeof(lv_area_t));
    return n;
}

static esp_err_t rotate_piece(const present_job_t *job)
{
    const lv_area_t *area = &job->area;
    const uint32_t width = lv_area_get_width(area), height = lv_area_get_height(area);
    lv_area_t native = native_area(area, job->rot);
    const ppa_srm_oper_config_t op = {
        .in = {.buffer = job->pixels, .pic_w = width, .pic_h = height, .block_w = width, .block_h = height,
               .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .out = {.buffer = frames[back], .buffer_size = FRAME_BYTES, .pic_w = NATIVE_W, .pic_h = NATIVE_H,
                .block_offset_x = native.x1, .block_offset_y = native.y1, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .rotation_angle = job->rot == LV_DISPLAY_ROTATION_270 ? PPA_SRM_ROTATION_ANGLE_270 : PPA_SRM_ROTATION_ANGLE_90,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    const int64_t started = esp_timer_get_time();
    esp_err_t err = ppa_do_scale_rotate_mirror(ppa, &op);
    if (job->traced) {
        T.rotate_us += esp_timer_get_time() - started;
        T.pieces++;
        T.pixels += (uint64_t)width * height;
    }
    if (err == ESP_OK) {
        add_damage(damage, &damage_count, native);
    }
    return err;
}

/* copy an area the candidate buffer missed from the newest frame */
static esp_err_t repair_piece(unsigned candidate, lv_area_t rect)
{
    const lv_area_t *r = &rect;
    const ppa_srm_oper_config_t op = {
        .in = {.buffer = frames[latest], .pic_w = NATIVE_W, .pic_h = NATIVE_H, .block_w = lv_area_get_width(r),
               .block_h = lv_area_get_height(r), .block_offset_x = r->x1, .block_offset_y = r->y1,
               .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .out = {.buffer = frames[candidate], .buffer_size = FRAME_BYTES, .pic_w = NATIVE_W, .pic_h = NATIVE_H,
                .block_offset_x = r->x1, .block_offset_y = r->y1, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.f,
        .scale_y = 1.f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(ppa, &op);
}

/* Pick a back buffer that is no longer scanned (two VSYNC edges after it was replaced: IDF 5.5
   handles DPI VSYNC and the DMA restart in separate interrupts) and bring it up to date. Runs
   for the first piece of a refresh, before anything of that refresh is written, so it may
   leave out everything the refresh redraws (job->cover, LVGL's invalid areas). Without that
   list only the incoming piece is left out, as in Aurora. */
static esp_err_t finish_present(const present_job_t *job)
{
    if (!pending) return ESP_OK;
    unsigned candidate = 3;
    for (;;) {
        uint32_t now = __atomic_load_n(&vsync_sequence, __ATOMIC_ACQUIRE);
        for (unsigned i = 0; i < 3; i++) {
            if (i != latest && (!retired[i] || (int32_t)(now - retired_until[i]) >= 0)) {
                if (candidate == 3 || stale_count[i] < stale_count[candidate]) candidate = i;
            }
        }
        if (candidate < 3) break;
        if (esp_timer_get_time() - published_at >= 150000) return ESP_ERR_TIMEOUT;
        (void)xSemaphoreTake(vsync, pdMS_TO_TICKS(5));
    }
    const int64_t started = esp_timer_get_time();
    const lv_area_t piece = native_area(&job->area, job->rot);
    const lv_area_t *cover = job->ncover ? job->cover : &piece;
    const int ncover = job->ncover ? job->ncover : 1;
    for (unsigned i = 0; i < stale_count[candidate]; i++) {
        lv_area_t rest[MAX_REST];
        int n = subtract(stale[candidate][i], cover, ncover, rest, MAX_REST);
        if (n < 0) { /* too fragmented: repair it whole (nothing of this refresh is written yet) */
            ESP_RETURN_ON_ERROR(repair_piece(candidate, stale[candidate][i]), TAG, "repair");
            continue;
        }
        for (int k = 0; k < n; k++) {
            ESP_RETURN_ON_ERROR(repair_piece(candidate, rest[k]), TAG, "repair");
        }
    }
    if (job->traced) {
        T.repair_us += esp_timer_get_time() - started;
    }
    stale_count[candidate] = 0;
    retired[candidate] = false;
    back = candidate;
    damage_count = 0;
    pending = false;
    return ESP_OK;
}

static esp_err_t present(const present_job_t *job)
{
    /* The DPI driver flips to the complete frame buffer; the row range only limits the cache
       write-back. All writes here are PPA DMA writes, so only the damaged rows matter. */
    int first_row = 0, last_row = NATIVE_H - 1;
    if (damage_count) {
        first_row = damage[0].y1;
        last_row = damage[0].y2;
        for (unsigned i = 1; i < damage_count; i++) {
            if (damage[i].y1 < first_row) first_row = damage[i].y1;
            if (damage[i].y2 > last_row) last_row = damage[i].y2;
        }
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(panel, 0, first_row, NATIVE_W, last_row + 1, frames[back]), TAG, "publish");
    published_sequence = __atomic_load_n(&vsync_sequence, __ATOMIC_ACQUIRE);
    published_at = esp_timer_get_time();
    /* the old frame may have started scanning right before the switch: keep it for two VSYNCs */
    retired[latest] = true;
    retired_until[latest] = published_sequence + 2;
    latest = back;
    for (unsigned i = 0; i < 3; i++) {
        if (i != latest) {
            for (unsigned j = 0; j < damage_count; j++) add_damage(stale[i], &stale_count[i], damage[j]);
        }
    }
    pending = true;
    ++presents;
    if (job->traced) {
        T.present = published_at;
        T.vsync = 0;
        T.want_vsync = true;
        esp_timer_stop(report_timer);
        esp_timer_start_once(report_timer, 60000);
    } else if (T.mark && published_at - T.mark < 600000) {
        T.extra++; /* a second repaint shortly after the switch */
        ESP_LOGI(TAG, "%s: extra frame %lu at %lld ms", T.what, (unsigned long)T.extra, (published_at - T.mark) / 1000);
    }
    return ESP_OK;
}

static void report_cb(void *arg)
{
    (void)arg;
    const int64_t vs = T.vsync ? T.vsync : T.present;
    int64_t build = T.built - T.mark, wait = T.refr_start - T.built, render = T.refr_ready - T.refr_start;
    int64_t tail = T.present - T.refr_ready;
    ESP_LOGI(TAG, "%s: tap->visible %lld ms (published %lld) | build %lld, wait %lld, render %lld (PPA stall %lld), "
                  "last PPA+publish %lld, vsync %lld | %lu pieces, %llu kpx, PPA %llu ms, repair %llu ms",
             T.what, (vs - T.mark) / 1000, (T.present - T.mark) / 1000, build / 1000, wait / 1000, render / 1000,
             T.stall_us / 1000, (tail > 0 ? tail : 0) / 1000, (vs - T.present) / 1000, (unsigned long)T.pieces,
             T.pixels / 1000, T.rotate_us / 1000, T.repair_us / 1000);
}

static void process_presentation(const present_job_t *job)
{
    if (!failed) {
        esp_err_t err = finish_present(job);
        if (err == ESP_OK) err = rotate_piece(job);
        if (err == ESP_OK && job->last) err = present(job);
        if (err != ESP_OK) {
            /* keep the last image; never write into a buffer that may be scanned or hang LVGL */
            failed = true;
            ESP_LOGE(TAG, "presentation stopped: %s", esp_err_to_name(err));
        }
    }
    xSemaphoreGive(presentation_done);
}

static void presentation_worker(void *arg)
{
    (void)arg;
    static present_job_t job; /* ~160 bytes, keep it off the stack */
    for (;;) {
        if (xQueueReceive(presentation_queue, &job, portMAX_DELAY) == pdTRUE) {
            process_presentation(&job);
        }
    }
}

static void wait_for_flush(lv_display_t *display)
{
    (void)display;
    if (transfer_pending) {
        /* LVGL calls this after drawing into its OTHER buffer: CPU and PPA overlap */
        const int64_t started = T.in_refresh ? esp_timer_get_time() : 0;
        xSemaphoreTake(presentation_done, portMAX_DELAY);
        transfer_pending = false;
        if (started) T.stall_us += esp_timer_get_time() - started;
    }
}

static void drain_presentation(void)
{
    if (transfer_pending) {
        wait_for_flush(screen);
        lv_display_flush_ready(screen);
    }
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    wait_for_flush(display);
    static present_job_t job;
    job.area = *area;
    job.pixels = pixels;
    job.rot = lv_display_get_rotation(display);
    job.last = lv_display_flush_is_last(display);
    job.traced = T.in_refresh;
    job.ncover = 0;
    if (frame_first) { /* the areas LVGL redraws in this refresh, all of them rendered in full */
        frame_first = false;
        for (uint32_t i = 0; i < display->inv_p; i++) {
            if (display->inv_area_joined[i]) continue;
            if (job.ncover == MAX_COVER) {
                job.ncover = 0; /* too many: fall back to leaving out only this piece */
                break;
            }
            job.cover[job.ncover++] = native_area(&display->inv_areas[i], job.rot);
        }
    }
    transfer_pending = true;
    if (xQueueSend(presentation_queue, &job, 0) != pdTRUE) {
        transfer_pending = false;
        failed = true;
        ESP_LOGE(TAG, "presentation queue rejected a frame");
        lv_display_flush_ready(display);
    }
}

#if L5_TOUCH_TRACE
/* ---------- temporary lost-tap trace (L5_TOUCH_TRACE in l5_display.h) ---------- */

static const char *TTAG = "l5_touch";
static lv_indev_read_cb_t s_touch_read; /* esp_lvgl_port's reader */
static volatile uint32_t s_int_edges;   /* falling edges on the ST7123 INT line: reports the IC made */
static struct {                         /* LVGL task only */
    int64_t last_read, down_at, up_at, render_start, render_end, render_us, lone_at, max_gap;
    uint32_t edges, lone_edges;
    int32_t x0, y0, x, y, moved;
    unsigned reads;
    bool down;
    lv_obj_t *scroll;
} TT;

static void IRAM_ATTR touch_int_isr(void *arg)
{
    (void)arg;
    s_int_edges++;
}

void l5_touch_trace_busy(const char *what, int64_t start_us)
{
    int64_t ms = (esp_timer_get_time() - start_us) / 1000;
    if (ms > 30) ESP_LOGI(TTAG, "busy %s %lld ms", what, ms);
}

/* the first label text below obj names the row or button in the log */
static const char *first_text(lv_obj_t *obj, int depth)
{
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);
        return t && t[0] ? t : NULL;
    }
    for (uint32_t i = 0; depth > 0 && i < lv_obj_get_child_count(obj) && i < 8; i++) {
        const char *t = first_text(lv_obj_get_child(obj, i), depth - 1);
        if (t) return t;
    }
    return NULL;
}

static const char *obj_name(lv_obj_t *obj, char *out, size_t len)
{
    if (!obj) return "-";
    const char *t = first_text(obj, 3);
    snprintf(out, len, "%p \"%.28s\"", (void *)obj, t ? t : "");
    for (char *p = out; *p; p++) {
        if (*p == '\n') *p = ' ';
    }
    return out;
}

static void render_info(char *out, size_t len, int64_t now)
{
    snprintf(out, len, "last render %lld ms ago, took %lld ms", (now - TT.render_end) / 1000, TT.render_us / 1000);
}

/* every 16 ms in the LVGL task: a read gap above that means the task was busy and could miss a tap */
static void traced_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    int64_t now = esp_timer_get_time();
    int64_t gap = TT.last_read ? (now - TT.last_read) / 1000 : 0;
    TT.last_read = now;
    s_touch_read(indev, data);
    uint32_t edges = s_int_edges, fresh = edges - TT.edges;
    TT.edges = edges;
    char r[80], n[48];

    lv_obj_t *scroll = lv_indev_get_scroll_obj(indev);
    if (scroll != TT.scroll) {
        TT.scroll = scroll;
        if (scroll) {
            ESP_LOGI(TTAG, "scroll begins on %s after %ld px: no CLICKED at release", obj_name(scroll, n, sizeof(n)),
                     (long)TT.moved);
        }
    }
    if (data->state == LV_INDEV_STATE_PRESSED) {
        int32_t x = data->point.x, y = data->point.y;
        if (!TT.down) {
            TT.down = true;
            TT.down_at = now;
            TT.x0 = x;
            TT.y0 = y;
            TT.moved = 0;
            TT.reads = 0;
            TT.max_gap = gap;
            render_info(r, sizeof(r), now);
            ESP_LOGI(TTAG, "down %ld,%ld | read gap %lld ms, INT +%lu | %s", (long)x, (long)y, gap, (unsigned long)fresh, r);
        }
        TT.reads++;
        if (gap > TT.max_gap) TT.max_gap = gap;
        int32_t dx = x > TT.x0 ? x - TT.x0 : TT.x0 - x, dy = y > TT.y0 ? y - TT.y0 : TT.y0 - y;
        if (dx > TT.moved) TT.moved = dx;
        if (dy > TT.moved) TT.moved = dy;
        TT.x = x;
        TT.y = y;
        return;
    }
    if (TT.down) {
        TT.down = false;
        TT.up_at = now;
        ESP_LOGI(TTAG, "up %ld,%ld | held %lld ms, %u reads, moved %ld px, longest read gap %lld ms", (long)TT.x,
                 (long)TT.y, (now - TT.down_at) / 1000, TT.reads, (long)TT.moved, gap > TT.max_gap ? gap : TT.max_gap);
        return;
    }
    if (!fresh || now - TT.up_at < 200000) return; /* the IC also reports the lift-off */
    if (gap > 40) { /* the IC reported something while the LVGL task was busy, and the read sees no touch */
        render_info(r, sizeof(r), now);
        ESP_LOGW(TTAG, "INT +%lu during a %lld ms read gap, no touch in the read: missed tap? | %s", (unsigned long)fresh,
                 gap, r);
        return;
    }
    TT.lone_edges += fresh; /* INT without a touch report, at most one line per second */
    if (now - TT.lone_at > 1000000) {
        ESP_LOGI(TTAG, "INT +%lu without a touch report (IC wake-up or idle scan?)", (unsigned long)TT.lone_edges);
        TT.lone_edges = 0;
        TT.lone_at = now;
    }
}

/* LVGL sends these pointer events to the indev too, with the object as parameter */
static void trace_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    const char *what = code == LV_EVENT_PRESSED         ? "PRESSED"
                       : code == LV_EVENT_RELEASED      ? "RELEASED"
                       : code == LV_EVENT_SHORT_CLICKED ? "SHORT_CLICKED"
                       : code == LV_EVENT_CLICKED       ? "CLICKED"
                       : code == LV_EVENT_LONG_PRESSED  ? "LONG_PRESSED"
                                                        : NULL;
    if (!what) return;
    char n[48], m[48];
    lv_obj_t *obj = lv_event_get_param(e);
    lv_obj_t *scroll = code == LV_EVENT_RELEASED ? lv_indev_get_scroll_obj(touch_input) : NULL;
    ESP_LOGI(TTAG, "%s %s%s%s", what, obj_name(obj, n, sizeof(n)), scroll ? " | was a scroll of " : "",
             scroll ? obj_name(scroll, m, sizeof(m)) : "");
}

static void touch_trace_start(lv_indev_t *indev, esp_lcd_touch_handle_t touch)
{
    s_touch_read = lv_indev_get_read_cb(indev);
    lv_indev_set_read_cb(indev, traced_read);
    lv_indev_add_event_cb(indev, trace_event, LV_EVENT_ALL, NULL);
    /* ST7123 boards: INT on GPIO 23, the reader above has its interrupt removed (GT911 boards hold it low) */
    gpio_num_t pin = touch->config.int_gpio_num;
    bool irq = pin != GPIO_NUM_NC && gpio_set_intr_type(pin, GPIO_INTR_NEGEDGE) == ESP_OK &&
               gpio_isr_handler_add(pin, touch_int_isr, NULL) == ESP_OK && gpio_intr_enable(pin) == ESP_OK;
    ESP_LOGI(TTAG, "trace on: read every 16 ms in the LVGL task, scroll limit %u px, long press %u ms, INT counter %s",
             (unsigned)indev->scroll_limit, (unsigned)indev->long_press_time, irq ? "on (GPIO 23)" : "off");
}
#endif

static void refr_event(lv_event_t *e)
{
#if L5_TOUCH_TRACE
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
        TT.render_start = esp_timer_get_time();
    } else {
        TT.render_end = esp_timer_get_time();
        TT.render_us = TT.render_end - TT.render_start;
        if (TT.render_us > 60000) ESP_LOGI(TTAG, "busy render %lld ms", TT.render_us / 1000);
    }
#endif
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
        frame_first = true;
        if (T.armed) { /* the first refresh after the switch: everything it draws is traced */
            T.armed = false;
            T.in_refresh = true;
            T.refr_start = esp_timer_get_time();
            T.stall_us = 0;
            T.rotate_us = T.repair_us = T.pixels = 0;
            T.pieces = 0;
        }
    } else if (T.in_refresh) { /* LV_EVENT_REFR_READY */
        T.in_refresh = false;
        T.refr_ready = esp_timer_get_time();
    }
    lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, NULL);
}

static void wake_display(lv_event_t *event)
{
    (void)event;
    lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, NULL);
}

void l5_display_mark(const char *what)
{
    strlcpy(T.what, what, sizeof(T.what));
    T.mark = esp_timer_get_time();
    T.built = T.mark;
    T.extra = 0;
    T.armed = false; /* armed by l5_display_mark_built(), when the view is ready */
}

void l5_display_mark_built(void)
{
    T.built = esp_timer_get_time();
    T.armed = true;
}

void l5_display_refresh_now(void)
{
    if (screen) {
        lv_timer_ready(lv_display_get_refr_timer(screen)); /* no wait for the next 16 ms period */
    }
}

lv_indev_t *l5_display_touch(void)
{
    return touch_input;
}

esp_err_t l5_display_reveal(uint8_t brightness)
{
    if (!screen || failed) return ESP_ERR_INVALID_STATE;
    drain_presentation();
    lv_refr_now(screen);
    drain_presentation();
    if (failed || !presents) return ESP_ERR_INVALID_STATE;
    const int64_t started = esp_timer_get_time();
    while ((int32_t)(__atomic_load_n(&vsync_sequence, __ATOMIC_ACQUIRE) - published_sequence) < 2) {
        if (esp_timer_get_time() - started >= 150000) return ESP_ERR_TIMEOUT;
        (void)xSemaphoreTake(vsync, pdMS_TO_TICKS(5));
    }
    ESP_RETURN_ON_ERROR(bsp_display_brightness_set(brightness > 100 ? 100 : brightness), TAG, "backlight");
    lv_display_trigger_activity(screen);
    return ESP_OK;
}

/* what the probe below found: the ST712x touch of the later boards (0x55), and its firmware byte */
static bool s_st712x;
static uint8_t s_touch_fw;

/* BSP 1.3.2 keeps its board detection private but needs 965 Mbps for the ST7121 panel
   (touch firmware byte 1 at register 0 of the ST712x touch, 3 = ST7123). Same probe as Aurora.
   A board that matches none of the known ones is not started: bsp_display_new() stops at an assert() for it (a
   panic, and a panic boots the app in otadata), or its panel would get a lane rate and an init sequence made for
   another one. NULL when the board is known, else why not (for the NO DISPLAY line). The same tests as the BSP's
   own detection (ST712x at 0x55 with firmware byte 1 or 3, else GT911 at 0x14). */
static const char *detect_panel(uint32_t *lane_mbps)
{
    *lane_mbps = 1000;
    if (bsp_feature_enable(BSP_FEATURE_TOUCH, true) != ESP_OK) {
        return "touch power (TOUCH_EN on IO expander 0x43) could not be set";
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (i2c_master_probe(bus, 0x55, 100) != ESP_OK) {
        if (i2c_master_probe(bus, 0x14, 100) == ESP_OK) return NULL; /* GT911: the first boards, ILI9881C */
        return "no touch controller at I2C 0x55 (ST712x) or 0x14 (GT911): a Tab5 board revision this launcher does not know";
    }
    s_st712x = true;
    const i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x55, .scl_speed_hz = 400000};
    i2c_master_dev_handle_t dev;
    uint8_t fw = 0;
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &dev);
    if (err == ESP_OK) {
        const uint8_t reg[] = {0, 0};
        err = i2c_master_transmit_receive(dev, reg, 2, &fw, 1, 100);
        i2c_master_bus_rm_device(dev);
    }
    s_touch_fw = fw;
    if (err == ESP_OK && fw == 1) {
        *lane_mbps = 965; /* ST7121 */
        return NULL;
    }
    if (err == ESP_OK && fw == 3) return NULL; /* ST7123 */
    ESP_LOGE(TAG, "ST712x touch firmware byte %u (1 = ST7121 panel, 3 = ST7123)", fw);
    return "ST712x touch with an unknown firmware byte: a Tab5 panel this launcher does not know";
}

const char *l5_display_new_panel(void)
{
    if (!s_st712x) return NULL;
    return s_touch_fw == 1 ? "ST7121 + ST712x" : s_touch_fw == 3 ? "ST7123 + ST712x" : "ST712x display and touch";
}

static esp_err_t enable_lcd_power(void)
{
    esp_err_t err = ESP_FAIL;
    bool bus_reset_attempted = false;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        err = bsp_i2c_init();
        i2c_master_bus_handle_t bus = err == ESP_OK ? bsp_i2c_get_handle() : NULL;
        if (err == ESP_OK && !bus) err = ESP_ERR_INVALID_STATE;
        if (err == ESP_OK) err = bsp_io_expander_init() ? bsp_feature_enable(BSP_FEATURE_LCD, true) : ESP_FAIL;
        if (err == ESP_OK) return ESP_OK;
        ESP_LOGW(TAG, "LCD power attempt %u/3 failed: %s", attempt + 1, esp_err_to_name(err));
        if (attempt == 2) break;
        if (bus && !bus_reset_attempted) { /* start-up only, before touch / RTC exist */
            bus_reset_attempted = true;
            esp_err_t reset_error = i2c_master_bus_reset(bus);
            if (reset_error != ESP_OK) ESP_LOGW(TAG, "I2C bus recovery failed: %s", esp_err_to_name(reset_error));
        }
        vTaskDelay(pdMS_TO_TICKS(50 * (attempt + 1)));
    }
    return err;
}

/* The DPI frame buffer reader (DW-GDMA master 1) must win against bursty redraw / SD traffic,
   otherwise a missed PSRAM deadline flashes the panel blue. Read priority only, once at start. */
static void prioritize_scanout_reads(void)
{
    const uint32_t write_priority = AXI_ICM.mst_awqos_reg0.reg_gdma_mst2_awqos;
    axi_icm_ll_set_dw_gdma_qos_arbiter_prio(1, write_priority, 15);
}

/* every start-up step has a name: when one fails, the log and the NO DISPLAY line (l5_board.c) say which */
#define STEP(call, what)                                                \
    do {                                                                \
        *step = (what);                                                 \
        esp_err_t step_err_ = (call);                                   \
        if (step_err_ != ESP_OK) {                                      \
            ESP_LOGE(TAG, "%s: %s", *step, esp_err_to_name(step_err_)); \
            return step_err_;                                           \
        }                                                               \
    } while (0)

esp_err_t l5_display_start(uint16_t rot, lv_display_t **result, const char **step)
{
    *result = NULL;
    lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
    port.task_priority = 4;
    port.task_stack = 16384;
    port.task_affinity = 0;
    port.task_max_sleep_ms = 100;
    port.timer_period_ms = 5;
    STEP(enable_lcd_power(), "LCD power (I2C bus, IO expander 0x43, LCD_EN)");
    STEP(lvgl_port_init(&port), "LVGL task");
    uint32_t lane_mbps;
    *step = "display / touch detection";
    const char *unknown = detect_panel(&lane_mbps);
    if (unknown) { /* the NO DISPLAY line names the reason, the log both */
        ESP_LOGE(TAG, "%s: %s", *step, unknown);
        *step = unknown;
        return ESP_ERR_NOT_SUPPORTED;
    }
    bsp_display_config_t config = {.dsi_bus = {.lane_bit_rate_mbps = lane_mbps}};
    esp_lcd_panel_io_handle_t io;
    prioritize_scanout_reads();
    STEP(bsp_display_new(&config, &panel, &io), "BSP panel start (MIPI-DSI)");
    STEP(esp_lcd_dpi_panel_get_frame_buffer(panel, 3, (void **)&frames[0], (void **)&frames[1], (void **)&frames[2]),
         "3 frame buffers (CONFIG_BSP_LCD_DPI_BUFFER_NUMS=3)");
    const ppa_client_config_t ppa_config = {.oper_type = PPA_OPERATION_SRM}; /* burst: max (128 B) by default */
    STEP(ppa_register_client(&ppa_config, &ppa), "PPA");
    vsync = xSemaphoreCreateBinary();
    STEP(vsync ? ESP_OK : ESP_ERR_NO_MEM, "VSYNC semaphore");
    const esp_timer_create_args_t report_args = {.callback = report_cb, .name = "l5_frame"};
    STEP(esp_timer_create(&report_args, &report_timer), "report timer");
    const esp_lcd_dpi_panel_event_callbacks_t callbacks = {.on_refresh_done = on_refresh};
    STEP(esp_lcd_dpi_panel_register_event_callbacks(panel, &callbacks, NULL), "VSYNC callback");
    const unsigned caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA;
    void *draw0 = heap_caps_aligned_alloc(64, DRAW_PIXELS * 2, caps);
    void *draw1 = heap_caps_aligned_alloc(64, DRAW_PIXELS * 2, caps);
    if (!draw0 || !draw1) {
        free(draw0);
        free(draw1);
    }
    STEP(draw0 && draw1 ? ESP_OK : ESP_ERR_NO_MEM, "LVGL draw buffers in PSRAM");
    const bool locked = lvgl_port_lock(1000);
    if (!locked) {
        free(draw0);
        free(draw1);
    }
    STEP(locked ? ESP_OK : ESP_ERR_TIMEOUT, "LVGL lock");
    lv_display_t *display = lv_display_create(NATIVE_W, NATIVE_H);
    presentation_queue = xQueueCreate(1, sizeof(present_job_t));
    presentation_done = xSemaphoreCreateBinary();
    if (!display || !presentation_queue || !presentation_done ||
        xTaskCreatePinnedToCore(presentation_worker, "l5_present", 6144, NULL, 5, NULL, 0) != pdPASS) {
        lvgl_port_unlock();
        STEP(ESP_ERR_NO_MEM, "LVGL display, presentation task");
    }
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw0, draw1, DRAW_PIXELS * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_flush_wait_cb(display, wait_for_flush);
    lv_display_set_rotation(display, rot == 180 ? LV_DISPLAY_ROTATION_270 : LV_DISPLAY_ROTATION_90);
    lv_display_add_event_cb(display, wake_display, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(display, wake_display, LV_EVENT_REFR_REQUEST, NULL);
    lv_display_add_event_cb(display, refr_event, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(display, refr_event, LV_EVENT_REFR_READY, NULL);
    lvgl_port_unlock();
    screen = display;
    *result = display;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "LCD on");
    if (esp_register_shutdown_handler(blank_on_restart) != ESP_OK) {
        ESP_LOGW(TAG, "no shutdown handler: the panel shows noise during a restart");
    }

    esp_lcd_touch_handle_t touch = NULL;
    const bsp_touch_config_t touch_config = {0};
    esp_err_t err = bsp_touch_new(&touch_config, &touch);
    if (err == ESP_OK) {
        const lvgl_port_touch_cfg_t touch_port = {.disp = display, .handle = touch};
        touch_input = lvgl_port_add_touch(&touch_port);
        if (touch_input && lvgl_port_lock(1000)) {
            /* one regular polling path instead of IRQ + fallback reader */
            esp_lcd_touch_register_interrupt_callback(touch, NULL);
            lv_indev_set_mode(touch_input, LV_INDEV_MODE_TIMER);
            lv_timer_set_period(lv_indev_get_read_timer(touch_input), 16);
#if L5_TOUCH_TRACE
            touch_trace_start(touch_input, touch);
#endif
            lvgl_port_unlock();
        }
    }
    ESP_LOGI(TAG, "atomic landscape: 3 frame buffers, PPA rotation %d, touch %s", rot == 180 ? 270 : 90,
             touch_input ? "ok" : esp_err_to_name(err));
    return ESP_OK;
}
