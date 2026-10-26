#include "l5_slots.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "bootloader_common.h"
#include "esp_check.h"
#include "esp_flash_partitions.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/efuse_hal.h"
#include "nvs.h"
#include "soc/lp_system_reg.h"
#include "soc/soc.h"
#include "l5_guard.h"
#include "l5_ptable.h"

static const char *TAG = "l5_slots";
#define NVS_NS "l5"
#define SECTOR 0x1000

l5_slots_t g_slots = {.boot = -1};

/* fixed-size char field (maybe without a NUL) into a C string */
static void field_copy(char *dst, size_t len, const char *src, size_t src_len)
{
    size_t n = strnlen(src, src_len);
    n = n < len ? n : len - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* strlcpy that never cuts a UTF-8 sequence in half */
static void utf8_copy(char *dst, size_t len, const char *src)
{
    size_t n = strlen(src);
    if (n >= len) {
        n = len - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* ---------- app registry: NVS "l5" / "appN" (what L5.install keeps in S.slots[i]) ---------- */

typedef struct {
    uint8_t elf_sha[32]; /* the entry only applies to the image it was written for */
    uint32_t size;
    char name[L5_NAME_LEN];
    char src[8];
    char date[11];
} slot_meta_v1_t;

typedef struct { /* v2 (OTA): v1 + catalog author, firmware id and version; v1 entries still read */
    slot_meta_v1_t v1;
    char author[33];
    char fid[33];
    char ver[24];
} slot_meta_t;

static void meta_key(char key[16], int index)
{
    snprintf(key, 16, "app%d", index);
}

static bool meta_read(int index, slot_meta_t *m)
{
    nvs_handle_t h;
    char key[16];
    size_t len = sizeof(*m);
    bool ok = false;
    memset(m, 0, sizeof(*m));
    meta_key(key, index);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_blob(h, key, m, &len) == ESP_OK && (len == sizeof(*m) || len == sizeof(m->v1));
        nvs_close(h);
    }
    if (ok && len == sizeof(m->v1)) {
        memset((uint8_t *)m + sizeof(m->v1), 0, sizeof(*m) - sizeof(m->v1));
    }
    if (ok) {
        m->v1.name[sizeof(m->v1.name) - 1] = m->v1.src[sizeof(m->v1.src) - 1] = m->v1.date[sizeof(m->v1.date) - 1] = 0;
        m->author[sizeof(m->author) - 1] = m->fid[sizeof(m->fid) - 1] = m->ver[sizeof(m->ver) - 1] = 0;
    }
    return ok;
}

static esp_err_t meta_write(int index, const slot_meta_t *m)
{
    nvs_handle_t h;
    char key[16];
    meta_key(key, index);
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs open");
    esp_err_t err = nvs_set_blob(h, key, m, sizeof(*m));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void meta_erase(int index)
{
    nvs_handle_t h;
    char key[16];
    meta_key(key, index);
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_erase_key(h, key) == ESP_OK) {
            nvs_commit(h);
        }
        nvs_close(h);
    }
}

static void today(char out[11])
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 >= 2024) { /* the RTC was set */
        strftime(out, 11, "%Y-%m-%d", &tm);
    } else {
        out[0] = 0;
    }
}

static const char *match_scheme(void)
{
    const l5_layout_t l = {.n = g_slots.n, .slot_kb = g_slots.slot_kb, .app0_kb = g_slots.app0_kb, .spiffs_kb = g_slots.spiffs_kb,
                           .fat_kb = g_slots.fat_kb};
    const l5_scheme_def_t *s = l5_layout_match(&l);
    return s ? s->label : "Custom";
}

/* *saved: the key exists (-1 is a saved "none", e.g. after a scheme change erased every app) */
static int8_t nvs_boot_read(bool *saved)
{
    nvs_handle_t h;
    int8_t v = -1;
    *saved = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        *saved = nvs_get_i8(h, "boot", &v) == ESP_OK;
        nvs_close(h);
    }
    return v;
}

static void nvs_boot_write(int8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i8(h, "boot", v);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void read_slot(int index)
{
    l5_slot_t *s = &g_slots.slot[index];
    esp_app_desc_t desc;
    s->used = false;
    s->size = 0;
    s->name[0] = s->project[0] = s->ver[0] = s->date[0] = s->src[0] = s->installed[0] = 0;
    s->author[0] = s->fid[0] = s->cver[0] = 0;
    memset(s->elf_sha, 0, sizeof(s->elf_sha));
    if (esp_ota_get_partition_description(s->part, &desc) != ESP_OK) {
        return; /* erased or foreign data */
    }
    const esp_partition_pos_t pos = {.offset = s->part->address, .size = s->part->size};
    esp_image_metadata_t meta;
    if (esp_image_get_metadata(&pos, &meta) != ESP_OK) {
        return;
    }
    s->used = true;
    s->size = meta.image_len;
    field_copy(s->project, sizeof(s->project), desc.project_name, sizeof(desc.project_name));
    field_copy(s->ver, sizeof(s->ver), desc.version, sizeof(desc.version));
    field_copy(s->date, sizeof(s->date), desc.date, sizeof(desc.date));
    memcpy(s->elf_sha, desc.app_elf_sha256, sizeof(s->elf_sha));
    strlcpy(s->name, s->project, sizeof(s->name));

    /* the registry entry wins when it was written for this very image (not flashed over by hand) */
    slot_meta_t m;
    if (meta_read(index, &m) && m.v1.size == s->size && memcmp(m.v1.elf_sha, s->elf_sha, sizeof(m.v1.elf_sha)) == 0) {
        if (m.v1.name[0]) {
            strlcpy(s->name, m.v1.name, sizeof(s->name));
        }
        strlcpy(s->src, m.v1.src, sizeof(s->src));
        strlcpy(s->installed, m.v1.date, sizeof(s->installed));
        strlcpy(s->author, m.author, sizeof(s->author));
        strlcpy(s->fid, m.fid, sizeof(s->fid));
        strlcpy(s->cver, m.ver, sizeof(s->cver));
    }
}

/* the first app the launcher installed itself (it has a registry entry): only such an app becomes the boot app
   without the user picking it */
static int first_installed(void)
{
    for (int i = 0; i < g_slots.n; i++) {
        if (g_slots.slot[i].used && g_slots.slot[i].src[0]) {
            return i;
        }
    }
    return -1;
}

void l5_slots_scan(void)
{
    g_slots.n = 0;
    for (int i = 0; i < L5_MAX_SLOTS; i++) {
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_MIN + i, NULL);
        if (!p) {
            break;
        }
        g_slots.slot[i].part = p;
        read_slot(i);
        g_slots.n++;
    }
    const uint32_t kb0 = g_slots.n ? g_slots.slot[0].part->size / 1024 : 0;
    g_slots.slot_kb = g_slots.n > 1 ? g_slots.slot[1].part->size / 1024 : kb0; /* app1.. are equal (l5_ptable) */
    g_slots.app0_kb = g_slots.n > 1 && kb0 != g_slots.slot_kb ? kb0 : 0;
    const esp_partition_t *sp = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
    const esp_partition_t *fat = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
    g_slots.spiffs_kb = sp ? sp->size / 1024 : 0;
    g_slots.fat_kb = fat ? fat->size / 1024 : 0;
    g_slots.scheme = match_scheme();

    /* S.boot: the saved choice; without one, the slot otadata points at, else the first app (L5.syncSlots), but only
       among apps the launcher installed (registry entry). An app flashed by hand or left in flash from before the
       launcher does not start by itself after the countdown: HOME says so until the user picks it. */
    bool saved;
    int boot = nvs_boot_read(&saved);
    if (!saved) {
        ESP_LOGW(TAG, "no boot choice in NVS (first start): apps in flash that the launcher did not install do not "
                      "start by themselves");
    }
    if (boot < 0 || boot >= g_slots.n || !g_slots.slot[boot].used) {
        boot = -1;
        const esp_partition_t *bp = esp_ota_get_boot_partition();
        for (int i = 0; bp && i < g_slots.n; i++) {
            if (g_slots.slot[i].part == bp && g_slots.slot[i].used && g_slots.slot[i].src[0]) {
                boot = i;
            }
        }
        if (boot < 0) {
            boot = first_installed();
        }
    }
    g_slots.boot = boot;
    char slots[96];
    l5_slots_text(slots, sizeof(slots));
    ESP_LOGI(TAG, "%d slots, %s (%s scheme), %d used, boot=%d", g_slots.n, slots, g_slots.scheme, l5_slots_used(),
             g_slots.boot);
}

int l5_slots_used(void)
{
    int n = 0;
    for (int i = 0; i < g_slots.n; i++) {
        n += g_slots.slot[i].used;
    }
    return n;
}

const l5_slot_t *l5_slots_boot_app(void)
{
    return g_slots.boot >= 0 && g_slots.slot[g_slots.boot].used ? &g_slots.slot[g_slots.boot] : NULL;
}

int l5_slots_list(char *out, size_t len)
{
    int count = 0;
    size_t n = 0;
    if (len) out[0] = 0;
    for (int i = 0; i < g_slots.n; i++) {
        if (!g_slots.slot[i].used) continue;
        count++;
        if (n + 1 < len) {
            int w = snprintf(out + n, len - n, "%s%s in app%d", n ? ", " : "", g_slots.slot[i].name, i);
            n = w < 0 ? n : n + (size_t)w < len ? n + (size_t)w : len - 1;
        }
    }
    return count;
}

void l5_slots_select(int index)
{
    if (index < 0 || index >= g_slots.n || !g_slots.slot[index].used || index == g_slots.boot) {
        return;
    }
    g_slots.boot = index;
    nvs_boot_write((int8_t)index);
}

void l5_slots_forget_all(void)
{
    for (int i = 0; i < L5_MAX_SLOTS; i++) {
        meta_erase(i);
    }
    nvs_boot_write(-1);
    l5_slots_clear_stopped();
    l5_slots_guard_clear();
}

/* ---------- boot-loop guard: the registers bootloader_start.c writes (l5_guard.h) ---------- */

#define GUARD_STATE_REG LP_SYSTEM_REG_LP_STORE13_REG
#define GUARD_TIME_REG LP_SYSTEM_REG_LP_STORE14_REG
#define GUARD_CHECK_REG LP_SYSTEM_REG_LP_STORE15_REG

void l5_slots_guard_clear(void)
{
    REG_WRITE(GUARD_STATE_REG, 0);
    REG_WRITE(GUARD_TIME_REG, 0);
    REG_WRITE(GUARD_CHECK_REG, 0);
}

bool l5_slots_guard_take(int *slot, int *restarts, int *returns)
{
    l5_guard_regs_t r = {REG_READ(GUARD_STATE_REG), REG_READ(GUARD_TIME_REG), REG_READ(GUARD_CHECK_REG)};
    if (!l5_guard_valid(&r) || !(r.state & L5_GUARD_STOPPED)) {
        return false; /* the rows stay: early returns are counted across the launcher's runs */
    }
    uint32_t boots = r.state & L5_GUARD_BOOTS_MASK;
    *slot = (int)((r.state >> L5_GUARD_SLOT_SHIFT) & 0xF);
    *restarts = boots >= L5_GUARD_QUICK_BOOTS ? (int)boots - 1 : 0;
    *returns = (int)((r.state & L5_GUARD_RETURNS_MASK) >> L5_GUARD_RETURNS_SHIFT);
    l5_slots_guard_clear(); /* the NVS mark keeps the stop from here on */
    return true;
}

typedef struct {
    int8_t slot;
    uint8_t elf_sha[8]; /* the mark is for this image only */
} stopped_t;

void l5_slots_mark_stopped(int index)
{
    nvs_handle_t h;
    if (index < 0 || index >= g_slots.n || !g_slots.slot[index].used || nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    stopped_t m = {.slot = (int8_t)index};
    memcpy(m.elf_sha, g_slots.slot[index].elf_sha, sizeof(m.elf_sha));
    if (nvs_set_blob(h, "stopped", &m, sizeof(m)) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

int l5_slots_stopped(void)
{
    nvs_handle_t h;
    stopped_t m;
    size_t len = sizeof(m);
    bool ok = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_blob(h, "stopped", &m, &len) == ESP_OK && len == sizeof(m);
        nvs_close(h);
    }
    if (!ok || m.slot < 0 || m.slot >= g_slots.n || !g_slots.slot[m.slot].used ||
        memcmp(m.elf_sha, g_slots.slot[m.slot].elf_sha, sizeof(m.elf_sha))) {
        return -1;
    }
    return m.slot;
}

void l5_slots_clear_stopped(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_erase_key(h, "stopped") == ESP_OK) nvs_commit(h);
        nvs_close(h);
    }
}

/* ---------- otadata ---------- */

static const esp_partition_t *otadata_part(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
}

static uint32_t ota_app_count(void)
{
    uint32_t n = 0;
    while (n < 16 && esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_MIN + n, NULL)) {
        n++;
    }
    return n;
}

static esp_err_t read_otadata(const esp_partition_t *od, esp_ota_select_entry_t sel[2])
{
    ESP_RETURN_ON_ERROR(esp_partition_read(od, 0, &sel[0], sizeof(sel[0])), TAG, "otadata[0]");
    return esp_partition_read(od, SECTOR, &sel[1], sizeof(sel[1]));
}

/* Same sequence arithmetic as esp_ota_ops.c esp_rewrite_ota_data(), plus a launcher
   marker in seq_label (not covered by the CRC) that the bootloader override reads. */
static esp_err_t write_otadata(uint32_t ota_index, uint32_t count, const char *label)
{
    const esp_partition_t *od = otadata_part();
    if (!od || od->size < 2 * SECTOR || ota_index >= count) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_ota_select_entry_t sel[2];
    ESP_RETURN_ON_ERROR(read_otadata(od, sel), TAG, "read otadata");
    int active = bootloader_common_get_active_otadata(sel);
    if (active >= 0 && (sel[active].ota_seq - 1) % count == ota_index &&
        strncmp((const char *)sel[active].seq_label, label, sizeof(sel[active].seq_label)) == 0) {
        return ESP_OK; /* already selected: spare the flash */
    }
    uint32_t seq = ota_index + 1;
    int next = 0;
    if (active >= 0) {
        uint32_t i = 0;
        while (sel[active].ota_seq > (ota_index + 1) % count + i * count) {
            i++;
        }
        seq = (ota_index + 1) % count + i * count;
        next = active ^ 1;
    }
    esp_ota_select_entry_t entry;
    memset(&entry, 0xff, sizeof(entry));
    entry.ota_seq = seq;
    strlcpy((char *)entry.seq_label, label, sizeof(entry.seq_label));
    entry.ota_state = ESP_OTA_IMG_UNDEFINED;
    entry.crc = bootloader_common_ota_select_crc(&entry);
    ESP_RETURN_ON_ERROR(esp_partition_erase_range(od, next * SECTOR, SECTOR), TAG, "erase otadata");
    return esp_partition_write(od, next * SECTOR, &entry, sizeof(entry));
}

static bool otadata_selects(int index)
{
    const esp_partition_t *od = otadata_part();
    esp_ota_select_entry_t sel[2];
    uint32_t count = ota_app_count();
    if (!od || !count || read_otadata(od, sel) != ESP_OK) {
        return false;
    }
    int active = bootloader_common_get_active_otadata(sel);
    return active >= 0 && (int)((sel[active].ota_seq - 1) % count) == index;
}

static esp_err_t clear_otadata(void)
{
    const esp_partition_t *od = otadata_part();
    esp_ota_select_entry_t sel[2];
    if (!od || read_otadata(od, sel) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    if (bootloader_common_get_active_otadata(sel) < 0) {
        return ESP_OK; /* already empty: the factory launcher boots */
    }
    return esp_partition_erase_range(od, 0, od->size);
}

esp_err_t l5_slots_handoff(int index, bool deep_sleep_starts_launcher)
{
    if (index < 0 || index >= g_slots.n || !g_slots.slot[index].used) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_partition_t *p = g_slots.slot[index].part;
    const esp_partition_pos_t pos = {.offset = p->address, .size = p->size};
    esp_image_metadata_t meta;
    /* checks the ESP32-P4 chip id, segments and SHA-256 before we commit to it */
    ESP_RETURN_ON_ERROR(esp_image_verify(ESP_IMAGE_VERIFY, &pos, &meta), TAG, "app%d image", index);
    ESP_RETURN_ON_ERROR(write_otadata(p->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN, ota_app_count(),
                                      deep_sleep_starts_launcher ? "L5:DS=1" : "L5:DS=0"), TAG, "otadata");
    ESP_LOGI(TAG, "booting app%d (%s) at 0x%lx", index, g_slots.slot[index].name, (unsigned long)p->address);
    esp_restart();
    return ESP_FAIL;
}

esp_err_t l5_slots_handoff_at(uint32_t ota_index, uint32_t ota_count, uint32_t addr, uint32_t size,
                              bool deep_sleep_starts_launcher)
{
    const esp_partition_pos_t pos = {.offset = addr, .size = size};
    esp_image_metadata_t meta;
    ESP_RETURN_ON_ERROR(esp_image_verify(ESP_IMAGE_VERIFY, &pos, &meta), TAG, "image at 0x%lx", (unsigned long)addr);
    ESP_RETURN_ON_ERROR(write_otadata(ota_index, ota_count, deep_sleep_starts_launcher ? "L5:DS=1" : "L5:DS=0"), TAG,
                        "otadata");
    ESP_LOGI(TAG, "booting ota_%lu at 0x%lx (new partition table)", (unsigned long)ota_index, (unsigned long)addr);
    esp_restart();
    return ESP_FAIL;
}

void l5_slots_release(int index)
{
    if (otadata_selects(index)) {
        clear_otadata(); /* the old image goes away now: a crash must not boot into it */
    }
    meta_erase(index);
    if (l5_slots_stopped() == index) l5_slots_clear_stopped();
    l5_slots_guard_clear(); /* the rows were for the image that is replaced */
}

void l5_slots_set_boot(int index)
{
    if (index >= 0 && index < L5_MAX_SLOTS) {
        g_slots.boot = index;
        nvs_boot_write((int8_t)index);
    }
}

esp_err_t l5_slots_register(int index, const uint8_t elf_sha[32], uint32_t image_len, const l5_slot_reg_t *r)
{
    slot_meta_t m;
    memset(&m, 0, sizeof(m));
    m.v1.size = image_len;
    memcpy(m.v1.elf_sha, elf_sha, sizeof(m.v1.elf_sha));
    utf8_copy(m.v1.name, sizeof(m.v1.name), r->name ? r->name : "");
    strlcpy(m.v1.src, r->src ? r->src : "", sizeof(m.v1.src));
    today(m.v1.date);
    utf8_copy(m.author, sizeof(m.author), r->author ? r->author : "");
    strlcpy(m.fid, r->fid ? r->fid : "", sizeof(m.fid));
    strlcpy(m.ver, r->ver ? r->ver : "", sizeof(m.ver));
    return meta_write(index, &m);
}

void l5_restart_to_launcher(void)
{
    esp_err_t err = clear_otadata();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "clear otadata: %s", esp_err_to_name(err));
    }
    esp_restart();
}

esp_err_t l5_slots_erase(int index, void (*progress)(int pct, void *user), void *user)
{
    if (index < 0 || index >= g_slots.n) {
        return ESP_ERR_INVALID_ARG;
    }
    l5_slot_t *s = &g_slots.slot[index];
    bool selected_in_otadata = otadata_selects(index);
    const uint32_t chunk = 64 * 1024;
    for (uint32_t off = 0; off < s->part->size; off += chunk) {
        ESP_RETURN_ON_ERROR(esp_partition_erase_range(s->part, off, chunk), TAG, "erase app%d", index);
        if (progress) {
            progress((int)((uint64_t)(off + chunk) * 100 / s->part->size), user);
        }
    }
    meta_erase(index);
    if (l5_slots_stopped() == index) l5_slots_clear_stopped();
    l5_slots_guard_clear(); /* the rows were for the image that is gone */
    read_slot(index);
    if (selected_in_otadata) {
        clear_otadata();
    }
    if (g_slots.boot == index) {
        g_slots.boot = first_installed();
        nvs_boot_write((int8_t)g_slots.boot);
    }
    return ESP_OK;
}

/* ---------- app images in files ---------- */

const char *l5_chip_name(uint16_t chip_id)
{
    switch (chip_id) {
    case ESP_CHIP_ID_ESP32: return "ESP32";
    case ESP_CHIP_ID_ESP32S2: return "ESP32-S2";
    case ESP_CHIP_ID_ESP32C3: return "ESP32-C3";
    case ESP_CHIP_ID_ESP32S3: return "ESP32-S3";
    case ESP_CHIP_ID_ESP32C2: return "ESP32-C2";
    case ESP_CHIP_ID_ESP32C6: return "ESP32-C6";
    case ESP_CHIP_ID_ESP32H2: return "ESP32-H2";
    case ESP_CHIP_ID_ESP32P4: return "ESP32-P4";
    case ESP_CHIP_ID_ESP32C5: return "ESP32-C5";
    case ESP_CHIP_ID_ESP32C61: return "ESP32-C61";
    default: return "unknown chip";
    }
}

/* bootloader_common_check_chip_revision_validity() without its error log */
bool l5_chip_rev_ok(uint16_t rev_min_full, uint16_t rev_max_full)
{
    unsigned rev = efuse_hal_chip_revision();
    if (rev < rev_min_full) {
        return false;
    }
    return rev_max_full == 0xFFFF || rev <= rev_max_full || efuse_hal_get_disable_wafer_version_major();
}

static bool rev_ok(const esp_image_header_t *h)
{
    return l5_chip_rev_ok(h->min_chip_rev_full, h->max_chip_rev_full);
}

esp_err_t l5_bin_probe(const char *path, l5_bin_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = L5_BIN_UNKNOWN;
    struct stat st;
    if (stat(path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    out->size = (uint32_t)st.st_size;
    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    /* app image: image header, first segment header, then esp_app_desc_t */
    struct __attribute__((packed)) {
        esp_image_header_t img;
        esp_image_segment_header_t seg;
        esp_app_desc_t desc;
    } h;
    size_t n = fread(&h, 1, sizeof(h), f);
    if (n >= sizeof(h.img) && h.img.magic != ESP_IMAGE_HEADER_MAGIC && fseek(f, 0x2000, SEEK_SET) == 0 &&
        fread(&h.img, 1, sizeof(h.img), f) == sizeof(h.img) && h.img.magic == ESP_IMAGE_HEADER_MAGIC) {
        n = sizeof(h.img); /* the bootloader at 0x2000: a merged image that starts at flash 0x0 (l5_fw_merged_base) */
    }
    fclose(f);
    if (n < sizeof(h.img) || h.img.magic != ESP_IMAGE_HEADER_MAGIC) {
        return ESP_OK;
    }
    out->chip_id = h.img.chip_id;
    out->rev_min = h.img.min_chip_rev_full;
    out->rev_max = h.img.max_chip_rev_full;
    out->rev_chip = (uint16_t)efuse_hal_chip_revision();
    if (n < sizeof(h) || h.desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        out->kind = L5_BIN_NO_APP;
        return ESP_OK;
    }
    out->desc = h.desc;
    out->desc.version[sizeof(out->desc.version) - 1] = 0;
    out->desc.project_name[sizeof(out->desc.project_name) - 1] = 0;
    out->desc.time[sizeof(out->desc.time) - 1] = 0;
    out->desc.date[sizeof(out->desc.date) - 1] = 0;
    out->desc.idf_ver[sizeof(out->desc.idf_ver) - 1] = 0;
    if (h.img.chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        out->kind = L5_BIN_OTHER_CHIP;
    } else {
        out->kind = rev_ok(&h.img) ? L5_BIN_APP : L5_BIN_CHIP_REV;
    }
    return ESP_OK;
}

const char *l5_slots_best_scheme(uint32_t bytes)
{
    static const char *const order[] = {"default", "uiflow2", "minspiffs", "maxapp"}; /* L5.bestScheme */
    for (size_t o = 0; o < sizeof(order) / sizeof(order[0]); o++) {
        const l5_scheme_def_t *s = l5_scheme_find(order[o]);
        const l5_layout_t l = s ? l5_layout_of_scheme(s) : (l5_layout_t){0};
        if (s && (uint64_t)l5_layout_max_slot_kb(&l) * 1024 >= bytes) {
            return s->label;
        }
    }
    return NULL;
}
