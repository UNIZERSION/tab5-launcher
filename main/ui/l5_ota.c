/* OTA: the online installer. Port of L5.views.ota (views.js) on the catalog Launcher 2.10 uses for
   the Tab5 (src/onlineLauncher.cpp, boards/m5stack-tab5 OTA_TAG "tab5"):
     list      GET https://api.launcherhub.net/firmwares?category=tab5&order_by=name|date[&page=N][&q=..][&star=1]
     versions  GET .../firmwares?fid=X
     install   GET .../firmwares?fid=X&version=V   (manifest: app offset and size, data partitions)
     files     https://m5burner-cdn.m5stack.com/firmware/<file>, straight: launcherhub's /download and /proxy
               answer 400 without the HWID header (a device ID), which the launcher does not send
   Chips ALL / STARRED / FAVORITES / DOWNLOADED, search on the keyboard (q=), A-Z / LATEST, pages of
   100. The side pane shows only what the catalog gives: version, date, author, star, and from the
   install manifest the app size and the data partitions. INSTALL runs the flow shared with SD
   (l5_inst_ui.c), TO SD saves to /sdcard/downloads with Launcher's downloaded.json, FAVORITE and
   ADD LINK edit "favorite" in /config.conf, UPDATE ALL fetches newer versions of downloaded files.
   Catalog requests run in one worker task (latest request wins); results come back under the LVGL
   lock. Rows carry an initials badge instead of the prototype's canvas cover (100 canvases would
   take 800 KB and seconds to draw); the selected entry gets the full cover. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "l5_board.h"
#include "l5_config.h"
#include "l5_inst.h"
#include "l5_net.h"
#include "l5_slots.h"
#include "l5_ui.h"
#include "l5_wifi.h"

static const char *TAG = "l5_ota";

#define API "https://api.launcherhub.net/firmwares"
#define CDN "https://m5burner-cdn.m5stack.com/firmware/" /* M5_SERVER_PATH */
#define DL_DIR CONFIG_BSP_SD_MOUNT_POINT "/downloads"
#define DL_JSON DL_DIR "/downloaded.json"
#define MAX_ITEMS 100
#define MAX_VERS 16
#define MAX_DL 32
#define MAX_ROWS (MAX_ITEMS > L5_FAV_MAX + MAX_DL ? MAX_ITEMS : L5_FAV_MAX + MAX_DL)
#define NAMES 64

typedef struct {
    char fid[40], name[64], author[48];
    bool star;
} item_t;

typedef struct {
    char version[24], file[96], date[12];
} ver_t;

typedef enum { TAB_ALL, TAB_STAR, TAB_FAV, TAB_DL } tab_t;

static struct {
    tab_t tab;
    char q[49];
    bool by_date;
    int page, pages, total;
    item_t *items; /* the catalog page of ALL / STARRED */
    int nitems;
    bool loaded, loading, list_star; /* list_star: the page is the STARRED list */
    char error[112];
    uint32_t gen;
    char sel[40];  /* fid, or "#n" for custom link favorite n */
    int ver;       /* selected version */
    char dfid[40]; /* versions of this fid */
    ver_t *vers;
    int nvers;
    bool dloading;
    char derror[112], dname[64], dauthor[48];
    bool dstar;
    l5_fw_t *man; /* install manifest of dfid / mver */
    char mver[24];
    bool mloading, mfound;
    char merror[112];
    l5_favs_t *favs;
    struct {
        char fid[40], ver[24];
    } dl[MAX_DL];
    int ndl;
    struct {
        char fid[40], name[64];
    } names[NAMES]; /* fid -> name of every entry seen, for FAVORITES / DOWNLOADED rows */
    int nnames;
    char keys[MAX_ROWS][40];
    int nrows;     /* rows in the list, keys[] in their order */
    lv_obj_t *main, *list, *side;
    bool job;      /* prepare / TO SD / UPDATE ALL running */
} O = {.page = 1};

static void build_list(void);
static void build_side(void);

/* ---------- small helpers ---------- */

static bool contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; n && *hay; hay++) {
        if (strncasecmp(hay, needle, n) == 0) return true;
    }
    return n == 0;
}

/* the catalog page must be fetched again (tab, search, sort or page changed); an answer to an older
   request is dropped by its generation */
static void invalidate(void)
{
    O.loaded = O.loading = false;
}

static void remember_name(const char *fid, const char *name)
{
    if (!fid[0] || !name[0]) return;
    for (int i = 0; i < O.nnames; i++) {
        if (!strcmp(O.names[i].fid, fid)) {
            strlcpy(O.names[i].name, name, sizeof(O.names[i].name));
            return;
        }
    }
    int i = O.nnames < NAMES ? O.nnames++ : (int)(esp_timer_get_time() % NAMES);
    strlcpy(O.names[i].fid, fid, sizeof(O.names[i].fid));
    strlcpy(O.names[i].name, name, sizeof(O.names[i].name));
}

static const char *name_of(const char *fid)
{
    for (int i = 0; i < O.nnames; i++) {
        if (!strcmp(O.names[i].fid, fid)) return O.names[i].name;
    }
    return NULL;
}

static const char *dl_version(const char *fid)
{
    for (int i = 0; i < O.ndl; i++) {
        if (!strcmp(O.dl[i].fid, fid)) return O.dl[i].ver;
    }
    return NULL;
}

static int fav_index(const char *fid)
{
    for (int i = 0; O.favs && fid[0] && i < O.favs->n; i++) {
        if (!strcmp(O.favs->item[i].fid, fid)) return i;
    }
    return -1;
}

static const item_t *item_of(const char *fid)
{
    for (int i = 0; i < O.nitems; i++) {
        if (!strcmp(O.items[i].fid, fid)) return &O.items[i];
    }
    return NULL;
}

/* buildSourceUrl() without the proxy: catalog file names live on the M5Burner CDN */
static void file_url(char *out, size_t len, const char *file)
{
    if (strncmp(file, "http", 4)) snprintf(out, len, CDN "%s", file);
    else strlcpy(out, file, len);
}

/* replaceChars(): a file name the SD card takes */
static void safe_name(char *out, size_t len, const char *name)
{
    size_t o = 0;
    for (const char *c = name; *c && o + 1 < len; c++) {
        out[o++] = isalnum((unsigned char)*c) || *c == '.' || *c == '-' || *c == '_' ? *c : '_';
    }
    out[o] = 0;
    if (!o) strlcpy(out, "firmware", len);
}

/* ---------- downloaded.json: [{"fid": "version", ...}] as Launcher keeps it ---------- */

static cJSON *dl_read(void)
{
    FILE *f = fopen(DL_JSON, "rb");
    if (!f) return NULL;
    char *text = heap_caps_malloc(16 * 1024, MALLOC_CAP_SPIRAM);
    cJSON *root = NULL;
    if (text) {
        text[fread(text, 1, 16 * 1024 - 1, f)] = 0;
        root = cJSON_Parse(text);
        heap_caps_free(text);
    }
    fclose(f);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static void dl_load(void)
{
    O.ndl = 0;
    cJSON *root = dl_read();
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetArrayItem(root, 0)) {
        if (O.ndl < MAX_DL && cJSON_IsString(e) && e->string && strlen(e->string) < sizeof(O.dl[0].fid)) {
            strlcpy(O.dl[O.ndl].fid, e->string, sizeof(O.dl[0].fid));
            strlcpy(O.dl[O.ndl].ver, e->valuestring, sizeof(O.dl[0].ver));
            O.ndl++;
        }
    }
    cJSON_Delete(root);
}

/* saveDownloadedFirmware(): the first object of the array gets fid = version */
static esp_err_t dl_save(const char *fid, const char *ver)
{
    if (!fid[0] || !ver[0]) return ESP_OK;
    cJSON *root = dl_read();
    if (!root) root = cJSON_CreateArray();
    cJSON *obj = cJSON_GetArrayItem(root, 0);
    if (!cJSON_IsObject(obj)) {
        obj = cJSON_CreateObject();
        cJSON_InsertItemInArray(root, 0, obj);
    }
    cJSON_DeleteItemFromObjectCaseSensitive(obj, fid);
    cJSON_AddStringToObject(obj, fid, ver);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    FILE *f = text ? fopen(DL_JSON, "wb") : NULL;
    bool ok = f && fputs(text, f) >= 0;
    ok = f && fclose(f) == 0 && ok;
    cJSON_free(text);
    return ok ? ESP_OK : ESP_FAIL;
}

/* ---------- catalog worker ---------- */

static struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    bool list, detail, manifest; /* pending, latest wins */
    uint32_t gen;
    char list_url[256];
    char fid[40], ver[24];
} W;

static void worker_kick(void)
{
    if (W.task) xTaskNotifyGive(W.task);
}

/* GetJsonFromLauncherHub(): order_by, page, q (encoded), star */
static void request_list(void)
{
    char enc[150];
    O.loading = true;
    O.list_star = O.tab == TAB_STAR;
    O.error[0] = 0;
    l5_net_urlencode(enc, sizeof(enc), O.q);
    xSemaphoreTake(W.lock, portMAX_DELAY);
    W.gen = ++O.gen;
    int n = snprintf(W.list_url, sizeof(W.list_url), API "?category=tab5&order_by=%s", O.by_date ? "date" : "name");
    if (O.page > 1) n += snprintf(W.list_url + n, sizeof(W.list_url) - n, "&page=%d", O.page);
    if (O.q[0]) n += snprintf(W.list_url + n, sizeof(W.list_url) - n, "&q=%s", enc);
    if (O.list_star) snprintf(W.list_url + n, sizeof(W.list_url) - n, "&star=1");
    W.list = true;
    xSemaphoreGive(W.lock);
    worker_kick();
}

static void request_detail(const char *fid)
{
    strlcpy(O.dfid, fid, sizeof(O.dfid));
    O.nvers = 0;
    O.dloading = true;
    O.derror[0] = O.mver[0] = 0;
    O.mfound = O.mloading = false;
    xSemaphoreTake(W.lock, portMAX_DELAY);
    strlcpy(W.fid, fid, sizeof(W.fid));
    W.detail = true;
    W.manifest = false;
    xSemaphoreGive(W.lock);
    worker_kick();
}

static void request_manifest(void)
{
    if (O.ver >= O.nvers) return;
    strlcpy(O.mver, O.vers[O.ver].version, sizeof(O.mver));
    O.mloading = true;
    O.mfound = false;
    O.merror[0] = 0;
    xSemaphoreTake(W.lock, portMAX_DELAY);
    strlcpy(W.fid, O.dfid, sizeof(W.fid));
    strlcpy(W.ver, O.mver, sizeof(W.ver));
    W.manifest = true;
    xSemaphoreGive(W.lock);
    worker_kick();
}

static int parse_list(const char *json, item_t *items, int *total, int *pages)
{
    cJSON *root = cJSON_Parse(json);
    int n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "items")) {
        if (n == MAX_ITEMS) break;
        const cJSON *fid = cJSON_GetObjectItemCaseSensitive(it, "fid"), *name = cJSON_GetObjectItemCaseSensitive(it, "name");
        const cJSON *author = cJSON_GetObjectItemCaseSensitive(it, "author");
        if (!cJSON_IsString(fid) || !cJSON_IsString(name)) continue;
        item_t *x = &items[n++];
        strlcpy(x->fid, fid->valuestring, sizeof(x->fid));
        strlcpy(x->name, name->valuestring, sizeof(x->name));
        strlcpy(x->author, cJSON_IsString(author) ? author->valuestring : "", sizeof(x->author));
        const cJSON *star = cJSON_GetObjectItemCaseSensitive(it, "star");
        x->star = cJSON_IsTrue(star) || (cJSON_IsNumber(star) && star->valueint);
    }
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "total"), *ps = cJSON_GetObjectItemCaseSensitive(root, "page_size");
    *total = cJSON_IsNumber(t) ? t->valueint : n;
    int size = cJSON_IsNumber(ps) && ps->valueint > 0 ? ps->valueint : MAX_ITEMS;
    *pages = *total > 0 ? (*total + size - 1) / size : 1;
    if (!cJSON_IsObject(root)) n = -1;
    cJSON_Delete(root);
    return n;
}

/* versions of a fid; the newest first (published_at, then catalog order) */
static int parse_detail(const char *json, ver_t *vers, char *name, char *author, bool *star)
{
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -1;
    }
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "name"), *a = cJSON_GetObjectItemCaseSensitive(root, "author");
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(root, "star"), *it;
    strlcpy(name, cJSON_IsString(v) ? v->valuestring : "", 64);
    strlcpy(author, cJSON_IsString(a) ? a->valuestring : "", 48);
    *star = cJSON_IsTrue(s) || (cJSON_IsNumber(s) && s->valueint);
    int n = 0;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "versions")) {
        const cJSON *ver = cJSON_GetObjectItemCaseSensitive(it, "version"), *file = cJSON_GetObjectItemCaseSensitive(it, "file");
        const cJSON *date = cJSON_GetObjectItemCaseSensitive(it, "published_at");
        if (n == MAX_VERS || !cJSON_IsString(ver) || !cJSON_IsString(file)) continue;
        ver_t x = {0};
        /* cut short, it would name a version the catalog does not have (?fid=X&version=V): left out instead */
        if (strlen(ver->valuestring) >= sizeof(x.version) || strlen(file->valuestring) >= sizeof(x.file)) {
            ESP_LOGW(TAG, "version \"%.60s\" (file %.40s) left out: the name is longer than the launcher keeps",
                     ver->valuestring, file->valuestring);
            continue;
        }
        strlcpy(x.version, ver->valuestring, sizeof(x.version));
        strlcpy(x.file, file->valuestring, sizeof(x.file));
        strlcpy(x.date, cJSON_IsString(date) ? date->valuestring : "", sizeof(x.date));
        int k = n++; /* insertion sort, newest date first, stable */
        while (k > 0 && strcmp(vers[k - 1].date, x.date) < 0) {
            vers[k] = vers[k - 1];
            k--;
        }
        vers[k] = x;
    }
    cJSON_Delete(root);
    return n;
}

static void worker(void *arg)
{
    (void)arg;
    item_t *items = heap_caps_calloc(MAX_ITEMS, sizeof(item_t), MALLOC_CAP_SPIRAM);
    ver_t *vers = heap_caps_calloc(MAX_VERS, sizeof(ver_t), MALLOC_CAP_SPIRAM);
    l5_fw_t *fw = heap_caps_calloc(1, sizeof(l5_fw_t), MALLOC_CAP_SPIRAM);
    char url[256], fid[40], ver[24], name[64], author[48], err[112];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            int kind = 0;
            uint32_t gen = 0;
            xSemaphoreTake(W.lock, portMAX_DELAY);
            if (W.list) {
                kind = 1, W.list = false, gen = W.gen;
                strlcpy(url, W.list_url, sizeof(url));
            } else if (W.detail) {
                kind = 2, W.detail = false;
                strlcpy(fid, W.fid, sizeof(fid));
                snprintf(url, sizeof(url), API "?fid=%s", fid);
            } else if (W.manifest) {
                char enc[64];
                kind = 3, W.manifest = false;
                strlcpy(fid, W.fid, sizeof(fid));
                strlcpy(ver, W.ver, sizeof(ver));
                l5_net_urlencode(enc, sizeof(enc), ver);
                snprintf(url, sizeof(url), API "?fid=%s&version=%s", fid, enc);
            }
            xSemaphoreGive(W.lock);
            if (!kind) break;
            char *json = NULL;
            size_t len;
            l5_net_err_t ne = {0};
            esp_err_t e = items && vers && fw ? l5_net_fetch(url, kind == 1 ? 96 * 1024 : 32 * 1024, NULL, &json, &len, &ne)
                                              : ESP_ERR_NO_MEM;
            strlcpy(err, e == ESP_ERR_NO_MEM && !ne.text[0] ? "Not enough memory." : ne.text, sizeof(err));
            int n = -1, total = 0, pages = 1;
            bool star = false;
            esp_err_t me = ESP_FAIL;
            if (e == ESP_OK && kind == 1) n = parse_list(json, items, &total, &pages);
            if (e == ESP_OK && kind == 2) n = parse_detail(json, vers, name, author, &star);
            if (e == ESP_OK && kind == 3) {
                l5_fw_init(fw, L5_FW_MERGED);
                me = l5_fw_from_manifest(fw, json, err, sizeof(err));
            }
            if (e == ESP_OK && kind != 3 && n < 0) strlcpy(err, "The catalog answer is not valid JSON.", sizeof(err));
            heap_caps_free(json);
            l5_board_lock(0);
            if (kind == 1 && gen == O.gen) { /* only the latest list request counts */
                O.loading = false;
                O.loaded = n >= 0;
                if (n >= 0) {
                    item_t *t = O.items;
                    O.items = items;
                    items = t ? t : heap_caps_calloc(MAX_ITEMS, sizeof(item_t), MALLOC_CAP_SPIRAM);
                    O.nitems = n;
                    O.total = total;
                    O.pages = pages;
                    for (int i = 0; i < n; i++) remember_name(O.items[i].fid, O.items[i].name);
                } else {
                    strlcpy(O.error, err, sizeof(O.error));
                }
                if (O.list) {
                    build_list();
                    build_side();
                }
            } else if (kind == 2 && !strcmp(fid, O.dfid)) {
                O.dloading = false;
                if (n >= 0) {
                    memcpy(O.vers, vers, sizeof(ver_t) * n);
                    O.nvers = n;
                    strlcpy(O.dname, name, sizeof(O.dname));
                    strlcpy(O.dauthor, author, sizeof(O.dauthor));
                    O.dstar = star;
                    remember_name(fid, name);
                    if (O.ver >= n) O.ver = 0;
                    if (n) request_manifest();
                } else {
                    strlcpy(O.derror, err, sizeof(O.derror));
                }
                if (O.list) build_side();
            } else if (kind == 3 && !strcmp(fid, O.dfid) && !strcmp(ver, O.mver)) {
                O.mloading = false;
                O.mfound = e == ESP_OK && me == ESP_OK;
                if (O.mfound) memcpy(O.man, fw, sizeof(*fw));
                else if (e != ESP_OK || me != ESP_ERR_NOT_FOUND) strlcpy(O.merror, err, sizeof(O.merror));
                if (O.list) build_side();
            }
            l5_board_unlock();
        }
    }
}

static bool ensure_worker(void)
{
    if (W.task) return true;
    if (!O.vers) O.vers = heap_caps_calloc(MAX_VERS, sizeof(ver_t), MALLOC_CAP_SPIRAM);
    if (!O.man) O.man = heap_caps_calloc(1, sizeof(l5_fw_t), MALLOC_CAP_SPIRAM);
    if (!O.favs) O.favs = heap_caps_calloc(1, sizeof(l5_favs_t), MALLOC_CAP_SPIRAM);
    if (!W.lock) W.lock = xSemaphoreCreateMutex();
    if (!O.vers || !O.man || !O.favs || !W.lock) return false;
    if (l5_board_sd_ready()) l5_favs_load(O.favs);
    return xTaskCreatePinnedToCore(worker, "l5_ota", 10240, NULL, 3, &W.task, 1) == pdPASS;
}

/* ---------- jobs with a progress dialog: install prepare, TO SD, UPDATE ALL ---------- */

typedef struct {
    int kind;                /* 0 install, 1 TO SD, 2 UPDATE ALL */
    char fid[40], ver[24], file[96], name[64], author[48], link[200];
    l5_fw_t *fw;             /* install: the manifest when the side pane had it */
    volatile bool cancel;
    l5_progress_t *progress;
    char text[200];
    bool ok;
    int ndl; /* UPDATE ALL: the downloaded entries, copied when it starts */
    struct {
        char fid[40], ver[24], name[64];
    } dl[MAX_DL];
} job_t;

static void job_progress(job_t *j, int pct, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void job_progress(job_t *j, int pct, const char *fmt, ...)
{
    char t[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    if (l5_board_lock(0)) {
        l5_progress_set(j->progress, pct, t);
        l5_board_unlock();
    }
}

/* getRemoteFileSize(): one byte of the file, so a wrong link shows before anything else happens */
static bool check_url(job_t *j, const char *url)
{
    l5_net_err_t ne = {0};
    l5_net_stream_t *s = NULL;
    if (l5_net_open(url, 0, 1, &j->cancel, &s, NULL, NULL, &ne) == ESP_OK) {
        l5_net_close(s);
        return true;
    }
    strlcpy(j->text, ne.text, sizeof(j->text));
    return false;
}

static esp_err_t fetch_detail_newest(job_t *j, char *ver, char *file, char *name)
{
    char url[128], *json = NULL, author[48];
    size_t len;
    bool star;
    l5_net_err_t ne = {0};
    ver_t *vers = heap_caps_calloc(MAX_VERS, sizeof(ver_t), MALLOC_CAP_SPIRAM);
    snprintf(url, sizeof(url), API "?fid=%s", j->fid);
    esp_err_t err = vers ? l5_net_fetch(url, 32 * 1024, &j->cancel, &json, &len, &ne) : ESP_ERR_NO_MEM;
    int n = err == ESP_OK ? parse_detail(json, vers, name, author, &star) : -1;
    if (n > 0) {
        strlcpy(ver, vers[0].version, 24);
        strlcpy(file, vers[0].file, 96);
    } else {
        strlcpy(j->text, err == ESP_OK ? "The catalog lists no version." : ne.text, sizeof(j->text));
        err = ESP_FAIL;
    }
    heap_caps_free(json);
    heap_caps_free(vers);
    return err;
}

/* downloadFirmware(): the main file to /sdcard/downloads/<name>.bin through a .part file */
static esp_err_t download_to_sd(job_t *j, const char *url, const char *name, int from, int span)
{
    char path[128], part[136];
    char safe[64];
    safe_name(safe, sizeof(safe), name);
    snprintf(path, sizeof(path), DL_DIR "/%s.bin", safe);
    snprintf(part, sizeof(part), "%s.part", path);
    mkdir(DL_DIR, 0777);
    l5_net_stream_t *s = NULL;
    l5_net_err_t ne = {0};
    int64_t length = -1, t0 = esp_timer_get_time(), t_sd = 0;
    /* with ESP-Hosted up, internal RAM may have no 16 KB block left: then PSRAM, cache-line aligned so the SD driver
       can take it (slower, but the file arrives) */
    ESP_LOGI(TAG, "to SD starts: internal RAM %u bytes free, largest block %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    uint8_t *buf = heap_caps_malloc(16 * 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGW(TAG, "to SD: no 16 KB block of internal RAM, the buffer is in PSRAM");
        buf = heap_caps_aligned_alloc(64, 16 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    }
    FILE *f = buf ? fopen(part, "wb") : NULL;
    esp_err_t err = !buf ? ESP_ERR_NO_MEM : !f ? ESP_FAIL : l5_net_open(url, 0, 0, &j->cancel, &s, &length, NULL, &ne);
    if (err != ESP_OK) {
        strlcpy(j->text, ne.text[0] ? ne.text : "The SD card cannot be written.", sizeof(j->text));
    }
    uint64_t got = 0;
    char a[16], b[16];
    while (err == ESP_OK) {
        int r = l5_net_read(s, buf, 16 * 1024, &ne);
        if (r < 0) {
            strlcpy(j->text, ne.text, sizeof(j->text));
            err = ESP_FAIL;
            break;
        }
        if (r == 0) break;
        int64_t tw = esp_timer_get_time();
        if (fwrite(buf, 1, r, f) != (size_t)r) {
            strlcpy(j->text, "Writing the SD card failed (card full?).", sizeof(j->text));
            err = ESP_FAIL;
            break;
        }
        t_sd += esp_timer_get_time() - tw;
        got += r;
        l5_fmt_bytes(a, sizeof(a), got);
        if (length > 0) l5_fmt_bytes(b, sizeof(b), length);
        job_progress(j, from + (length > 0 ? (int)(got * span / length) : 0), "Downloading %s%s%s", a, length > 0 ? " of " : "",
                     length > 0 ? b : "");
    }
    if (s) l5_net_close(s);
    if (f && fclose(f) != 0 && err == ESP_OK) {
        strlcpy(j->text, "Writing the SD card failed.", sizeof(j->text));
        err = ESP_FAIL;
    }
    heap_caps_free(buf);
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "to SD %s: %llu bytes in %lld ms (%llu KB/s), SD writes %lld ms; internal RAM %u bytes free, largest block %u",
             path, (unsigned long long)got, ms, ms > 0 ? (unsigned long long)(got * 1000 / 1024 / ms) : 0ULL, t_sd / 1000,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (err == ESP_OK) {
        unlink(path);
        if (rename(part, path) != 0) {
            strlcpy(j->text, "The downloaded file could not be renamed on the SD card.", sizeof(j->text));
            err = ESP_FAIL;
        } else {
            snprintf(j->text, sizeof(j->text), "/sd/downloads/%s.bin", safe);
        }
    } else {
        unlink(part);
    }
    return err;
}

static void job_end(void *arg);

static void job_task(void *arg)
{
    job_t *j = arg;
    char url[L5_FW_URL_LEN];
    if (j->kind == 0) { /* install: manifest (or the file's own table), a working URL, then the flow */
        if (!j->link[0] && !j->fw) {
            char murl[160], enc[64], *json = NULL;
            size_t len;
            l5_net_err_t ne = {0};
            job_progress(j, 10, "Reading the install data");
            l5_net_urlencode(enc, sizeof(enc), j->ver);
            snprintf(murl, sizeof(murl), API "?fid=%s&version=%s", j->fid, enc);
            j->fw = heap_caps_calloc(1, sizeof(l5_fw_t), MALLOC_CAP_SPIRAM);
            esp_err_t e = j->fw ? l5_net_fetch(murl, 32 * 1024, &j->cancel, &json, &len, &ne) : ESP_ERR_NO_MEM;
            if (e == ESP_OK) {
                l5_fw_init(j->fw, L5_FW_MERGED);
                e = l5_fw_from_manifest(j->fw, json, j->text, sizeof(j->text));
                if (e == ESP_ERR_NOT_FOUND) e = ESP_OK, j->fw->has_table = false; /* probed below */
            } else {
                strlcpy(j->text, ne.text, sizeof(j->text));
            }
            heap_caps_free(json);
            if (e != ESP_OK) goto out;
        }
        if (j->link[0]) {
            strlcpy(url, j->link, sizeof(url));
        } else {
            file_url(url, sizeof(url), j->file);
        }
        job_progress(j, 40, "Checking the download link");
        if (!check_url(j, url)) goto out;
        if (!j->fw || !j->fw->has_table) { /* no install data: the file tells app or merged image */
            uint8_t *head = heap_caps_malloc(L5_FW_PROBE_LEN, MALLOC_CAP_SPIRAM);
            size_t len = 0;
            uint32_t size = 0;
            bool net;
            if (!j->fw) j->fw = heap_caps_calloc(1, sizeof(l5_fw_t), MALLOC_CAP_SPIRAM);
            job_progress(j, 60, "Reading the image header");
            esp_err_t e = head && j->fw ? l5_inst_probe(url, &j->cancel, head, &len, &size, j->text, sizeof(j->text), &net)
                                        : ESP_ERR_NO_MEM;
            if (e == ESP_OK) {
                char keep_ver[32], keep_fid[40];
                strlcpy(keep_ver, j->fw->version, sizeof(keep_ver));
                strlcpy(keep_fid, j->fw->fid, sizeof(keep_fid));
                l5_fw_init(j->fw, L5_FW_APP_ONLY);
                strlcpy(j->fw->version, keep_ver[0] ? keep_ver : j->ver, sizeof(j->fw->version));
                strlcpy(j->fw->fid, keep_fid[0] ? keep_fid : j->fid, sizeof(j->fw->fid));
                e = l5_fw_from_probe(j->fw, head, len, size, j->text, sizeof(j->text));
            }
            heap_caps_free(head);
            if (e != ESP_OK) goto out;
        }
        for (int i = 0; i < j->fw->nsrc; i++) { /* every file straight from the CDN (or the link) */
            char u[L5_FW_URL_LEN];
            if (!strcmp(j->fw->src[i].key, "firmware")) strlcpy(u, url, sizeof(u));
            else file_url(u, sizeof(u), j->fw->src[i].url);
            strlcpy(j->fw->src[i].url, u, sizeof(j->fw->src[i].url));
        }
        if (!j->fw->fid[0]) strlcpy(j->fw->fid, j->fid, sizeof(j->fw->fid));
        if (!j->fw->version[0]) strlcpy(j->fw->version, j->ver, sizeof(j->fw->version));
        j->ok = true;
    } else if (j->kind == 1) { /* TO SD */
        if (j->link[0]) strlcpy(url, j->link, sizeof(url));
        else file_url(url, sizeof(url), j->file);
        job_progress(j, 2, "Checking the download link");
        if (!check_url(j, url)) goto out;
        if (download_to_sd(j, url, j->name, 5, 95) != ESP_OK) goto out;
        if (!j->link[0]) dl_save(j->fid, j->ver);
        j->ok = true;
    } else { /* UPDATE ALL: newer versions of the downloaded files */
        int n = j->ndl, updated = 0, current = 0, failed = 0;
        char upd[96] = "";
        for (int i = 0; i < n && !j->cancel; i++) {
            char ver[24], file[96], name[64];
            strlcpy(j->fid, j->dl[i].fid, sizeof(j->fid));
            job_progress(j, i * 100 / n, "Checking %s", j->dl[i].name);
            if (fetch_detail_newest(j, ver, file, name) != ESP_OK) {
                failed++;
                continue;
            }
            if (!strcmp(ver, j->dl[i].ver)) {
                current++;
                continue;
            }
            file_url(url, sizeof(url), file);
            if (check_url(j, url) && download_to_sd(j, url, name[0] ? name : j->fid, i * 100 / n, 100 / n) == ESP_OK) {
                dl_save(j->fid, ver);
                updated++;
                size_t k = strlen(upd);
                snprintf(upd + k, sizeof(upd) - k, "%s%s %s", k ? ", " : "", name, ver);
            } else {
                failed++;
            }
        }
        snprintf(j->text, sizeof(j->text), "%d updated%s%s, %d already current%s.", updated, updated ? ": " : "", upd, current,
                 failed ? ", some could not be checked or downloaded" : "");
        j->ok = !j->cancel;
    }
out:
    if (j->cancel) strlcpy(j->text, "Cancelled.", sizeof(j->text));
    l5_board_lock(0);
    job_end(j);
    l5_board_unlock();
    vTaskDelete(NULL);
}

static void job_end(void *arg)
{
    job_t *j = arg;
    l5_progress_close(j->progress);
    O.job = false;
    if (j->kind == 0 && j->ok) {
        l5_fw_t *fw = j->fw;
        j->fw = NULL;
        l5_install_fw(fw, j->name, j->author, j->link[0] ? "LINK" : "OTA"); /* takes fw */
    } else if (j->kind == 1 && j->ok) {
        dl_load();
        l5_toast("Saved %s", j->text);
        if (O.list) build_list();
    } else if (j->kind == 2) {
        dl_load();
        l5_info("OTA", j->ok ? "Downloaded firmware checked" : "Update stopped", j->text);
        if (O.list) build_list();
    } else {
        l5_info("OTA", j->kind == 0 ? "Cannot install" : "Download failed", j->text);
    }
    heap_caps_free(j->fw);
    heap_caps_free(j);
}

static void job_start(job_t *j, const char *title)
{
    if (O.job || l5_install_busy()) {
        heap_caps_free(j->fw);
        heap_caps_free(j);
        l5_toast("Wait: a download is running");
        return;
    }
    O.job = true;
    j->progress = l5_progress_open("OTA", title);
    l5_progress_cancelable(j->progress, &j->cancel);
    l5_progress_set(j->progress, 0, "Connecting");
    if (xTaskCreatePinnedToCore(job_task, "l5_ota_job", 10240, j, 3, NULL, 1) != pdPASS) {
        O.job = false;
        l5_progress_close(j->progress);
        heap_caps_free(j->fw);
        heap_caps_free(j);
        l5_info("OTA", "Cannot start", "Not enough memory for the task.");
    }
}

/* the selection as a job: catalog entry (with its selected version) or custom link */
static job_t *job_for_selection(int kind)
{
    job_t *j = heap_caps_calloc(1, sizeof(job_t), MALLOC_CAP_SPIRAM);
    if (!j) return NULL;
    j->kind = kind;
    if (O.sel[0] == '#') {
        const l5_fav_t *f = &O.favs->item[atoi(O.sel + 1)];
        strlcpy(j->name, f->name, sizeof(j->name));
        strlcpy(j->link, f->link, sizeof(j->link));
        return j;
    }
    if (O.ver >= O.nvers || strcmp(O.dfid, O.sel)) {
        heap_caps_free(j);
        return NULL;
    }
    strlcpy(j->fid, O.sel, sizeof(j->fid));
    strlcpy(j->ver, O.vers[O.ver].version, sizeof(j->ver));
    strlcpy(j->file, O.vers[O.ver].file, sizeof(j->file));
    strlcpy(j->name, O.dname[0] ? O.dname : name_of(O.sel) ? name_of(O.sel) : O.sel, sizeof(j->name));
    strlcpy(j->author, O.dauthor, sizeof(j->author));
    if (kind == 0 && O.mfound && !strcmp(O.mver, j->ver)) { /* the manifest the side pane shows */
        j->fw = heap_caps_malloc(sizeof(l5_fw_t), MALLOC_CAP_SPIRAM);
        if (j->fw) memcpy(j->fw, O.man, sizeof(l5_fw_t));
    }
    return j;
}

/* ---------- actions ---------- */

static void install_cb(lv_event_t *e)
{
    (void)e;
    job_t *j = job_for_selection(0);
    char title[96];
    if (!j) {
        l5_toast("Wait for the version list");
        return;
    }
    snprintf(title, sizeof(title), "Preparing %s", j->name);
    job_start(j, title);
}

static void tosd_cb(lv_event_t *e)
{
    (void)e;
    if (!l5_board_sd_ready()) {
        l5_info("OTA", "No SD card", "TO SD saves the firmware to /sd/downloads: insert a card first.");
        return;
    }
    job_t *j = job_for_selection(1);
    char title[96];
    if (!j) {
        l5_toast("Wait for the version list");
        return;
    }
    snprintf(title, sizeof(title), "Downloading %s", j->name);
    job_start(j, title);
}

static void save_favs(const char *msg)
{
    if (!l5_board_sd_ready() || l5_favs_save(O.favs) != ESP_OK) {
        l5_toast("Favorites not saved: no SD card");
        return;
    }
    l5_toast("%s", msg);
}

static void fav_cb(lv_event_t *e)
{
    (void)e;
    if (O.sel[0] == '#') { /* custom link: remove */
        int i = atoi(O.sel + 1);
        memmove(&O.favs->item[i], &O.favs->item[i + 1], (O.favs->n - i - 1) * sizeof(l5_fav_t));
        O.favs->n--;
        O.sel[0] = 0;
        save_favs("Deleted from Favorites");
    } else if (fav_index(O.sel) >= 0) {
        int i = fav_index(O.sel);
        memmove(&O.favs->item[i], &O.favs->item[i + 1], (O.favs->n - i - 1) * sizeof(l5_fav_t));
        O.favs->n--;
        save_favs("Deleted from Favorites");
    } else if (O.favs->n < L5_FAV_MAX) {
        l5_fav_t *f = &O.favs->item[O.favs->n++];
        memset(f, 0, sizeof(*f));
        strlcpy(f->fid, O.sel, sizeof(f->fid));
        strlcpy(f->name, O.dname[0] ? O.dname : name_of(O.sel) ? name_of(O.sel) : O.sel, sizeof(f->name));
        save_favs("Added to Favorites");
    } else {
        l5_toast("Favorites are full (%d)", L5_FAV_MAX);
    }
    l5_ui_render_views_async(L5_VIEW_BIT(L5_VIEW_OTA));
}

static char s_link_name[48];

static void link_url_cb(const char *text, void *user)
{
    (void)user;
    if (!text) return;
    if (strncmp(text, "https://", 8) || strlen(text) < 12 || strchr(text, ' ')) {
        l5_info("OTA", "Not a link", "The link must start with https:// and point to a .bin file.");
        return;
    }
    if (O.favs->n >= L5_FAV_MAX) {
        l5_toast("Favorites are full (%d)", L5_FAV_MAX);
        return;
    }
    l5_fav_t *f = &O.favs->item[O.favs->n++];
    memset(f, 0, sizeof(*f));
    strlcpy(f->name, s_link_name, sizeof(f->name));
    strlcpy(f->link, text, sizeof(f->link));
    snprintf(O.sel, sizeof(O.sel), "#%d", O.favs->n - 1);
    O.tab = TAB_FAV;
    save_favs("Link added to Favorites");
    l5_ui_render_views_async(L5_VIEW_BIT(L5_VIEW_OTA));
}

static void link_name_cb(const char *text, void *user)
{
    (void)user;
    if (!text || !text[0]) return;
    strlcpy(s_link_name, text, sizeof(s_link_name));
    const l5_keyboard_opts_t o = {.title = "BINARY LINK (https://\xE2\x80\xA6/firmware.bin)", .value = "https://", .max_len = 199};
    l5_keyboard(&o, link_url_cb, NULL);
}

static void addlink_cb(lv_event_t *e)
{
    (void)e;
    const l5_keyboard_opts_t o = {.title = "FAVORITE NAME", .value = "My firmware", .max_len = 40};
    l5_keyboard(&o, link_name_cb, NULL);
}

static void updall_cb(lv_event_t *e)
{
    (void)e;
    job_t *j = heap_caps_calloc(1, sizeof(job_t), MALLOC_CAP_SPIRAM);
    if (!j || !O.ndl) {
        heap_caps_free(j);
        return;
    }
    j->kind = 2;
    j->ndl = O.ndl;
    for (int i = 0; i < O.ndl; i++) {
        strlcpy(j->dl[i].fid, O.dl[i].fid, sizeof(j->dl[i].fid));
        strlcpy(j->dl[i].ver, O.dl[i].ver, sizeof(j->dl[i].ver));
        strlcpy(j->dl[i].name, name_of(O.dl[i].fid) ? name_of(O.dl[i].fid) : O.dl[i].fid, sizeof(j->dl[i].name));
    }
    job_start(j, "Updating downloaded firmware");
}

static void search_done_cb(const char *text, void *user)
{
    (void)user;
    if (!text) return;
    char q[49];
    strlcpy(q, text, sizeof(q));
    for (char *c = q + strlen(q); c > q && c[-1] == ' '; c--) c[-1] = 0; /* trim */
    char *b = q;
    while (*b == ' ') b++;
    if (!strcmp(b, O.q)) return;
    strlcpy(O.q, b, sizeof(O.q));
    O.page = 1;
    invalidate();
    O.sel[0] = 0;
    l5_ui_render_views_async(L5_VIEW_BIT(L5_VIEW_OTA));
}

static void search_cb(lv_event_t *e)
{
    (void)e;
    const l5_keyboard_opts_t o = {.title = "SEARCH FIRMWARE", .value = O.q, .max_len = 48};
    l5_keyboard(&o, search_done_cb, NULL);
}

static void clear_cb(lv_event_t *e)
{
    (void)e;
    O.q[0] = 0;
    O.page = 1;
    invalidate();
    O.sel[0] = 0;
    l5_ui_render_views_async(L5_VIEW_BIT(L5_VIEW_OTA));
}

static void sort_cb(lv_event_t *e)
{
    (void)e;
    O.by_date = !O.by_date;
    O.page = 1;
    invalidate();
    l5_ui_render_views_async(L5_VIEW_BIT(L5_VIEW_OTA));
}

static void tab_cb(lv_event_t *e)
{
    tab_t t = (tab_t)(intptr_t)lv_event_get_user_data(e);
    if (t == O.tab) return;
    O.tab = t;
    O.page = 1;
    O.sel[0] = 0;
    l5_ui_render_views_async(L5_VIEW_BIT(L5_VIEW_OTA));
}

static void page_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int p = O.page + d;
    if (p < 1 || p > O.pages) return;
    O.page = p;
    invalidate();
    O.sel[0] = 0;
    l5_ui_render_views_async(L5_VIEW_BIT(L5_VIEW_OTA));
}

static void retry_cb(lv_event_t *e)
{
    (void)e;
    invalidate();
    l5_ui_render_views_async(L5_VIEW_BIT(L5_VIEW_OTA));
}

static void ver_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    if (v == O.ver || v >= O.nvers) return;
    O.ver = v;
    request_manifest();
    build_side();
}

static void select_key(const char *key)
{
    strlcpy(O.sel, key, sizeof(O.sel));
    O.ver = 0;
    if (key[0] == '#') return;
    if (strcmp(O.dfid, key)) request_detail(key);
    else if (O.nvers && strcmp(O.mver, O.vers[0].version)) request_manifest();
}

static void row_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_current_target(e);
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!O.list || !strcmp(O.sel, O.keys[i])) return;
    select_key(O.keys[i]);
    uint32_t n = lv_obj_get_child_count(O.list);
    for (uint32_t k = 0; k < n; k++) lv_obj_remove_state(lv_obj_get_child(O.list, k), LV_STATE_CHECKED);
    lv_obj_add_state(row, LV_STATE_CHECKED);
    build_side();
}

/* the keyboard's selection: the row as a tap marks it, scrolled into view */
static void select_row(int i)
{
    if (!O.list || i < 0 || i >= O.nrows) return;
    if (strcmp(O.sel, O.keys[i])) select_key(O.keys[i]);
    for (int k = 0; k < O.nrows; k++) lv_obj_set_state(lv_obj_get_child(O.list, k), LV_STATE_CHECKED, k == i);
    lv_obj_scroll_to_view(lv_obj_get_child(O.list, i), LV_ANIM_OFF);
    build_side();
}

static void wifi_cb(lv_event_t *e)
{
    (void)e;
    l5_wifi_pick();
}

static void why_cb(lv_event_t *e)
{
    (void)e;
    l5_wifi_why();
}

/* ---------- main pane ---------- */

static lv_obj_t *bar(lv_obj_t *m)
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
    return b;
}

/* initials on a colour from the name (the prototype's cover seed) */
static void badge(lv_obj_t *parent, const char *name)
{
    uint32_t h = 2166136261u;
    for (const char *c = name; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    lv_obj_t *b = l5_box(parent);
    lv_obj_set_size(b, 48, 48);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, l5_mix(lv_color_hex(l5_colors[h % l5_color_count].hex), L5_PANEL_2, 110), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, L5_PANEL_LINE, 0);
    char ini[3] = {0};
    int n = 0;
    bool word = false;
    for (const char *c = name; *c && n < 2; c++) {
        bool an = isalnum((unsigned char)*c);
        if (an && !word) ini[n++] = (char)toupper((unsigned char)*c);
        word = an;
    }
    lv_obj_center(l5_label(b, &l5_chakra_b24, L5_WHITE, n ? ini : "?"));
}

static void row(int i, const char *key, const char *name, const char *sub, const char *meta, bool star, bool fav, bool upd)
{
    strlcpy(O.keys[i], key, sizeof(O.keys[i]));
    lv_obj_t *r = lv_button_create(O.list); /* .orow */
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), 74);
    lv_obj_set_style_pad_hor(r, 18, 0);
    lv_obj_set_style_pad_column(r, 14, 0);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, L5_PANEL_LINE, 0);
    lv_obj_set_style_bg_color(r, L5P.accent, LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(r, 56, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(r, L5_PANEL_3, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(r, l5w_row_sel_draw, LV_EVENT_DRAW_POST, NULL);
    lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    if (!strcmp(key, O.sel)) lv_obj_add_state(r, LV_STATE_CHECKED);
    badge(r, name);
    lv_obj_t *col = l5_box(r);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_style_pad_row(col, 4, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *nr = l5_box(col);
    lv_obj_set_size(nr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(nr, 8, 0);
    lv_obj_set_flex_flow(nr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *nm = l5w_line_label(nr, &l5_chakra_sb19, L5_ON_PANEL, name);
    lv_obj_set_style_max_width(nm, 500, 0);
    if (star) l5_icon(nr, &l5_icon_20, L5_WARN, L5_ICON_STAR);
    if (fav) l5_icon(nr, &l5_icon_20, L5P.accent, L5_ICON_HEART);
    lv_obj_t *s = l5w_line_label(col, &l5_mono_14, L5_ON_PANEL_MUTED, sub);
    lv_obj_set_width(s, lv_pct(100));
    lv_obj_t *mt = l5_label(r, &l5_mono_13, upd ? L5P.accent : L5_ON_PANEL_MUTED, meta);
    lv_obj_set_style_text_align(mt, LV_TEXT_ALIGN_RIGHT, 0);
}

static void foot(lv_obj_t *m, int shown)
{
    lv_obj_t *f = l5_box(m); /* .foot */
    lv_obj_set_size(f, lv_pct(100), 50);
    lv_obj_set_style_pad_hor(f, 18, 0);
    lv_obj_set_style_pad_column(f, 10, 0);
    lv_obj_set_style_border_side(f, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(f, 1, 0);
    lv_obj_set_style_border_color(f, L5_PANEL_LINE, 0);
    lv_obj_set_style_text_font(f, &l5_mono_13, 0);
    lv_obj_set_style_text_letter_space(f, 1, 0);
    lv_obj_set_style_text_color(f, L5_ON_PANEL_MUTED, 0);
    lv_obj_set_flex_flow(f, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(f, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *l = lv_label_create(f);
    bool catalog = O.tab == TAB_ALL || O.tab == TAB_STAR;
    if (catalog) lv_label_set_text_fmt(l, "PAGE %d/%d \xC2\xB7 %d FIRMWARE", O.page, O.pages ? O.pages : 1, O.total);
    else lv_label_set_text_fmt(l, "%d FIRMWARE", shown);
    lv_obj_t *sp = l5_box(f);
    lv_obj_set_height(sp, 1);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_t *b;
    if (catalog && O.pages > 1) {
        b = l5w_chip(f, L5_ICON_CHEV_L, "", false, page_cb);
        lv_obj_set_height(b, 38);
        lv_obj_remove_event_cb(b, page_cb);
        lv_obj_add_event_cb(b, page_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
        if (O.page <= 1) lv_obj_add_state(b, LV_STATE_DISABLED);
        b = l5w_chip(f, L5_ICON_CHEV, "", false, page_cb);
        lv_obj_set_height(b, 38);
        lv_obj_remove_event_cb(b, page_cb);
        lv_obj_add_event_cb(b, page_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
        if (O.page >= O.pages) lv_obj_add_state(b, LV_STATE_DISABLED);
    }
    if (O.tab == TAB_DL) {
        b = l5w_chip(f, L5_ICON_RESTART, "[UPDATE ALL]", O.ndl > 0, updall_cb);
        lv_obj_set_height(b, 38);
        if (!O.ndl) lv_obj_add_state(b, LV_STATE_DISABLED);
    }
    if (O.tab == TAB_FAV) {
        b = l5w_chip(f, L5_ICON_LINK, "ADD LINK", false, addlink_cb);
        lv_obj_set_height(b, 38);
    }
}

/* the rows of the current tab, filtered by the search text where the server does not do it */
static void build_list(void)
{
    if (!O.list) return;
    lv_obj_clean(O.list);
    char sub[160], meta[48];
    int n = 0;
    lv_obj_t *sel = NULL;
    if (O.tab == TAB_ALL || O.tab == TAB_STAR) {
        for (int i = 0; O.loaded && O.list_star == (O.tab == TAB_STAR) && i < O.nitems && n < MAX_ROWS; i++) {
            const item_t *x = &O.items[i];
            const char *dv = dl_version(x->fid);
            snprintf(sub, sizeof(sub), "%s%s%s", x->author[0] ? x->author : "unknown author", dv ? " \xC2\xB7 on SD " : "",
                     dv ? dv : "");
            row(n++, x->fid, x->name, sub, "", x->star, fav_index(x->fid) >= 0, false);
        }
    } else if (O.tab == TAB_FAV) {
        for (int k = 0; k < O.favs->n && n < MAX_ROWS; k++) {
            int i = O.by_date ? O.favs->n - 1 - k : k; /* LATEST: the last added first */
            const l5_fav_t *f = &O.favs->item[i];
            char key[40];
            if (O.q[0] && !contains_ci(f->name, O.q)) continue;
            if (f->fid[0]) snprintf(key, sizeof(key), "%s", f->fid);
            else snprintf(key, sizeof(key), "#%d", i);
            const char *dv = f->fid[0] ? dl_version(f->fid) : NULL;
            snprintf(sub, sizeof(sub), "%s%s%s", f->fid[0] ? "catalog" : f->link, dv ? " \xC2\xB7 on SD " : "", dv ? dv : "");
            row(n++, key, f->name, sub, f->fid[0] ? "" : "LINK", false, true, false);
        }
    } else {
        for (int k = 0; k < O.ndl && n < MAX_ROWS; k++) {
            int i = O.by_date ? O.ndl - 1 - k : k;
            const char *nm = name_of(O.dl[i].fid);
            if (O.q[0] && !contains_ci(nm ? nm : O.dl[i].fid, O.q)) continue;
            snprintf(sub, sizeof(sub), "on SD %s \xC2\xB7 /sd/downloads", O.dl[i].ver);
            snprintf(meta, sizeof(meta), "%s", "DOWNLOADED");
            row(n++, O.dl[i].fid, nm ? nm : O.dl[i].fid, sub, meta, false, fav_index(O.dl[i].fid) >= 0, false);
        }
    }
    O.nrows = n;
    for (int i = 0; i < n; i++) {
        if (!strcmp(O.keys[i], O.sel)) sel = lv_obj_get_child(O.list, i);
    }
    if (!n) {
        const char *t = "Nothing here", *b = "No firmware matches the search.";
        if (O.tab <= TAB_STAR && O.loading) t = "Fetching the firmware list\xE2\x80\xA6", b = "launcherhub \xC2\xB7 category tab5";
        else if (O.tab <= TAB_STAR && O.error[0]) t = "Catalog not available", b = O.error;
        else if (O.tab == TAB_FAV && !O.q[0]) b = "Add firmware with the heart button, or add your own binary link.";
        else if (O.tab == TAB_DL && !O.q[0]) b = "Firmware you save with TO SD shows up here.";
        l5w_empty(O.list, t, b);
        if (O.tab <= TAB_STAR && O.error[0] && !O.loading) {
            lv_obj_t *r = l5_act_button(lv_obj_get_child(O.list, 0), L5_ICON_RESTART, "TRY AGAIN", true);
            lv_obj_set_size(r, 260, 56);
            lv_obj_add_event_cb(r, retry_cb, LV_EVENT_CLICKED, NULL);
        }
    }
    lv_obj_t *f = lv_obj_get_child(O.main, -1); /* the foot follows the list */
    if (f) lv_obj_delete(f);
    foot(O.main, n);
    if (sel) {
        lv_obj_update_layout(O.list);
        lv_obj_scroll_to_view(sel, LV_ANIM_OFF);
    }
}

/* ---------- side pane ---------- */

static void acts_row(lv_obj_t *s, bool can_install, bool link)
{
    lv_obj_t *grow = l5_box(s); /* .acts { margin-top: auto } */
    lv_obj_set_width(grow, lv_pct(100));
    lv_obj_set_flex_grow(grow, 1);
    lv_obj_t *acts = l5_box(s);
    lv_obj_set_size(acts, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(acts, 22, 0);
    lv_obj_set_style_pad_bottom(acts, 17, 0);
    lv_obj_set_style_pad_row(acts, 10, 0);
    lv_obj_set_style_pad_column(acts, 10, 0);
    lv_obj_set_flex_flow(acts, LV_FLEX_FLOW_ROW_WRAP);
    char t[48];
    if (link || O.ver >= O.nvers) snprintf(t, sizeof(t), "INSTALL");
    else snprintf(t, sizeof(t), "INSTALL %s", O.vers[O.ver].version);
    lv_obj_t *b = l5_act_button(acts, L5_ICON_INSTALL, t, true);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_add_event_cb(b, install_cb, LV_EVENT_CLICKED, NULL);
    if (!can_install) lv_obj_add_state(b, LV_STATE_DISABLED);
    b = l5_act_button(acts, L5_ICON_INSTALL, "TO SD", false);
    lv_obj_set_width(b, 186);
    lv_obj_add_event_cb(b, tosd_cb, LV_EVENT_CLICKED, NULL);
    if (!can_install) lv_obj_add_state(b, LV_STATE_DISABLED);
    b = l5_act_button(acts, link ? L5_ICON_TRASH : L5_ICON_HEART, link ? "REMOVE" : fav_index(O.sel) >= 0 ? "UNFAVORITE" : "FAVORITE",
                      false);
    lv_obj_set_width(b, 186);
    lv_obj_add_event_cb(b, fav_cb, LV_EVENT_CLICKED, NULL);
}

static void build_side(void)
{
    lv_obj_t *s = O.side;
    if (!s) return;
    lv_obj_clean(s);
    char t[200], a[16];
    if (!O.sel[0]) {
        lv_obj_t *h = l5w_side_head(s);
        l5w_big_icon(h, L5_ICON_OTA);
        l5w_head_text(h, "OTA UPDATE", "Online installer", "Pick a firmware on the left.");
        lv_obj_t *g = l5w_kv_grid(s);
        l5w_kv(g, "SOURCE", "launcherhub \xC2\xB7 M5Burner CDN", false);
        l5w_kv(g, "DEVICE", "tab5", false);
        l5w_kv(g, "PAGING", "100 per page", false);
        return;
    }
    if (O.sel[0] == '#') { /* custom link */
        const l5_fav_t *f = &O.favs->item[atoi(O.sel + 1)];
        lv_obj_t *h = l5w_side_head(s);
        l5w_big_icon(h, L5_ICON_WEB);
        l5w_head_text(h, "CUSTOM LINK", f->name, "your own binary link");
        lv_obj_t *g = l5w_kv_grid(s);
        l5w_kv(g, "LINK", f->link, false);
        l5w_kv(g, "FILES", "checked when installing", false);
        acts_row(s, true, true);
        return;
    }
    const item_t *it = item_of(O.sel);
    const char *name = O.dname[0] && !strcmp(O.dfid, O.sel) ? O.dname : it ? it->name : name_of(O.sel) ? name_of(O.sel) : O.sel;
    const char *author = !strcmp(O.dfid, O.sel) && O.dauthor[0] ? O.dauthor : it ? it->author : "";
    bool star = !strcmp(O.dfid, O.sel) ? O.dstar : it && it->star;
    lv_obj_t *h = l5w_side_head(s);
    l5_cover_create(h, 132, name, name, author, 0, true);
    snprintf(t, sizeof(t), "CATALOG%s", star ? " \xC2\xB7 STARRED" : "");
    l5w_head_text(h, t, name, author);
    lv_obj_t *g = l5w_kv_grid(s);
    bool vers_ok = !strcmp(O.dfid, O.sel) && !O.dloading && O.nvers > 0;
    if (O.dloading) {
        l5w_kv(g, "VERSIONS", "loading\xE2\x80\xA6", false);
    } else if (O.derror[0]) {
        l5w_kv(g, "CATALOG", O.derror, true);
    } else if (!O.nvers) {
        l5w_kv(g, "VERSIONS", "none listed", true);
    }
    if (vers_ok) {
        const ver_t *v = &O.vers[O.ver];
        l5w_kv(g, "VERSION", v->version, false);
        if (v->date[0]) l5w_kv(g, "PUBLISHED", v->date, false);
        if (O.mloading) {
            l5w_kv(g, "FILES", "loading\xE2\x80\xA6", false);
        } else if (O.mfound && !strcmp(O.mver, v->version)) {
            const l5_fw_t *fw = O.man;
            if (fw->app_size) {
                l5_fmt_bytes(a, sizeof(a), fw->app_size);
                bool fits = !g_slots.n || (uint64_t)fw->app_size <= (uint64_t)g_slots.slot_kb * 1024;
                snprintf(t, sizeof(t), "%s%s", a, fits ? "" : " \xC2\xB7 larger than an app slot");
                l5w_kv(g, "APP", t, !fits);
            }
            size_t n = 0;
            t[0] = 0;
            for (int i = 0; i < fw->nparts && n < sizeof(t) - 40; i++) {
                const l5_fw_part_t *p = &fw->part[i];
                if (l5_fw_system_part(p)) continue;
                l5_fmt_bytes(a, sizeof(a), p->size);
                n += snprintf(t + n, sizeof(t) - n, "%s%s %s %s%s", n ? " \xC2\xB7 " : "", p->label, l5_fw_subtype_name(p->subtype), a,
                              p->copy_size ? "+" : "");
            }
            if (t[0]) l5w_kv(g, "DATA", t, false); /* "+": the firmware brings an image for it */
            l5w_kv(g, "FILES", fw->format == L5_FW_MULTI ? "app + separate data files" : fw->format == L5_FW_MERGED ? "merged image"
                                                                                                                       : "app image", false);
        } else if (O.merror[0]) {
            l5w_kv(g, "FILES", O.merror, true);
        } else {
            l5w_kv(g, "FILES", "checked when installing", false);
        }
    }
    const char *dv = dl_version(O.sel);
    if (dv) l5w_kv(g, "ON SD", dv, false);
    size_t n = 0;
    t[0] = 0;
    for (int i = 0; i < g_slots.n && n < sizeof(t) - 24; i++) {
        if (g_slots.slot[i].used && !strcmp(g_slots.slot[i].fid, O.sel)) {
            n += snprintf(t + n, sizeof(t) - n, "%sapp%d %s", n ? ", " : "", i, g_slots.slot[i].cver);
        }
    }
    if (t[0]) l5w_kv(g, "IN FLASH", t, false);
    if (vers_ok && O.nvers > 1) { /* .vers */
        lv_obj_t *vr = l5_box(s);
        lv_obj_set_size(vr, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_max_height(vr, 110, 0);
        lv_obj_add_flag(vr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(vr, LV_DIR_VER);
        lv_obj_set_style_pad_top(vr, 14, 0);
        lv_obj_set_style_pad_hor(vr, 22, 0);
        lv_obj_set_style_pad_row(vr, 8, 0);
        lv_obj_set_style_pad_column(vr, 8, 0);
        lv_obj_set_flex_flow(vr, LV_FLEX_FLOW_ROW_WRAP);
        for (int i = 0; i < O.nvers; i++) {
            lv_obj_t *c = l5w_chip(vr, NULL, O.vers[i].version, i == O.ver, ver_cb);
            lv_obj_set_height(c, 40);
            lv_obj_remove_event_cb(c, ver_cb);
            lv_obj_add_event_cb(c, ver_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    }
    acts_row(s, vers_ok, false);
}

/* ---------- view ---------- */

static void main_deleted_cb(lv_event_t *e)
{
    (void)e;
    O.main = O.list = O.side = NULL;
    O.nrows = 0;
}

static void no_wifi(lv_obj_t *m, lv_obj_t *s, const l5_wifi_status_t *st)
{
    lv_obj_t *b = bar(m);
    l5_dot(b);
    lv_obj_t *l = l5w_line_label(b, &l5_mono_16, L5_ON_PANEL, "CONNECT TO WI-FI");
    lv_obj_set_flex_grow(l, 1);
    l5w_chip(b, L5_ICON_WIFI, "WI-FI", false, wifi_cb);
    lv_obj_t *box = l5_box(m);
    lv_obj_set_width(box, lv_pct(100));
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    char t[96];
    l5w_empty(box, st->state == L5_WIFI_CONNECTING || st->state == L5_WIFI_STARTING ? "Connecting\xE2\x80\xA6" : "No Wi-Fi connection",
              l5_wifi_summary(t, sizeof(t)));
    if (st->state == L5_WIFI_FAILED) { /* e.g. a C6 with the factory ESP-Hosted 1.4.1: what to flash */
        lv_obj_t *c = l5_act_button(box, L5_ICON_WARN, "WHY NO WI-FI", false);
        lv_obj_set_size(c, 260, 56);
        lv_obj_add_event_cb(c, why_cb, LV_EVENT_CLICKED, NULL);
    } else if (st->state != L5_WIFI_CONNECTING && st->state != L5_WIFI_STARTING) {
        lv_obj_t *c = l5_act_button(box, L5_ICON_WIFI, "CONNECT", true);
        lv_obj_set_size(c, 260, 56);
        lv_obj_add_event_cb(c, wifi_cb, LV_EVENT_CLICKED, NULL);
    }
    lv_obj_t *h = l5w_side_head(s);
    l5w_big_icon(h, L5_ICON_OTA);
    l5w_head_text(h, "OTA UPDATE", "Online installer", "Needs internet");
    lv_obj_t *g = l5w_kv_grid(s);
    l5w_kv(g, "SOURCES", "launcherhub catalog \xC2\xB7 GitHub and other https links \xC2\xB7 Favorites", false);
    l5w_kv(g, "DEVICE", "tab5", false);
    l5w_kv(g, "PAGING", "100 per page", false);
    l5w_kv(g, "OFFLINE", "Files saved with TO SD install from the SD tab.", false);
}

/* keys of the Tab5 Keyboard (l5_keys.c). The list: Up / Down, Home / End, PgUp / PgDn by 6 rows move the selection,
   Enter = INSTALL, / or Ctrl+F = search. Without Wi-Fi: Enter = CONNECT (WHY NO WI-FI after a failure, nothing while
   it connects). */
bool l5_ota_key(const l5_key_t *k)
{
    if (!O.list) {
        if (k->kind != L5_KEY_ENTER) return false;
        l5_wifi_status_t st;
        l5_wifi_get_status(&st);
        if (st.state == L5_WIFI_FAILED) {
            l5_wifi_why();
        } else if (st.state != L5_WIFI_STARTING && st.state != L5_WIFI_CONNECTING) {
            l5_wifi_pick();
        }
        return true;
    }
    int i = -1, last = O.nrows - 1;
    for (int r = 0; r < O.nrows; r++) {
        if (!strcmp(O.keys[r], O.sel)) i = r;
    }
    switch (k->kind) {
    case L5_KEY_UP: select_row(i < 0 ? last : i > 0 ? i - 1 : 0); return true;
    case L5_KEY_DOWN: select_row(i < 0 ? 0 : i < last ? i + 1 : last); return true;
    case L5_KEY_HOME: select_row(0); return true;
    case L5_KEY_END: select_row(last); return true;
    case L5_KEY_PGUP: select_row(i > 6 ? i - 6 : 0); return true;
    case L5_KEY_PGDN: select_row(i < 0 ? 0 : i + 6 < last ? i + 6 : last); return true;
    case L5_KEY_ENTER:
        if (i < 0) return false;
        install_cb(NULL);
        return true;
    case L5_KEY_CHAR:
        if (k->ch != '/' && !(k->ctrl && (k->ch | 0x20) == 'f')) return false;
        search_cb(NULL);
        return true;
    default: return false;
    }
}

static bool s_connected;

void l5_ota_paint_wifi(void)
{
    l5_wifi_status_t st;
    l5_wifi_get_status(&st);
    bool c = st.state == L5_WIFI_CONNECTED;
    if (O.main && (c != s_connected || !c)) l5_ui_render_views_async(L5_VIEW_BIT(L5_VIEW_OTA));
    s_connected = c;
}

void l5_ota_render(lv_obj_t *view)
{
    lv_obj_clean(view);
    lv_obj_t *m = l5w_pane(view, true, L5_GAP, L5_MAIN_W);
    lv_obj_t *s = l5w_pane(view, false, L5_SIDE_X, L5_SIDE_W);
    O.main = m;
    O.side = s;
    O.list = NULL;
    lv_obj_add_event_cb(m, main_deleted_cb, LV_EVENT_DELETE, NULL);
    l5_wifi_status_t st;
    l5_wifi_get_status(&st);
    s_connected = st.state == L5_WIFI_CONNECTED;
    if (!s_connected) {
        no_wifi(m, s, &st);
        return;
    }
    if (!ensure_worker()) {
        l5w_empty(m, "Not enough memory", "The online installer could not start.");
        return;
    }
    if (l5_board_sd_ready()) dl_load();
    static const struct {
        const char *label, *icon;
    } T[] = {{"ALL", L5_ICON_LIST}, {"STARRED", L5_ICON_STAR}, {"FAVORITES", L5_ICON_HEART}, {"DOWNLOADED", L5_ICON_INSTALL}};
    lv_obj_t *b = bar(m);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *c = l5w_chip(b, T[i].icon, T[i].label, O.tab == (tab_t)i, tab_cb);
        lv_obj_remove_event_cb(c, tab_cb);
        lv_obj_add_event_cb(c, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    b = bar(m); /* search + sort */
    lv_obj_t *sr = l5w_chip(b, L5_ICON_SEARCH, O.q[0] ? O.q : "Search name, author", O.q[0] != 0, search_cb);
    lv_obj_set_flex_grow(sr, 1);
    lv_obj_set_flex_align(sr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_label_set_long_mode(lv_obj_get_child(sr, -1), LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(lv_obj_get_child(sr, -1), 1);
    if (O.q[0]) l5w_chip(b, L5_ICON_X, "", false, clear_cb);
    l5w_chip(b, L5_ICON_SORT, O.by_date ? "LATEST" : "A\xE2\x80\x93Z", false, sort_cb);
    O.list = l5_box(m); /* .scroll */
    lv_obj_set_width(O.list, lv_pct(100));
    lv_obj_set_flex_grow(O.list, 1);
    lv_obj_set_flex_flow(O.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(O.list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(O.list, LV_DIR_VER);
    lv_obj_set_style_bg_opa(O.list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(O.list, L5P.accent, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(O.list, 5, LV_PART_SCROLLBAR);
    l5_box(m); /* the foot's place: build_list replaces the last child */
    bool catalog = O.tab == TAB_ALL || O.tab == TAB_STAR;
    if (catalog && (O.list_star != (O.tab == TAB_STAR) || (!O.loaded && !O.loading))) request_list();
    build_list();
    if (O.sel[0] && O.sel[0] != '#' && strcmp(O.dfid, O.sel)) request_detail(O.sel);
    build_side();
}
