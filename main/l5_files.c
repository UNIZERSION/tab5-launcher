#include "l5_files.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#ifdef ESP_PLATFORM
#include "esp_vfs_fat.h"
#include "ff.h"
#include "l5_board.h"
#endif

#ifndef O_BINARY
#define O_BINARY 0 /* the host test on Windows: no CR LF translation */
#endif

static const char *TAG = "l5_files";

#define MAX_DEPTH 48          /* folders inside folders: deeper trees are refused, not recursed */
#define COPY_BLOCK (64 * 1024)
#define TEXT_OUT_MAX (192 * 1024) /* tabs widen the text: the viewer keeps at most this much */

#ifdef ESP_PLATFORM
const char *l5_files_root = CONFIG_BSP_SD_MOUNT_POINT;
#else
const char *l5_files_root = "/sdcard"; /* the host test points it at its own folder */
uint64_t l5_files_host_total, l5_files_host_free;
uint32_t l5_files_host_cluster = 32768;
long l5_files_host_write_budget = -1; /* bytes until writes come back short, as on a full card */
#endif

/* names and listings go to PSRAM on the Tab5: a big folder would take tens of KB of internal RAM */
static void *ps_alloc(size_t n)
{
#ifdef ESP_PLATFORM
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
#else
    return malloc(n);
#endif
}

static void *ps_realloc(void *p, size_t n)
{
#ifdef ESP_PLATFORM
    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM);
    return q ? q : realloc(p, n);
#else
    return realloc(p, n);
#endif
}

/* A buffer the SD driver reads and writes directly: cache-line aligned PSRAM (an unaligned buffer
   goes through a 512-byte bounce buffer, one sector per transfer). */
static uint8_t *io_alloc(size_t want, size_t *got)
{
#ifdef ESP_PLATFORM
    size_t n = (want + 127) & ~(size_t)127;
    uint8_t *b = heap_caps_aligned_alloc(128, n, MALLOC_CAP_SPIRAM);
    if (!b) {
        n = 16 * 1024 < n ? 16 * 1024 : n;
        b = heap_caps_aligned_alloc(64, n, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    *got = b ? n : 0;
    return b;
#else
    uint8_t *b = malloc(want);
    *got = b ? want : 0;
    return b;
#endif
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static bool cancelled(const l5_files_job_t *job)
{
    return job && job->cancel && *job->cancel;
}

/* ---------- UTF-8 and names ---------- */

/* next code point, -1 for a broken byte (one byte consumed) */
static int32_t utf8_get(const uint8_t **p, const uint8_t *end)
{
    const uint8_t *s = *p;
    uint32_t c = *s++;
    int n;
    uint32_t min;
    if (c < 0x80) {
        *p = s;
        return (int32_t)c;
    } else if (c >= 0xC2 && c <= 0xDF) {
        n = 1, c &= 0x1F, min = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
        n = 2, c &= 0x0F, min = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
        n = 3, c &= 0x07, min = 0x10000;
    } else {
        *p = s;
        return -1;
    }
    if (end - s < n) {
        *p = s;
        return -1;
    }
    for (int i = 0; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *p = s;
            return -1;
        }
        c = c << 6 | (s[i] & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
        *p = s;
        return -1;
    }
    *p = s + n;
    return (int32_t)c;
}

/* upper case as FatFs compares names */
static uint32_t fold(uint32_t c)
{
#ifdef ESP_PLATFORM
    return c <= 0xFFFF ? ff_wtoupper(c) : c;
#else /* ASCII, Latin-1 and Latin Extended-A are enough for the host test */
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x131 || c == 0x138 || c == 0x149 || c == 0x17F) return c;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return c & 1 ? c : c - 1;
        return c & ~1u;
    }
    return c;
#endif
}

/* compares the first n bytes of a with b folded; returns bytes of a used, or 0 when they differ */
static size_t prefix_folded(const char *a, const char *b)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    const uint8_t *xe = x + strlen(a), *ye = y + strlen(b);
    while (y < ye) {
        if (x >= xe) return 0;
        int32_t cx = utf8_get(&x, xe), cy = utf8_get(&y, ye);
        if (cx < 0 || cy < 0 ? cx != cy : fold((uint32_t)cx) != fold((uint32_t)cy)) return 0;
    }
    return (size_t)(x - (const uint8_t *)a);
}

bool l5_files_starts_with(const char *name, const char *prefix)
{
    return !*prefix || prefix_folded(name, prefix) > 0;
}

bool l5_files_same_name(const char *a, const char *b)
{
    size_t n = prefix_folded(a, b);
    return (n || !*b) && a[n] == 0;
}

size_t l5_files_utf8(char *out, uint32_t c)
{
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xC0 | c >> 6);
        out[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | c >> 12);
        out[1] = (char)(0x80 | (c >> 6 & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | c >> 18);
    out[1] = (char)(0x80 | (c >> 12 & 0x3F));
    out[2] = (char)(0x80 | (c >> 6 & 0x3F));
    out[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

void l5_files_upper(char *dst, size_t size, const char *src)
{
    if (!size) return;
    const uint8_t *s = (const uint8_t *)src, *e = s + strlen(src);
    size_t n = 0;
    while (s < e) {
        const uint8_t *at = s;
        const int32_t c = utf8_get(&s, e);
        char b[4];
        size_t k = 1;
        if (c < 0) b[0] = (char)*at; /* a broken byte stays as it is */
        else k = l5_files_utf8(b, fold((uint32_t)c));
        if (n + k >= size) break; /* whole characters only */
        memcpy(dst + n, b, k);
        n += k;
    }
    dst[n] = 0;
}

bool l5_files_hidden(const char *name)
{
    return name[0] == '.' || l5_files_same_name(name, "System Volume Information");
}

bool l5_files_name_ok(char *name, char *why, size_t len)
{
    char *s = name;
    while (*s == ' ') s++;
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') n--;
    memmove(name, s, n);
    name[n] = 0;
    if (!n) {
        snprintf(why, len, "The name is empty.");
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7F) {
            snprintf(why, len, "Control characters cannot be part of a name.");
            return false;
        }
        if (strchr("/\\:*?\"<>|", c)) {
            snprintf(why, len, "Names cannot contain %c. FAT and exFAT refuse these characters: / \\ : * ? \" < > |", c);
            return false;
        }
    }
    if (name[0] == '.') {
        snprintf(why, len, "Names starting with a dot are hidden in the SD list.");
        return false;
    }
    if (name[n - 1] == '.') {
        snprintf(why, len, "A name cannot end with a dot: FAT drops it.");
        return false;
    }
    if (l5_files_hidden(name)) {
        snprintf(why, len, "\"System Volume Information\" is the folder Windows keeps for itself.");
        return false;
    }
    size_t units = 0; /* FAT long names: at most 255 UTF-16 units */
    const uint8_t *p = (const uint8_t *)name, *end = p + n;
    while (p < end) {
        int32_t c = utf8_get(&p, end);
        if (c < 0) {
            snprintf(why, len, "The name is not valid UTF-8 text.");
            return false;
        }
        units += c > 0xFFFF ? 2 : 1;
    }
    if (units > 255) {
        snprintf(why, len, "The name is too long: at most 255 characters.");
        return false;
    }
    return true;
}

bool l5_files_join(char *out, size_t len, const char *dir, const char *name)
{
    int n = snprintf(out, len, "%s/%s", dir, name);
    return n > 0 && (size_t)n < len;
}

bool l5_files_inside(const char *path, const char *dir)
{
    size_t n = prefix_folded(path, dir);
    return n && (path[n] == 0 || path[n] == '/');
}

bool l5_files_parent(const char *path, char *out, size_t len)
{
    const char *s = strrchr(path, '/');
    if (!s || l5_files_same_name(path, l5_files_root) || (size_t)(s - path) >= len) return false;
    memcpy(out, path, s - path);
    out[s - path] = 0;
    return l5_files_inside(out, l5_files_root);
}

bool l5_files_is_text(const char *name)
{
    static const char *const EXT[] = {".txt", ".conf", ".json", ".cfg", ".ini", ".md", ".csv", ".log", ".py"};
    const char *dot = strrchr(name, '.');
    for (size_t i = 0; dot && i < sizeof(EXT) / sizeof(EXT[0]); i++) {
        if (!strcasecmp(dot, EXT[i])) return true;
    }
    return false;
}

static bool exists(const char *dir, const char *name)
{
    char p[L5_FILES_PATH];
    struct stat st;
    return !l5_files_join(p, sizeof(p), dir, name) || stat(p, &st) == 0; /* too long counts as taken */
}

bool l5_files_unique(const char *dir, const char *name, char *out, size_t len)
{
    if (!exists(dir, name)) {
        return strlcpy(out, name, len) < len;
    }
    const char *dot = strrchr(name, '.'); /* views.js: /^(.*?)(\.[^.]*)?$/ */
    int base = dot ? (int)(dot - name) : (int)strlen(name);
    const char *ext = dot ? dot : "";
    for (int i = 1; i < 10000; i++) {
        char num[8] = "";
        if (i > 1) snprintf(num, sizeof(num), "%d", i);
        int n = snprintf(out, len, "%.*s-copy%s%s", base, name, num, ext);
        if (n < 0 || (size_t)n >= len) return false;
        if (!exists(dir, out)) return true;
    }
    return false;
}

bool l5_files_free_name(const char *dir, const char *base, char *out, size_t len)
{
    for (int i = 1; i < 10000; i++) {
        int n = i == 1 ? snprintf(out, len, "%s", base) : snprintf(out, len, "%s%d", base, i);
        if (n < 0 || (size_t)n >= len) return false;
        if (!exists(dir, out)) return true;
    }
    return false;
}

/* ---------- listing ---------- */

#ifdef ESP_PLATFORM
/* "/sdcard/a" -> "0:/a" on the card's FatFs drive */
static bool ff_path(const char *path, char *out, size_t len)
{
    int drv = l5_board_sd_drive();
    size_t rl = strlen(l5_files_root);
    if (drv < 0 || strncmp(path, l5_files_root, rl) || (path[rl] && path[rl] != '/')) return false;
    int n = snprintf(out, len, "%d:%s", drv, path[rl] ? path + rl : "/");
    return n > 0 && (size_t)n < len;
}

static time_t fat_time(WORD d, WORD t)
{
    if (!d) return 0;
    struct tm tm = {
        .tm_year = (d >> 9) + 80, .tm_mon = ((d >> 5) & 15) - 1, .tm_mday = d & 31,
        .tm_hour = t >> 11, .tm_min = (t >> 5) & 63, .tm_sec = (t & 31) * 2, .tm_isdst = -1,
    };
    return mktime(&tm);
}
#endif

esp_err_t l5_files_list(const char *dir, l5_files_entry_cb_t cb, void *user)
{
#ifdef ESP_PLATFORM
    char fp[L5_FILES_PATH + 8];
    if (ff_path(dir, fp, sizeof(fp))) {
        FF_DIR *d = ps_alloc(sizeof(FF_DIR));
        FILINFO *fi = ps_alloc(sizeof(FILINFO));
        FRESULT fr = d && fi ? f_opendir(d, fp) : FR_NOT_ENOUGH_CORE;
        if (fr == FR_OK) {
            while ((fr = f_readdir(d, fi)) == FR_OK && fi->fname[0]) {
                const l5_files_entry_t e = {
                    .name = fi->fname, .dir = (fi->fattrib & AM_DIR) != 0, .size = fi->fsize,
                    .mtime = fat_time(fi->fdate, fi->ftime),
                };
                if (!cb(&e, user)) break;
            }
            f_closedir(d);
        }
        free(d);
        free(fi);
        if (fr == FR_OK) return ESP_OK;
        return fr == FR_NO_PATH || fr == FR_NO_FILE ? ESP_ERR_NOT_FOUND : fr == FR_NOT_ENOUGH_CORE ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
#endif
    DIR *d = opendir(dir);
    if (!d) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    char p[L5_FILES_PATH];
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        l5_files_entry_t e = {.name = de->d_name};
        struct stat st;
        if (l5_files_join(p, sizeof(p), dir, de->d_name) && stat(p, &st) == 0) {
            e.dir = S_ISDIR(st.st_mode);
            e.size = (uint64_t)st.st_size;
            e.mtime = st.st_mtime;
        }
        if (!cb(&e, user)) break;
    }
    closedir(d);
    return ESP_OK;
}

typedef struct {
    int n, max;
} items_n_t;

static bool items_cb(const l5_files_entry_t *e, void *user)
{
    items_n_t *k = user;
    k->n += !l5_files_hidden(e->name);
    return k->n < k->max;
}

int l5_files_items(const char *dir, int max)
{
    items_n_t k = {0, max};
    return l5_files_list(dir, items_cb, &k) == ESP_OK ? k.n : -1;
}

/* one folder level, read completely before anything in it is copied or removed */
typedef struct {
    char *names; /* NUL-terminated, one after the other */
    size_t used, size;
    struct item {
        uint64_t size;
        uint32_t off;
        bool dir;
    } *v;
    int n, cap;
    bool oom;
} items_t;

static bool collect_cb(const l5_files_entry_t *e, void *user)
{
    items_t *it = user;
    size_t l = strlen(e->name) + 1;
    if (it->n == it->cap) {
        int cap = it->cap ? it->cap * 2 : 32;
        struct item *v = ps_realloc(it->v, cap * sizeof(*v));
        if (!v) goto oom;
        it->v = v;
        it->cap = cap;
    }
    if (it->used + l > it->size) {
        size_t size = it->size ? it->size * 2 : 1024;
        while (size < it->used + l) size *= 2;
        char *names = ps_realloc(it->names, size);
        if (!names) goto oom;
        it->names = names;
        it->size = size;
    }
    memcpy(it->names + it->used, e->name, l);
    it->v[it->n++] = (struct item){.size = e->size, .off = (uint32_t)it->used, .dir = e->dir};
    it->used += l;
    return true;
oom:
    it->oom = true;
    return false;
}

static void items_free(items_t *it)
{
    free(it->names);
    free(it->v);
    memset(it, 0, sizeof(*it));
}

static esp_err_t items_read(const char *dir, items_t *it)
{
    memset(it, 0, sizeof(*it));
    esp_err_t err = l5_files_list(dir, collect_cb, it);
    if (err == ESP_OK && it->oom) err = ESP_ERR_NO_MEM;
    if (err != ESP_OK) items_free(it);
    return err;
}

/* ---------- error sentences ---------- */

static const char *errno_text(int e)
{
    switch (e) {
    case ENOSPC: return "the card is full";
    case ENOENT: return "it is no longer on the card";
    case EEXIST: return "something with that name is already there";
    case EACCES: return "the card refused it (read-only, or the card is full)";
    case EIO: return "the card did not answer (I/O error)";
    case EINVAL:
    case ENAMETOOLONG: return "the card refused the name";
    case ENOMEM: return "not enough memory";
    case ENODEV:
    case ENXIO: return "the card is not mounted";
    default: return NULL;
    }
}

static esp_err_t fail(char *why, size_t len, int e, const char *what, const char *path)
{
    const char *t = errno_text(e);
    if (t) {
        snprintf(why, len, "%s %s: %s.", what, base_name(path), t);
    } else {
        snprintf(why, len, "%s %s: error %d.", what, base_name(path), e);
    }
    ESP_LOGW(TAG, "%s %s: errno %d", what, path, e);
    return e == ENOSPC ? ESP_ERR_NO_MEM : e == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
}

/* below the root (the root itself not), no "." or ".." on the way */
static bool below_root(const char *path)
{
    if (!l5_files_inside(path, l5_files_root) || l5_files_same_name(path, l5_files_root)) return false;
    for (const char *s = path; (s = strchr(s, '/')); s++) {
        if ((s[1] == '.' && (s[2] == '/' || !s[2])) || (s[1] == '.' && s[2] == '.' && (s[3] == '/' || !s[3])) ||
            s[1] == '/' || !s[1]) {
            return false;
        }
    }
    return true;
}

static bool make_writable(const char *path)
{
#ifdef ESP_PLATFORM
    char fp[L5_FILES_PATH + 8];
    return ff_path(path, fp, sizeof(fp)) && f_chmod(fp, 0, AM_RDO) == FR_OK;
#else
    return chmod(path, 0777) == 0;
#endif
}

/* ---------- count ---------- */

typedef struct {
    const l5_files_job_t *job;
    l5_files_count_t *c;
    uint64_t cluster, seen;
} count_t;

static uint64_t clusters(uint64_t bytes, uint64_t cluster)
{
    return (bytes + cluster - 1) / cluster * cluster;
}

static esp_err_t count_dir(count_t *k, char *path, int depth)
{
    if (depth > MAX_DEPTH) return ESP_ERR_INVALID_SIZE;
    items_t it;
    esp_err_t err = items_read(path, &it);
    size_t l = strlen(path);
    for (int i = 0; err == ESP_OK && i < it.n; i++) {
        if (cancelled(k->job)) {
            err = L5_FILES_CANCELLED;
            break;
        }
        if (!l5_files_join(path + l, L5_FILES_PATH - l, "", it.names + it.v[i].off)) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (it.v[i].dir) {
            k->c->dirs++;
            k->c->alloc += k->cluster;
            err = count_dir(k, path, depth + 1);
        } else {
            k->c->files++;
            k->c->bytes += it.v[i].size;
            k->c->alloc += clusters(it.v[i].size, k->cluster);
        }
        path[l] = 0;
        if (k->job && k->job->progress && ++k->seen % 32 == 0) k->job->progress(k->seen, 0, k->job->user);
    }
    items_free(&it);
    return err;
}

esp_err_t l5_files_count(const char *path, const l5_files_job_t *job, l5_files_count_t *out)
{
    memset(out, 0, sizeof(*out));
    count_t k = {.job = job, .c = out, .cluster = job && job->cluster ? job->cluster : 32768};
    struct stat st;
    if (stat(path, &st) != 0) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    if (!S_ISDIR(st.st_mode)) {
        out->files = 1;
        out->bytes = (uint64_t)st.st_size;
        out->alloc = clusters(out->bytes, k.cluster);
        return ESP_OK;
    }
    char *p = ps_alloc(L5_FILES_PATH);
    if (!p || strlcpy(p, path, L5_FILES_PATH) >= L5_FILES_PATH) {
        free(p);
        return ESP_ERR_NO_MEM;
    }
    out->dirs = 1;
    out->alloc = k.cluster;
    esp_err_t err = count_dir(&k, p, 1);
    free(p);
    return err;
}

/* ---------- delete ---------- */

typedef struct {
    const l5_files_job_t *job;
    l5_files_result_t *res;
    uint64_t total, done;
} del_t;

static esp_err_t remove_one(del_t *k, const char *path, bool dir)
{
    int r = dir ? rmdir(path) : unlink(path);
    if (r != 0 && errno == EACCES && make_writable(path)) { /* read-only attribute (set by Windows) */
        r = dir ? rmdir(path) : unlink(path);
    }
    if (r != 0) {
        return fail(k->res->why, sizeof(k->res->why), errno, dir ? "Could not remove the folder" : "Could not remove",
                    path);
    }
    if (dir) {
        k->res->dirs++;
    } else {
        k->res->files++;
    }
    k->done++;
    if (k->job && k->job->progress) k->job->progress(k->done, k->total, k->job->user);
    return ESP_OK;
}

static esp_err_t remove_tree(del_t *k, char *path, bool dir, int depth)
{
    if (!dir) return remove_one(k, path, false);
    if (depth > MAX_DEPTH) {
        snprintf(k->res->why, sizeof(k->res->why), "Folders nested more than %d deep are not removed.", MAX_DEPTH);
        return ESP_ERR_INVALID_SIZE;
    }
    items_t it;
    esp_err_t err = items_read(path, &it);
    if (err != ESP_OK) {
        return fail(k->res->why, sizeof(k->res->why), err == ESP_ERR_NO_MEM ? ENOMEM : err == ESP_ERR_NOT_FOUND ? ENOENT : EIO,
                    "Could not read", path);
    }
    size_t l = strlen(path);
    for (int i = 0; err == ESP_OK && i < it.n; i++) {
        if (cancelled(k->job)) {
            err = L5_FILES_CANCELLED;
            break;
        }
        if (!l5_files_join(path + l, L5_FILES_PATH - l, "", it.names + it.v[i].off)) {
            snprintf(k->res->why, sizeof(k->res->why), "A path inside %s is too long.", base_name(path));
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        err = remove_tree(k, path, it.v[i].dir, depth + 1);
        path[l] = 0;
    }
    items_free(&it);
    return err == ESP_OK ? remove_one(k, path, true) : err;
}

esp_err_t l5_files_delete(const char *path, const l5_files_job_t *job, uint32_t total, l5_files_result_t *res)
{
    memset(res, 0, sizeof(*res));
    int64_t t0 = now_ms();
    if (!below_root(path)) {
        snprintf(res->why, sizeof(res->why), "Only files and folders on the SD card can be deleted, not the card itself.");
        return ESP_ERR_INVALID_ARG;
    }
    struct stat st;
    if (stat(path, &st) != 0) return fail(res->why, sizeof(res->why), errno, "Could not delete", path);
    char *p = ps_alloc(L5_FILES_PATH);
    if (!p || strlcpy(p, path, L5_FILES_PATH) >= L5_FILES_PATH) {
        free(p);
        snprintf(res->why, sizeof(res->why), "Not enough memory.");
        return ESP_ERR_NO_MEM;
    }
    del_t k = {.job = job, .res = res, .total = total};
    esp_err_t err = remove_tree(&k, p, S_ISDIR(st.st_mode), 1);
    free(p);
    res->cancelled = err == L5_FILES_CANCELLED;
    res->ms = (uint32_t)(now_ms() - t0);
    return err;
}

/* ---------- copy ---------- */

typedef struct {
    const l5_files_job_t *job;
    l5_files_result_t *res;
    uint64_t total;
    uint8_t *buf;
    size_t buf_len;
    bool created; /* dst exists because of this copy: removed again after an error */
} copy_t;

static esp_err_t copy_file(copy_t *c, const char *src, const char *dst)
{
    char *why = c->res->why;
    size_t wl = sizeof(c->res->why);
    int in = open(src, O_RDONLY | O_BINARY);
    if (in < 0) return fail(why, wl, errno, "Could not read", src);
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_BINARY, 0666);
    if (out < 0) {
        int e = errno;
        close(in);
        return fail(why, wl, e, "Could not create", dst);
    }
    c->created = true;
    esp_err_t err = ESP_OK;
    for (;;) {
        if (cancelled(c->job)) {
            err = L5_FILES_CANCELLED;
            break;
        }
        ssize_t n = read(in, c->buf, c->buf_len);
        if (n < 0) {
            err = fail(why, wl, errno, "Could not read", src);
            break;
        }
        if (n == 0) break;
        size_t len = (size_t)n;
#ifndef ESP_PLATFORM
        if (l5_files_host_write_budget >= 0) {
            len = (long)len > l5_files_host_write_budget ? (size_t)l5_files_host_write_budget : len;
            l5_files_host_write_budget -= (long)len;
        }
#endif
        ssize_t w = len ? write(out, c->buf, len) : 0;
        if (w != n) { /* FatFs writes less when the card is full */
            err = fail(why, wl, w < 0 ? errno : ENOSPC, "Could not write", dst);
            break;
        }
        c->res->bytes += (uint64_t)n;
        if (c->job && c->job->progress) c->job->progress(c->res->bytes, c->total, c->job->user);
    }
    close(in);
    if (close(out) != 0 && err == ESP_OK) err = fail(why, wl, errno, "Could not write", dst);
    if (err == ESP_OK) c->res->files++;
    return err;
}

static esp_err_t copy_tree(copy_t *c, char *src, char *dst, int depth)
{
    char *why = c->res->why;
    size_t wl = sizeof(c->res->why);
    if (depth > MAX_DEPTH) {
        snprintf(why, wl, "Folders nested more than %d deep are not copied.", MAX_DEPTH);
        return ESP_ERR_INVALID_SIZE;
    }
    if (mkdir(dst, 0777) != 0) return fail(why, wl, errno, "Could not create the folder", dst);
    c->created = true;
    c->res->dirs++;
    items_t it;
    esp_err_t err = items_read(src, &it);
    if (err != ESP_OK) {
        return fail(why, wl, err == ESP_ERR_NO_MEM ? ENOMEM : err == ESP_ERR_NOT_FOUND ? ENOENT : EIO, "Could not read", src);
    }
    size_t sl = strlen(src), dl = strlen(dst);
    for (int i = 0; err == ESP_OK && i < it.n; i++) {
        if (cancelled(c->job)) {
            err = L5_FILES_CANCELLED;
            break;
        }
        const char *name = it.names + it.v[i].off;
        if (!l5_files_join(src + sl, L5_FILES_PATH - sl, "", name) || !l5_files_join(dst + dl, L5_FILES_PATH - dl, "", name)) {
            snprintf(why, wl, "The path of %s is too long.", name);
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        err = it.v[i].dir ? copy_tree(c, src, dst, depth + 1) : copy_file(c, src, dst);
        src[sl] = dst[dl] = 0;
    }
    items_free(&it);
    return err;
}

esp_err_t l5_files_copy(const char *src, const char *dst, const l5_files_job_t *job, const l5_files_count_t *total,
                        l5_files_result_t *res)
{
    memset(res, 0, sizeof(*res));
    int64_t t0 = now_ms();
    struct stat st;
    if (!below_root(src) || !below_root(dst)) {
        snprintf(res->why, sizeof(res->why), "Only files and folders on the SD card can be copied.");
        return ESP_ERR_INVALID_ARG;
    }
    if (l5_files_inside(dst, src)) {
        snprintf(res->why, sizeof(res->why), "A folder cannot be copied into itself.");
        return ESP_ERR_INVALID_ARG;
    }
    if (stat(src, &st) != 0) return fail(res->why, sizeof(res->why), errno, "Could not copy", src);
    struct stat dst_st;
    if (stat(dst, &dst_st) == 0) return fail(res->why, sizeof(res->why), EEXIST, "Could not copy to", dst);
    copy_t c = {.job = job, .res = res, .total = total ? total->bytes : 0};
    char *s = ps_alloc(2 * L5_FILES_PATH);
    c.buf = io_alloc(COPY_BLOCK, &c.buf_len);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (!s || !c.buf) {
        snprintf(res->why, sizeof(res->why), "Not enough memory for the copy.");
    } else if (strlcpy(s, src, L5_FILES_PATH) >= L5_FILES_PATH || strlcpy(s + L5_FILES_PATH, dst, L5_FILES_PATH) >= L5_FILES_PATH) {
        snprintf(res->why, sizeof(res->why), "The path is too long.");
        err = ESP_ERR_INVALID_SIZE;
    } else {
        err = S_ISDIR(st.st_mode) ? copy_tree(&c, s, s + L5_FILES_PATH, 1) : copy_file(&c, s, s + L5_FILES_PATH);
    }
    if (err != ESP_OK && c.created) { /* the partial copy goes again */
        l5_files_result_t r;
        if (strlcpy(s, dst, L5_FILES_PATH) < L5_FILES_PATH && stat(s, &st) == 0) {
            del_t k = {.res = &r};
            memset(&r, 0, sizeof(r));
            res->left = remove_tree(&k, s, S_ISDIR(st.st_mode), 1) != ESP_OK;
            if (res->left) ESP_LOGW(TAG, "partial copy %s not removed: %s", dst, r.why);
        }
    }
    free(s);
    heap_caps_free(c.buf);
    res->cancelled = err == L5_FILES_CANCELLED;
    res->ms = (uint32_t)(now_ms() - t0);
    return err;
}

/* ---------- rename, new folder, space ---------- */

esp_err_t l5_files_rename(const char *from, const char *to, char *why, size_t len)
{
    if (!below_root(from) || !below_root(to)) {
        snprintf(why, len, "Only files and folders on the SD card can be renamed.");
        return ESP_ERR_INVALID_ARG;
    }
    if (rename(from, to) == 0) return ESP_OK;
    return fail(why, len, errno, "Could not rename", from);
}

esp_err_t l5_files_mkdir(const char *path, char *why, size_t len)
{
    if (!below_root(path)) {
        snprintf(why, len, "Folders can only be created on the SD card.");
        return ESP_ERR_INVALID_ARG;
    }
    if (mkdir(path, 0777) == 0) return ESP_OK;
    return fail(why, len, errno, "Could not create", path);
}

esp_err_t l5_files_space(uint64_t *total, uint64_t *free_bytes, uint32_t *cluster)
{
#ifdef ESP_PLATFORM
    esp_err_t err = esp_vfs_fat_info(l5_files_root, total, free_bytes);
    char fp[16];
    FATFS *fs = NULL;
    DWORD nclst;
    *cluster = 32768;
    if (ff_path(l5_files_root, fp, sizeof(fp)) && f_getfree(fp, &nclst, &fs) == FR_OK && fs) {
#if FF_MAX_SS != FF_MIN_SS
        *cluster = (uint32_t)fs->csize * fs->ssize;
#else
        *cluster = (uint32_t)fs->csize * FF_MAX_SS;
#endif
    }
    return err;
#else
    *total = l5_files_host_total;
    *free_bytes = l5_files_host_free;
    *cluster = l5_files_host_cluster;
    return ESP_OK;
#endif
}

/* ---------- text preview ---------- */

/* the 64 KB end may cut a character in half: drop it */
static size_t utf8_trim_end(const uint8_t *s, size_t n)
{
    size_t i = n;
    int k = 0;
    while (i > 0 && k < 3 && (s[i - 1] & 0xC0) == 0x80) {
        i--;
        k++;
    }
    if (i == 0) return n;
    uint8_t c = s[i - 1];
    size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    return c >= 0xC0 && n - (i - 1) < need ? i - 1 : n;
}

typedef struct {
    char *o;
    size_t w, cap;
    size_t piece;  /* where the current piece starts */
    size_t space;  /* after the last space of the current line in this piece, 0 = none */
    size_t nl;     /* after the last newline in this piece, 0 = none */
    uint32_t pieces, lines, col;
    bool at_nl;    /* the last thing written ended a line */
} text_out_t;

static void end_piece(text_out_t *t)
{
    t->o[t->w++] = 0;
    t->piece = t->w;
    t->space = t->nl = 0;
    t->pieces++;
}

/* One character (k bytes). A full piece ends first: at the last space of a line that is longer than
   half a piece, else at its last newline (the line moves to the next piece whole), else here. */
static void put(text_out_t *t, const char *s, size_t k)
{
    if (t->w - t->piece + k > L5_TEXT_PIECE - 1) {
        size_t cut = t->space && t->space - t->piece > L5_TEXT_PIECE / 2 ? t->space : t->nl;
        if (cut) {
            t->o[cut - 1] = 0; /* that space or newline ends the piece */
            t->pieces++;
            t->piece = cut;
            t->space = t->nl = 0;
        } else {
            end_piece(t);
        }
    }
    memcpy(t->o + t->w, s, k);
    t->w += k;
    if (k == 1 && s[0] == ' ') t->space = t->w;
    t->col++;
    t->at_nl = false;
}

static void newline(text_out_t *t)
{
    t->lines++;
    t->col = 0;
    t->at_nl = true;
    if (t->w - t->piece >= L5_TEXT_PIECE / 2) {
        end_piece(t); /* the NUL stands for the newline */
    } else {
        t->o[t->w++] = '\n';
        t->nl = t->w;
        t->space = 0;
    }
}

bool l5_text_prepare(const uint8_t *in, size_t n, l5_text_t *out)
{
    out->text = NULL;
    out->len = 0;
    out->pieces = out->lines = 0;
    if (memchr(in, 0, n)) {
        out->binary = true;
        return false;
    }
    size_t cap = n * 4 + 64;
    cap = cap > TEXT_OUT_MAX ? TEXT_OUT_MAX : cap;
    text_out_t t = {.o = ps_alloc(cap), .cap = cap, .lines = 1};
    if (!t.o) return false;
    const uint8_t *p = in, *end = in + n;
    if (n >= 3 && !memcmp(p, "\xEF\xBB\xBF", 3)) p += 3; /* UTF-8 BOM */
    while (p < end) {
        if (t.w + 16 > t.cap) { /* tabs made it longer than the viewer keeps */
            out->cut = true;
            break;
        }
        uint8_t c = *p;
        if (c == '\r' || c == '\n' || c == '\f') {
            p += c == '\r' && p + 1 < end && p[1] == '\n' ? 2 : 1;
            newline(&t);
        } else if (c == '\t') {
            p++;
            for (uint32_t s = 4 - t.col % 4; s; s--) put(&t, " ", 1);
        } else if (c == 0x1B) { /* terminal colour codes in logs: ESC [ ... m */
            p++;
            if (p < end && *p == '[') {
                p++;
                while (p < end && *p >= 0x20 && *p <= 0x3F) p++;
                if (p < end && *p >= 0x40 && *p <= 0x7E) p++;
            } else if (p < end && *p >= 0x40 && *p <= 0x5F) {
                p++;
            }
        } else if (c < 0x20 || c == 0x7F) {
            p++;
            put(&t, "?", 1);
        } else {
            const uint8_t *s = p;
            if (utf8_get(&p, end) < 0) {
                put(&t, "?", 1);
            } else {
                put(&t, (const char *)s, (size_t)(p - s));
            }
        }
    }
    if (t.at_nl) { /* the newline that ends the file: no empty line after it */
        t.lines--;
        if (t.w > t.piece && t.o[t.w - 1] == '\n') t.w--;
    }
    if (t.w > t.piece || !t.pieces) end_piece(&t);
    out->text = t.o;
    out->len = t.w;
    out->pieces = t.pieces;
    out->lines = n ? t.lines : 0;
    return true;
}

void l5_text_free(l5_text_t *t)
{
    free(t->text);
    t->text = NULL;
    t->len = 0;
}

esp_err_t l5_files_read_text(const char *path, l5_text_t *out, char *why, size_t len)
{
    memset(out, 0, sizeof(*out));
    struct stat st;
    if (stat(path, &st) != 0) return fail(why, len, errno, "Could not open", path);
    out->file_size = (uint64_t)st.st_size;
    size_t want = out->file_size > L5_TEXT_MAX ? L5_TEXT_MAX : (size_t)out->file_size;
    size_t got = 0, cap;
    uint8_t *buf = io_alloc(want ? want : 1, &cap);
    if (!buf) {
        snprintf(why, len, "Not enough memory to show the file.");
        return ESP_ERR_NO_MEM;
    }
    int fd = open(path, O_RDONLY | O_BINARY);
    if (fd < 0) {
        heap_caps_free(buf);
        return fail(why, len, errno, "Could not open", path);
    }
    while (got < want) {
        ssize_t r = read(fd, buf + got, want - got);
        if (r < 0) {
            int e = errno;
            close(fd);
            heap_caps_free(buf);
            return fail(why, len, e, "Could not read", path);
        }
        if (r == 0) break;
        got += (size_t)r;
    }
    close(fd);
    out->cut = out->file_size > got;
    if (out->cut) got = utf8_trim_end(buf, got);
    bool ok = l5_text_prepare(buf, got, out);
    heap_caps_free(buf);
    if (!ok && !out->binary) {
        snprintf(why, len, "Not enough memory to show the file.");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
