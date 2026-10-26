#include "l5_ptable.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "esp_check.h"
#include "esp_flash.h"
#include "esp_flash_encrypt.h"
#include "esp_flash_internal.h"
#include "esp_flash_partitions.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_rom_md5.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "l5_board.h"
#include "l5_slots.h"

static const char *TAG = "l5_ptable";
#define SECTOR 0x1000
#define BLOCK 0x10000
#define CHUNK (32 * 1024)

/* data.js schemes (KB) under the aurora_cfg rule: n x slot + FAT = 13312 KB before 0xF10000,
   SPIFFS = the 872 KB gap after aurora_cfg. Big apps come before several apps: Default is one 8 MB
   slot (Aurora 6.2 MB and the UIFlow2 app fit without PMAN); UiFlow2 holds UIFlow2 2.5's 7,709,696 B
   app (its 7 MB slot no longer did). */
const l5_scheme_def_t l5_schemes[] = {
    {"default", "Default", 8192, 1, L5_GAP_KB, 5120, 0},    {"multi2", "2 Apps", 8192, 2, L5_GAP_KB, 5120, 0},
    {"multi4", "4 Apps", 8192, 4, L5_GAP_KB, 5120, 0},      {"minspiffs", "Min SPIFFS", 10240, 1, L5_GAP_KB, 3072, 0},
    {"uiflow2", "UiFlow2", 7552, 1, L5_GAP_KB, 5760, 0},    {"doom", "Doom", 4096, 1, L5_GAP_KB, 9216, 0},
    {"maxapp", "Max App", 12864, 1, L5_GAP_KB, 448, 0},
    /* one app up to the UiFlow2 size (ORBIT DRUM's limit) next to two small ones (PSTX1, Spotify: up to 2.5 MB) */
    {"bigsmall", "1 Big + 2 Small", 7552 + 2 * 2560, 3, L5_GAP_KB, 640, 7552},
};
const int l5_scheme_count = sizeof(l5_schemes) / sizeof(l5_schemes[0]);

static void say(char *why, size_t len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void say(char *why, size_t len, const char *fmt, ...)
{
    if (!why || !len) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, len, fmt, ap);
    va_end(ap);
}

static void report(l5_pt_progress_t cb, void *user, int pct, const char *label)
{
    if (cb) {
        cb(pct, label, user);
    }
}

/* ---------- layouts ---------- */

l5_layout_t l5_layout_of_scheme(const l5_scheme_def_t *s)
{
    if (s->app0_kb && s->n > 1) {
        return (l5_layout_t){.n = s->n, .app0_kb = s->app0_kb, .slot_kb = (s->app_kb - s->app0_kb) / (s->n - 1) / 64 * 64,
                             .spiffs_kb = s->spiffs_kb, .fat_kb = s->fat_kb};
    }
    return (l5_layout_t){.n = s->n, .slot_kb = s->app_kb / s->n / 64 * 64, .spiffs_kb = s->spiffs_kb, .fat_kb = s->fat_kb};
}

uint32_t l5_layout_slot_kb(const l5_layout_t *l, uint32_t i)
{
    return i == 0 && l->app0_kb ? l->app0_kb : l->slot_kb;
}

uint32_t l5_layout_apps_kb(const l5_layout_t *l)
{
    if (!l->n) return 0;
    return l5_layout_slot_kb(l, 0) + (l->n - 1) * l->slot_kb;
}

uint32_t l5_layout_max_slot_kb(const l5_layout_t *l)
{
    const uint32_t a0 = l5_layout_slot_kb(l, 0);
    return l->n > 1 && l->slot_kb > a0 ? l->slot_kb : a0;
}

const char *l5_layout_fat_label(const l5_layout_t *l)
{
    return l->fat_label[0] ? l->fat_label : "vfs";
}

const char *l5_layout_gap_label(const l5_layout_t *l)
{
    return l->gap_label[0] ? l->gap_label : "spiffs";
}

static uint8_t gap_subtype(const l5_layout_t *l)
{
    return l->gap_subtype ? l->gap_subtype : ESP_PARTITION_SUBTYPE_DATA_SPIFFS;
}

bool l5_layout_equal(const l5_layout_t *a, const l5_layout_t *b)
{
    if (a->n != b->n || a->slot_kb != b->slot_kb || a->app0_kb != b->app0_kb || a->spiffs_kb != b->spiffs_kb ||
        a->fat_kb != b->fat_kb || a->nextra != b->nextra) {
        return false;
    }
    for (uint32_t i = 0; i < a->nextra && i < L5_MAX_EXTRA; i++) {
        if (strcmp(a->extra[i].label, b->extra[i].label) || a->extra[i].subtype != b->extra[i].subtype ||
            a->extra[i].kb != b->extra[i].kb) {
            return false;
        }
    }
    return (!a->fat_kb || !strcmp(l5_layout_fat_label(a), l5_layout_fat_label(b))) &&
           (!a->spiffs_kb || (!strcmp(l5_layout_gap_label(a), l5_layout_gap_label(b)) && gap_subtype(a) == gap_subtype(b)));
}

uint32_t l5_layout_extras_kb(const l5_layout_t *l)
{
    uint32_t kb = 0;
    for (uint32_t i = 0; i < l->nextra && i < L5_MAX_EXTRA; i++) kb += l->extra[i].kb;
    return kb;
}

const l5_scheme_def_t *l5_layout_match(const l5_layout_t *l)
{
    for (int i = 0; i < l5_scheme_count; i++) {
        l5_layout_t s = l5_layout_of_scheme(&l5_schemes[i]);
        if (l5_layout_equal(&s, l)) {
            return &l5_schemes[i];
        }
    }
    return NULL;
}

const l5_scheme_def_t *l5_scheme_find(const char *key)
{
    for (int i = 0; key && i < l5_scheme_count; i++) {
        if (strcmp(l5_schemes[i].id, key) == 0 || strcmp(l5_schemes[i].label, key) == 0) {
            return &l5_schemes[i];
        }
    }
    return NULL;
}

uint32_t l5_layout_front_kb(const l5_layout_t *l)
{
    return l5_layout_apps_kb(l) + l5_layout_extras_kb(l) + l->fat_kb;
}

l5_layout_t l5_pt_base_layout(const l5_flash_map_t *map)
{
    l5_layout_t l = map->layout;
    if (map->aurora) {
        return l; /* already under the aurora_cfg rule, AUTO PARTITION layouts included */
    }
    l.spiffs_kb = l.spiffs_kb ? L5_GAP_KB : 0;
    uint32_t apps = l5_layout_apps_kb(&l);
    if (apps > L5_FRONT_KB) { /* cannot happen with launcher tables; keep it buildable */
        l.n = 1;
        l.app0_kb = 0;
        l.slot_kb = apps = L5_FRONT_KB;
    }
    if (apps + l.fat_kb > L5_FRONT_KB) {
        l.fat_kb = (L5_FRONT_KB - apps) / 64 * 64;
    }
    if (l.fat_kb && l.fat_kb < L5_MIN_DATA_KB) {
        l.fat_kb = 0;
    }
    return l;
}

static bool is_aurora(const l5_part_t *p)
{
    return strcmp(p->label, L5_AURORA_LABEL) == 0 && p->type == ESP_PARTITION_TYPE_DATA &&
           p->subtype == ESP_PARTITION_SUBTYPE_DATA_NVS && p->offset == L5_AURORA_ADDR && p->size == L5_AURORA_SIZE;
}

/* ---------- binary format (esp_partition_info_t + MD5 record, as gen_esp32part.py) ---------- */

static void serialize(const l5_ptable_t *t, uint8_t out[L5_PT_LEN])
{
    memset(out, 0xff, L5_PT_LEN);
    for (int i = 0; i < t->n; i++) {
        esp_partition_info_t e = {
            .magic = ESP_PARTITION_MAGIC, .type = t->p[i].type, .subtype = t->p[i].subtype,
            .pos = {.offset = t->p[i].offset, .size = t->p[i].size}, .flags = t->p[i].flags,
        };
        memset(e.label, 0, sizeof(e.label));
        memcpy(e.label, t->p[i].label, strnlen(t->p[i].label, sizeof(e.label)));
        memcpy(out + i * sizeof(e), &e, sizeof(e));
    }
    uint8_t *md5 = out + t->n * sizeof(esp_partition_info_t);
    md5[0] = md5[1] = 0xeb; /* MD5_PARTITION_BEGIN: 0xEBEB + 14 x 0xFF, then the digest */
    md5_context_t ctx;
    esp_rom_md5_init(&ctx);
    esp_rom_md5_update(&ctx, out, t->n * sizeof(esp_partition_info_t));
    esp_rom_md5_final(md5 + ESP_PARTITION_MD5_OFFSET, &ctx);
}

static void parse(const uint8_t *buf, l5_ptable_t *t)
{
    t->n = 0;
    for (int i = 0; i < L5_PT_LEN / (int)sizeof(esp_partition_info_t) && t->n < L5_PT_MAX; i++) {
        esp_partition_info_t e;
        memcpy(&e, buf + i * sizeof(e), sizeof(e));
        if (e.magic != ESP_PARTITION_MAGIC) {
            if (e.magic == ESP_PARTITION_MAGIC_MD5) {
                continue;
            }
            break;
        }
        l5_part_t *p = &t->p[t->n++];
        memset(p, 0, sizeof(*p));
        memcpy(p->label, e.label, sizeof(e.label));
        p->type = e.type;
        p->subtype = e.subtype;
        p->offset = e.pos.offset;
        p->size = e.pos.size;
        p->flags = e.flags;
    }
}

static bool part_equal(const l5_part_t *a, const l5_part_t *b)
{
    return strcmp(a->label, b->label) == 0 && a->type == b->type && a->subtype == b->subtype && a->offset == b->offset &&
           a->size == b->size && a->flags == b->flags;
}

static bool table_equal(const l5_ptable_t *a, const l5_ptable_t *b)
{
    if (a->n != b->n) {
        return false;
    }
    for (int i = 0; i < a->n; i++) {
        if (!part_equal(&a->p[i], &b->p[i])) {
            return false;
        }
    }
    return true;
}

static bool is_ota_app(const l5_part_t *p)
{
    return p->type == ESP_PARTITION_TYPE_APP && p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
           p->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX;
}

static bool is_data(const l5_part_t *p, uint8_t sub)
{
    return p->type == ESP_PARTITION_TYPE_DATA && p->subtype == sub;
}

/* data partitions the app area may hold: FAT, SPIFFS, LittleFS, and NVS / undefined partitions with
   their own label (a firmware's table asks for them, AUTO PARTITION) */
static bool pool_data_kind(const l5_part_t *p)
{
    if (p->type != ESP_PARTITION_TYPE_DATA) return false;
    switch (p->subtype) {
    case ESP_PARTITION_SUBTYPE_DATA_FAT:
    case ESP_PARTITION_SUBTYPE_DATA_SPIFFS:
    case L5_SUBTYPE_LITTLEFS:
    case L5_SUBTYPE_UNDEFINED:
        return true;
    case ESP_PARTITION_SUBTYPE_DATA_NVS:
        return strcmp(p->label, "nvs") && strcmp(p->label, L5_AURORA_LABEL);
    default:
        return false;
    }
}

/* ---------- reading the current table ---------- */

esp_err_t l5_pt_read_map(l5_flash_map_t *m)
{
    memset(m, 0, sizeof(*m));
    uint8_t *buf = heap_caps_malloc(L5_PT_LEN, MALLOC_CAP_INTERNAL);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_flash_read(NULL, buf, L5_PT_OFFSET, L5_PT_LEN);
    int count = 0;
    if (err == ESP_OK && esp_partition_table_verify((const esp_partition_info_t *)buf, false, &count) != ESP_OK) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        parse(buf, &m->table);
    }
    heap_caps_free(buf);
    if (err != ESP_OK) {
        say(m->why, sizeof(m->why), "The table at 0x8000 cannot be read (%s).", esp_err_to_name(err));
        return err;
    }
    esp_flash_get_size(NULL, &m->flash_size);

    const esp_partition_t *run = esp_ota_get_running_partition();
    const l5_part_t *factory = NULL;
    for (int i = 0; i < m->table.n; i++) {
        const l5_part_t *p = &m->table.p[i];
        if (p->type == ESP_PARTITION_TYPE_APP && p->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
            factory = p;
        }
    }
    if (!factory || factory->offset != 0x10000 || !run || run->address != factory->offset || run->size != factory->size) {
        say(m->why, sizeof(m->why), "The launcher is not the factory app at 0x10000.");
        return ESP_OK;
    }
    m->pool_start = factory->offset + factory->size;
    m->pool_end = m->flash_size;
    if (m->pool_start % BLOCK) {
        say(m->why, sizeof(m->why), "The launcher partition does not end on a 64 KB boundary.");
        return ESP_OK;
    }
    for (int i = 0; i < m->table.n; i++) { /* the coredump at the end closes the managed area */
        const l5_part_t *p = &m->table.p[i];
        if (is_data(p, ESP_PARTITION_SUBTYPE_DATA_COREDUMP) && p->offset >= m->pool_start && p->offset < m->pool_end) {
            m->pool_end = p->offset;
        }
    }
    bool seen[16] = {0};
    uint32_t kb[16] = {0}; /* size of ota_k */
    int spiffs = 0, fat = 0;
    l5_layout_t l = {0};
    for (int i = 0; i < m->table.n; i++) { /* the aurora_cfg rule decides how data partitions are read */
        m->aurora |= is_aurora(&m->table.p[i]);
    }
    /* under the rule (launcher tables): apps, data partitions before aurora_cfg (the last FAT is "vfs",
       the others are AUTO PARTITION extras), one partition in the gap after it */
    const l5_part_t *front[L5_MAX_EXTRA + 2];
    int nfront = 0, ngap = 0;
    for (int i = 0; i < m->table.n; i++) {
        const l5_part_t *p = &m->table.p[i];
        if (p->offset + p->size <= m->pool_start) {
            continue; /* fixed region: nvs, otadata, launcher, ... */
        }
        if (p->offset < m->pool_start) {
            say(m->why, sizeof(m->why), "%s overlaps the end of the launcher partition.", p->label);
            return ESP_OK;
        }
        if (p->offset >= m->pool_end) {
            if (!is_data(p, ESP_PARTITION_SUBTYPE_DATA_COREDUMP)) {
                say(m->why, sizeof(m->why), "%s lies after the coredump partition.", p->label);
                return ESP_OK;
            }
            continue;
        }
        if (p->offset + p->size > m->pool_end) {
            say(m->why, sizeof(m->why), "%s overlaps the coredump partition.", p->label);
            return ESP_OK;
        }
        if (is_ota_app(p)) {
            int k = p->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
            if (seen[k]) {
                say(m->why, sizeof(m->why), "The app slots are not unique.");
                return ESP_OK;
            }
            seen[k] = true;
            kb[k] = p->size / 1024;
            l.n++;
        } else if (is_aurora(p)) {
            /* Aurora's PIN store: kept as it is */
        } else if (m->aurora && pool_data_kind(p)) {
            if (p->offset + p->size <= L5_AURORA_ADDR && nfront < L5_MAX_EXTRA + 1) {
                front[nfront++] = p;
            } else if (p->offset >= L5_GAP_ADDR && !ngap++) {
                l.spiffs_kb = p->size / 1024;
                if (strcmp(p->label, "spiffs")) strlcpy(l.gap_label, p->label, sizeof(l.gap_label));
                if (p->subtype != ESP_PARTITION_SUBTYPE_DATA_SPIFFS) l.gap_subtype = p->subtype;
            } else {
                say(m->why, sizeof(m->why), "Partition %s does not fit the launcher's data layout.", p->label);
                return ESP_OK;
            }
        } else if (!m->aurora && is_data(p, ESP_PARTITION_SUBTYPE_DATA_SPIFFS) && !spiffs++) {
            l.spiffs_kb = p->size / 1024; /* before the rule: one SPIFFS and one FAT anywhere */
        } else if (!m->aurora && is_data(p, ESP_PARTITION_SUBTYPE_DATA_FAT) && !fat++) {
            l.fat_kb = p->size / 1024;
        } else {
            say(m->why, sizeof(m->why), "Partition %s is not an app slot, SPIFFS, FAT or aurora_cfg at 0xF10000.", p->label);
            return ESP_OK;
        }
    }
    if (nfront && front[nfront - 1]->subtype == ESP_PARTITION_SUBTYPE_DATA_FAT) { /* the last FAT is vfs */
        const l5_part_t *f = front[--nfront];
        l.fat_kb = f->size / 1024;
        if (strcmp(f->label, "vfs")) strlcpy(l.fat_label, f->label, sizeof(l.fat_label));
    }
    if (nfront > L5_MAX_EXTRA) {
        say(m->why, sizeof(m->why), "More than %d data partitions before aurora_cfg.", L5_MAX_EXTRA + 1);
        return ESP_OK;
    }
    for (int i = 0; i < nfront; i++) {
        l5_extra_t *x = &l.extra[l.nextra++];
        strlcpy(x->label, front[i]->label, sizeof(x->label));
        x->subtype = front[i]->subtype;
        x->kb = front[i]->size / 1024;
    }
    for (uint32_t k = 0; k < l.n; k++) {
        if (!seen[k]) {
            say(m->why, sizeof(m->why), "App slot ota_%lu is missing.", (unsigned long)k);
            return ESP_OK;
        }
    }
    /* app0 may have its own size; ota_1.. are all the same */
    l.slot_kb = l.n > 1 ? kb[1] : kb[0];
    for (uint32_t k = 2; k < l.n; k++) {
        if (kb[k] != l.slot_kb) {
            say(m->why, sizeof(m->why), "The app slots after app0 are not the same size.");
            return ESP_OK;
        }
    }
    if (l.n > 1 && kb[0] != l.slot_kb) l.app0_kb = kb[0];
    m->layout = l;
    /* tables made before the aurora_cfg rule (vfs over 0xF10000, SPIFFS of any size) are managed too:
       the next scheme change rebuilds the area; only the area bounds themselves must match */
    if (m->pool_start != L5_POOL_START || m->pool_end != L5_POOL_END) {
        say(m->why, sizeof(m->why), "The app area is not 0x210000..0xFF0000 (launcher 2 MB, coredump at the end).");
        return ESP_OK;
    }
    m->managed = true;
    return ESP_OK;
}

/* ---------- building + validating a new table ---------- */

static esp_err_t validate(const l5_ptable_t *t, const l5_flash_map_t *m, char *why, size_t len)
{
    if (t->n < 1 || t->n > L5_PT_MAX) {
        say(why, len, "The table would have %d entries.", t->n);
        return ESP_ERR_INVALID_SIZE;
    }
    bool seen_ota[16] = {0};
    int ota = 0, aurora = 0;
    bool otadata = false, nvs = false, factory = false;
    uint32_t prev_end = L5_PT_OFFSET + SECTOR;
    for (int i = 0; i < t->n; i++) {
        const l5_part_t *p = &t->p[i];
        size_t ll = strnlen(p->label, sizeof(p->label));
        if (ll == 0 || ll > 16) {
            say(why, len, "Entry %d has no valid label.", i);
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t c = 0; c < ll; c++) {
            if (p->label[c] < 0x21 || p->label[c] > 0x7e) {
                say(why, len, "Label %s has an invalid character.", p->label);
                return ESP_ERR_INVALID_ARG;
            }
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(t->p[j].label, p->label) == 0) {
                say(why, len, "Label %s is used twice.", p->label);
                return ESP_ERR_INVALID_ARG;
            }
        }
        if (p->size == 0 || p->offset % SECTOR || p->size % SECTOR) {
            say(why, len, "%s is not aligned to 4 KB.", p->label);
            return ESP_ERR_INVALID_ARG;
        }
        if (p->type == ESP_PARTITION_TYPE_APP && (p->offset % BLOCK || p->size % BLOCK)) {
            say(why, len, "App partition %s is not aligned to 64 KB.", p->label);
            return ESP_ERR_INVALID_ARG;
        }
        if (p->offset < prev_end) {
            say(why, len, "%s overlaps the entry before it.", p->label);
            return ESP_ERR_INVALID_ARG;
        }
        prev_end = p->offset + p->size;
        if (prev_end > m->flash_size || prev_end < p->offset) {
            say(why, len, "%s ends after the end of flash.", p->label);
            return ESP_ERR_INVALID_SIZE;
        }
        otadata |= is_data(p, ESP_PARTITION_SUBTYPE_DATA_OTA) && p->size >= 2 * SECTOR;
        nvs |= is_data(p, ESP_PARTITION_SUBTYPE_DATA_NVS);
        factory |= p->type == ESP_PARTITION_TYPE_APP && p->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY &&
                   p->offset == 0x10000;
        bool in_pool = p->offset >= m->pool_start && p->offset < m->pool_end;
        if (in_pool) {
            if (p->offset + p->size > m->pool_end) {
                say(why, len, "%s does not fit the app area.", p->label);
                return ESP_ERR_INVALID_SIZE;
            }
            if (is_aurora(p)) {
                aurora++;
            } else if (strcmp(p->label, L5_AURORA_LABEL) == 0) {
                say(why, len, "aurora_cfg must stay data/nvs at 0xF10000 with 24 KB.");
                return ESP_ERR_INVALID_ARG;
            } else if (p->offset < L5_AURORA_ADDR + L5_AURORA_SIZE && p->offset + p->size > L5_AURORA_ADDR) {
                say(why, len, "%s would cover aurora_cfg at 0xF10000.", p->label);
                return ESP_ERR_INVALID_ARG;
            } else if (is_ota_app(p)) {
                int k = p->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
                if (seen_ota[k] || p->size < L5_MIN_APP_KB * 1024) {
                    say(why, len, "App slot %s is a duplicate or smaller than 1 MB.", p->label);
                    return ESP_ERR_INVALID_ARG;
                }
                seen_ota[k] = true;
                ota++;
            } else if (pool_data_kind(p)) {
                /* sizes as the layout gives them: l5_pt_build keeps the main FAT at data.js minData (448 KB),
                   a firmware's own partitions (AUTO PARTITION) keep the sizes its table asks for */
            } else {
                say(why, len, "%s does not belong in the app area.", p->label);
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    for (int k = 0; k < ota; k++) {
        if (!seen_ota[k]) {
            say(why, len, "App slot ota_%d is missing.", k);
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (!ota || !otadata || !nvs || !factory) {
        say(why, len, "The table must keep nvs, otadata, the launcher and at least one app slot.");
        return ESP_ERR_INVALID_ARG;
    }
    if (aurora != 1) {
        say(why, len, "The table must hold aurora_cfg (data, nvs) at 0xF10000, 24 KB.");
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; m->aurora && i < m->table.n; i++) { /* an existing PIN store entry stays byte for byte */
        for (int j = 0; is_aurora(&m->table.p[i]) && j < t->n; j++) {
            if (is_aurora(&t->p[j]) && !part_equal(&m->table.p[i], &t->p[j])) {
                say(why, len, "aurora_cfg would change.");
                return ESP_ERR_INVALID_STATE;
            }
        }
    }
    /* the region below and after the managed area must stay byte for byte */
    int fa = 0, fb = 0;
    for (int i = 0; i < m->table.n; i++) {
        const l5_part_t *p = &m->table.p[i];
        if (p->offset < m->pool_start || p->offset >= m->pool_end) {
            while (fb < t->n && t->p[fb].offset >= m->pool_start && t->p[fb].offset < m->pool_end) {
                fb++;
            }
            if (fb >= t->n || !part_equal(p, &t->p[fb])) {
                say(why, len, "System partition %s would change.", p->label);
                return ESP_ERR_INVALID_STATE;
            }
            fa++;
            fb++;
        }
    }
    for (; fb < t->n; fb++) {
        if (t->p[fb].offset < m->pool_start || t->p[fb].offset >= m->pool_end) {
            say(why, len, "The table would gain system partition %s.", t->p[fb].label);
            return ESP_ERR_INVALID_STATE;
        }
    }
    (void)fa;
    /* the bootloader's own check (layout, flash size, MD5) on the exact bytes, then a round trip */
    uint8_t *buf = heap_caps_malloc(L5_PT_LEN, MALLOC_CAP_INTERNAL);
    l5_ptable_t *back = heap_caps_malloc(sizeof(l5_ptable_t), MALLOC_CAP_DEFAULT);
    esp_err_t err = ESP_OK;
    if (!buf || !back) {
        err = ESP_ERR_NO_MEM;
    } else {
        serialize(t, buf);
        int count = 0;
        if (esp_partition_table_verify((const esp_partition_info_t *)buf, true, &count) != ESP_OK || count != t->n) {
            say(why, len, "The new table does not pass the bootloader check.");
            err = ESP_ERR_INVALID_STATE;
        } else {
            parse(buf, back);
            if (!table_equal(back, t)) {
                say(why, len, "The new table does not read back the same.");
                err = ESP_ERR_INVALID_STATE;
            }
        }
    }
    heap_caps_free(buf);
    heap_caps_free(back);
    return err;
}

static void add(l5_ptable_t *t, const char *label, uint8_t type, uint8_t sub, uint32_t off, uint32_t size)
{
    l5_part_t *p = &t->p[t->n++];
    memset(p, 0, sizeof(*p));
    strlcpy(p->label, label, sizeof(p->label));
    p->type = type;
    p->subtype = sub;
    p->offset = off;
    p->size = size;
}

esp_err_t l5_pt_build(const l5_flash_map_t *m, const l5_layout_t *l, l5_ptable_t *out, char *why, size_t len)
{
    if (!m->managed) {
        say(why, len, "%s Flash partitions.csv of the launcher over USB first.", m->why);
        return ESP_ERR_INVALID_STATE;
    }
    if (l->n < 1 || l->n > L5_MAX_SLOTS || l->slot_kb < L5_MIN_APP_KB || l->slot_kb % 64 ||
        (l->app0_kb && (l->n < 2 || l->app0_kb < L5_MIN_APP_KB || l->app0_kb % 64)) ||
        (l->fat_kb && (l->fat_kb < L5_MIN_DATA_KB || l->fat_kb % 64))) {
        say(why, len, "Slots need 1 to %d x at least 1 MB, FAT at least 448 KB, all in 64 KB steps.", L5_MAX_SLOTS);
        return ESP_ERR_INVALID_ARG;
    }
    if (l->spiffs_kb > L5_GAP_KB || l->spiffs_kb % 4) {
        say(why, len, "The partition after aurora_cfg fits the %u KB gap (0xF16000..0xFF0000) in 4 KB steps.",
            (unsigned)L5_GAP_KB);
        return ESP_ERR_INVALID_ARG;
    }
    if (l->nextra > L5_MAX_EXTRA) {
        say(why, len, "At most %d extra data partitions.", L5_MAX_EXTRA);
        return ESP_ERR_INVALID_ARG;
    }
    static const char *const RESERVED[] = {"nvs", "otadata", "launcher", "factory", L5_AURORA_LABEL, "coredump", "phy_init"};
    for (uint32_t i = 0; i < l->nextra; i++) {
        const l5_extra_t *x = &l->extra[i];
        l5_part_t probe = {.type = ESP_PARTITION_TYPE_DATA, .subtype = x->subtype};
        strlcpy(probe.label, x->label, sizeof(probe.label));
        bool reserved = !x->label[0] || !strncmp(x->label, "app", 3);
        for (size_t r = 0; r < sizeof(RESERVED) / sizeof(RESERVED[0]); r++) reserved |= !strcmp(x->label, RESERVED[r]);
        if (reserved || !pool_data_kind(&probe) || !x->kb || x->kb % 4) {
            say(why, len, "Extra partition \"%s\" needs its own label, a data type and a size in 4 KB steps.", x->label);
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (l5_layout_front_kb(l) > L5_FRONT_KB) {
        say(why, len, "%lu KB of app slots and data partitions do not fit the %u KB before aurora_cfg (0xF10000).",
            (unsigned long)l5_layout_front_kb(l), (unsigned)L5_FRONT_KB);
        return ESP_ERR_INVALID_SIZE;
    }
    out->n = 0;
    for (int i = 0; i < m->table.n; i++) {
        if (m->table.p[i].offset < m->pool_start) {
            out->p[out->n++] = m->table.p[i];
        }
    }
    uint32_t off = m->pool_start;
    for (uint32_t i = 0; i < l->n; i++) {
        char label[16];
        snprintf(label, sizeof(label), "app%lu", (unsigned long)i);
        add(out, label, ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_MIN + i, off, l5_layout_slot_kb(l, i) * 1024);
        off += l5_layout_slot_kb(l, i) * 1024;
    }
    for (uint32_t i = 0; i < l->nextra; i++) { /* a firmware's own data partitions, its labels and sizes */
        add(out, l->extra[i].label, ESP_PARTITION_TYPE_DATA, l->extra[i].subtype, off, l->extra[i].kb * 1024);
        off += l->extra[i].kb * 1024;
    }
    if (l->fat_kb) {
        add(out, l5_layout_fat_label(l), ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, off, l->fat_kb * 1024);
    }
    /* Aurora's PIN store: the existing entry byte for byte, or a new one */
    const l5_part_t *old = NULL;
    for (int i = 0; i < m->table.n; i++) {
        if (is_aurora(&m->table.p[i])) old = &m->table.p[i];
    }
    if (old) {
        out->p[out->n++] = *old;
    } else {
        add(out, L5_AURORA_LABEL, ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, L5_AURORA_ADDR, L5_AURORA_SIZE);
    }
    if (l->spiffs_kb) {
        add(out, l5_layout_gap_label(l), ESP_PARTITION_TYPE_DATA, gap_subtype(l), L5_GAP_ADDR, l->spiffs_kb * 1024);
    }
    for (int i = 0; i < m->table.n; i++) {
        if (m->table.p[i].offset >= m->pool_end) {
            out->p[out->n++] = m->table.p[i];
        }
    }
    return validate(out, m, why, len);
}

static const l5_part_t *pool_find(const l5_ptable_t *t, const l5_flash_map_t *m, uint8_t type, uint8_t sub)
{
    for (int i = 0; i < t->n; i++) {
        const l5_part_t *p = &t->p[i];
        if (p->offset >= m->pool_start && p->offset < m->pool_end && p->type == type && p->subtype == sub) {
            return p;
        }
    }
    return NULL;
}

static l5_data_change_t data_change(const l5_part_t *old, const l5_part_t *now)
{
    if (!old && !now) return L5_DATA_NONE;
    if (!now) return L5_DATA_REMOVED;
    if (old && old->offset == now->offset && old->size == now->size) return L5_DATA_KEPT;
    return L5_DATA_ERASED;
}

static bool pool_data(const l5_part_t *p, const l5_flash_map_t *m)
{
    return p->type == ESP_PARTITION_TYPE_DATA && p->offset >= m->pool_start && p->offset < m->pool_end && !is_aurora(p);
}

/* the partition after aurora_cfg / the last FAT before it (vfs, or its AUTO PARTITION label) */
static const l5_part_t *gap_part(const l5_ptable_t *t, const l5_flash_map_t *m)
{
    for (int i = 0; i < t->n; i++) {
        if (pool_data(&t->p[i], m) && t->p[i].offset >= L5_GAP_ADDR) return &t->p[i];
    }
    for (int i = 0; i < t->n; i++) { /* before the rule: the SPIFFS anywhere */
        if (pool_data(&t->p[i], m) && !m->aurora && t == &m->table && t->p[i].subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS)
            return &t->p[i];
    }
    return NULL;
}

static const l5_part_t *main_fat(const l5_ptable_t *t, const l5_flash_map_t *m)
{
    const l5_part_t *f = NULL;
    for (int i = 0; i < t->n; i++) {
        if (pool_data(&t->p[i], m) && t->p[i].subtype == ESP_PARTITION_SUBTYPE_DATA_FAT && t->p[i].offset < L5_GAP_ADDR)
            f = &t->p[i];
    }
    return f;
}

static void append(char *s, size_t len, const char *label)
{
    if (s[0]) strlcat(s, ", ", len);
    strlcat(s, label, len);
}

void l5_pt_diff(const l5_flash_map_t *m, const l5_ptable_t *next, l5_pt_diff_t *d)
{
    memset(d, 0, sizeof(*d));
    for (int k = 0; k < 16; k++) {
        const l5_part_t *a = pool_find(&m->table, m, ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_MIN + k);
        const l5_part_t *b = pool_find(next, m, ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_MIN + k);
        if ((a == NULL) != (b == NULL) || (a && (a->offset != b->offset || a->size != b->size))) {
            d->apps = true;
        }
    }
    d->spiffs = data_change(gap_part(&m->table, m), gap_part(next, m));
    d->fat = data_change(main_fat(&m->table, m), main_fat(next, m));
    /* every data partition by label: kept only with the same type, place and size */
    for (int i = 0; i < next->n; i++) {
        const l5_part_t *p = &next->p[i];
        if (!pool_data(p, m)) continue;
        const l5_part_t *old = NULL;
        for (int j = 0; j < m->table.n && !old; j++) {
            if (pool_data(&m->table.p[j], m) && !strcmp(m->table.p[j].label, p->label)) old = &m->table.p[j];
        }
        if (!old || !part_equal(old, p)) {
            if (d->nerase < (int)(sizeof(d->erase) / sizeof(d->erase[0]))) d->erase[d->nerase++] = p;
            append(d->erased, sizeof(d->erased), p->label);
        }
    }
    for (int j = 0; j < m->table.n; j++) {
        const l5_part_t *o = &m->table.p[j];
        bool gone = pool_data(o, m);
        for (int i = 0; gone && i < next->n; i++) gone = !(pool_data(&next->p[i], m) && !strcmp(next->p[i].label, o->label));
        if (gone) append(d->removed, sizeof(d->removed), o->label);
    }
    bool now = false;
    for (int i = 0; i < next->n; i++) {
        now |= is_aurora(&next->p[i]);
    }
    d->aurora_new = now && !m->aurora; /* an existing aurora_cfg is never erased */
}

/* ---------- writing ---------- */

static void backup_name(char *out, size_t len, const char *stem, const char *ext)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    struct stat st;
    if (tm.tm_year + 1900 >= 2024) {
        snprintf(out, len, "%s/%s_%04d%02d%02d%s", L5_BACKUP_DIR, stem, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, ext);
        if (stat(out, &st) != 0) {
            return;
        }
        snprintf(out, len, "%s/%s_%04d%02d%02d-%02d%02d%02d%s", L5_BACKUP_DIR, stem, tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ext);
        return;
    }
    for (int i = 1; i < 1000; i++) { /* RTC not set */
        snprintf(out, len, "%s/%s_%03d%s", L5_BACKUP_DIR, stem, i, ext);
        if (stat(out, &st) != 0) {
            return;
        }
    }
}

static bool ensure_backup_dir(void)
{
    if (!l5_board_sd_ready()) {
        return false;
    }
    return mkdir(L5_BACKUP_DIR, 0777) == 0 || errno == EEXIST;
}

static esp_err_t raw_erase(const l5_flash_map_t *m, uint32_t addr, uint32_t len, bool aurora_new)
{
    /* everything erased after the table write lies inside the managed area, and never in
       Aurora's PIN store unless that entry is created right now */
    bool hits_aurora = addr < L5_AURORA_ADDR + L5_AURORA_SIZE && addr + len > L5_AURORA_ADDR;
    if (addr < m->pool_start || addr + len > m->pool_end || addr % SECTOR || len % SECTOR || (hits_aurora && !aurora_new)) {
        ESP_LOGE(TAG, "refusing to erase 0x%lx+0x%lx", (unsigned long)addr, (unsigned long)len);
        return ESP_ERR_INVALID_ARG;
    }
    return esp_flash_erase_region(NULL, addr, len);
}

esp_err_t l5_pt_apply(const l5_flash_map_t *m, const l5_ptable_t *next, l5_pt_progress_t cb, void *user,
                      bool *table_written, char *why, size_t len)
{
    *table_written = false;
    if (esp_flash_encryption_enabled()) {
        say(why, len, "Flash encryption is on: the launcher does not rewrite the partition table.");
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t err = validate(next, m, why, len);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t *nbuf = heap_caps_malloc(L5_PT_LEN, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    uint8_t *obuf = heap_caps_malloc(SECTOR, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    uint8_t *rbuf = heap_caps_malloc(SECTOR, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    l5_ptable_t *now = heap_caps_malloc(sizeof(l5_ptable_t), MALLOC_CAP_DEFAULT);
    if (!nbuf || !obuf || !rbuf || !now) {
        say(why, len, "Not enough memory.");
        err = ESP_ERR_NO_MEM;
        goto out;
    }
    serialize(next, nbuf);

    report(cb, user, 2, "Reading the current table");
    if ((err = esp_flash_read(NULL, obuf, L5_PT_OFFSET, SECTOR)) != ESP_OK) {
        say(why, len, "Reading 0x8000 failed (%s). Nothing changed.", esp_err_to_name(err));
        goto out;
    }
    parse(obuf, now);
    if (!table_equal(now, &m->table)) {
        say(why, len, "The partition table changed since PMAN read it. Nothing changed.");
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }

    report(cb, user, 5, "Saving a copy of the old table");
    nvs_handle_t h;
    err = nvs_open("l5", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "pt_old", obuf, L5_PT_LEN);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        say(why, len, "Could not keep a copy of the old table in NVS (%s). Nothing changed.", esp_err_to_name(err));
        goto out;
    }
    if (ensure_backup_dir()) {
        char path[96];
        backup_name(path, sizeof(path), "partition-table", ".bin");
        FILE *f = fopen(path, "wb");
        bool ok = f && fwrite(obuf, 1, L5_PT_LEN, f) == L5_PT_LEN;
        ok = f && fclose(f) == 0 && ok;
        ESP_LOGI(TAG, "old table %s %s", ok ? "saved to" : "NOT saved to", path);
    }

    report(cb, user, 8, "Clearing otadata");
    const esp_partition_t *od = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (od && (err = esp_partition_erase_range(od, 0, od->size)) != ESP_OK) {
        say(why, len, "Clearing otadata failed (%s). The partition table was not touched.", esp_err_to_name(err));
        goto out;
    }

    /* erase -> write -> read back, as short as possible; IDF's own OTA path for the partition
       table lifts the dangerous-write guard the same way, only for this sector */
    report(cb, user, 12, "Writing the partition table");
    ESP_LOGW(TAG, "rewriting the partition table at 0x%x (%d entries)", L5_PT_OFFSET, next->n);
    esp_flash_set_dangerous_write_protection(esp_flash_default_chip, false);
    err = esp_flash_erase_region(NULL, L5_PT_OFFSET, SECTOR);
    if (err == ESP_OK) {
        err = esp_flash_write(NULL, nbuf, L5_PT_OFFSET, L5_PT_LEN);
    }
    if (err == ESP_OK) {
        err = esp_flash_read(NULL, rbuf, L5_PT_OFFSET, L5_PT_LEN);
    }
    if (err == ESP_OK && memcmp(rbuf, nbuf, L5_PT_LEN) != 0) {
        err = ESP_ERR_INVALID_CRC;
    }
    int count = 0;
    if (err == ESP_OK && esp_partition_table_verify((const esp_partition_info_t *)rbuf, true, &count) != ESP_OK) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "table write failed (%s), writing the old table back", esp_err_to_name(err));
        esp_err_t r = esp_flash_erase_region(NULL, L5_PT_OFFSET, SECTOR);
        if (r == ESP_OK) {
            r = esp_flash_write(NULL, obuf, L5_PT_OFFSET, SECTOR);
        }
        if (r == ESP_OK) {
            r = esp_flash_read(NULL, rbuf, L5_PT_OFFSET, SECTOR);
        }
        if (r == ESP_OK && memcmp(rbuf, obuf, SECTOR) != 0) {
            r = ESP_ERR_INVALID_CRC;
        }
        esp_flash_set_dangerous_write_protection(esp_flash_default_chip, true);
        if (r == ESP_OK) {
            say(why, len, "Writing the new table failed (%s). The old table was written back; apps and data "
                "are unchanged.", esp_err_to_name(err));
        } else {
            say(why, len, "Writing failed (%s) and the old table could not be restored (%s). Do not restart: "
                "flash partition-table.bin over USB.", esp_err_to_name(err), esp_err_to_name(r));
            ESP_LOGE(TAG, "partition table damaged");
        }
        goto out;
    }
    esp_flash_set_dangerous_write_protection(esp_flash_default_chip, true);
    *table_written = true;
    ESP_LOGW(TAG, "new partition table written and verified");

    /* what moved: app slot headers (no image is left at a slot start) and changed data areas */
    l5_pt_diff_t d;
    l5_pt_diff(m, next, &d);
    uint64_t total = 0, done = 0;
    for (int a = 0; a < d.nerase; a++) total += d.erase[a]->size;
    if (d.apps) {
        total += (uint64_t)BLOCK * 16;
    }
    for (int i = 0; d.apps && i < next->n; i++) {
        const l5_part_t *p = &next->p[i];
        if (is_ota_app(p) && p->offset >= m->pool_start) {
            report(cb, user, 20 + (int)(done * 78 / (total ? total : 1)), "Erasing the app slots");
            if ((err = raw_erase(m, p->offset, BLOCK, false)) != ESP_OK) {
                break;
            }
            done += BLOCK;
        }
    }
    if (err == ESP_OK && d.aurora_new) { /* new aurora_cfg on old FAT bytes: Aurora needs it erased */
        report(cb, user, 20 + (int)(done * 78 / (total ? total : 1)), "Preparing aurora_cfg");
        err = raw_erase(m, L5_AURORA_ADDR, L5_AURORA_SIZE, true);
    }
    for (int a = 0; a < d.nerase && err == ESP_OK; a++) { /* new, moved or resized data partitions */
        const l5_part_t *p = d.erase[a];
        char what[40];
        snprintf(what, sizeof(what), "Erasing %s", p->label);
        for (uint32_t off = 0; off < p->size && err == ESP_OK; off += BLOCK) {
            uint32_t n = p->size - off < BLOCK ? p->size - off : BLOCK;
            err = raw_erase(m, p->offset + off, n, false);
            done += n;
            report(cb, user, 20 + (int)(done * 78 / (total ? total : 1)), what);
            vTaskDelay(1);
        }
    }
    if (d.apps) {
        l5_slots_forget_all();
    }
    if (err != ESP_OK) {
        say(why, len, "The new table is written, but erasing stopped (%s). Restart and format the data "
            "partitions in PMAN.", esp_err_to_name(err));
    } else {
        report(cb, user, 100, "Done");
    }

out:
    heap_caps_free(nbuf);
    heap_caps_free(obuf);
    heap_caps_free(rbuf);
    heap_caps_free(now);
    return err;
}

/* ---------- aurora_cfg after a USB flash ---------- */

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool l5_pt_aurora_sanitize(void)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, L5_AURORA_LABEL);
    if (!p || p->address != L5_AURORA_ADDR || p->size != L5_AURORA_SIZE || p->encrypted) {
        return false;
    }
    uint8_t *page = heap_caps_malloc(SECTOR, MALLOC_CAP_DEFAULT);
    if (!page) {
        return false;
    }
    bool erased = true, nvs = false, ok = true;
    for (uint32_t off = 0; off < p->size; off += SECTOR) {
        if (esp_partition_read(p, off, page, SECTOR) != ESP_OK) {
            ok = false;
            break;
        }
        for (int i = 0; erased && i < SECTOR; i++) {
            if (page[i] != 0xff) erased = false;
        }
        /* NVS page header (nvs_page.hpp): ACTIVE / FULL / FREEING state and the CRC32 over bytes 4..27,
           the same header test as Aurora's probe_private_region(); any such page counts as NVS */
        const uint32_t state = le32(page);
        if ((state == 0xfffffffe || state == 0xfffffffc || state == 0xfffffff8) &&
            esp_rom_crc32_le(0xffffffff, page + 4, 24) == le32(page + 28)) {
            nvs = true;
        }
    }
    heap_caps_free(page);
    if (!ok || erased || nvs) {
        return false; /* erased: Aurora starts fresh; NVS: it may hold the PIN, never touched */
    }
    ESP_LOGW(TAG, "aurora_cfg holds neither erased flash nor NVS (old FAT bytes?): erasing its 24 KB for Aurora");
    return esp_partition_erase_range(p, 0, p->size) == ESP_OK;
}

/* ---------- data partition tools ---------- */

static bool editable(const esp_partition_t *p)
{
    if (!p || p->type != ESP_PARTITION_TYPE_DATA) {
        return false;
    }
    switch (p->subtype) {
    case ESP_PARTITION_SUBTYPE_DATA_OTA:
    case ESP_PARTITION_SUBTYPE_DATA_PHY:
    case ESP_PARTITION_SUBTYPE_DATA_NVS:
    case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS:
    case ESP_PARTITION_SUBTYPE_DATA_EFUSE_EM:
    case ESP_PARTITION_SUBTYPE_DATA_COREDUMP:
        return false;
    default:
        return true;
    }
}

static const esp_partition_t *data_part(const char *label)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    return editable(p) ? p : NULL;
}

bool l5_part_editable(const char *label)
{
    return data_part(label) != NULL;
}

static esp_err_t erase_part(const esp_partition_t *p, l5_pt_progress_t cb, void *user, int from, int span)
{
    for (uint32_t off = 0; off < p->size; off += BLOCK) {
        uint32_t n = p->size - off < BLOCK ? p->size - off : BLOCK;
        ESP_RETURN_ON_ERROR(esp_partition_erase_range(p, off, n), TAG, "erase %s", p->label);
        report(cb, user, from + (int)((uint64_t)(off + n) * span / p->size), NULL);
        vTaskDelay(1);
    }
    return ESP_OK;
}

esp_err_t l5_part_erase(const char *label, l5_pt_progress_t cb, void *user)
{
    const esp_partition_t *p = data_part(label);
    return p ? erase_part(p, cb, user, 0, 100) : ESP_ERR_NOT_FOUND;
}

esp_err_t l5_part_backup(const char *label, char *out_path, size_t len, l5_pt_progress_t cb, void *user)
{
    const esp_partition_t *p = data_part(label);
    if (!p) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!ensure_backup_dir()) {
        return ESP_ERR_INVALID_STATE;
    }
    backup_name(out_path, len, label, ".bin");
    uint8_t *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    FILE *f = fopen(out_path, "wb");
    esp_err_t err = buf && f ? ESP_OK : (buf ? ESP_FAIL : ESP_ERR_NO_MEM);
    for (uint32_t off = 0; err == ESP_OK && off < p->size; off += CHUNK) {
        uint32_t n = p->size - off < CHUNK ? p->size - off : CHUNK;
        err = esp_partition_read(p, off, buf, n);
        if (err == ESP_OK && fwrite(buf, 1, n, f) != n) {
            err = ESP_FAIL;
        }
        report(cb, user, (int)((uint64_t)(off + n) * 100 / p->size), NULL);
    }
    if (f && fclose(f) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    if (err != ESP_OK && f) {
        unlink(out_path);
    }
    heap_caps_free(buf);
    return err;
}

esp_err_t l5_part_restore(const char *label, const char *path, l5_pt_progress_t cb, void *user)
{
    const esp_partition_t *p = data_part(label);
    struct stat st;
    if (!p) {
        return ESP_ERR_NOT_FOUND;
    }
    if (stat(path, &st) != 0 || st.st_size <= 0 || (uint32_t)st.st_size > p->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *f = fopen(path, "rb");
    uint8_t *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    esp_err_t err = buf && f ? ESP_OK : (buf ? ESP_FAIL : ESP_ERR_NO_MEM);
    if (err == ESP_OK) {
        err = erase_part(p, cb, user, 0, 40);
    }
    uint32_t total = (uint32_t)st.st_size;
    for (uint32_t off = 0; err == ESP_OK && off < total;) {
        size_t n = fread(buf, 1, CHUNK, f);
        if (n == 0) {
            err = ESP_FAIL;
            break;
        }
        err = esp_partition_write(p, off, buf, n);
        off += n;
        report(cb, user, 40 + (int)((uint64_t)off * 60 / total), NULL);
    }
    if (f) {
        fclose(f);
    }
    heap_caps_free(buf);
    return err;
}

int l5_part_blank(const char *label)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    uint8_t *buf = p ? heap_caps_malloc(SECTOR, MALLOC_CAP_DEFAULT) : NULL;
    int r = -1;
    if (buf && esp_partition_read(p, 0, buf, SECTOR) == ESP_OK) {
        r = 1;
        for (int i = 0; i < SECTOR; i++) {
            if (buf[i] != 0xff) {
                r = 0;
                break;
            }
        }
    }
    heap_caps_free(buf);
    return r;
}

/* ---------- SANITY CHECK: the bootloader's image checks, on a file ---------- */

esp_err_t l5_image_check_file(const char *path, uint32_t offset, l5_image_check_t *o)
{
    memset(o, 0, sizeof(*o));
    struct stat st;
    FILE *f = fopen(path, "rb");
    if (!f || stat(path, &st) != 0) {
        if (f) {
            fclose(f);
        }
        strlcpy(o->error, "The file cannot be opened.", sizeof(o->error));
        return ESP_ERR_NOT_FOUND;
    }
    o->file_size = (uint32_t)st.st_size;
    uint8_t *buf = heap_caps_malloc(SECTOR, MALLOC_CAP_DEFAULT);
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    esp_image_header_t h;
    if (!buf) {
        err = ESP_ERR_NO_MEM;
        strlcpy(o->error, "Not enough memory.", sizeof(o->error));
        goto out;
    }
    if (fseek(f, offset, SEEK_SET) != 0 || fread(&h, 1, sizeof(h), f) != sizeof(h) || h.magic != ESP_IMAGE_HEADER_MAGIC) {
        strlcpy(o->error, "Not an ESP image (no 0xE9 header).", sizeof(o->error));
        goto out;
    }
    if (h.segment_count == 0 || h.segment_count > ESP_IMAGE_MAX_SEGMENTS) {
        snprintf(o->error, sizeof(o->error), "Header lists %u segments.", h.segment_count);
        goto out;
    }
    o->chip_id = h.chip_id;
    o->segments = h.segment_count;
    o->hash_appended = h.hash_appended == 1;
    mbedtls_sha256_update(&sha, (const uint8_t *)&h, sizeof(h));
    uint32_t checksum = 0xEF, pos = offset + sizeof(h); /* ESP_ROM_CHECKSUM_INITIAL, XOR of 32-bit words */
    for (int s = 0; s < h.segment_count; s++) {
        esp_image_segment_header_t sh;
        if (fread(&sh, 1, sizeof(sh), f) != sizeof(sh)) {
            snprintf(o->error, sizeof(o->error), "File ends inside segment %d.", s);
            goto out;
        }
        mbedtls_sha256_update(&sha, (const uint8_t *)&sh, sizeof(sh));
        pos += sizeof(sh);
        if (sh.data_len % 4 || sh.data_len > o->file_size - pos) {
            snprintf(o->error, sizeof(o->error), "Segment %d has an invalid length.", s);
            goto out;
        }
        for (uint32_t left = sh.data_len; left;) {
            uint32_t n = left < SECTOR ? left : SECTOR;
            if (fread(buf, 1, n, f) != n) {
                snprintf(o->error, sizeof(o->error), "File ends inside segment %d.", s);
                goto out;
            }
            mbedtls_sha256_update(&sha, buf, n);
            for (uint32_t i = 0; i < n; i += 4) {
                uint32_t w;
                memcpy(&w, buf + i, 4);
                checksum ^= w;
            }
            left -= n;
            pos += n;
        }
    }
    /* checksum byte at the end of the next 16-byte block (esp_image_format.c verify_checksum) */
    uint32_t length = (pos - offset + 1 + 15) & ~15u, pad = length - (pos - offset);
    if (fread(buf, 1, pad, f) != pad) {
        strlcpy(o->error, "File ends before the checksum byte.", sizeof(o->error));
        goto out;
    }
    mbedtls_sha256_update(&sha, buf, pad);
    uint8_t calc = (checksum >> 24) ^ (checksum >> 16) ^ (checksum >> 8) ^ checksum;
    o->checksum_ok = buf[pad - 1] == calc;
    o->image_len = length;
    if (o->hash_appended) {
        uint8_t digest[32], stored[32];
        mbedtls_sha256_finish(&sha, digest);
        o->hash_ok = fread(stored, 1, sizeof(stored), f) == sizeof(stored) && memcmp(digest, stored, 32) == 0;
        o->image_len += 32;
    }
    o->ok = o->checksum_ok && (!o->hash_appended || o->hash_ok);
    if (!o->ok) {
        strlcpy(o->error, !o->checksum_ok ? "Checksum does not match." : "SHA-256 does not match.", sizeof(o->error));
    }
    err = o->ok ? ESP_OK : ESP_ERR_INVALID_CRC;

out:
    mbedtls_sha256_free(&sha);
    heap_caps_free(buf);
    fclose(f);
    return err;
}
