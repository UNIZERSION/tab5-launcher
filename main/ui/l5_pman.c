/* PMAN: partition manager on the real table at 0x8000. Port of renderPM / pmAction (views2.js)
   and L5.pickScheme / L5.applyScheme (app.js). Every layout change goes through change_layout():
   build + validate, a confirmation that lists what is erased, then l5_pt_apply() in a task. */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "l5_board.h"
#include "l5_config.h"
#include "l5_inst.h"
#include "l5_ptable.h"
#include "l5_slots.h"
#include "l5_ui.h"

#define FREE_LABEL "(free)"      /* unallocated before aurora_cfg: room for FAT */
#define GAP_LABEL "(spiffs gap)" /* unallocated after aurora_cfg: room for SPIFFS */
#define MAX_ROWS (L5_PT_MAX + 4)
#define MAX_BINS 16

static l5_flash_map_t s_map;  /* re-read on every render */
static char s_sel[17] = "app0";
static bool s_busy;

typedef struct {
    const l5_part_t *p; /* NULL for unallocated space */
    uint32_t offset, size;
    bool fixed;
    lv_color_t color;
} row_t;
static row_t s_rows[MAX_ROWS];
static int s_nrows;

/* widgets that follow the selection in place: a tap redraws the old and new row and segment and
   rebuilds the side pane, without re-reading the table and rebuilding the view; NULL while PMAN
   is not built */
static struct {
    lv_obj_t *seg[MAX_ROWS], *tr[MAX_ROWS], *mark[MAX_ROWS], *side;
} V;

static void view_deleted_cb(lv_event_t *e)
{
    (void)e;
    memset(&V, 0, sizeof(V));
}

static void paint_sel(const row_t *sel)
{
    for (int r = 0; r < s_nrows; r++) {
        bool on = &s_rows[r] == sel;
        if (V.seg[r]) lv_obj_set_state(V.seg[r], LV_STATE_CHECKED, on);
        if (V.tr[r]) lv_obj_set_state(V.tr[r], LV_STATE_CHECKED, on);
        if (V.mark[r]) lv_obj_set_flag(V.mark[r], LV_OBJ_FLAG_HIDDEN, !on);
    }
}

bool l5_pman_busy(void)
{
    return s_busy;
}

static bool app_slot(const l5_part_t *p)
{
    return p && p->type == ESP_PARTITION_TYPE_APP && p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
           p->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX;
}

static bool aurora_part(const l5_part_t *p)
{
    return p && strcmp(p->label, L5_AURORA_LABEL) == 0 && p->offset == L5_AURORA_ADDR;
}

/* SPIFFS / FAT in the scheme-managed area (aurora_cfg is data too, but locked) */
static bool pool_data(const l5_part_t *p)
{
    return p && p->type == ESP_PARTITION_TYPE_DATA && p->offset >= s_map.pool_start && p->offset < s_map.pool_end &&
           (p->subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS || p->subtype == ESP_PARTITION_SUBTYPE_DATA_FAT);
}

/* the current layout under the aurora_cfg rule: starting point of every derived change */
/* AUTO PARTITION puts a firmware's own data partitions next to vfs: they keep the size its table asks for */
static int extra_index(const l5_layout_t *l, const char *label)
{
    for (uint32_t i = 0; i < l->nextra && i < L5_MAX_EXTRA; i++) {
        if (!strcmp(l->extra[i].label, label)) return (int)i;
    }
    return -1;
}

/* the one partition after aurora_cfg (before the rule: the SPIFFS anywhere) */
static bool gap_part(const l5_part_t *p)
{
    return s_map.aurora ? p->offset >= L5_GAP_ADDR : p->subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS;
}

static l5_layout_t base(void)
{
    return l5_pt_base_layout(&s_map);
}

static const char *type_str(const l5_part_t *p)
{
    return !p ? "\xE2\x80\x94" : p->type == ESP_PARTITION_TYPE_APP ? "app" : "data";
}

static const char *sub_str(const l5_part_t *p, char *buf, size_t len)
{
    if (!p) return "\xE2\x80\x94";
    if (app_slot(p)) {
        snprintf(buf, len, "ota_%d", p->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN);
        return buf;
    }
    if (p->type == ESP_PARTITION_TYPE_APP) return p->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY ? "factory" : "app";
    switch (p->subtype) {
    case ESP_PARTITION_SUBTYPE_DATA_OTA: return "ota";
    case ESP_PARTITION_SUBTYPE_DATA_NVS: return "nvs";
    case ESP_PARTITION_SUBTYPE_DATA_PHY: return "phy";
    case ESP_PARTITION_SUBTYPE_DATA_COREDUMP: return "coredump";
    case ESP_PARTITION_SUBTYPE_DATA_SPIFFS: return "spiffs";
    case ESP_PARTITION_SUBTYPE_DATA_FAT: return "fat";
    default: snprintf(buf, len, "0x%02x", p->subtype); return buf;
    }
}

static lv_color_t part_color(const l5_part_t *p)
{
    if (!p) return lv_color_hex(0x26282c);
    if (app_slot(p)) {
        return (p->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN) % 2 ? l5_mix(L5P.accent, L5_PANEL, 178) : L5P.accent;
    }
    if (p->type == ESP_PARTITION_TYPE_APP) return lv_color_hex(0x3b3e44);
    switch (p->subtype) {
    case ESP_PARTITION_SUBTYPE_DATA_OTA: return lv_color_hex(0x7b7f87);
    case ESP_PARTITION_SUBTYPE_DATA_NVS: return lv_color_hex(0x5d6168);
    case ESP_PARTITION_SUBTYPE_DATA_SPIFFS: return lv_color_hex(0x9aa0a8);
    case ESP_PARTITION_SUBTYPE_DATA_FAT: return lv_color_hex(0xc9ccd2);
    case ESP_PARTITION_SUBTYPE_DATA_COREDUMP: return lv_color_hex(0x4b4e54);
    default: return lv_color_hex(0x6f737c);
    }
}

static void add_row(const l5_part_t *p, uint32_t off, uint32_t size)
{
    if (s_nrows >= MAX_ROWS) return;
    row_t *r = &s_rows[s_nrows++];
    r->p = p;
    r->offset = off;
    r->size = size;
    r->fixed = p && (!(p->offset >= s_map.pool_start && p->offset < s_map.pool_end) || aurora_part(p));
    r->color = part_color(p);
}

static const char *row_label(const row_t *r)
{
    return r->p ? r->p->label : r->offset >= L5_GAP_ADDR ? GAP_LABEL : FREE_LABEL;
}

static void build_rows(void)
{
    s_nrows = 0;
    uint32_t prev = L5_PT_OFFSET + 0x1000;
    for (int i = 0; i < s_map.table.n; i++) {
        const l5_part_t *p = &s_map.table.p[i];
        uint32_t gs = prev > s_map.pool_start ? prev : s_map.pool_start;
        uint32_t ge = p->offset < s_map.pool_end ? p->offset : s_map.pool_end;
        if (s_map.pool_end > s_map.pool_start && ge >= gs + 0x1000) {
            add_row(NULL, gs, ge - gs); /* L5.table "(free)" */
        }
        add_row(p, p->offset, p->size);
        prev = p->offset + p->size;
    }
    uint32_t gs = prev > s_map.pool_start ? prev : s_map.pool_start;
    if (s_map.pool_end > gs + 0x1000) {
        add_row(NULL, gs, s_map.pool_end - gs);
    }
}

static const row_t *find_row(const char *label)
{
    for (int i = 0; i < s_nrows; i++) {
        if (strcmp(row_label(&s_rows[i]), label) == 0) {
            return &s_rows[i];
        }
    }
    return NULL;
}

/* free KB before aurora_cfg (for app slots and FAT) */
static uint32_t free_kb(void)
{
    if (!s_map.managed) return 0;
    const l5_layout_t l = base();
    return L5_FRONT_KB - l5_layout_front_kb(&l);
}

/* "64% · Sensor Hub" or the state of a data partition */
static void used_text(const row_t *r, char *out, size_t len, int *pct)
{
    *pct = -1;
    if (!r->p) {
        snprintf(out, len, "\xE2\x80\x94");
    } else if (r->fixed) {
        *pct = 100;
        snprintf(out, len, "100%%");
    } else if (app_slot(r->p)) {
        int k = r->p->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
        const l5_slot_t *s = k < g_slots.n ? &g_slots.slot[k] : NULL;
        *pct = s && s->used && r->size ? (int)((uint64_t)s->size * 100 / r->size) : 0;
        snprintf(out, len, "%d%%", *pct);
    } else {
        int blank = l5_part_blank(r->p->label);
        snprintf(out, len, "%s", blank == 1 ? "EMPTY" : blank == 0 ? "IN USE" : "?");
    }
}

/* ---------- scheme changes ---------- */

typedef struct {
    l5_flash_map_t map;
    l5_ptable_t next;
    char scheme_id[12];
    l5_progress_t *progress;
    int pct;
} apply_job_t;

static void apply_progress(int pct, const char *label, void *user)
{
    apply_job_t *j = user;
    if (pct == j->pct && !label) return;
    j->pct = pct;
    if (l5_board_lock(0)) {
        l5_progress_set(j->progress, pct, label);
        l5_board_unlock();
    }
}

static void restart_cb(bool ok, void *user)
{
    (void)user;
    if (ok) l5_restart();
}

static void apply_task(void *arg)
{
    apply_job_t *j = arg;
    char why[220] = "";
    bool written = false;
    esp_err_t err = l5_pt_apply(&j->map, &j->next, apply_progress, j, &written, why, sizeof(why));
    if (written) { /* cfg.scheme follows what flash holds now */
        strlcpy(g_cfg.scheme, j->scheme_id, sizeof(g_cfg.scheme));
        if (l5_board_sd_ready()) l5_cfg_save_now(); /* the restart follows */
    }
    l5_board_lock(0);
    l5_progress_close(j->progress);
    if (err == ESP_OK) {
        l5_restart(); /* RESTARTING... then esp_restart into the launcher with the new table */
    } else if (written) { /* the table is complete; only erasing stopped */
        s_busy = false;
        l5_ask("PARTITION", "Restart needed", why, "LATER", "RESTART NOW", L5_ICON_RESTART, restart_cb, NULL);
    } else {
        s_busy = false;
        l5_info("PARTITION", "Partition table not changed", why);
        l5_ui_render();
    }
    l5_board_unlock();
    free(j);
    vTaskDelete(NULL);
}

static void confirm_apply_cb(bool ok, void *user)
{
    apply_job_t *j = user;
    if (!ok) {
        free(j);
        return;
    }
    s_busy = true; /* the progress scrim covers the screen; the power menu refuses while busy */
    j->pct = -1;
    j->progress = l5_progress_open("PARTITION", "Writing partition table");
    l5_progress_set(j->progress, 0, "Checking the new table");
    if (xTaskCreatePinnedToCore(apply_task, "l5_ptable", 8192, j, 4, NULL, 1) != pdPASS) {
        s_busy = false;
        l5_progress_close(j->progress);
        free(j);
        l5_info("PARTITION", "Partition table not changed", "Not enough memory for the task.");
    }
}

/* " Erased: vfs, spiffs. Removed with its data: sys." / " The data partitions stay." */
static void data_sentence(const l5_pt_diff_t *d, char *out, size_t len)
{
    out[0] = 0;
    if (d->erased[0]) snprintf(out, len, " Erased: %s.", d->erased);
    if (d->removed[0]) {
        size_t n = strlen(out);
        snprintf(out + n, len - n, " Removed with %s data: %s.", strchr(d->removed, ',') ? "their" : "its", d->removed);
    }
    if (!out[0]) snprintf(out, len, " The data partitions stay.");
}

static void change_layout(const l5_layout_t *l, const char *title, const char *scheme_id, const char *extra)
{
    apply_job_t *j = calloc(1, sizeof(apply_job_t));
    char why[220];
    if (!j) {
        l5_toast("Not enough memory");
        return;
    }
    if (l5_pt_read_map(&j->map) != ESP_OK) {
        l5_info("PARTITION", "Cannot read the partition table", j->map.why);
        free(j);
        return;
    }
    if (j->map.managed && j->map.aurora && l5_layout_equal(&j->map.layout, l)) {
        l5_toast("That is the current layout");
        free(j);
        return;
    }
    if (l5_pt_build(&j->map, l, &j->next, why, sizeof(why)) != ESP_OK) {
        l5_info("PARTITION", "Cannot use this layout", why);
        free(j);
        return;
    }
    l5_pt_diff_t d;
    l5_pt_diff(&j->map, &j->next, &d);
    strlcpy(j->scheme_id, scheme_id ? scheme_id : "custom", sizeof(j->scheme_id));
    char slot[16], sp[16], fat[16], data[220], extras[96] = "", apps[300], body[900];
    l5_fmt_kb(slot, sizeof(slot), l->slot_kb);
    l5_fmt_kb(sp, sizeof(sp), l->spiffs_kb);
    l5_fmt_kb(fat, sizeof(fat), l->fat_kb);
    for (uint32_t i = 0; i < l->nextra; i++) { /* AUTO PARTITION: the firmware's own partitions */
        char kb[16];
        l5_fmt_kb(kb, sizeof(kb), l->extra[i].kb);
        size_t n = strlen(extras);
        snprintf(extras + n, sizeof(extras) - n, " \xC2\xB7 %s %sB", l->extra[i].label, kb);
    }
    data_sentence(&d, data, sizeof(data));
    if (d.apps) l5w_apps_erased(apps, sizeof(apps));
    else snprintf(apps, sizeof(apps), "The installed apps stay.");
    snprintf(body, sizeof(body),
             "%lu app slot%s \xC3\x97 %sB%s \xC2\xB7 %s %s \xC2\xB7 %s %s.\n\n%s%s%s%s\n\nLauncher stays in its own "
             "partition. The Tab5 restarts at the end: do not turn it off or press RESET before that.",
             (unsigned long)l->n, l->n == 1 ? "" : "s", slot, extras, l5_layout_gap_label(l), l->spiffs_kb ? sp : "none",
             l5_layout_fat_label(l), l->fat_kb ? fat : "none",
             apps, data, extra ? " " : "",
             extra ? extra : "");
    l5_confirm("PARTITION", title, body, "ERASE AND APPLY", true, confirm_apply_cb, j);
}

static void scheme_cb(const char *id, void *user);

void l5_pman_use_scheme(const char *id)
{
    const l5_scheme_def_t *s = l5_scheme_find(id);
    if (!s) return;
    l5_layout_t l = l5_layout_of_scheme(s);
    char title[48];
    snprintf(title, sizeof(title), "Change to %s?", s->label);
    change_layout(&l, title, s->id, NULL);
}

/* Custom: slot count -> slot size -> SPIFFS -> FAT */
static l5_layout_t s_custom;

static void custom_fat_cb(const char *id, void *user)
{
    (void)user;
    if (!id) return;
    s_custom.fat_kb = (uint32_t)atol(id);
    change_layout(&s_custom, "Apply the custom layout?", "custom", NULL);
}

static void custom_sp_cb(const char *id, void *user)
{
    (void)user;
    if (!id) return;
    s_custom.spiffs_kb = (uint32_t)atol(id);
    uint32_t rest = L5_FRONT_KB - s_custom.n * s_custom.slot_kb; /* FAT ends at aurora_cfg (0xF10000) */
    char ids[2][12], labels[2][64], kb[16];
    l5_fmt_kb(kb, sizeof(kb), rest);
    snprintf(ids[0], sizeof(ids[0]), "%lu", (unsigned long)rest);
    snprintf(labels[0], sizeof(labels[0]), "FAT gets the rest \xC2\xB7 %sB", kb);
    snprintf(ids[1], sizeof(ids[1]), "0");
    snprintf(labels[1], sizeof(labels[1]), "No FAT \xC2\xB7 %sB stay free", kb);
    const l5_menu_item_t items[] = {
        {rest >= L5_MIN_DATA_KB ? ids[0] : NULL, L5_ICON_PLUS, labels[0], false},
        {ids[1], L5_ICON_X, labels[1], false},
    };
    l5_menu("PARTITION", "FAT (vfs) partition", items, 2, custom_fat_cb, NULL);
}

static void custom_slot_cb(const char *id, void *user)
{
    (void)user;
    if (!id) return;
    s_custom.slot_kb = (uint32_t)atol(id);
    /* SPIFFS lives in the gap after aurora_cfg: all of it or nothing */
    char ids[2][12], labels[2][64], kb[16];
    l5_fmt_kb(kb, sizeof(kb), L5_GAP_KB);
    snprintf(ids[0], sizeof(ids[0]), "%u", (unsigned)L5_GAP_KB);
    snprintf(labels[0], sizeof(labels[0]), "SPIFFS %sB after aurora_cfg", kb);
    snprintf(ids[1], sizeof(ids[1]), "0");
    snprintf(labels[1], sizeof(labels[1]), "No SPIFFS \xC2\xB7 %sB stay free", kb);
    const l5_menu_item_t items[] = {
        {ids[0], L5_ICON_PLUS, labels[0], false},
        {ids[1], L5_ICON_X, labels[1], false},
    };
    l5_menu("PARTITION", "SPIFFS partition", items, 2, custom_sp_cb, NULL);
}

/* L5 resize steps: 512 KB below 2 MB, then 1 MB (coarser when the list would be too long) */
static int size_options(uint32_t min, uint32_t max, uint32_t opts[], int maxn)
{
    uint32_t big = 1024;
    int n;
    for (;;) {
        n = 0;
        for (uint32_t v = min; v <= max; v += v < 2048 ? 512 : big) n++;
        if (n < maxn) break;
        big *= 2;
    }
    n = 0;
    for (uint32_t v = min; v <= max && n < maxn - 1; v += v < 2048 ? 512 : big) opts[n++] = v;
    if (max >= min && (!n || opts[n - 1] != max)) opts[n++] = max;
    return n;
}

static void custom_n_cb(const char *id, void *user)
{
    (void)user;
    if (!id) return;
    s_custom.n = (uint32_t)atol(id);
    uint32_t max = L5_FRONT_KB / s_custom.n / 64 * 64; /* FAT may then be left out */
    uint32_t opts[16];
    int n = size_options(L5_MIN_APP_KB, max, opts, 12);
    l5_menu_item_t items[16];
    char ids[16][12], labels[16][48];
    for (int i = 0; i < n; i++) {
        char kb[16];
        l5_fmt_kb(kb, sizeof(kb), opts[i]);
        snprintf(ids[i], sizeof(ids[i]), "%lu", (unsigned long)opts[i]);
        snprintf(labels[i], sizeof(labels[i]), "%sB%s%s", kb, s_custom.n > 1 ? " each" : "", opts[i] == max ? " \xC2\xB7 max" : "");
        items[i] = (l5_menu_item_t){ids[i], L5_ICON_PMAN, labels[i], false};
    }
    l5_menu("PARTITION", "Size of each app slot", items, n, custom_slot_cb, NULL);
}

static void custom_start(void)
{
    memset(&s_custom, 0, sizeof(s_custom));
    l5_menu_item_t items[4];
    char ids[4][4], labels[4][48];
    for (int i = 0; i < 4; i++) {
        char kb[16];
        l5_fmt_kb(kb, sizeof(kb), L5_FRONT_KB / (i + 1) / 64 * 64);
        snprintf(ids[i], sizeof(ids[i]), "%d", i + 1);
        snprintf(labels[i], sizeof(labels[i]), "%d app slot%s \xC2\xB7 up to %sB each", i + 1, i ? "s" : "", kb);
        items[i] = (l5_menu_item_t){ids[i], L5_ICON_PMAN, labels[i], false};
    }
    l5_menu("PARTITION", "How many app slots?", items, 4, custom_n_cb, NULL);
}

static void scheme_cb(const char *id, void *user)
{
    (void)user;
    if (!id) return;
    if (strcmp(id, "custom") == 0) {
        custom_start();
        return;
    }
    l5_pman_use_scheme(id);
}

void l5_pman_pick_scheme(void)
{
    if (l5_pt_read_map(&s_map) != ESP_OK) {
        l5_info("PARTITION", "Cannot read the partition table", s_map.why);
        return;
    }
    l5_menu_item_t items[8];
    char labels[8][96];
    int k = 0;
    bool known = false;
    for (int i = 0; i < l5_scheme_count && k < 7; i++, k++) {
        l5_layout_t l = l5_layout_of_scheme(&l5_schemes[i]);
        char slot[16], sp[16], fat[16];
        l5_fmt_kb(slot, sizeof(slot), l.slot_kb);
        l5_fmt_kb(sp, sizeof(sp), l.spiffs_kb);
        l5_fmt_kb(fat, sizeof(fat), l.fat_kb);
        snprintf(labels[k], sizeof(labels[k]), "%s \xC2\xB7 %lu\xC3\x97 app %s \xC2\xB7 spiffs %s \xC2\xB7 fat %s", l5_schemes[i].label,
                 (unsigned long)l.n, slot, sp, fat);
        bool cur = s_map.managed && s_map.aurora && l5_layout_equal(&l, &s_map.layout);
        known |= cur;
        items[k] = (l5_menu_item_t){l5_schemes[i].id, cur ? L5_ICON_CHECK : L5_ICON_PMAN, labels[k], false};
    }
    snprintf(labels[k], sizeof(labels[k]), "Custom \xC2\xB7 choose slots, SPIFFS and FAT");
    items[k] = (l5_menu_item_t){"custom", s_map.managed && !known ? L5_ICON_CHECK : L5_ICON_CFG, labels[k], false};
    k++;
    l5_menu("PARTITION", "Partition scheme (16 MB)", items, k, scheme_cb, NULL);
}

/* ---------- format / backup / restore / sanity (worker task + progress) ---------- */

typedef enum { OP_FORMAT_APP, OP_FORMAT_DATA, OP_BACKUP, OP_RESTORE, OP_SANITY } op_kind_t;
typedef struct {
    op_kind_t op;
    char label[17];
    int slot;
    char path[200];
    char name[64];
    l5_progress_t *progress;
    int pct;
    bool merged;          /* SANITY: the file is a merged image, its app was checked */
    uint32_t base, app_offset;
    char note[240];       /* SANITY: why INSTALL refuses the merged image (whole flash copy, the launcher) */
} op_job_t;

static void op_progress(int pct, const char *label, void *user)
{
    op_job_t *j = user;
    if (pct == j->pct && !label) return;
    j->pct = pct;
    if (l5_board_lock(0)) {
        l5_progress_set(j->progress, pct, label);
        l5_board_unlock();
    }
}

static void op_slot_progress(int pct, void *user)
{
    op_progress(pct, NULL, user);
}

/* SANITY CHECK of a file: an app image, or the app inside a merged image (either base) */
static esp_err_t sanity_check(op_job_t *j, l5_image_check_t *c)
{
    uint8_t *head = heap_caps_malloc(L5_FW_PROBE_LEN, MALLOC_CAP_SPIRAM);
    l5_fw_t *fw = heap_caps_calloc(1, sizeof(l5_fw_t), MALLOC_CAP_SPIRAM);
    struct stat st;
    FILE *f = head && fw && stat(j->path, &st) == 0 ? fopen(j->path, "rb") : NULL;
    size_t n = f ? fread(head, 1, L5_FW_PROBE_LEN, f) : 0;
    if (f) fclose(f);
    j->merged = false;
    j->app_offset = j->base = 0;
    j->note[0] = 0;
    if (n) {
        l5_fw_init(fw, L5_FW_APP_ONLY);
        esp_err_t err = l5_fw_from_probe(fw, head, n, (uint32_t)st.st_size, j->note, sizeof(j->note));
        j->merged = fw->format == L5_FW_MERGED && fw->has_table;
        if (j->merged) {
            j->base = fw->base;
            j->app_offset = fw->app_offset;
        }
        if (err == ESP_OK || !j->merged) j->note[0] = 0;
    }
    heap_caps_free(head);
    heap_caps_free(fw);
    return l5_image_check_file(j->path, j->app_offset, c);
}

static void sanity_report(const op_job_t *j, const l5_image_check_t *c)
{
    char title[96], body[700], size[16];
    l5_fmt_bytes(size, sizeof(size), c->file_size);
    snprintf(title, sizeof(title), "%.60s: %s", j->name, c->ok ? "image OK" : "image damaged");
    if (c->chip_id == 0 && !c->segments) {
        snprintf(body, sizeof(body), "%s Size %s.", c->error, size);
    } else {
        uint64_t slot = (uint64_t)g_slots.slot_kb * 1024;
        const char *best = l5_slots_best_scheme(c->image_len);
        int n = j->merged ? snprintf(body, sizeof(body), "Merged image from 0x%X: the app at 0x%X of the file was checked.%s%s\n",
                                     (unsigned)j->base, (unsigned)j->app_offset, j->note[0] ? " " : "", j->note)
                          : 0;
        snprintf(body + n, sizeof(body) - n,
                 "magic 0xE9 \xC2\xB7 chip id 0x%04X (%s) \xC2\xB7 segments %u \xC2\xB7 checksum %s \xC2\xB7 SHA-256 %s.\n%s%s"
                 "%s %s \xC2\xB7 %s%s%s.",
                 c->chip_id, l5_chip_name(c->chip_id), c->segments, c->checksum_ok ? "OK" : "WRONG",
                 !c->hash_appended ? "not appended" : c->hash_ok ? "valid" : "WRONG", c->ok ? "" : c->error, c->ok ? "" : "\n",
                 j->merged ? "File" : "Size", size, c->image_len <= slot ? "the app fits an app slot" : "the app does not fit an app slot in this scheme",
                 c->image_len > slot && best ? ": use " : "", c->image_len > slot && best ? best : "");
    }
    l5_info("SANITY", title, body);
}

static void op_task(void *arg)
{
    op_job_t *j = arg;
    char path[200] = "";
    l5_image_check_t check;
    esp_err_t err = ESP_OK;
    switch (j->op) {
    case OP_FORMAT_APP: err = l5_slots_erase(j->slot, op_slot_progress, j); break;
    case OP_FORMAT_DATA: err = l5_part_erase(j->label, op_progress, j); break;
    case OP_BACKUP: err = l5_part_backup(j->label, path, sizeof(path), op_progress, j); break;
    case OP_RESTORE: err = l5_part_restore(j->label, j->path, op_progress, j); break;
    case OP_SANITY: err = sanity_check(j, &check); break;
    }
    l5_board_lock(0);
    l5_progress_close(j->progress);
    if (j->op == OP_FORMAT_APP) {
        l5_slots_scan();
    }
    if (j->op == OP_SANITY) {
        sanity_report(j, &check);
    } else if (err != ESP_OK) {
        char body[160];
        snprintf(body, sizeof(body), "%s stopped: %s.", j->label, esp_err_to_name(err));
        l5_info("PMAN", "Operation failed", body);
    } else if (j->op == OP_BACKUP) {
        l5_toast("Saved %s", strncmp(path, "/sdcard", 7) == 0 ? path + 7 : path);
    } else {
        l5_toast("%s %s", j->label, j->op == OP_RESTORE ? "restored" : "formatted");
    }
    if (l5_ui_view() == L5_VIEW_PMAN || l5_ui_view() == L5_VIEW_CFG || l5_ui_view() == L5_VIEW_HOME) {
        l5_ui_render();
    }
    l5_board_unlock();
    free(j);
    vTaskDelete(NULL);
}

static void op_start(op_kind_t op, const char *label, int slot, const char *path, const char *name, const char *title)
{
    op_job_t *j = calloc(1, sizeof(op_job_t));
    if (!j) {
        l5_toast("Not enough memory");
        return;
    }
    j->op = op;
    j->slot = slot;
    j->pct = -1;
    strlcpy(j->label, label ? label : "", sizeof(j->label));
    strlcpy(j->path, path ? path : "", sizeof(j->path));
    strlcpy(j->name, name ? name : "", sizeof(j->name));
    j->progress = l5_progress_open(op == OP_SANITY ? "SANITY" : "PMAN", title);
    l5_progress_set(j->progress, op == OP_SANITY ? 30 : 0,
                    op == OP_SANITY ? "Verifying segments + SHA-256" : op == OP_BACKUP ? "Reading the partition" : "Erasing");
    if (xTaskCreatePinnedToCore(op_task, "l5_pman_op", 8192, j, 3, NULL, 1) != pdPASS) {
        l5_progress_close(j->progress);
        free(j);
        l5_toast("Not enough memory");
    }
}

static int slot_of(const char *label)
{
    for (int i = 0; i < g_slots.n; i++) {
        if (strcmp(g_slots.slot[i].part->label, label) == 0) return i;
    }
    return -1;
}

static void format_cb(bool ok, void *user)
{
    const char *label = user;
    if (!ok) return;
    char title[40];
    snprintf(title, sizeof(title), "Formatting %s", label);
    int slot = slot_of(label);
    op_start(slot >= 0 ? OP_FORMAT_APP : OP_FORMAT_DATA, label, slot, NULL, NULL, title);
}

void l5_pman_format(const char *label)
{
    static char lab[17];
    strlcpy(lab, label, sizeof(lab));
    int slot = slot_of(lab);
    if (slot < 0 && !l5_part_editable(lab)) {
        l5_info("PMAN", "Not available", "This partition is kept by the launcher.");
        return;
    }
    char title[40], body[200];
    snprintf(title, sizeof(title), "Format %s?", lab);
    if (slot >= 0 && slot < g_slots.n && g_slots.slot[slot].used) {
        snprintf(body, sizeof(body), "%.60s in %s is erased: install it again from SD or OTA afterwards.",
                 g_slots.slot[slot].name, lab);
    } else if (slot >= 0) {
        snprintf(body, sizeof(body), "%s is erased.", lab);
    } else {
        snprintf(body, sizeof(body), "All data in %s is erased. The app that uses it formats it again on its next start.",
                 lab);
    }
    l5_confirm("PMAN", title, body, "FORMAT", true, format_cb, lab);
}

void l5_pman_backup(const char *label)
{
    if (!l5_part_editable(label)) {
        l5_info("PMAN", "Not available", "Only data partitions (SPIFFS, FAT) can be backed up.");
        return;
    }
    if (!l5_board_sd_ready()) {
        l5_info("PMAN", "No SD card", "Backups are written to /backups on the SD card.");
        return;
    }
    char title[40];
    snprintf(title, sizeof(title), "Backing up %s", label);
    op_start(OP_BACKUP, label, -1, NULL, NULL, title);
}

static char s_restore_label[17];
static char s_restore_files[MAX_BINS][64];

static void restore_confirm_cb(bool ok, void *user)
{
    if (!ok) return;
    int i = (int)(intptr_t)user;
    char path[200], title[40];
    snprintf(path, sizeof(path), "%s/%s", L5_BACKUP_DIR, s_restore_files[i]);
    snprintf(title, sizeof(title), "Restoring %s", s_restore_label);
    op_start(OP_RESTORE, s_restore_label, -1, path, s_restore_files[i], title);
}

static void restore_pick_cb(const char *id, void *user)
{
    (void)user;
    if (!id) return;
    int i = atoi(id);
    char title[48], body[200];
    snprintf(title, sizeof(title), "Restore %s?", s_restore_label);
    snprintf(body, sizeof(body), "Everything in %s is replaced with %s.", s_restore_label, s_restore_files[i]);
    l5_confirm("PMAN", title, body, "RESTORE", true, restore_confirm_cb, (void *)(intptr_t)i);
}

static int name_desc(const void *a, const void *b)
{
    return strcmp((const char *)b, (const char *)a);
}

void l5_pman_restore(const char *label)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!p || !l5_part_editable(label)) {
        l5_info("PMAN", "Not available", "Only data partitions (SPIFFS, FAT) can be restored.");
        return;
    }
    if (!l5_board_sd_ready()) {
        l5_info("PMAN", "No SD card", "Backups are read from /backups on the SD card.");
        return;
    }
    strlcpy(s_restore_label, label, sizeof(s_restore_label));
    char prefix[24];
    snprintf(prefix, sizeof(prefix), "%s_", label);
    int n = 0;
    DIR *d = opendir(L5_BACKUP_DIR);
    for (struct dirent *e; d && (e = readdir(d)) && n < MAX_BINS;) {
        size_t len = strlen(e->d_name);
        if (strncmp(e->d_name, prefix, strlen(prefix)) != 0 || len < 5 || len >= sizeof(s_restore_files[0]) ||
            strcasecmp(e->d_name + len - 4, ".bin") != 0) {
            continue;
        }
        char path[280];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", L5_BACKUP_DIR, e->d_name);
        if (stat(path, &st) == 0 && st.st_size > 0 && (uint32_t)st.st_size <= p->size) {
            strlcpy(s_restore_files[n++], e->d_name, sizeof(s_restore_files[0]));
        }
    }
    if (d) closedir(d);
    if (!n) {
        char body[160];
        snprintf(body, sizeof(body), "Back up %s first. Backups are stored in /backups on the SD card.", label);
        l5_info("PMAN", "No backup found", body);
        return;
    }
    qsort(s_restore_files, n, sizeof(s_restore_files[0]), name_desc); /* newest date first */
    l5_menu_item_t items[MAX_BINS];
    char ids[MAX_BINS][4];
    for (int i = 0; i < n; i++) {
        snprintf(ids[i], sizeof(ids[i]), "%d", i);
        items[i] = (l5_menu_item_t){ids[i], L5_ICON_INSTALL, s_restore_files[i], false};
    }
    char title[40];
    snprintf(title, sizeof(title), "Restore %s", label);
    l5_menu("PMAN", title, items, n, restore_pick_cb, NULL);
}

/* ---------- .bin files on the SD card (AUTO PARTITION, SANITY CHECK) ---------- */

typedef struct {
    char path[200];
    char name[64];
    uint32_t size;
} bin_t;
static bin_t s_bins[MAX_BINS];
static int s_nbins;
static bool s_auto; /* which action the picker is for */

static void scan_bins(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    for (struct dirent *e; d && (e = readdir(d)) && s_nbins < MAX_BINS;) {
        if (e->d_name[0] == '.' || strcasecmp(e->d_name, "System Volume Information") == 0 ||
            strcasecmp(e->d_name, "backups") == 0) {
            continue;
        }
        char path[200];
        if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path)) continue;
        size_t len = strlen(e->d_name);
        if (e->d_type == DT_DIR) {
            if (depth < 2) scan_bins(path, depth + 1);
        } else if (len > 4 && strcasecmp(e->d_name + len - 4, ".bin") == 0) {
            struct stat st;
            if (stat(path, &st) == 0) {
                bin_t *b = &s_bins[s_nbins++];
                strlcpy(b->path, path, sizeof(b->path));
                strlcpy(b->name, e->d_name, sizeof(b->name));
                b->size = (uint32_t)st.st_size;
            }
        }
    }
    if (d) closedir(d);
}

/* a merged image: the layout its own partition table asks for (l5_fw_plan), as INSTALL offers it */
static void auto_partition_merged(const bin_t *b)
{
    l5_fw_t *fw = heap_caps_calloc(1, sizeof(l5_fw_t), MALLOC_CAP_SPIRAM);
    uint8_t *head = heap_caps_malloc(L5_FW_PROBE_LEN, MALLOC_CAP_SPIRAM);
    char why[400] = "", title[96];
    size_t len = 0;
    uint32_t size = 0, app = 0;
    bool net;
    l5_layout_t l;
    esp_err_t err = fw && head ? l5_inst_probe(b->path, NULL, head, &len, &size, why, sizeof(why), &net) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        l5_fw_init(fw, L5_FW_APP_ONLY);
        err = l5_fw_from_probe(fw, head, len, size, why, sizeof(why));
    }
    if (err == ESP_OK && fw->format != L5_FW_MERGED) {
        snprintf(why, sizeof(why), "The file is not a merged image.");
        err = ESP_ERR_NOT_SUPPORTED;
    }
    if (err == ESP_OK && l5_inst_app_len_file(b->path, fw->app_offset, l5_fw_app_bound(fw), &app) == ESP_OK) fw->app_size = app;
    if (err == ESP_OK) err = l5_fw_plan(fw, &l, why, sizeof(why));
    heap_caps_free(fw);
    heap_caps_free(head);
    if (err != ESP_OK) {
        l5_info("PMAN", "Cannot fit partitions", why[0] ? why : "Not enough memory.");
        return;
    }
    snprintf(title, sizeof(title), "Fit partitions to %.60s?", b->name);
    change_layout(&l, title, "custom", why[0] ? why : NULL);
}

static void auto_partition(const bin_t *b)
{
    l5_bin_info_t info;
    if (l5_bin_probe(b->path, &info) == ESP_OK && info.kind == L5_BIN_NO_APP) {
        auto_partition_merged(b);
        return;
    }
    if (info.kind != L5_BIN_APP) {
        l5_info("PMAN", "Not an ESP32-P4 image", "AUTO PARTITION needs an ESP32-P4 app image or a merged image.");
        return;
    }
    /* views2.js: slot sized for this firmware, keep the current slot count if FAT (448 KB) still fits
       next to it before aurora_cfg; SPIFFS takes the gap after aurora_cfg */
    uint32_t slot = ((b->size / 1024 + 256) + 63) / 64 * 64;
    if (slot < L5_MIN_APP_KB) slot = L5_MIN_APP_KB;
    const l5_layout_t cur = base();
    uint32_t cn = s_map.managed && cur.n ? cur.n : 1;
    uint32_t n = slot * cn <= L5_FRONT_KB - L5_MIN_DATA_KB ? cn : 1;
    if (slot * n > L5_FRONT_KB) {
        l5_info("PMAN", "Too large", "This binary does not fit the 13 MB before aurora_cfg (0xF10000).");
        return;
    }
    uint32_t fat = L5_FRONT_KB - slot * n;
    l5_layout_t l = {.n = n, .slot_kb = slot, .spiffs_kb = L5_GAP_KB, .fat_kb = fat >= L5_MIN_DATA_KB ? fat : 0};
    char title[96];
    snprintf(title, sizeof(title), "Fit partitions to %.60s?", b->name);
    change_layout(&l, title, "custom", NULL);
}

static void bin_cb(const char *id, void *user)
{
    (void)user;
    if (!id) return;
    const bin_t *b = &s_bins[atoi(id)];
    if (s_auto) {
        auto_partition(b);
    } else {
        op_start(OP_SANITY, NULL, -1, b->path, b->name, "Sanity check");
    }
}

static void pick_bin(bool for_auto)
{
    if (!l5_board_sd_ready()) {
        l5_info("PMAN", "No SD card", "Put the firmware .bin on the SD card first.");
        return;
    }
    s_auto = for_auto;
    s_nbins = 0;
    scan_bins(CONFIG_BSP_SD_MOUNT_POINT, 0);
    if (!s_nbins) {
        l5_info("PMAN", "No .bin files", "No firmware .bin files were found on the SD card.");
        return;
    }
    l5_menu_item_t items[MAX_BINS];
    char ids[MAX_BINS][4], labels[MAX_BINS][96];
    for (int i = 0; i < s_nbins; i++) {
        char size[16];
        l5_fmt_bytes(size, sizeof(size), s_bins[i].size);
        snprintf(ids[i], sizeof(ids[i]), "%d", i);
        snprintf(labels[i], sizeof(labels[i]), "%.60s \xC2\xB7 %s", s_bins[i].name, size);
        items[i] = (l5_menu_item_t){ids[i], L5_ICON_BIN, labels[i], false};
    }
    l5_menu("PMAN", for_auto ? "Fit partitions to firmware" : "Check firmware image", items, s_nbins, bin_cb, NULL);
}

static void optimize(void)
{
    uint32_t biggest = 0;
    for (int i = 0; i < g_slots.n; i++) {
        if (g_slots.slot[i].used && g_slots.slot[i].size > biggest) biggest = g_slots.slot[i].size;
    }
    if (!biggest) {
        l5_info("PMAN", "Nothing to optimize", "Install an app first. Optimize shrinks the app slots to the largest "
                "installed app and gives the rest to FAT.");
        return;
    }
    if (!s_map.managed) {
        l5_info("PMAN", "Cannot optimize", s_map.why);
        return;
    }
    l5_layout_t l = base();
    uint32_t slot = (uint32_t)(((uint64_t)biggest * 11 / 10 / 1024 + 63) / 64 * 64);
    if (slot < L5_MIN_APP_KB) slot = L5_MIN_APP_KB;
    if (slot >= l.slot_kb) {
        l5_info("PMAN", "Already tight", "The app slots are already close to the largest installed app.");
        return;
    }
    l.slot_kb = slot;
    uint32_t fat = L5_FRONT_KB - l.n * slot - l5_layout_extras_kb(&l); /* the rest up to aurora_cfg goes to FAT */
    l.fat_kb = fat >= L5_MIN_DATA_KB ? fat : 0;
    change_layout(&l, "Optimize flash?", "custom",
                  "Apps cannot be moved: install them again from SD afterwards (the .bin files on the card stay).");
}

/* ---------- resize / delete / create ---------- */

static char s_edit_label[17];
static bool s_edit_app;

static void resize_cb(const char *id, void *user)
{
    (void)user;
    if (!id) return;
    uint32_t kb = (uint32_t)atol(id);
    l5_layout_t l = base();
    char title[64], k[16];
    l5_fmt_kb(k, sizeof(k), kb);
    if (s_edit_app) {
        l.slot_kb = kb;
        snprintf(title, sizeof(title), "Resize the app slots to %sB?", k);
    } else {
        l.fat_kb = kb;
        snprintf(title, sizeof(title), "Resize %s to %sB?", s_edit_label, k);
    }
    change_layout(&l, title, "custom", NULL);
}

/* app slots (together) and the main FAT; the partition after aurora_cfg and a firmware's own
   partitions (AUTO PARTITION) keep their size */
static void resize(const row_t *r)
{
    if (!s_map.managed) {
        l5_info("PMAN", "Cannot resize", s_map.why);
        return;
    }
    if (r->p->type == ESP_PARTITION_TYPE_DATA && gap_part(r->p)) {
        l5_info("PMAN", "Fixed size", "The partition after aurora_cfg uses the gap up to the coredump "
                "(0xF16000..0xFF0000, 872 KB at most). DELETE frees it.");
        return;
    }
    const l5_layout_t now = base();
    if (r->p->type == ESP_PARTITION_TYPE_DATA && extra_index(&now, r->p->label) >= 0) {
        l5_info("PMAN", "Fixed size", "AUTO PARTITION made this partition with the size the firmware's own table "
                "asks for, so its image fits. DELETE frees it.");
        return;
    }
    s_edit_app = app_slot(r->p);
    strlcpy(s_edit_label, r->p->label, sizeof(s_edit_label));
    const l5_layout_t base_l = base(), *L = &base_l;
    uint32_t n = s_edit_app ? L->n : 1;
    uint32_t cur = s_edit_app ? L->slot_kb : L->fat_kb;
    uint32_t max = (cur * n + free_kb()) / n / 64 * 64;
    uint32_t min = s_edit_app ? L5_MIN_APP_KB : L5_MIN_DATA_KB;
    uint32_t opts[16];
    int k = size_options(min, max, opts, 14);
    l5_menu_item_t items[16];
    char ids[16][12], labels[16][64];
    for (int i = 0; i < k; i++) {
        char kb[16];
        l5_fmt_kb(kb, sizeof(kb), opts[i]);
        snprintf(ids[i], sizeof(ids[i]), "%lu", (unsigned long)opts[i]);
        snprintf(labels[i], sizeof(labels[i]), "%sB%s%s%s", kb, n > 1 ? " each" : "", opts[i] == cur ? " \xC2\xB7 current" : "",
                 opts[i] == max ? " \xC2\xB7 max" : "");
        items[i] = (l5_menu_item_t){ids[i], L5_ICON_PMAN, labels[i], false};
    }
    char title[64], kb[16];
    l5_fmt_kb(kb, sizeof(kb), cur);
    if (s_edit_app) snprintf(title, sizeof(title), "Resize all %lu app slots (now %sB)", (unsigned long)n, kb);
    else snprintf(title, sizeof(title), "Resize %s (now %sB)", s_edit_label, kb);
    l5_menu("PMAN", title, items, k, resize_cb, NULL);
}

static void delete_data(const row_t *r)
{
    if (!s_map.managed) {
        l5_info("PMAN", "Cannot delete", s_map.why);
        return;
    }
    l5_layout_t l = base();
    int x = extra_index(&l, r->p->label);
    if (x >= 0) { /* a firmware's own partition (AUTO PARTITION) */
        memmove(&l.extra[x], &l.extra[x + 1], (l.nextra - x - 1) * sizeof(l.extra[0]));
        l.nextra--;
    } else if (gap_part(r->p)) {
        l.spiffs_kb = 0;
    } else {
        l.fat_kb = 0;
    }
    char title[48];
    snprintf(title, sizeof(title), "Delete %s?", r->p->label);
    change_layout(&l, title, "custom", "Its space becomes free flash.");
}

/* "(free)" before aurora_cfg takes FAT, "(spiffs gap)" after it takes SPIFFS */
static void create_data(const row_t *r)
{
    if (!s_map.managed) {
        l5_info("PMAN", "Cannot create", s_map.why);
        return;
    }
    l5_layout_t l = base();
    if (r->offset >= L5_GAP_ADDR) {
        l.spiffs_kb = L5_GAP_KB;
        change_layout(&l, "Create SPIFFS (872 KB)?", "custom", NULL);
        return;
    }
    if (free_kb() < L5_MIN_DATA_KB || l.fat_kb) {
        l5_info("PMAN", "Not available", "FAT needs at least 448 KB (0x70000) of free flash before aurora_cfg, and "
                "there is only one FAT partition.");
        return;
    }
    l.fat_kb = free_kb();
    change_layout(&l, "Create FAT (vfs)?", "custom", NULL);
}

/* ---------- UI ---------- */

static void act_cb(lv_event_t *e)
{
    const char *a = lv_event_get_user_data(e);
    const row_t *r = find_row(s_sel);
    if (strcmp(a, "scheme") == 0) l5_pman_pick_scheme();
    else if (strcmp(a, "auto") == 0) pick_bin(true);
    else if (strcmp(a, "sanity") == 0) pick_bin(false);
    else if (strcmp(a, "optimize") == 0) optimize();
    else if (!r) return;
    else if (strcmp(a, "create") == 0) create_data(r);
    else if (!r->p) return;
    else if (strcmp(a, "format") == 0) l5_pman_format(r->p->label);
    else if (strcmp(a, "backup") == 0) l5_pman_backup(r->p->label);
    else if (strcmp(a, "restore") == 0) l5_pman_restore(r->p->label);
    else if (strcmp(a, "resize") == 0) resize(r);
    else if (strcmp(a, "delete") == 0) delete_data(r);
}

static void side_pane(lv_obj_t *s, const row_t *r);

static void select_cb(lv_event_t *e)
{
    const row_t *r = lv_event_get_user_data(e);
    strlcpy(s_sel, row_label(r), sizeof(s_sel));
    if (!V.side) { /* not built as expected: full rebuild */
        l5_ui_render_views_async(L5_VIEW_BIT(L5_VIEW_PMAN));
        return;
    }
    paint_sel(r);
    lv_obj_clean(V.side); /* the finger is on the map or the table, not here */
    side_pane(V.side, r);
}

static lv_obj_t *chip(lv_obj_t *parent, const char *icon, const char *text, const char *action)
{
    lv_obj_t *b = lv_button_create(parent); /* .chip */
    lv_obj_remove_style_all(b);
    lv_obj_set_height(b, 46);
    lv_obj_set_style_pad_hor(b, 14, 0);
    lv_obj_set_style_pad_column(b, 8, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x474a51), 0);
    lv_obj_set_style_radius(b, 3, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1a1b1f), 0);
    lv_obj_set_style_bg_color(b, L5_PANEL_3, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    l5_icon(b, &l5_icon_20, L5_ON_PANEL, icon);
    lv_obj_t *l = l5_label(b, &l5_mono_14, L5_ON_PANEL, text);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_obj_add_event_cb(b, act_cb, LV_EVENT_CLICKED, (void *)action);
    return b;
}

static lv_obj_t *pane(lv_obj_t *view, bool dark, int32_t x, int32_t w)
{
    lv_obj_t *p = l5_box(view);
    l5_panel(p, dark);
    lv_obj_set_pos(p, x, L5_PANEL_Y);
    lv_obj_set_size(p, w, L5_PANEL_H);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    l5_chamfer(p, L5_CHAMFER, L5_PLATE);
    return p;
}

static lv_obj_t *cell(lv_obj_t *row, int32_t w, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = l5_label(row, font, color, text);
    if (w > 0) lv_obj_set_width(l, w);
    else lv_obj_set_flex_grow(l, 1);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    return l;
}

static void main_pane(lv_obj_t *m, const row_t *sel)
{
    /* .bar */
    lv_obj_t *bar = l5_box(m);
    lv_obj_set_size(bar, lv_pct(100), 66);
    lv_obj_set_style_pad_left(bar, 18, 0);
    lv_obj_set_style_pad_right(bar, 12, 0);
    lv_obj_set_style_pad_column(bar, 10, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, L5_PANEL_LINE, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    l5_dot(bar);
    char t[64];
    snprintf(t, sizeof(t), "PARTITIONS \xC2\xB7 %lu MB FLASH", (unsigned long)(s_map.flash_size >> 20));
    lv_obj_t *lb = l5_label(bar, &l5_mono_14, L5_ON_PANEL, t);
    lv_obj_set_style_text_letter_space(lb, 2, 0);
    lv_obj_set_flex_grow(lb, 1);
    char scheme[24];
    const char *name = g_slots.scheme;
    size_t i = 0;
    for (; name[i] && i + 1 < sizeof(scheme); i++) scheme[i] = (char)toupper((unsigned char)name[i]);
    scheme[i] = 0;
    chip(bar, L5_ICON_PMAN, s_map.managed ? scheme : "UNMANAGED", "scheme");

    /* .fmap: one segment per row, width ~ max(kb, 90) */
    lv_obj_t *wrap = l5_box(m);
    lv_obj_set_size(wrap, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(wrap, 18, 0);
    lv_obj_set_style_pad_hor(wrap, 18, 0);
    lv_obj_set_style_pad_bottom(wrap, 6, 0);
    lv_obj_t *map = l5_box(wrap);
    const int32_t W = 816 - 36 - 2;
    lv_obj_set_size(map, W, 54);
    lv_obj_set_style_border_width(map, 1, 0);
    lv_obj_set_style_border_color(map, L5_BLACK, 0);
    lv_obj_set_style_radius(map, 3, 0);
    lv_obj_set_style_clip_corner(map, true, 0);
    lv_obj_set_flex_flow(map, LV_FLEX_FLOW_ROW);
    uint64_t sum = 0;
    for (int r = 0; r < s_nrows; r++) {
        uint32_t kb = s_rows[r].size / 1024;
        sum += kb > 90 ? kb : 90;
    }
    int32_t used = 0;
    for (int r = 0; r < s_nrows; r++) {
        const row_t *row = &s_rows[r];
        uint32_t kb = row->size / 1024;
        int32_t w = r == s_nrows - 1 ? W - 2 - used : (int32_t)((uint64_t)(kb > 90 ? kb : 90) * (W - 2) / (sum ? sum : 1));
        used += w;
        lv_obj_t *seg = lv_button_create(map);
        lv_obj_remove_style_all(seg);
        lv_obj_set_size(seg, w < 4 ? 4 : w, lv_pct(100));
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(seg, row->color, 0);
        lv_obj_set_style_border_side(seg, LV_BORDER_SIDE_RIGHT, 0);
        lv_obj_set_style_border_width(seg, 1, 0);
        lv_obj_set_style_border_color(seg, L5_BLACK, 0);
        lv_obj_set_style_border_side(seg, LV_BORDER_SIDE_FULL, LV_STATE_CHECKED); /* selected (paint_sel) */
        lv_obj_set_style_border_width(seg, 3, LV_STATE_CHECKED);
        lv_obj_set_style_border_color(seg, L5_WHITE, LV_STATE_CHECKED);
        lv_obj_add_style(seg, &L5S.press_ring, LV_STATE_PRESSED);
        lv_obj_set_state(seg, LV_STATE_CHECKED, row == sel);
        V.seg[r] = seg;
        if (kb > 900) {
            lv_obj_t *l = l5_label(seg, &l5_mono_12, L5_WHITE, row_label(row));
            lv_obj_center(l);
        }
        lv_obj_add_event_cb(seg, select_cb, LV_EVENT_CLICKED, (void *)row);
    }
    lv_obj_t *scale = l5_box(m); /* .fscale */
    lv_obj_set_size(scale, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(scale, 18, 0);
    lv_obj_set_style_pad_bottom(scale, 10, 0);
    lv_obj_set_flex_flow(scale, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(scale, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const char *const marks[] = {"0x009000", "4 MB", "8 MB", "12 MB", "0x1000000"};
    for (int k = 0; k < 5; k++) l5_label(scale, &l5_mono_12, L5_ON_PANEL_MUTED, marks[k]);

    /* .ptab */
    lv_obj_t *head = l5_box(m);
    lv_obj_set_size(head, lv_pct(100), 34);
    lv_obj_set_style_pad_hor(head, 12, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(head, L5_PANEL_2, 0);
    lv_obj_set_style_border_side(head, LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(head, 1, 0);
    lv_obj_set_style_border_color(head, L5_PANEL_LINE, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const char *const cols[] = {"LABEL", "TYPE", "SUBTYPE", "OFFSET", "SIZE", "USED"};
    static const int32_t widths[] = {170, 70, 110, 120, 90, 0};
    for (int k = 0; k < 6; k++) {
        lv_obj_t *c = cell(head, widths[k], &l5_mono_12, L5_ON_PANEL_MUTED, cols[k]);
        lv_obj_set_style_text_letter_space(c, 2, 0);
    }
    lv_obj_t *list = l5_box(m);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE); /* clickable: a drag between rows scrolls too */
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    for (int r = 0; r < s_nrows; r++) {
        const row_t *row = &s_rows[r];
        lv_obj_t *tr = l5_box(list);
        lv_obj_set_size(tr, lv_pct(100), 58);
        lv_obj_set_style_pad_hor(tr, 12, 0);
        lv_obj_set_style_border_side(tr, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(tr, 1, 0);
        lv_obj_set_style_border_color(tr, L5_PANEL_LINE, 0);
        lv_obj_set_flex_flow(tr, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(tr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(tr, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tr, select_cb, LV_EVENT_CLICKED, (void *)row);
        lv_obj_add_style(tr, &L5S.press_tint, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(tr, 56, LV_STATE_CHECKED); /* tr.sel (paint_sel) */
        lv_obj_set_style_bg_color(tr, L5P.accent, LV_STATE_CHECKED);
        lv_obj_set_state(tr, LV_STATE_CHECKED, row == sel);
        lv_obj_t *mark = l5_box(tr);
        lv_obj_add_flag(mark, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(mark, 4, lv_pct(100));
        lv_obj_set_pos(mark, -12, 0);
        lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(mark, L5P.accent, 0);
        lv_obj_set_flag(mark, LV_OBJ_FLAG_HIDDEN, row != sel);
        V.tr[r] = tr;
        V.mark[r] = mark;
        lv_obj_t *lab = l5_box(tr);
        lv_obj_set_size(lab, 170, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_column(lab, 10, 0);
        lv_obj_set_flex_flow(lab, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(lab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *sw = l5_box(lab);
        lv_obj_set_size(sw, 12, 12);
        lv_obj_set_style_radius(sw, 2, 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(sw, row->color, 0);
        cell(lab, 140, &l5_mono_16, L5_ON_PANEL, row_label(row));
        char sub[16], off[16], size[16], use[24];
        cell(tr, 70, &l5_mono_16, L5_ON_PANEL, type_str(row->p));
        cell(tr, 110, &l5_mono_16, L5_ON_PANEL, sub_str(row->p, sub, sizeof(sub)));
        snprintf(off, sizeof(off), "0x%06lX", (unsigned long)row->offset);
        cell(tr, 120, &l5_mono_16, L5_ON_PANEL, off);
        l5_fmt_kb(size, sizeof(size), row->size / 1024);
        cell(tr, 90, &l5_mono_16, L5_ON_PANEL, size);
        int pct;
        used_text(row, use, sizeof(use), &pct);
        lv_obj_t *uc = l5_box(tr);
        lv_obj_set_height(uc, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(uc, 1);
        lv_obj_set_style_pad_column(uc, 8, 0);
        lv_obj_set_flex_flow(uc, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(uc, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        if (pct >= 0) { /* .ubar */
            lv_obj_t *ub = lv_bar_create(uc);
            lv_obj_remove_flag(ub, LV_OBJ_FLAG_CLICKABLE); /* a tap on the bar selects the row */
            lv_obj_remove_style_all(ub);
            lv_obj_set_size(ub, 90, 6);
            lv_obj_set_style_bg_opa(ub, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(ub, lv_color_hex(0x33363c), LV_PART_MAIN);
            lv_obj_add_style(ub, &L5S.meter_ind, LV_PART_INDICATOR);
            lv_bar_set_range(ub, 0, 100);
            lv_bar_set_value(ub, pct > 100 ? 100 : pct, LV_ANIM_OFF);
        }
        l5_label(uc, &l5_mono_14, L5_ON_PANEL, use);
    }

    lv_obj_t *foot = l5_box(m); /* .foot height 66 */
    lv_obj_set_size(foot, lv_pct(100), 66);
    lv_obj_set_style_pad_hor(foot, 18, 0);
    lv_obj_set_style_pad_column(foot, 10, 0);
    lv_obj_set_style_border_side(foot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(foot, 1, 0);
    lv_obj_set_style_border_color(foot, L5_PANEL_LINE, 0);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    chip(foot, L5_ICON_CFG, "AUTO PARTITION", "auto");
    chip(foot, L5_ICON_CHECK, "SANITY CHECK", "sanity");
    chip(foot, L5_ICON_PMAN, "OPTIMIZE FLASH", "optimize");
}

static lv_obj_t *side_act(lv_obj_t *grid, const char *icon, const char *text, const char *action, bool enabled, bool wide)
{
    lv_obj_t *b = l5_act_button(grid, icon, text, wide && !strcmp(action, "create"));
    lv_obj_set_width(b, wide ? lv_pct(100) : 182);
    lv_obj_add_event_cb(b, act_cb, LV_EVENT_CLICKED, (void *)action);
    if (!enabled) lv_obj_add_state(b, LV_STATE_DISABLED);
    return b;
}

static void kv_row(lv_obj_t *parent, const char *k, const char *v)
{
    lv_obj_t *r = l5_box(parent);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(r, 12, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_t *kl = l5_label(r, &l5_mono_15, L5_MUTED, k);
    lv_obj_set_width(kl, 110);
    lv_obj_set_style_text_letter_space(kl, 1, 0);
    lv_obj_t *vl = l5_label(r, &l5_mono_15, L5_INK, v);
    lv_obj_set_flex_grow(vl, 1);
    lv_label_set_long_mode(vl, LV_LABEL_LONG_WRAP);
}

static void side_pane(lv_obj_t *s, const row_t *r)
{
    const l5_part_t *p = r ? r->p : NULL;
    bool free_row = r && !p, fixed = r && r->fixed, app = app_slot(p), data = pool_data(p);

    lv_obj_t *h = l5_box(s); /* .side-head */
    lv_obj_set_size(h, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(h, 22, 0);
    lv_obj_set_style_pad_hor(h, 22, 0);
    lv_obj_set_style_pad_column(h, 16, 0);
    lv_obj_set_flex_flow(h, LV_FLEX_FLOW_ROW);
    lv_obj_t *ic = l5_box(h); /* .bigic */
    lv_obj_set_size(ic, 132, 132);
    lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ic, L5_PANEL_2, 0);
    lv_obj_set_style_border_width(ic, 1, 0);
    lv_obj_set_style_border_color(ic, L5_PANEL_LINE, 0);
    lv_obj_center(l5_icon(ic, &l5_icon_64, app ? L5P.accent : L5_ON_PANEL_MUTED, fixed ? L5_ICON_LOCK : L5_ICON_PMAN));
    lv_obj_t *tt = l5_box(h);
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
    lv_obj_t *kl = l5_label(k, &l5_mono_13, L5_MUTED, free_row ? "UNALLOCATED" : fixed ? "SYSTEM \xC2\xB7 LOCKED" : "PARTITION");
    lv_obj_set_style_text_letter_space(kl, 2, 0);
    lv_obj_t *b = l5_label(tt, &l5_chakra_b26, L5_INK, r ? row_label(r) : "-");
    lv_obj_set_width(b, lv_pct(100));
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    char sub[16], t[96];
    snprintf(t, sizeof(t), "%s / %s", type_str(p), sub_str(p, sub, sizeof(sub)));
    l5_label(tt, &l5_chakra_sb17, L5_MUTED, t);
    if (!r) return;

    lv_obj_t *kv = l5_box(s); /* .kv */
    lv_obj_set_size(kv, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(kv, 16, 0);
    lv_obj_set_style_pad_hor(kv, 22, 0);
    lv_obj_set_style_pad_row(kv, 6, 0);
    lv_obj_set_flex_flow(kv, LV_FLEX_FLOW_COLUMN);
    char v[96], size[16];
    snprintf(v, sizeof(v), "0x%06lX", (unsigned long)r->offset);
    kv_row(kv, "OFFSET", v);
    l5_fmt_kb(size, sizeof(size), r->size / 1024);
    snprintf(v, sizeof(v), "%sB \xC2\xB7 0x%lX", size, (unsigned long)r->size);
    kv_row(kv, "SIZE", v);
    if (p) {
        int pct;
        char use[24];
        used_text(r, use, sizeof(use), &pct);
        int slot = app ? p->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN : -1;
        if (slot >= 0 && slot < g_slots.n && g_slots.slot[slot].used) {
            snprintf(v, sizeof(v), "%s \xC2\xB7 %s", use, g_slots.slot[slot].name);
        } else {
            snprintf(v, sizeof(v), "%s", use);
        }
        kv_row(kv, "USED", v);
    }
    const bool gap = free_row && r->offset >= L5_GAP_ADDR;
    const char *desc = aurora_part(p) ? "Aurora Player keeps its PIN here (NVS at 0xF10000, 24 KB). Every scheme keeps "
                                        "it in place; the launcher never formats, moves or resizes it."
                       : fixed ? "Launcher keeps this partition fixed so it can always boot back into itself."
                       : gap ? "Free flash after aurora_cfg. Create SPIFFS here (872 KB)."
                       : free_row ? "Free flash before aurora_cfg. Create FAT (vfs) here."
                       : app ? "App slot. Pick which slot boots on HOME. Resizing changes every slot and erases all "
                               "installed apps."
                             : "Data partition used by apps. Back it up to SD before formatting.";
    lv_obj_t *dl = l5_label(s, &l5_chakra_r17, L5_INK_SOFT, s_map.managed || fixed ? desc : s_map.why);
    lv_obj_set_width(dl, lv_pct(100));
    lv_obj_set_style_pad_top(dl, 14, 0);
    lv_obj_set_style_pad_hor(dl, 22, 0);
    lv_label_set_long_mode(dl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(dl, 4, 0);

    lv_obj_t *grow = l5_box(s);
    lv_obj_set_flex_grow(grow, 1);
    lv_obj_t *acts = l5_box(s); /* .acts: 2 columns */
    lv_obj_set_size(acts, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(acts, 22, 0);
    lv_obj_set_style_pad_bottom(acts, 18, 0);
    lv_obj_set_style_pad_row(acts, 12, 0);
    lv_obj_set_style_pad_column(acts, 10, 0);
    lv_obj_set_flex_flow(acts, LV_FLEX_FLOW_ROW_WRAP);
    bool managed = s_map.managed;
    if (free_row) {
        const l5_layout_t l = base();
        bool can = managed && (gap ? !l.spiffs_kb : !l.fat_kb && free_kb() >= L5_MIN_DATA_KB);
        side_act(acts, L5_ICON_PLUS, "CREATE PARTITION", "create", can, true);
    } else {
        const l5_layout_t now = base();
        bool fat = data && p->subtype == ESP_PARTITION_SUBTYPE_DATA_FAT && !gap_part(p) && extra_index(&now, p->label) < 0;
        side_act(acts, L5_ICON_TRASH, "FORMAT", "format", !fixed && (app || l5_part_editable(p->label)), false);
        side_act(acts, L5_ICON_PMAN, "RESIZE", "resize", managed && (app || fat), false);
        side_act(acts, L5_ICON_INSTALL, "BACKUP", "backup", data, false);
        side_act(acts, L5_ICON_RESTART, "RESTORE", "restore", data, false);
        side_act(acts, L5_ICON_TRASH, "DELETE", "delete", managed && data, true);
    }
}

void l5_pman_render(lv_obj_t *view)
{
    lv_obj_clean(view);
    memset(&V, 0, sizeof(V));
    if (l5_pt_read_map(&s_map) != ESP_OK) {
        lv_obj_t *m = pane(view, true, 12, 1256);
        l5_col_header(m, "PARTITIONS", "UNREADABLE");
        lv_obj_t *l = l5_label(m, &l5_chakra_r17, L5_ON_PANEL_MUTED, s_map.why);
        lv_obj_set_style_pad_all(l, 30, 0);
        return;
    }
    build_rows();
    const row_t *sel = find_row(s_sel);
    if (!sel && s_nrows) { /* default: the first app slot, else the first row */
        for (int i = 0; i < s_nrows && !sel; i++) {
            if (app_slot(s_rows[i].p)) sel = &s_rows[i];
        }
        if (!sel) sel = &s_rows[0];
        strlcpy(s_sel, row_label(sel), sizeof(s_sel));
    }
    main_pane(pane(view, true, 12, 816), sel);
    V.side = pane(view, false, 840, 428);
    lv_obj_add_event_cb(V.side, view_deleted_cb, LV_EVENT_DELETE, NULL);
    side_pane(V.side, sel);
}
