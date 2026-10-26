#include "l5_fw.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cJSON.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"

#define APP_DESC_MAGIC 0xABCD5432 /* ESP_APP_DESC_MAGIC_WORD: esp_app_desc_t right after the first segment header */
#define CHIP_ID_ESP32P4 0x0012
#define MAX_PP (L5_MAX_EXTRA + 2) /* extras, the main FAT, the partition after aurora_cfg */

const char *l5_chip_name(uint16_t chip_id); /* l5_slots.c */

static void say(char *out, size_t len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void say(char *out, size_t len, const char *fmt, ...)
{
    if (!out || !len) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, len, fmt, ap);
    va_end(ap);
}

/* appends a sentence to notes */
static void note(char *out, size_t len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void note(char *out, size_t len, const char *fmt, ...)
{
    size_t o = strlen(out);
    if (o + 2 >= len) return;
    if (o) out[o++] = ' ', out[o] = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + o, len - o, fmt, ap);
    va_end(ap);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t up(uint32_t v, uint32_t a)
{
    return (v + a - 1) / a * a;
}

void l5_fw_size(char *out, size_t len, uint64_t b)
{
    if (b >= 1024 * 1024) {
        unsigned t = (unsigned)((b * 10 + 512 * 1024) / (1024 * 1024));
        if (t % 10) snprintf(out, len, "%u.%u MB", t / 10, t % 10);
        else snprintf(out, len, "%u MB", t / 10);
    } else if (b >= 1024) {
        snprintf(out, len, "%u KB", (unsigned)((b + 1023) / 1024));
    } else {
        snprintf(out, len, "%u B", (unsigned)b);
    }
}

const char *l5_fw_subtype_name(uint8_t sub)
{
    switch (sub) {
    case ESP_PARTITION_SUBTYPE_DATA_FAT: return "FAT";
    case ESP_PARTITION_SUBTYPE_DATA_SPIFFS: return "SPIFFS";
    case L5_SUBTYPE_LITTLEFS: return "LittleFS";
    case ESP_PARTITION_SUBTYPE_DATA_NVS: return "NVS";
    case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS: return "NVS keys";
    default: return "data";
    }
}

/* data partitions every table has its own of (the launcher's otadata, phy_init, coredump) */
static bool ignored_data(uint8_t sub)
{
    return sub == ESP_PARTITION_SUBTYPE_DATA_OTA || sub == ESP_PARTITION_SUBTYPE_DATA_PHY ||
           sub == ESP_PARTITION_SUBTYPE_DATA_COREDUMP || sub == ESP_PARTITION_SUBTYPE_DATA_EFUSE_EM;
}

bool l5_fw_system_part(const l5_fw_part_t *p)
{
    return (p->subtype == ESP_PARTITION_SUBTYPE_DATA_NVS && !strcmp(p->label, "nvs")) || !strcmp(p->label, L5_AURORA_LABEL);
}

void l5_fw_init(l5_fw_t *fw, l5_fw_format_t format)
{
    memset(fw, 0, sizeof(*fw));
    fw->format = format;
    fw->nsrc = 1;
    strcpy(fw->src[0].key, "firmware");
}

const char *l5_fw_source_url(const l5_fw_t *fw, const char *key)
{
    if (!key || !*key) key = "firmware";
    for (int i = 0; i < fw->nsrc; i++) {
        if (!strcmp(fw->src[i].key, key)) return fw->src[i].url[0] ? fw->src[i].url : NULL;
    }
    return NULL;
}

bool l5_fw_set_source(l5_fw_t *fw, const char *key, const char *url)
{
    if (!key || !*key) key = "firmware";
    int i = 0;
    while (i < fw->nsrc && strcmp(fw->src[i].key, key)) i++;
    if (i == fw->nsrc) {
        if (i == L5_FW_MAX_SRC || strlen(key) >= sizeof(fw->src[i].key)) return false;
        strcpy(fw->src[i].key, key);
        fw->nsrc++;
    }
    return strlcpy(fw->src[i].url, url ? url : "", sizeof(fw->src[i].url)) < sizeof(fw->src[i].url);
}

uint32_t l5_fw_app_bound(const l5_fw_t *fw)
{
    if (fw->app_size) return fw->app_size;
    uint32_t b = fw->app_part_size ? fw->app_part_size : UINT32_MAX;
    if (!fw->app_source[0]) { /* the main file: up to its end or the next image in it */
        if (fw->file_size > fw->app_offset && fw->file_size - fw->app_offset < b) b = fw->file_size - fw->app_offset;
        for (int i = 0; i < fw->nparts; i++) {
            const l5_fw_part_t *p = &fw->part[i];
            if (!p->source[0] && p->copy_size && p->src_offset > fw->app_offset && p->src_offset - fw->app_offset < b)
                b = p->src_offset - fw->app_offset;
        }
    }
    return b == UINT32_MAX ? 0 : b;
}

/* ---------- the firmware's own table (merged image) ---------- */

int32_t l5_fw_merged_base(const uint8_t *head, size_t len)
{
    if (len >= L5_FW_TABLE_OFFSET + 2 && head[0] != 0xE9 && head[L5_FW_BOOT_OFFSET] == 0xE9 &&
        le16(head + L5_FW_TABLE_OFFSET) == ESP_PARTITION_MAGIC) {
        return 0; /* 0xFF up to the bootloader at 0x2000, table at 0x8000: the file is flash from 0x0 */
    }
    const size_t table = L5_FW_TABLE_OFFSET - L5_FW_BOOT_OFFSET;
    if (len >= table + 2 && len >= 36 && head[0] == 0xE9 && le32(head + 32) != APP_DESC_MAGIC &&
        le16(head + table) == ESP_PARTITION_MAGIC) {
        return L5_FW_BOOT_OFFSET; /* it starts with the bootloader, the table 0x6000 later */
    }
    return -1;
}

esp_err_t l5_fw_from_table(l5_fw_t *fw, const uint8_t *table, size_t len, uint32_t file_size, uint32_t base)
{
    int rank = 9;
    fw->nparts = 0;
    fw->base = base;
    fw->table_end = 0;
    for (size_t off = 0; off + sizeof(esp_partition_info_t) <= len; off += sizeof(esp_partition_info_t)) {
        esp_partition_info_t e;
        memcpy(&e, table + off, sizeof(e));
        if (e.magic == ESP_PARTITION_MAGIC_MD5) continue;
        if (e.magic != ESP_PARTITION_MAGIC) break;
        if (e.pos.offset + e.pos.size > fw->table_end) fw->table_end = e.pos.offset + e.pos.size;
        if (e.pos.offset < base) continue; /* nothing of it can be in the file */
        uint32_t at = e.pos.offset - base; /* where its image starts in the file */
        if (e.type == ESP_PARTITION_TYPE_APP) {
            /* the app a merged image boots while otadata is empty: factory, else ota_0, else the first */
            int r = e.subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY ? 0 : e.subtype == ESP_PARTITION_SUBTYPE_APP_OTA_MIN ? 1 : 2;
            if (r < rank) {
                rank = r;
                fw->app_offset = at;
                fw->app_part_size = e.pos.size;
                memcpy(fw->app_label, e.label, sizeof(e.label));
                fw->app_label[sizeof(fw->app_label) - 1] = 0;
            }
        } else if (e.type == ESP_PARTITION_TYPE_DATA && !ignored_data(e.subtype) && fw->nparts < L5_FW_MAX_PARTS) {
            l5_fw_part_t *p = &fw->part[fw->nparts++];
            memset(p, 0, sizeof(*p));
            memcpy(p->label, e.label, sizeof(e.label));
            p->type = e.type;
            p->subtype = e.subtype;
            p->offset = e.pos.offset;
            p->src_offset = at;
            p->size = e.pos.size;
            if (file_size > at) {
                p->copy_size = file_size - at < e.pos.size ? file_size - at : e.pos.size;
            }
        }
    }
    if (rank == 9) return ESP_ERR_NOT_FOUND;
    fw->file_size = file_size;
    fw->has_table = true;
    return ESP_OK;
}

esp_err_t l5_fw_from_probe(l5_fw_t *fw, const uint8_t *head, size_t len, uint32_t file_size, char *why, size_t wlen)
{
    int32_t base = l5_fw_merged_base(head, len);
    const uint8_t *hdr = base == 0 ? head + L5_FW_BOOT_OFFSET : head; /* the header that names the chip */
    if (base < 0 && (len < 24 || head[0] != 0xE9)) {
        say(why, wlen, "Not an ESP firmware image (no 0xE9 header).");
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t chip = le16(hdr + 12);
    if (chip != CHIP_ID_ESP32P4) {
        say(why, wlen, "The image is for %s, not ESP32-P4.", l5_chip_name(chip));
        return ESP_ERR_NOT_SUPPORTED;
    }
    fw->file_size = file_size;
    if (base < 0 && len >= 36 && le32(head + 32) == APP_DESC_MAGIC) { /* an app image: nothing else in the file */
        fw->format = L5_FW_APP_ONLY;
        fw->app_offset = 0;
        fw->app_size = file_size; /* L5.fits checks the file size, the header gives the exact length later */
        fw->nparts = 0;
        fw->has_table = true;
        return ESP_OK;
    }
    if (base < 0) {
        say(why, wlen, "The image is neither an app nor a merged image (a bootloader alone?).");
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* merged: bootloader, the table at 0x8000 and the app and data images at their flash offsets - base */
    fw->format = L5_FW_MERGED;
    size_t at = L5_FW_TABLE_OFFSET - (size_t)base;
    if (l5_fw_from_table(fw, head + at, len - at < L5_PT_LEN ? len - at : L5_PT_LEN, file_size, (uint32_t)base) != ESP_OK) {
        say(why, wlen, "The partition table inside the image has no app partition.");
        return ESP_ERR_NOT_FOUND;
    }
    if (base == 0 && file_size >= 0x400000 && !(file_size & (file_size - 1)) && file_size >= fw->table_end) {
        say(why, wlen, "This file is a copy of a whole %u MB flash (bootloader, partition table and every partition), "
            "not a firmware: write it back with esptool if you need it. The launcher does not install it.",
            (unsigned)(file_size >> 20));
        return ESP_ERR_INVALID_SIZE;
    }
    if (!strcmp(fw->app_label, "launcher")) {
        say(why, wlen, "This image boots the Tab5 Launcher itself (its app partition is \"launcher\"): it is not "
            "installed into an app slot.");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

/* ---------- launcherhub install manifest ---------- */

static const cJSON *jobj(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsObject(v) ? v : NULL;
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

static bool jnum(const cJSON *o, const char *k, uint32_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    double d;
    if (cJSON_IsNumber(v)) {
        d = v->valuedouble;
    } else if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) { /* "0x10000" */
        char *end;
        d = (double)strtoull(v->valuestring, &end, 0);
        if (*end) return false;
    } else {
        return false;
    }
    if (d < 0 || d > 4294967295.0) return false;
    *out = (uint32_t)d;
    return true;
}

static int data_subtype(const char *s)
{
    static const struct {
        const char *n;
        uint8_t v;
    } T[] = {
        {"fat", 0x81}, {"spiffs", 0x82}, {"littlefs", 0x83}, {"nvs", 0x02}, {"ota", 0x00}, {"phy", 0x01},
        {"coredump", 0x03}, {"nvs_keys", 0x04}, {"efuse", 0x05}, {"undefined", 0x06}, {"esphttpd", 0x80},
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        if (!strcasecmp(s, T[i].n)) return T[i].v;
    }
    char *end;
    long v = strtol(s, &end, 0);
    return *s && !*end && v >= 0 && v < 0xff ? (int)v : -1;
}

esp_err_t l5_fw_from_manifest(l5_fw_t *fw, const char *json, char *why, size_t len)
{
    cJSON *root = cJSON_Parse(json);
    esp_err_t err = ESP_OK;
    const cJSON *ver = jobj(root, "version"), *inst = jobj(ver, "install");
    if (!cJSON_IsObject(root)) {
        say(why, len, "The catalog answer is not JSON.");
        err = ESP_ERR_INVALID_RESPONSE;
        goto out;
    }
    if (*jstr(root, "fid")) strlcpy(fw->fid, jstr(root, "fid"), sizeof(fw->fid));
    if (*jstr(root, "name")) strlcpy(fw->name, jstr(root, "name"), sizeof(fw->name));
    if (*jstr(root, "author")) strlcpy(fw->author, jstr(root, "author"), sizeof(fw->author));
    const char *esp = jstr(root, "esp");
    if (*esp && strcasecmp(esp, "p4") && strcasecmp(esp, "esp32p4") && strcasecmp(esp, "esp32-p4")) {
        char chip[16];
        size_t i = 0;
        for (; esp[i] && i + 1 < sizeof(chip); i++) chip[i] = (char)toupper((unsigned char)esp[i]);
        chip[i] = 0;
        say(why, len, "This firmware is for ESP32-%s, not ESP32-P4.", chip);
        err = ESP_ERR_NOT_SUPPORTED;
        goto out;
    }
    const char *file = jstr(ver, "file");
    if (!ver || !*file) {
        say(why, len, "The catalog gives no file for this version.");
        err = ESP_ERR_INVALID_RESPONSE;
        goto out;
    }
    strlcpy(fw->version, jstr(ver, "version"), sizeof(fw->version));
    strlcpy(fw->date, jstr(ver, "published_at"), sizeof(fw->date));
    l5_fw_set_source(fw, "firmware", file);
    if (!inst) {
        say(why, len, "The catalog has no install data for this version.");
        err = ESP_ERR_NOT_FOUND;
        goto out;
    }
    const char *target = jstr(inst, "target");
    if (*target && strcasecmp(target, "tab5")) {
        say(why, len, "This firmware is made for %.40s, not for the Tab5.", target);
        err = ESP_ERR_NOT_SUPPORTED;
        goto out;
    }
    const cJSON *app = jobj(inst, "app"), *s, *p;
    jnum(app, "source_offset", &fw->app_offset);
    jnum(app, "image_size", &fw->app_size);
    jnum(app, "partition_size", &fw->app_part_size);
    cJSON_ArrayForEach(s, jobj(inst, "sources")) {
        if (cJSON_IsString(s) && s->string && strcmp(s->string, "firmware")) l5_fw_set_source(fw, s->string, s->valuestring);
    }
    bool multi = false;
    fw->nparts = 0;
    cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(inst, "partitions")) {
        const char *src = jstr(p, "source");
        if (!strcmp(src, "firmware")) src = "";
        if (!strcasecmp(jstr(p, "type"), "app")) {
            if (!fw->app_part_size) jnum(p, "size", &fw->app_part_size);
            if (!fw->app_size) jnum(p, "copy_size", &fw->app_size);
            if (!fw->app_offset) jnum(p, "source_offset", &fw->app_offset);
            strlcpy(fw->app_source, src, sizeof(fw->app_source));
            continue;
        }
        int st = data_subtype(jstr(p, "subtype"));
        if (strcasecmp(jstr(p, "type"), "data") || st < 0 || ignored_data((uint8_t)st) || fw->nparts == L5_FW_MAX_PARTS) {
            continue;
        }
        l5_fw_part_t *q = &fw->part[fw->nparts];
        memset(q, 0, sizeof(*q));
        strlcpy(q->label, jstr(p, "label"), sizeof(q->label));
        strlcpy(q->source, src, sizeof(q->source));
        q->type = ESP_PARTITION_TYPE_DATA;
        q->subtype = (uint8_t)st;
        jnum(p, "size", &q->size);
        jnum(p, "source_offset", &q->src_offset);
        jnum(p, "copy_size", &q->copy_size);
        if (!jnum(p, "flash_offset", &q->offset)) jnum(p, "offset", &q->offset);
        if (q->copy_size > q->size || (q->copy_size && !l5_fw_source_url(fw, q->source))) {
            q->copy_size = 0; /* an image larger than its partition, or from a file the entry does not list */
        }
        if (!q->label[0] || !q->size) continue;
        multi |= q->source[0] && q->copy_size;
        fw->nparts++;
    }
    multi |= fw->app_source[0] != 0;
    fw->format = multi ? L5_FW_MULTI : (!strcasecmp(jstr(inst, "format"), "merged") || fw->app_offset) ? L5_FW_MERGED
                                                                                                       : L5_FW_APP_ONLY;
    fw->has_table = true;
out:
    cJSON_Delete(root);
    return err;
}

/* ---------- compared with a partition table ---------- */

static const l5_part_t *by_label(const l5_ptable_t *t, const char *label)
{
    for (int i = 0; i < t->n; i++) {
        if (!strcmp(t->p[i].label, label)) return &t->p[i];
    }
    return NULL;
}

const l5_part_t *l5_fw_match(const l5_fw_part_t *p, const l5_ptable_t *t)
{
    const l5_part_t *q = by_label(t, p->label);
    if (!q || l5_fw_system_part(p)) return NULL;
    return q->type == p->type && q->subtype == p->subtype && q->size == p->size ? q : NULL;
}

void l5_fw_check(const l5_fw_t *fw, const l5_ptable_t *cur, uint32_t slot_size, l5_fw_report_t *rep)
{
    memset(rep, 0, sizeof(*rep));
    for (int i = 0; !slot_size && i < cur->n; i++) { /* largest ota_N */
        const l5_part_t *q = &cur->p[i];
        if (q->type == ESP_PARTITION_TYPE_APP && q->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
            q->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX && q->size > rep->slot_size)
            rep->slot_size = q->size;
    }
    if (slot_size) rep->slot_size = slot_size;
    rep->app_need = l5_fw_app_bound(fw);
    rep->app_known = fw->app_size > 0;
    /* an upper bound only (merged image without catalog data): the image itself decides while installing */
    rep->app_fits = !rep->app_known || rep->app_need <= rep->slot_size;
    for (int i = 0; i < fw->nparts && rep->n < L5_FW_MAX_PARTS; i++) {
        const l5_fw_part_t *p = &fw->part[i];
        const l5_part_t *q = by_label(cur, p->label);
        l5_fw_item_t *it = &rep->item[rep->n];
        memset(it, 0, sizeof(*it));
        strlcpy(it->label, p->label, sizeof(it->label));
        it->subtype = p->subtype;
        it->want = p->size;
        it->have = q ? q->size : 0;
        if (l5_fw_system_part(p)) {
            if (!q || q->size >= p->size) continue; /* the launcher's own is as large: nothing to say */
            it->st = L5_FWI_INFO;
            rep->n++;
            continue;
        }
        it->image = p->copy_size > 0;
        if (!q) it->st = L5_FWI_MISSING;
        else if (q->type != p->type || q->subtype != p->subtype) it->st = L5_FWI_TYPE;
        else if (q->size == p->size) it->st = L5_FWI_OK;
        else it->st = q->size > p->size ? L5_FWI_LARGER : L5_FWI_SMALL;
        it->writable = it->image && it->st == L5_FWI_OK;
        rep->problems += it->st == L5_FWI_MISSING || it->st == L5_FWI_SMALL || it->st == L5_FWI_TYPE;
        rep->lost += it->image && !it->writable;
        rep->images += it->writable;
        rep->n++;
    }
}

/* problems first, then images that are not written, then the rest */
static int item_rank(const l5_fw_item_t *it)
{
    if (it->st == L5_FWI_MISSING || it->st == L5_FWI_SMALL || it->st == L5_FWI_TYPE) return 0;
    if (it->image && !it->writable) return 1;
    return it->st == L5_FWI_INFO ? 2 : 3;
}

void l5_fw_report_text(const l5_fw_report_t *rep, char *out, size_t len)
{
    char need[16], slot[16], want[16], have[16];
    size_t o = 0;
    l5_fw_size(need, sizeof(need), rep->app_need);
    l5_fw_size(slot, sizeof(slot), rep->slot_size);
    if (!rep->app_need) o += snprintf(out, len, "app \xC2\xB7 size read from the image \xC2\xB7 slot %s", slot);
    else o += snprintf(out, len, "app \xC2\xB7 %s%s \xC2\xB7 slot %s%s", rep->app_known ? "" : "up to ", need, slot,
                       !rep->app_fits ? ": too small" : rep->app_known ? ": fits" : "");
    for (int r = 0; r < 4; r++) {
        for (int i = 0; i < rep->n && o < len; i++) {
            const l5_fw_item_t *it = &rep->item[i];
            if (item_rank(it) != r) continue;
            l5_fw_size(want, sizeof(want), it->want);
            l5_fw_size(have, sizeof(have), it->have);
            char st[48];
            switch (it->st) {
            case L5_FWI_MISSING: snprintf(st, sizeof(st), "missing"); break;
            case L5_FWI_TYPE: snprintf(st, sizeof(st), "another type here"); break;
            case L5_FWI_SMALL: snprintf(st, sizeof(st), "%s here: too small", have); break;
            case L5_FWI_INFO: snprintf(st, sizeof(st), "%s here (its settings may not fit)", have); break;
            case L5_FWI_LARGER: snprintf(st, sizeof(st), "%s here%s", have, it->image ? ": image not written" : ""); break;
            default: snprintf(st, sizeof(st), it->image ? "image written" : "ok"); break;
            }
            o += snprintf(out + o, len - o, "\n%s \xC2\xB7 %s %s%s \xC2\xB7 %s", it->label, l5_fw_subtype_name(it->subtype),
                          want, it->image ? " + image" : "", st);
        }
    }
}

/* ---------- AUTO PARTITION ---------- */

typedef struct {
    const l5_fw_part_t *p;
    uint32_t kb, want; /* KB in the plan / as the firmware asks */
    bool image;        /* its image is written: the size stays exact */
    bool gap;          /* placed after aurora_cfg */
    bool fat;          /* the layout's main FAT (64 KB steps, at least 448 KB) */
} pp_t;

static bool plan_label_ok(const char *l)
{
    static const char *const R[] = {"nvs", "otadata", "launcher", "factory", L5_AURORA_LABEL, "coredump", "phy_init"};
    if (!l[0] || !strncmp(l, "app", 3)) return false;
    for (size_t i = 0; i < sizeof(R) / sizeof(R[0]); i++) {
        if (!strcmp(l, R[i])) return false;
    }
    return true;
}

static bool plan_kind_ok(uint8_t sub)
{
    return sub == ESP_PARTITION_SUBTYPE_DATA_FAT || sub == ESP_PARTITION_SUBTYPE_DATA_SPIFFS || sub == L5_SUBTYPE_LITTLEFS ||
           sub == L5_SUBTYPE_UNDEFINED || sub == ESP_PARTITION_SUBTYPE_DATA_NVS;
}

static uint32_t front_kb(const pp_t *c, int n)
{
    uint32_t kb = 0;
    for (int i = 0; i < n; i++) kb += c[i].gap ? 0 : c[i].kb;
    return kb;
}

static uint32_t floor_kb(const pp_t *c)
{
    return c->want < L5_MIN_DATA_KB ? c->want : L5_MIN_DATA_KB;
}

/* partitions without an image give up space, largest first, down to 448 KB (or their own size) */
static void shrink(pp_t *c, int n, uint32_t avail)
{
    for (uint32_t front = front_kb(c, n); front > avail; front = front_kb(c, n)) {
        int k = -1;
        for (int i = 0; i < n; i++) {
            if (!c[i].gap && !c[i].image && c[i].kb > floor_kb(&c[i]) && (k < 0 || c[i].kb > c[k].kb)) k = i;
        }
        if (k < 0) return;
        uint32_t room = c[k].kb - floor_kb(&c[k]), cut = front - avail < room ? front - avail : room;
        uint32_t kb = c[k].kb - cut;
        if (c[k].fat) kb = kb / 64 * 64 < floor_kb(&c[k]) ? floor_kb(&c[k]) : kb / 64 * 64;
        if (kb == c[k].kb) return;
        c[k].kb = kb;
    }
}

esp_err_t l5_fw_plan(const l5_fw_t *fw, l5_layout_t *out, char *notes, size_t len)
{
    char a[16], b[16];
    memset(out, 0, sizeof(*out));
    if (len) notes[0] = 0;
    uint32_t need = l5_fw_app_bound(fw);
    if (!need) {
        say(notes, len, "The size of the app is not known.");
        return ESP_ERR_INVALID_SIZE;
    }
    uint32_t slot = up(up(need, 1024) / 1024, 64);
    if (slot < L5_MIN_APP_KB) slot = L5_MIN_APP_KB;
    if (slot > L5_FRONT_KB) {
        l5_fw_size(a, sizeof(a), need);
        say(notes, len, "The app needs %s, the flash has %u KB for app slots.", a, (unsigned)L5_FRONT_KB);
        return ESP_ERR_INVALID_SIZE;
    }
    pp_t c[MAX_PP];
    int n = 0;
    for (int i = 0; i < fw->nparts; i++) {
        const l5_fw_part_t *p = &fw->part[i];
        if (l5_fw_system_part(p)) continue;
        if (!plan_label_ok(p->label) || !plan_kind_ok(p->subtype) || n == MAX_PP) {
            note(notes, len, "Left out: %s (%s).", p->label,
                 n == MAX_PP ? "too many partitions" : !plan_kind_ok(p->subtype) ? "type not supported" : "reserved name");
            continue;
        }
        c[n] = (pp_t){.p = p, .kb = up(p->size, 4096) / 1024, .image = p->copy_size > 0};
        c[n].want = c[n].kb;
        n++;
    }
    /* the 872 KB after aurora_cfg: a partition with an image that fits there as it is, else the largest */
    int g = -1;
    for (int i = 0; i < n; i++) {
        if (c[i].kb > L5_GAP_KB) continue;
        if (g < 0 || (c[i].image && !c[g].image) || (c[i].image == c[g].image && c[i].kb > c[g].kb)) g = i;
    }
    if (g >= 0) c[g].gap = true;
    /* the main FAT of the layout: "vfs" if the firmware has one that qualifies, else the largest */
    int f = -1;
    for (int i = 0; i < n; i++) {
        if (c[i].gap || c[i].p->subtype != ESP_PARTITION_SUBTYPE_DATA_FAT || c[i].kb % 64 || c[i].kb < L5_MIN_DATA_KB) continue;
        if (f < 0 || !strcmp(c[i].p->label, "vfs") || (strcmp(c[f].p->label, "vfs") && c[i].kb > c[f].kb)) f = i;
    }
    if (f >= 0) c[f].fat = true;
    for (int extras = n - (g >= 0) - (f >= 0); extras > L5_MAX_EXTRA; extras--) { /* drop the smallest */
        int k = -1;
        for (int i = 0; i < n; i++) {
            if (!c[i].gap && !c[i].fat && (k < 0 || (!c[i].image && c[k].image) || (c[i].image == c[k].image && c[i].kb < c[k].kb)))
                k = i;
        }
        note(notes, len, "Left out: %s (too many partitions).", c[k].p->label);
        memmove(&c[k], &c[k + 1], (n - k - 1) * sizeof(c[0]));
        n--;
    }
    const uint32_t avail = L5_FRONT_KB - slot;
    shrink(c, n, avail);
    while (front_kb(c, n) > avail) { /* then images go, largest first (only where that frees space) */
        int k = -1;
        for (int i = 0; i < n; i++) {
            if (!c[i].gap && c[i].image && c[i].kb > floor_kb(&c[i]) && (k < 0 || c[i].kb > c[k].kb)) k = i;
        }
        if (k < 0) break;
        c[k].image = false;
        shrink(c, n, avail);
    }
    if (front_kb(c, n) > avail) {
        l5_fw_size(a, sizeof(a), need);
        say(notes, len, "The app (%s) and the data partitions need %lu KB, %u KB fit before aurora_cfg.", a,
            (unsigned long)(slot + front_kb(c, n)), (unsigned)L5_FRONT_KB);
        return ESP_ERR_INVALID_SIZE;
    }
    out->n = 1;
    out->slot_kb = slot + (avail - front_kb(c, n)) / 64 * 64; /* what is left: room for updates of this app */
    bool spiffs_label = false;
    for (int i = 0; i < n; i++) {
        const char *label = c[i].p->label;
        spiffs_label |= !strcmp(label, "spiffs");
        if (c[i].image != (c[i].p->copy_size > 0)) { /* the app finds it empty: no copy of the image's files */
            l5_fw_size(a, sizeof(a), (uint64_t)c[i].kb * 1024);
            l5_fw_size(b, sizeof(b), (uint64_t)c[i].want * 1024);
            if (c[i].kb != c[i].want) note(notes, len, "%s gets %s instead of %s and stays empty (no room for its image):", label, a, b);
            else note(notes, len, "%s stays empty (no room for its image):", label);
            note(notes, len, "the app formats it when it starts, the files in that image are not copied.");
        } else if (c[i].kb != c[i].want) {
            l5_fw_size(a, sizeof(a), (uint64_t)c[i].kb * 1024);
            l5_fw_size(b, sizeof(b), (uint64_t)c[i].want * 1024);
            note(notes, len, "%s: %s instead of %s (no room).", label, a, b);
        }
        if (c[i].gap) {
            out->spiffs_kb = c[i].kb;
            if (strcmp(label, "spiffs")) strlcpy(out->gap_label, label, sizeof(out->gap_label));
            if (c[i].p->subtype != ESP_PARTITION_SUBTYPE_DATA_SPIFFS) out->gap_subtype = c[i].p->subtype;
        } else if (c[i].fat) {
            out->fat_kb = c[i].kb;
            if (strcmp(label, "vfs")) strlcpy(out->fat_label, label, sizeof(out->fat_label));
        } else {
            l5_extra_t *x = &out->extra[out->nextra++];
            strlcpy(x->label, label, sizeof(x->label));
            x->subtype = c[i].p->subtype;
            x->kb = c[i].kb;
        }
    }
    if (g < 0 && !spiffs_label) out->spiffs_kb = L5_GAP_KB; /* nothing of the firmware there: the launcher's spiffs */
    return ESP_OK;
}

void l5_layout_text(const l5_layout_t *l, char *out, size_t len)
{
    char s[16];
    size_t o;
    l5_fw_size(s, sizeof(s), (uint64_t)l->slot_kb * 1024);
    if (l->n == 1) {
        o = snprintf(out, len, "app0 %s", s);
    } else if (l->app0_kb) { /* app0 of its own size before the others */
        char a[16];
        l5_fw_size(a, sizeof(a), (uint64_t)l->app0_kb * 1024);
        o = snprintf(out, len, "app0 %s + %lu x app %s", a, (unsigned long)(l->n - 1), s);
    } else {
        o = snprintf(out, len, "%lu x app %s", (unsigned long)l->n, s);
    }
    for (uint32_t i = 0; i < l->nextra && o < len; i++) {
        l5_fw_size(s, sizeof(s), (uint64_t)l->extra[i].kb * 1024);
        o += snprintf(out + o, len - o, " \xC2\xB7 %s %s %s", l->extra[i].label, l5_fw_subtype_name(l->extra[i].subtype), s);
    }
    if (l->fat_kb && o < len) {
        l5_fw_size(s, sizeof(s), (uint64_t)l->fat_kb * 1024);
        o += snprintf(out + o, len - o, " \xC2\xB7 %s FAT %s", l5_layout_fat_label(l), s);
    }
    if (l->spiffs_kb && o < len) {
        l5_fw_size(s, sizeof(s), (uint64_t)l->spiffs_kb * 1024);
        snprintf(out + o, len - o, " \xC2\xB7 %s %s %s", l5_layout_gap_label(l),
                 l5_fw_subtype_name(l->gap_subtype ? l->gap_subtype : ESP_PARTITION_SUBTYPE_DATA_SPIFFS), s);
    }
}

/* ---------- driver names in an app image ---------- */

static const char *const HW_NAMES[] = {"gt911", "ili9881", "st7123", "st7121", "st712x"}; /* L5_HW_* bit order */

/* names that start at d[0 .. starts) and end within d[0 .. n); `| 0x20` folds A-Z to a-z and keeps digits */
static void hw_match(l5_hw_scan_t *s, const uint8_t *d, size_t n, size_t starts)
{
    for (size_t i = 0; i < starts; i++) {
        uint8_t c = d[i] | 0x20;
        if (c != 'g' && c != 'i' && c != 's') continue;
        for (int k = 0; k < (int)(sizeof(HW_NAMES) / sizeof(HW_NAMES[0])); k++) {
            const char *p = HW_NAMES[k];
            size_t j = 0;
            while (p[j] && i + j < n && (d[i + j] | 0x20) == (uint8_t)p[j]) j++;
            if (!p[j]) s->found |= (uint8_t)(1u << k);
        }
    }
}

void l5_hw_scan_feed(l5_hw_scan_t *s, const uint8_t *d, size_t n)
{
    uint8_t join[12];
    size_t take = n < 6 ? n : 6;
    memcpy(join, s->tail, s->ntail);
    memcpy(join + s->ntail, d, take);
    hw_match(s, join, s->ntail + take, s->ntail); /* names that began in the previous chunk */
    hw_match(s, d, n, n);
    size_t total = s->ntail + n, keep = total < 6 ? total : 6;
    if (n >= keep) {
        memcpy(s->tail, d + n - keep, keep);
    } else { /* a short chunk: part of the old tail stays */
        memmove(s->tail, s->tail + s->ntail - (keep - n), keep - n);
        memcpy(s->tail + keep - n, d, n);
    }
    s->ntail = (uint8_t)keep;
}

bool l5_hw_old_only(const l5_hw_scan_t *s)
{
    return (s->found & L5_HW_OLD) && !(s->found & L5_HW_NEW);
}

/* ---------- ESP app image length while streaming ---------- */

void l5_img_begin(l5_img_parse_t *p)
{
    memset(p, 0, sizeof(*p));
}

static bool img_fail(l5_img_parse_t *p, const char *msg)
{
    p->failed = true;
    strlcpy(p->error, msg, sizeof(p->error));
    return false;
}

bool l5_img_feed(l5_img_parse_t *p, const uint8_t *d, size_t n)
{
    while (n && !p->failed && !p->len) {
        if (p->pos < p->want) { /* segment data */
            uint32_t k = p->want - p->pos < n ? p->want - p->pos : (uint32_t)n;
            p->pos += k, d += k, n -= k;
            continue;
        }
        uint32_t hl = p->nseg ? 8 : 24, k = hl - p->have < n ? hl - p->have : (uint32_t)n;
        memcpy(p->hdr + p->have, d, k);
        p->have += k, p->pos += k, d += k, n -= k;
        if (p->have < hl) break;
        p->have = 0;
        if (!p->nseg) { /* esp_image_header_t */
            if (p->hdr[0] != 0xE9) return img_fail(p, "Not an ESP image (no 0xE9 header).");
            if (!p->hdr[1] || p->hdr[1] > 16) return img_fail(p, "The image header lists an invalid segment count.");
            p->nseg = p->hdr[1];
            p->chip = le16(p->hdr + 12);
            p->rev_min = le16(p->hdr + 15);
            p->rev_max = le16(p->hdr + 17);
            p->hash = p->hdr[23];
            p->want = p->pos;
        } else { /* esp_image_segment_header_t: load address, data length */
            uint32_t dl = le32(p->hdr + 4);
            if (dl & 3 || dl >= 16 * 1024 * 1024) return img_fail(p, "A segment of the image has an invalid length.");
            p->want = p->pos + dl;
            if (++p->seg == p->nseg) { /* checksum byte padded to 16, then the SHA-256 */
                p->len = ((p->want + 1 + 15) & ~15u) + (p->hash == 1 ? 32 : 0);
            }
        }
    }
    return !p->failed;
}

uint32_t l5_img_skippable(const l5_img_parse_t *p)
{
    return p->len || p->failed || p->pos >= p->want ? 0 : p->want - p->pos;
}

void l5_img_skip(l5_img_parse_t *p, uint32_t n)
{
    p->pos += n;
}
