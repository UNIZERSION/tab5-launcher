/* SD: file browser and .bin installer. Port of L5.views.sd (views.js); INSTALL goes through the flow
   shared with OTA (l5_inst_ui.c: L5.install / L5.fits / L5.tooBig), for app images and merged images
   (bootloader + partition table + app + data images). COPY / PASTE, RENAME, NEW FOLDER, DELETE and VIEW
   are in l5_sd_ops.c. Not ported yet (README.md): USB mass storage. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "l5_board.h"
#include "l5_config.h"
#include "l5_files.h"
#include "l5_inst.h"
#include "l5_sd.h"
#include "l5_slots.h"
#include "l5_ui.h"

static const char *TAG = "l5_sd";

#define MOUNT CONFIG_BSP_SD_MOUNT_POINT
#define PATH_LEN L5_FILES_PATH
#define MAX_ENTRIES 200 /* rows built for one folder; the foot says when more are there */
#define COUNT_BUDGET_US 300000 /* folder item counts per listing; the rest are counted when selected */
#define ACTS_W 384 /* .acts content width: side pane 428 - 2 x 22 */

typedef struct {
    char *name;
    bool dir, bin;
    uint64_t size;  /* files */
    int items;      /* folders: entries inside, -1 = not counted or unreadable */
    time_t mtime;
} entry_t;

/* R.sd: cwd is relative to the card root ("/" or "/a/b"), sel is a name inside cwd */
static struct {
    char cwd[PATH_LEN];
    char sel[256];
    entry_t *e;
    int n;          /* rows */
    int listed;     /* entries passing the filter (n is capped at MAX_ENTRIES) */
    int all;        /* entries in the folder */
    int ndirs;
    bool dir_ok;
    uint64_t total, free;
    lv_obj_t *list, *side;
    lv_obj_t *paste_chip, *clip_label; /* follow the clipboard in place */
} R = {.cwd = "/"};

/* the selected .bin, installed by INSTALL */
static struct {
    char path[PATH_LEN];
    char name[128];
} s_pending;

/* ---------- paths and folder listing ---------- */

static bool is_root(void)
{
    return strcmp(R.cwd, "/") == 0;
}

static bool is_bin(const char *n)
{
    size_t l = strlen(n);
    return l >= 4 && strcasecmp(n + l - 4, ".bin") == 0;
}

/* base + cwd (+ "/" + name); false when it does not fit */
static bool join_path(char *out, size_t len, const char *base, const char *name, bool root_slash)
{
    size_t n = strlcpy(out, base, len);
    if (!is_root() || (root_slash && !name)) {
        n = strlcat(out, R.cwd, len);
    }
    if (name) {
        strlcat(out, "/", len);
        n = strlcat(out, name, len);
    }
    return n < len;
}

/* VFS path of cwd, or of `name` inside it */
static bool full_path(char *out, size_t len, const char *name)
{
    return join_path(out, len, MOUNT, name, false);
}

/* path as the prototype shows it: /sd/... */
static void shown_path(char *out, size_t len, const char *name)
{
    join_path(out, len, "/sd", name, true);
}

static int entry_cmp(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->dir != y->dir) {
        return x->dir ? -1 : 1; /* folders first */
    }
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->name, y->name);
}

static void free_entries(void)
{
    for (int i = 0; i < R.n; i++) {
        free(R.e[i].name);
    }
    R.n = R.listed = R.all = R.ndirs = 0;
}

static bool load_cb(const l5_files_entry_t *e, void *user)
{
    (void)user;
    if (l5_files_hidden(e->name)) {
        return true;
    }
    R.all++;
    if (!e->dir && g_cfg.onlyBins && !is_bin(e->name)) {
        return true;
    }
    R.listed++;
    R.ndirs += e->dir;
    if (R.n >= MAX_ENTRIES) {
        return true;
    }
    entry_t *x = &R.e[R.n];
    memset(x, 0, sizeof(*x));
    if (!(x->name = strdup(e->name))) {
        return false;
    }
    x->dir = e->dir;
    x->bin = !e->dir && is_bin(e->name);
    x->size = e->size;
    x->mtime = e->mtime;
    x->items = -1;
    R.n++;
    return true;
}

/* sdItems(): folders always, files filtered by cfg.onlyBins, folders first then by name. Name, size and
   date come from one pass over the folder (l5_files_list); folder item counts follow in list order for at
   most COUNT_BUDGET_US, a folder left over is counted when it is selected. */
static void load_dir(int64_t *read_us, int64_t *count_us)
{
    int64_t t0 = esp_timer_get_time();
    free_entries();
    if (!R.e) {
        R.e = calloc(MAX_ENTRIES, sizeof(entry_t));
    }
    char dir[PATH_LEN], p[PATH_LEN];
    full_path(dir, sizeof(dir), NULL);
    esp_err_t err = R.e ? l5_files_list(dir, load_cb, NULL) : ESP_ERR_NO_MEM;
    if (err == ESP_ERR_NOT_FOUND && !is_root()) { /* folder gone (card swapped): back to the root */
        free_entries();
        strcpy(R.cwd, "/");
        R.sel[0] = 0;
        full_path(dir, sizeof(dir), NULL);
        err = l5_files_list(dir, load_cb, NULL);
    }
    R.dir_ok = err == ESP_OK;
    qsort(R.e, R.n, sizeof(entry_t), entry_cmp);
    int64_t t1 = esp_timer_get_time();
    for (int i = 0; i < R.n && esp_timer_get_time() - t1 < COUNT_BUDGET_US; i++) {
        if (R.e[i].dir && full_path(p, sizeof(p), R.e[i].name)) {
            R.e[i].items = l5_files_items(p, 999);
        }
    }
    *read_us = t1 - t0;
    *count_us = esp_timer_get_time() - t1;
}

static int find(const char *name)
{
    for (int i = 0; name[0] && i < R.n; i++) {
        if (strcmp(R.e[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void fmt_time(char *out, size_t len, time_t t, bool with_time)
{
    struct tm tm;
    localtime_r(&t, &tm);
    if (t <= 0 || tm.tm_year + 1900 < 1981) { /* FAT has no dates before 1980 */
        snprintf(out, len, "-");
        return;
    }
    strftime(out, len, with_time ? "%Y-%m-%d %H:%M" : "%Y-%m-%d", &tm);
}

/* display name: the file name without .bin (L5.binInfo) */
static void bin_name(char *out, size_t len, const char *file)
{
    strlcpy(out, file, len);
    size_t l = strlen(out);
    if (is_bin(out) && l > 4) {
        out[l - 4] = 0;
    }
}

/* ---------- shared with l5_sd_ops.c (l5_sd.h) ---------- */

bool l5_sd_target(l5_sd_target_t *t)
{
    memset(t, 0, sizeof(*t));
    int i = find(R.sel);
    if (i >= 0) {
        if (!full_path(t->path, sizeof(t->path), R.e[i].name)) return false;
        t->dir = R.e[i].dir;
        t->items = R.e[i].items;
    } else if (!is_root()) {
        if (!full_path(t->path, sizeof(t->path), NULL)) return false;
        t->dir = t->is_cwd = true;
        t->items = R.all;
    } else {
        return false;
    }
    t->name = strrchr(t->path, '/') + 1;
    return true;
}

void l5_sd_cwd_path(char *out, size_t len)
{
    full_path(out, len, NULL);
}

void l5_sd_after_op(const char *cwd, const char *select)
{
    size_t ml = strlen(MOUNT);
    if (cwd && strncmp(cwd, MOUNT, ml) == 0) {
        strlcpy(R.cwd, cwd[ml] ? cwd + ml : "/", sizeof(R.cwd));
    }
    strlcpy(R.sel, select ? select : "", sizeof(R.sel));
    l5_ui_render_views(L5_VIEW_BIT(L5_VIEW_SD)); /* after the release while a finger is down */
}

static void clip_deleted_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    if (o == R.paste_chip) R.paste_chip = NULL;
    if (o == R.clip_label) R.clip_label = NULL;
}

void l5_sd_paint_clip(void)
{
    const char *clip = l5_sd_clip_name();
    if (R.paste_chip) {
        lv_obj_set_state(R.paste_chip, LV_STATE_DISABLED, !clip);
        lv_obj_set_state(R.paste_chip, LV_STATE_CHECKED, clip != NULL); /* .chip.acc */
    }
    if (R.clip_label) {
        if (clip) {
            lv_label_set_text_fmt(R.clip_label, "CLIPBOARD: %s", clip);
        } else {
            lv_label_set_text(R.clip_label, R.listed > R.n ? "" : "TAP A FOLDER TO OPEN \xC2\xB7 TAP A FILE TO SELECT");
        }
    }
}

/* ---------- install: the flow shared with OTA (l5_inst_ui.c) ---------- */

/* A merged image INSTALL takes (bootloader, table, app, data images; the file starts at flash 0x0 or at the
   bootloader, l5_fw_merged_base): true with its base, else false with why (empty: a bootloader alone) */
static bool merged_probe(const char *path, uint32_t size, uint32_t *base, char *why, size_t len)
{
    uint8_t *head = heap_caps_malloc(L5_FW_PROBE_LEN, MALLOC_CAP_SPIRAM);
    l5_fw_t *fw = heap_caps_calloc(1, sizeof(l5_fw_t), MALLOC_CAP_SPIRAM);
    FILE *f = head && fw ? fopen(path, "rb") : NULL;
    size_t n = f ? fread(head, 1, L5_FW_PROBE_LEN, f) : 0;
    if (f) fclose(f);
    bool ok = false;
    why[0] = 0;
    if (n) {
        l5_fw_init(fw, L5_FW_APP_ONLY);
        esp_err_t err = l5_fw_from_probe(fw, head, n, size, why, len);
        ok = err == ESP_OK && fw->format == L5_FW_MERGED;
        *base = fw->base;
        if (!ok && fw->format != L5_FW_MERGED) why[0] = 0; /* not merged at all: the bootloader line */
    }
    heap_caps_free(head);
    heap_caps_free(fw);
    return ok;
}

static void install_cb(lv_event_t *e)
{
    (void)e;
    l5_fw_t *fw = heap_caps_calloc(1, sizeof(l5_fw_t), MALLOC_CAP_SPIRAM);
    uint8_t *head = heap_caps_malloc(L5_FW_PROBE_LEN, MALLOC_CAP_SPIRAM);
    char why[120] = "";
    size_t len = 0;
    uint32_t size = 0, app = 0;
    bool net;
    esp_err_t err = fw && head ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        l5_fw_init(fw, L5_FW_APP_ONLY);
        err = l5_inst_probe(s_pending.path, NULL, head, &len, &size, why, sizeof(why), &net);
    }
    if (err == ESP_OK) err = l5_fw_from_probe(fw, head, len, size, why, sizeof(why));
    /* the exact app length (segment headers), so the slot check and AUTO PARTITION use the image */
    if (err == ESP_OK && l5_inst_app_len_file(s_pending.path, fw->app_offset, l5_fw_app_bound(fw), &app) == ESP_OK) {
        fw->app_size = app;
    }
    heap_caps_free(head);
    if (err != ESP_OK) {
        heap_caps_free(fw);
        l5_info("INSTALL", "Cannot install", why[0] ? why : "Not enough memory.");
        return;
    }
    l5_fw_set_source(fw, "firmware", s_pending.path);
    strlcpy(fw->name, s_pending.name, sizeof(fw->name));
    l5_install_fw(fw, s_pending.name, "", "SD");
}

/* ---------- side panel: selected entry, otherwise the current folder ---------- */

/* esp_app_desc_t from the file header, image check, APP SLOT (L5.fits), STATUS */
static void bin_rows(lv_obj_t *g, const l5_bin_info_t *bi, bool merged, uint32_t base, const char *why)
{
    char t[112], kb[16];
    if (bi->kind == L5_BIN_APP || bi->kind == L5_BIN_CHIP_REV || bi->kind == L5_BIN_OTHER_CHIP) {
        l5w_kv(g, "PROJECT", bi->desc.project_name, false);
        l5w_kv(g, "VERSION", bi->desc.version, false);
        snprintf(t, sizeof(t), "%s %s", bi->desc.date, bi->desc.time);
        l5w_kv(g, "BUILT", t, false);
        l5w_kv(g, "IDF", bi->desc.idf_ver, false);
    }
    switch (bi->kind) {
    case L5_BIN_APP:
        l5_fmt_kb(kb, sizeof(kb), g_slots.slot_kb);
        if (g_slots.n == 0) {
            l5w_kv(g, "APP SLOT", "no ota_N slot in flash", true);
        } else if ((uint64_t)bi->size <= (uint64_t)g_slots.slot_kb * 1024) {
            snprintf(t, sizeof(t), "fits (%sB)", kb);
            l5w_kv(g, "APP SLOT", t, false);
        } else {
            snprintf(t, sizeof(t), "too big for %sB", kb);
            l5w_kv(g, "APP SLOT", t, true);
        }
        break;
    case L5_BIN_CHIP_REV:
        if (bi->rev_max == 0xFFFF) {
            snprintf(t, sizeof(t), "needs chip rev v%u.%u+, this one is v%u.%u", bi->rev_min / 100, bi->rev_min % 100,
                     bi->rev_chip / 100, bi->rev_chip % 100);
        } else {
            snprintf(t, sizeof(t), "needs chip rev v%u.%u-v%u.%u, this one is v%u.%u", bi->rev_min / 100,
                     bi->rev_min % 100, bi->rev_max / 100, bi->rev_max % 100, bi->rev_chip / 100, bi->rev_chip % 100);
        }
        l5w_kv(g, "IMAGE", t, true);
        break;
    case L5_BIN_OTHER_CHIP:
        snprintf(t, sizeof(t), "%s app, not ESP32-P4", l5_chip_name(bi->chip_id));
        l5w_kv(g, "IMAGE", t, true);
        break;
    case L5_BIN_NO_APP:
        if (merged) {
            snprintf(t, sizeof(t), "merged image from 0x%X: bootloader, partition table, app, data", (unsigned)base);
            l5w_kv(g, "IMAGE", t, false);
        } else {
            l5w_kv(g, "IMAGE", why[0] ? why : "bootloader, not an app", true);
        }
        break;
    default:
        l5w_kv(g, "IMAGE", "not an ESP app image", true);
        break;
    }
    if (bi->kind == L5_BIN_APP) {
        int n = snprintf(t, sizeof(t), "in flash \xC2\xB7");
        bool any = false;
        for (int i = 0; i < g_slots.n && n > 0 && (size_t)n < sizeof(t); i++) {
            const l5_slot_t *s = &g_slots.slot[i];
            if (s->used && memcmp(s->elf_sha, bi->desc.app_elf_sha256, sizeof(s->elf_sha)) == 0) {
                n += snprintf(t + n, sizeof(t) - n, "%s app%d", any ? "," : "", i);
                any = true;
            }
        }
        if (any) {
            l5w_kv(g, "STATUS", t, false);
        }
    }
}

static void side_foot(lv_obj_t *s)
{
    lv_obj_t *f = l5_box(s); /* .side-foot */
    lv_obj_set_size(f, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(f, 12, 0);
    lv_obj_set_style_pad_hor(f, 22, 0);
    lv_obj_set_style_pad_bottom(f, 16, 0);
    lv_obj_set_style_pad_row(f, 8, 0);
    lv_obj_set_style_border_side(f, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(f, 1, 0);
    lv_obj_set_style_border_color(f, L5_PLATE_LO, 0);
    lv_obj_set_flex_flow(f, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *r = l5_box(f);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *l = l5_label(r, &l5_mono_14, L5_INK_SOFT, "SD CARD \xC2\xB7 FAT");
    lv_obj_set_style_text_letter_space(l, 1, 0);
    char t[48];
    uint64_t used = R.total > R.free ? R.total - R.free : 0;
    snprintf(t, sizeof(t), "%.1f / %.1f GB", used / 1073741824.0, R.total / 1073741824.0);
    l5_label(r, &l5_mono_14, L5_INK_SOFT, t);
    lv_obj_t *m = lv_bar_create(f); /* .meter on the plate */
    lv_obj_remove_flag(m, LV_OBJ_FLAG_CLICKABLE); /* lv_bar is clickable by default */
    lv_obj_remove_style_all(m);
    lv_obj_set_size(m, lv_pct(100), 8);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(m, lv_color_hex(0xc7c9ce), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(m, L5P.accent, LV_PART_INDICATOR);
    lv_bar_set_range(m, 0, 1000);
    lv_bar_set_value(m, R.total ? (int32_t)(used * 1000 / R.total) : 0, LV_ANIM_OFF);
}

static void act_cb(lv_event_t *e)
{
    l5_sd_action(lv_event_get_user_data(e));
}

/* one of the file actions in .acts: the full width (wide), half or a third of it */
static lv_obj_t *act(lv_obj_t *acts, const char *icon, const char *text, const char *id, int32_t w)
{
    lv_obj_t *b = l5_act_button(acts, icon, text, false);
    lv_obj_set_width(b, w);
    lv_obj_set_style_pad_hor(b, 8, 0);
    lv_obj_add_event_cb(b, act_cb, LV_EVENT_CLICKED, (void *)id);
    return b;
}

static void build_side(void)
{
    lv_obj_t *s = R.side;
    lv_obj_clean(s);
    int i = find(R.sel);
    entry_t *x = i >= 0 ? &R.e[i] : NULL;
    char t[PATH_LEN], sub[96], name[128];
    lv_obj_t *h = l5w_side_head(s);
    l5_bin_info_t bi;
    bool bin = x && x->bin && full_path(t, sizeof(t), x->name) && l5_bin_probe(t, &bi) == ESP_OK;
    char mwhy[240] = "";
    uint32_t mbase = 0;
    bool merged = bin && bi.kind == L5_BIN_NO_APP && merged_probe(t, bi.size, &mbase, mwhy, sizeof(mwhy));
    if (x && x->dir && x->items < 0 && full_path(t, sizeof(t), x->name)) {
        x->items = l5_files_items(t, 999); /* not counted while the folder was listed */
    }

    if (bin) {
        bin_name(name, sizeof(name), x->name);
        int num = 0; /* emblem number: position among the .bin files of this folder */
        for (int k = 0; k <= i; k++) {
            num += R.e[k].bin;
        }
        bool app = bi.kind == L5_BIN_APP || bi.kind == L5_BIN_CHIP_REV || bi.kind == L5_BIN_OTHER_CHIP;
        l5_cover_create(h, 132, name, name, app ? bi.desc.project_name : "", num, true);
        if (app) {
            snprintf(sub, sizeof(sub), "%s \xC2\xB7 v%s", bi.desc.project_name, bi.desc.version);
        } else {
            snprintf(sub, sizeof(sub), "%s", merged ? "merged image" : mwhy[0] ? "not installable" : "no app description");
        }
        l5w_head_text(h, "FIRMWARE", name, sub);
    } else if (x) {
        l5w_big_icon(h, x->dir ? L5_ICON_FOLDER : L5_ICON_FILE);
        if (x->dir) {
            snprintf(sub, sizeof(sub), "%d items", x->items < 0 ? 0 : x->items);
        }
        l5w_head_text(h, x->dir ? "FOLDER" : "FILE", x->name, x->dir ? sub : "");
    } else {
        const char *slash = strrchr(R.cwd, '/');
        l5w_big_icon(h, L5_ICON_FOLDER);
        snprintf(sub, sizeof(sub), "%d items", R.all);
        l5w_head_text(h, "CURRENT FOLDER", is_root() ? "SD card" : slash + 1, sub);
    }

    lv_obj_t *g = l5w_kv_grid(s);
    shown_path(t, sizeof(t), x ? x->name : NULL);
    lv_obj_set_style_text_font(l5w_kv(g, "PATH", t, false), &l5_mono15_tall, 0); /* accented capitals */
    if (x && !x->dir) {
        l5_fmt_bytes(sub, sizeof(sub), x->size);
        l5w_kv(g, "SIZE", sub, false);
    }
    if (x) {
        fmt_time(sub, sizeof(sub), x->mtime, true);
        l5w_kv(g, "MODIFIED", sub, false);
    } else {
        l5_fmt_bytes(sub, sizeof(sub), R.free);
        l5w_kv(g, "FREE", sub, false);
    }
    if (bin) {
        bin_rows(g, &bi, merged, mbase, mwhy);
    }

    lv_obj_t *grow = l5_box(s); /* .acts { margin-top: auto } */
    lv_obj_set_width(grow, lv_pct(100));
    lv_obj_set_flex_grow(grow, 1);
    if (x || !is_root()) { /* .acts: INSTALL / VIEW across, then the file actions (NEW FOLDER, PASTE: bar) */
        lv_obj_t *acts = l5_box(s);
        lv_obj_set_size(acts, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(acts, 22, 0);
        lv_obj_set_style_pad_bottom(acts, 17, 0); /* 14 + the 3px button shadow */
        lv_obj_set_style_pad_gap(acts, 10, 0);
        lv_obj_set_flex_flow(acts, LV_FLEX_FLOW_ROW_WRAP);
        if (bin) {
            lv_obj_t *b = l5_act_button(acts, L5_ICON_INSTALL, "INSTALL", true);
            lv_obj_set_width(b, lv_pct(100));
            lv_obj_add_event_cb(b, install_cb, LV_EVENT_CLICKED, NULL);
            if (bi.kind == L5_BIN_APP || merged) {
                full_path(s_pending.path, sizeof(s_pending.path), x->name);
                strlcpy(s_pending.name, name, sizeof(s_pending.name));
            } else {
                lv_obj_add_state(b, LV_STATE_DISABLED);
            }
        } else if (x && !x->dir && l5_files_is_text(x->name)) {
            act(acts, L5_ICON_FILE, "VIEW", "view", lv_pct(100));
        }
        int32_t w = x ? (ACTS_W - 2 * 10) / 3 : (ACTS_W - 10) / 2;
        act(acts, L5_ICON_EDIT, "RENAME", "rename", w);
        if (x) {
            act(acts, L5_ICON_COPY, "COPY", "copy", w);
        }
        act(acts, L5_ICON_TRASH, "DELETE", "delete", w);
    }
    side_foot(s);
}

/* ---------- main pane events ---------- */

static void open_dir(const char *name)
{
    size_t l = strlen(R.cwd);
    if (l + 1 + strlen(name) < sizeof(R.cwd)) {
        snprintf(R.cwd + l, sizeof(R.cwd) - l, "%s%s", is_root() ? "" : "/", name);
        R.sel[0] = 0;
    } else {
        l5_toast("Path too long");
    }
    l5_ui_render_async(); /* the rows are deleted by the re-render */
}

/* the keyboard's selection: the row as a tap marks it, scrolled into view */
static void select_row(int i)
{
    if (!R.list || i < 0 || i >= R.n) {
        return;
    }
    strlcpy(R.sel, R.e[i].name, sizeof(R.sel));
    for (int k = 0; k < R.n; k++) {
        lv_obj_set_state(lv_obj_get_child(R.list, k), LV_STATE_CHECKED, k == i);
    }
    lv_obj_scroll_to_view(lv_obj_get_child(R.list, i), LV_ANIM_OFF);
    build_side();
}

static void row_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_current_target(e);
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= R.n) {
        return;
    }
    const entry_t *x = &R.e[i];
    if (x->dir) {
        open_dir(x->name);
        return;
    }
    strlcpy(R.sel, x->name, sizeof(R.sel));
    uint32_t n = lv_obj_get_child_count(R.list);
    for (uint32_t k = 0; k < n; k++) {
        lv_obj_remove_state(lv_obj_get_child(R.list, k), LV_STATE_CHECKED);
    }
    lv_obj_add_state(row, LV_STATE_CHECKED);
    build_side();
}

static void up_cb(lv_event_t *e)
{
    (void)e;
    char *slash = strrchr(R.cwd, '/');
    if (is_root() || !slash) {
        return;
    }
    strlcpy(R.sel, slash + 1, sizeof(R.sel)); /* the folder we leave stays selected */
    if (slash == R.cwd) {
        strcpy(R.cwd, "/");
    } else {
        *slash = 0;
    }
    l5_ui_render_async();
}

static void filter_cb(lv_event_t *e)
{
    (void)e;
    g_cfg.onlyBins = !g_cfg.onlyBins;
    l5_cfg_save(); /* in the background */
    l5_cfg_paint(); /* CFG's "Only Bins" toggle, in place */
    l5_ui_render_views_async(L5_VIEW_BIT(L5_VIEW_SD)); /* the list changes; this chip is deleted with it */
}

static void mkdir_cb(lv_event_t *e)
{
    (void)e;
    l5_sd_action("mkdir");
}

static void paste_cb(lv_event_t *e)
{
    (void)e;
    l5_sd_action("paste");
}

/* ---------- main pane ---------- */

static void bar(lv_obj_t *m)
{
    lv_obj_t *b = l5_box(m); /* .bar */
    lv_obj_set_size(b, lv_pct(100), 66);
    lv_obj_set_style_pad_left(b, 18, 0);
    lv_obj_set_style_pad_right(b, 12, 0);
    lv_obj_set_style_pad_column(b, 10, 0);
    lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, L5_PANEL_LINE, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *up = l5w_chip(b, L5_ICON_CHEV_L, "UP", false, up_cb);
    if (is_root()) {
        lv_obj_add_state(up, LV_STATE_DISABLED);
    }
    char t[PATH_LEN];
    shown_path(t, sizeof(t), NULL);
    lv_obj_t *p = l5w_line_label(b, &l5_mono18_tall, L5_ON_PANEL, t); /* room for accented capitals */
    lv_obj_set_flex_grow(p, 1);
    l5w_chip(b, NULL, g_cfg.onlyBins ? "ONLY BINS" : "ALL FILES", g_cfg.onlyBins, filter_cb);
    l5w_chip(b, L5_ICON_PLUS, "FOLDER", false, mkdir_cb);
    lv_obj_t *pc = l5w_chip(b, L5_ICON_PASTE, "PASTE", false, paste_cb);
    lv_obj_set_style_bg_color(pc, L5P.accent, LV_STATE_CHECKED); /* .chip.acc while the clipboard holds something */
    lv_obj_set_style_border_color(pc, L5P.accent, LV_STATE_CHECKED);
    lv_obj_set_style_text_color(pc, L5P.accent_ink, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(pc, L5_PANEL_3, LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_remove_local_style_prop(lv_obj_get_child(pc, 0), LV_STYLE_TEXT_COLOR, 0); /* the icon follows the chip */
    R.paste_chip = pc;
    lv_obj_add_event_cb(pc, clip_deleted_cb, LV_EVENT_DELETE, NULL);
}

/* .frow, shared by all rows: 200 rows with their own local styles took twice as long to build */
static lv_style_t s_row, s_row_pr, s_row_sel, s_icon, s_name, s_meta, s_date;

static void row_styles(void)
{
    static bool done;
    if (!done) {
        lv_style_init(&s_row);
        lv_style_set_width(&s_row, lv_pct(100));
        lv_style_set_height(&s_row, 62);
        lv_style_set_pad_hor(&s_row, 18);
        lv_style_set_pad_column(&s_row, 14);
        lv_style_set_border_side(&s_row, LV_BORDER_SIDE_BOTTOM);
        lv_style_set_border_width(&s_row, 1);
        lv_style_set_border_color(&s_row, L5_PANEL_LINE);
        lv_style_set_text_color(&s_row, L5_ON_PANEL);
        lv_style_set_layout(&s_row, LV_LAYOUT_FLEX);
        lv_style_set_flex_flow(&s_row, LV_FLEX_FLOW_ROW);
        lv_style_set_flex_main_place(&s_row, LV_FLEX_ALIGN_START);
        lv_style_set_flex_cross_place(&s_row, LV_FLEX_ALIGN_CENTER);
        lv_style_set_flex_track_place(&s_row, LV_FLEX_ALIGN_CENTER);
        lv_style_init(&s_row_pr);
        lv_style_set_bg_color(&s_row_pr, L5_PANEL_3);
        lv_style_set_bg_opa(&s_row_pr, LV_OPA_COVER);
        lv_style_init(&s_row_sel);
        lv_style_set_bg_opa(&s_row_sel, 56); /* rgba(accent, .22) */
        lv_style_init(&s_icon);
        lv_style_set_text_font(&s_icon, &l5_icon_24);
        lv_style_set_text_color(&s_icon, L5_ON_PANEL_MUTED);
        lv_style_set_width(&s_icon, 28);
        lv_style_set_text_align(&s_icon, LV_TEXT_ALIGN_CENTER);
        lv_style_init(&s_name);
        lv_style_set_text_font(&s_name, &l5_chakra_sb19);
        lv_style_set_text_color(&s_name, L5_ON_PANEL);
        lv_style_set_height(&s_name, lv_font_get_line_height(&l5_chakra_sb19));
        lv_style_set_flex_grow(&s_name, 1);
        lv_style_init(&s_meta);
        lv_style_set_text_font(&s_meta, &l5_mono_14);
        lv_style_set_text_color(&s_meta, L5_ON_PANEL_MUTED);
        lv_style_init(&s_date);
        lv_style_set_width(&s_date, 150);
        lv_style_set_text_align(&s_date, LV_TEXT_ALIGN_RIGHT);
        done = true;
    }
    lv_style_set_bg_color(&s_row_sel, L5P.accent); /* CFG > UI color can change it */
}

static void row(lv_obj_t *list, int i)
{
    const entry_t *x = &R.e[i];
    lv_obj_t *r = lv_button_create(list); /* .frow */
    lv_obj_remove_style_all(r);
    lv_obj_add_style(r, &s_row, 0);
    lv_obj_add_style(r, &s_row_sel, LV_STATE_CHECKED);
    lv_obj_add_style(r, &s_row_pr, LV_STATE_PRESSED);
    lv_obj_add_event_cb(r, l5w_row_sel_draw, LV_EVENT_DRAW_POST, NULL);
    lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    if (strcmp(x->name, R.sel) == 0) {
        lv_obj_add_state(r, LV_STATE_CHECKED);
    }

    lv_obj_t *ic = lv_label_create(r);
    lv_obj_add_style(ic, &s_icon, 0);
    lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);
    if (x->bin) {
        lv_obj_set_style_text_color(ic, L5P.accent, 0);
    }
    lv_label_set_text_static(ic, x->dir ? L5_ICON_FOLDER : x->bin ? L5_ICON_BIN : L5_ICON_FILE);
    char t[300];
    snprintf(t, sizeof(t), "%s%s", x->name, x->dir ? "/" : "");
    lv_obj_t *nm = lv_label_create(r);
    lv_obj_add_style(nm, &s_name, 0);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_label_set_text(nm, t);
    if (x->dir) {
        if (x->items >= 0) {
            snprintf(t, sizeof(t), "%d items", x->items);
        } else {
            t[0] = 0; /* counted when selected */
        }
    } else {
        l5_fmt_bytes(t, sizeof(t), x->size);
    }
    lv_obj_t *sz = lv_label_create(r);
    lv_obj_add_style(sz, &s_meta, 0);
    lv_label_set_text(sz, t);
    fmt_time(t, sizeof(t), x->mtime, false);
    lv_obj_t *d = lv_label_create(r);
    lv_obj_add_style(d, &s_meta, 0);
    lv_obj_add_style(d, &s_date, 0);
    lv_label_set_text(d, t);
}

static void foot(lv_obj_t *m)
{
    lv_obj_t *f = l5_box(m); /* .foot */
    lv_obj_set_size(f, lv_pct(100), 44);
    lv_obj_set_style_pad_hor(f, 18, 0);
    lv_obj_set_style_pad_column(f, 12, 0);
    lv_obj_set_style_border_side(f, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(f, 1, 0);
    lv_obj_set_style_border_color(f, L5_PANEL_LINE, 0);
    lv_obj_set_style_text_font(f, &l5_mono_13, 0);
    lv_obj_set_style_text_letter_space(f, 1, 0);
    lv_obj_set_style_text_color(f, L5_ON_PANEL_MUTED, 0);
    lv_obj_set_flex_flow(f, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(f, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *l = lv_label_create(f);
    if (R.listed > R.n) {
        lv_label_set_text_fmt(l, "%d FOLDERS \xC2\xB7 %d FILES \xC2\xB7 FIRST %d LISTED", R.ndirs, R.listed - R.ndirs, R.n);
    } else {
        lv_label_set_text_fmt(l, "%d FOLDERS \xC2\xB7 %d FILES", R.ndirs, R.listed - R.ndirs);
    }
    /* CLIPBOARD: name, or the hint; takes the rest of the row and is cut with dots */
    R.clip_label = l5w_line_label(f, &l5_mono13_tall, L5_ON_PANEL_MUTED, "");
    lv_obj_set_flex_grow(R.clip_label, 1);
    lv_obj_set_style_text_align(R.clip_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_event_cb(R.clip_label, clip_deleted_cb, LV_EVENT_DELETE, NULL);
}

/* TRY AGAIN: card init can take seconds when nothing answers, so mount off the LVGL task */
static void retry_task(void *arg)
{
    (void)arg;
    esp_err_t err = l5_board_sd_mount();
    l5_board_lock(0);
    if (err == ESP_OK) {
        l5_toast("SD card mounted");
    } else {
        l5_toast("SD card: %s", l5_board_sd_status(false));
    }
    if (l5_ui_view() == L5_VIEW_SD) {
        l5_ui_render();
    }
    l5_board_unlock();
    vTaskDelete(NULL);
}

static void retry_cb(lv_event_t *e)
{
    lv_obj_t *b = lv_event_get_current_target(e);
    lv_obj_add_state(b, LV_STATE_DISABLED);
    lv_label_set_text(lv_obj_get_child(b, -1), "CHECKING\xE2\x80\xA6");
    if (xTaskCreatePinnedToCore(retry_task, "l5_sd_retry", 4096, NULL, 3, NULL, 1) != pdPASS) {
        lv_obj_remove_state(b, LV_STATE_DISABLED);
        l5_toast("Not enough memory to check the card");
    }
}

/* No card / unreadable card / driver error, instead of an empty list */
static void no_card(lv_obj_t *m, lv_obj_t *s)
{
    l5_sd_state_t st = l5_board_sd_state();
    const char *title = st == L5_SD_UNREADABLE ? "Card not readable" : st == L5_SD_ERROR ? "SD card error" : "No SD card";
    l5_col_header(m, "SD CARD", l5_board_sd_status(false));
    lv_obj_t *box = l5_box(m);
    lv_obj_set_width(box, lv_pct(100));
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_style_pad_bottom(box, 24, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    char body[320];
    if (st == L5_SD_ERROR) {
        snprintf(body, sizeof(body), "%s (%s)", l5_board_sd_status(true), esp_err_to_name(l5_board_sd_error()));
    } else {
        strlcpy(body, l5_board_sd_status(true), sizeof(body));
    }
    l5w_empty(box, title, body);
    lv_obj_t *b = l5_act_button(box, L5_ICON_RESTART, "TRY AGAIN", true);
    lv_obj_set_size(b, 280, 60);
    lv_obj_add_event_cb(b, retry_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *h = l5w_side_head(s);
    l5w_big_icon(h, L5_ICON_SD);
    l5w_head_text(h, "SD CARD", st == L5_SD_UNREADABLE ? "Not readable" : st == L5_SD_ERROR ? "Driver error" : "No card",
              "Firmware .bin files on a FAT32 or exFAT card show up here and install into a flash slot.");
}

/* keys of the Tab5 Keyboard (l5_keys.c): Up / Down, Home / End, PgUp / PgDn by 8 rows move the selection; Enter
   opens a folder, installs the .bin the side panel offers, views a text file; Esc / Backspace = UP, Delete = DELETE,
   Ctrl+C COPY, Ctrl+V PASTE, Ctrl+R RENAME, Ctrl+N NEW FOLDER */
bool l5_sd_key(const l5_key_t *k)
{
    if (!R.list) {
        return false; /* no card */
    }
    int i = find(R.sel), last = R.n - 1;
    switch (k->kind) {
    case L5_KEY_UP: select_row(i < 0 ? last : i > 0 ? i - 1 : 0); return true;
    case L5_KEY_DOWN: select_row(i < 0 ? 0 : i < last ? i + 1 : last); return true;
    case L5_KEY_HOME: select_row(0); return true;
    case L5_KEY_END: select_row(last); return true;
    case L5_KEY_PGUP: select_row(i > 8 ? i - 8 : 0); return true;
    case L5_KEY_PGDN: select_row(i < 0 ? 0 : i + 8 < last ? i + 8 : last); return true;
    case L5_KEY_ESC:
    case L5_KEY_BACKSPACE: up_cb(NULL); return true;
    case L5_KEY_DELETE: l5_sd_action("delete"); return true;
    case L5_KEY_ENTER: {
        if (i < 0) {
            return false;
        }
        const entry_t *x = &R.e[i];
        if (x->dir) {
            open_dir(x->name);
        } else if (x->bin) {
            char t[PATH_LEN]; /* INSTALL as the side panel offers it: only for an image it can install */
            if (full_path(t, sizeof(t), x->name) && strcmp(t, s_pending.path) == 0) {
                install_cb(NULL);
            }
        } else if (l5_files_is_text(x->name)) {
            l5_sd_action("view");
        }
        return true;
    }
    case L5_KEY_CHAR:
        if (!k->ctrl) {
            return false;
        }
        switch (k->ch | 0x20) {
        case 'c': l5_sd_action("copy"); return true;
        case 'v': l5_sd_action("paste"); return true;
        case 'r': l5_sd_action("rename"); return true;
        case 'n': l5_sd_action("mkdir"); return true;
        default: return false;
        }
    default: return false;
    }
}

void l5_sd_render(lv_obj_t *view)
{
    lv_obj_clean(view);
    lv_obj_t *m = l5w_pane(view, true, 12, 816);
    R.side = l5w_pane(view, false, 840, 428);
    R.list = NULL;
    if (!l5_board_sd_ready()) {
        no_card(m, R.side);
        return;
    }
    int64_t read_us, count_us;
    load_dir(&read_us, &count_us); /* re-read on every full render: tab entry, folder change, filter, file action */
    int64_t t0 = esp_timer_get_time();
    if (find(R.sel) < 0) {
        R.sel[0] = 0;
    }
    if (esp_vfs_fat_info(MOUNT, &R.total, &R.free) != ESP_OK) {
        R.total = R.free = 0;
    }

    bar(m);
    R.list = l5_box(m); /* .scroll */
    lv_obj_set_width(R.list, lv_pct(100));
    lv_obj_set_flex_grow(R.list, 1);
    lv_obj_set_flex_flow(R.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(R.list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE); /* clickable: a drag between rows scrolls too */
    lv_obj_set_scroll_dir(R.list, LV_DIR_VER);
    lv_obj_set_style_bg_opa(R.list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(R.list, L5P.accent, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(R.list, 5, LV_PART_SCROLLBAR);
    lv_obj_t *sel = NULL;
    row_styles();
    for (int i = 0; i < R.n; i++) {
        row(R.list, i);
        if (strcmp(R.e[i].name, R.sel) == 0) {
            sel = lv_obj_get_child(R.list, -1);
        }
    }
    if (!R.dir_ok) {
        l5w_empty(R.list, "Cannot read the folder", "The SD card did not answer. Reinsert it and restart the launcher.");
    } else if (R.n == 0) {
        l5w_empty(R.list, "Empty folder", g_cfg.onlyBins ? "Only .bin files are listed. Switch to ALL FILES to see "
              "everything." : "Tap FOLDER to create one, or PASTE a copied file.");
    }
    foot(m);
    l5_sd_paint_clip();
    build_side();
    if (sel) {
        lv_obj_update_layout(R.list);
        lv_obj_scroll_to_view(sel, LV_ANIM_OFF);
    }
    char t[PATH_LEN];
    shown_path(t, sizeof(t), NULL);
    ESP_LOGI(TAG, "%s: %d entries (%d listed, %d rows): read in %d ms, folders counted in %d ms, view built in %d ms", t,
             R.all, R.listed, R.n, (int)(read_us / 1000), (int)(count_us / 1000), (int)((esp_timer_get_time() - t0) / 1000));
}
