/* Stand-ins for what CFG, PMAN and OTA call outside LVGL and the launcher's UI files, so their first frame can be
   rendered: the partition table (the device's Custom table of 19.10.2026: one 11264 KB slot, vfs, aurora_cfg,
   spiffs), chip and PSRAM, NVS, the network (the OTA scene has no Wi-Fi), config.conf, firmware descriptions,
   system screens. The actions behind the buttons are not part of the snapshots. */
#include <stdio.h>
#include <string.h>
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "nvs.h"
#include "l5_config.h"
#include "l5_fw.h"
#include "l5_inst.h"
#include "l5_kbd.h"
#include "l5_net.h"
#include "l5_ptable.h"
#include "l5_slots.h"
#include "l5_ui.h"
#include "l5_wifi.h"

/* ---------- partition table ---------- */

static const l5_part_t PARTS[] = {
    {"nvs", 1, 0x02, 0x9000, 0x5000, 0},          {"otadata", 1, 0x00, 0xE000, 0x2000, 0},
    {"launcher", 0, 0x00, 0x10000, 0x200000, 0},  {"app0", 0, 0x10, 0x210000, 0xB00000, 0},
    {"vfs", 1, 0x81, 0xD10000, 0x200000, 0},      {"aurora_cfg", 1, 0x02, 0xF10000, 0x6000, 0},
    {"spiffs", 1, 0x82, 0xF16000, 0xDA000, 0},    {"coredump", 1, 0x03, 0xFF0000, 0x10000, 0},
};
#define NPARTS (int)(sizeof(PARTS) / sizeof(PARTS[0]))

/* copy of l5_ptable.c's data.js schemes */
const l5_scheme_def_t l5_schemes[] = {
    {"default", "Default", 8192, 1, L5_GAP_KB, 5120},   {"multi2", "2 Apps", 8192, 2, L5_GAP_KB, 5120},
    {"multi4", "4 Apps", 8192, 4, L5_GAP_KB, 5120},     {"minspiffs", "Min SPIFFS", 10240, 1, L5_GAP_KB, 3072},
    {"uiflow2", "UiFlow2", 7552, 1, L5_GAP_KB, 5760},   {"doom", "Doom", 4096, 1, L5_GAP_KB, 9216},
    {"maxapp", "Max App", 12864, 1, L5_GAP_KB, 448},
    {"bigsmall", "1 Big + 2 Small", 7552 + 2 * 2560, 3, L5_GAP_KB, 640, 7552},
};
const int l5_scheme_count = sizeof(l5_schemes) / sizeof(l5_schemes[0]);

esp_err_t l5_pt_read_map(l5_flash_map_t *map)
{
    memset(map, 0, sizeof(*map));
    map->table.n = NPARTS;
    memcpy(map->table.p, PARTS, sizeof(PARTS));
    map->flash_size = 16u << 20;
    map->pool_start = L5_POOL_START;
    map->pool_end = L5_POOL_END;
    map->managed = map->aurora = true;
    map->layout = (l5_layout_t){.n = 1, .slot_kb = 11264, .spiffs_kb = L5_GAP_KB, .fat_kb = 2048};
    return ESP_OK;
}

l5_layout_t l5_pt_base_layout(const l5_flash_map_t *map)
{
    return map->layout;
}

uint32_t l5_layout_extras_kb(const l5_layout_t *l)
{
    uint32_t kb = 0;
    for (uint32_t i = 0; i < l->nextra; i++) kb += l->extra[i].kb;
    return kb;
}

uint32_t l5_layout_slot_kb(const l5_layout_t *l, uint32_t i)
{
    return i == 0 && l->app0_kb ? l->app0_kb : l->slot_kb;
}

uint32_t l5_layout_apps_kb(const l5_layout_t *l)
{
    return l->n ? l5_layout_slot_kb(l, 0) + (l->n - 1) * l->slot_kb : 0;
}

uint32_t l5_layout_max_slot_kb(const l5_layout_t *l)
{
    const uint32_t a0 = l5_layout_slot_kb(l, 0);
    return l->n > 1 && l->slot_kb > a0 ? l->slot_kb : a0;
}

uint32_t l5_layout_front_kb(const l5_layout_t *l)
{
    return l5_layout_apps_kb(l) + l5_layout_extras_kb(l) + l->fat_kb;
}

const char *l5_layout_fat_label(const l5_layout_t *l)
{
    return l->fat_label[0] ? l->fat_label : "vfs";
}

const char *l5_layout_gap_label(const l5_layout_t *l)
{
    return l->gap_label[0] ? l->gap_label : "spiffs";
}

l5_layout_t l5_layout_of_scheme(const l5_scheme_def_t *s)
{
    if (s->app0_kb && s->n > 1) {
        return (l5_layout_t){.n = s->n, .app0_kb = s->app0_kb, .slot_kb = (s->app_kb - s->app0_kb) / (s->n - 1) / 64 * 64,
                             .spiffs_kb = s->spiffs_kb, .fat_kb = s->fat_kb};
    }
    return (l5_layout_t){.n = s->n, .slot_kb = s->app_kb / s->n / 64 * 64, .spiffs_kb = s->spiffs_kb, .fat_kb = s->fat_kb};
}

const l5_scheme_def_t *l5_scheme_find(const char *key)
{
    for (int i = 0; key && i < l5_scheme_count; i++) {
        if (!strcmp(key, l5_schemes[i].id) || !strcmp(key, l5_schemes[i].label)) return &l5_schemes[i];
    }
    return NULL;
}

bool l5_layout_equal(const l5_layout_t *a, const l5_layout_t *b)
{
    return a->n == b->n && a->slot_kb == b->slot_kb && a->app0_kb == b->app0_kb && a->spiffs_kb == b->spiffs_kb &&
           a->fat_kb == b->fat_kb && a->nextra == b->nextra;
}

esp_err_t l5_pt_build(const l5_flash_map_t *map, const l5_layout_t *l, l5_ptable_t *out, char *why, size_t len)
{
    (void)map, (void)l, (void)out;
    snprintf(why, len, "not in the snapshots");
    return ESP_FAIL;
}

void l5_pt_diff(const l5_flash_map_t *map, const l5_ptable_t *next, l5_pt_diff_t *d)
{
    (void)map, (void)next;
    memset(d, 0, sizeof(*d));
}

esp_err_t l5_pt_apply(const l5_flash_map_t *map, const l5_ptable_t *next, l5_pt_progress_t cb, void *user,
                      bool *table_written, char *why, size_t len)
{
    (void)map, (void)next, (void)cb, (void)user;
    *table_written = false;
    snprintf(why, len, "not in the snapshots");
    return ESP_FAIL;
}

bool l5_part_editable(const char *label)
{
    return !strcmp(label, "vfs") || !strcmp(label, "spiffs");
}

esp_err_t l5_part_erase(const char *label, l5_pt_progress_t cb, void *user)
{
    (void)label, (void)cb, (void)user;
    return ESP_FAIL;
}

esp_err_t l5_part_backup(const char *label, char *out_path, size_t len, l5_pt_progress_t cb, void *user)
{
    (void)label, (void)out_path, (void)len, (void)cb, (void)user;
    return ESP_FAIL;
}

esp_err_t l5_part_restore(const char *label, const char *path, l5_pt_progress_t cb, void *user)
{
    (void)label, (void)path, (void)cb, (void)user;
    return ESP_FAIL;
}

int l5_part_blank(const char *label)
{
    return strcmp(label, "spiffs") ? 0 : 1;
}

esp_err_t l5_image_check_file(const char *path, uint32_t offset, l5_image_check_t *out)
{
    (void)path, (void)offset;
    memset(out, 0, sizeof(*out));
    return ESP_FAIL;
}

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type, int subtype, const char *label)
{
    static esp_partition_t found[NPARTS];
    for (int i = 0; i < NPARTS; i++) {
        const l5_part_t *p = &PARTS[i];
        if (p->type != type || (subtype != ESP_PARTITION_SUBTYPE_ANY && p->subtype != subtype)) continue;
        if (label && strcmp(label, p->label)) continue;
        found[i] = (esp_partition_t){.address = p->offset, .size = p->size, .type = p->type, .subtype = p->subtype};
        strcpy(found[i].label, p->label);
        return &found[i];
    }
    return NULL;
}

void l5_slots_scan(void) {}
const char *l5_slots_best_scheme(uint32_t bytes)
{
    (void)bytes;
    return NULL;
}

/* ---------- firmware descriptions (l5_fw.c) ---------- */

esp_err_t l5_fw_plan(const l5_fw_t *fw, l5_layout_t *out, char *notes, size_t len)
{
    (void)fw, (void)out;
    snprintf(notes, len, "not in the snapshots");
    return ESP_FAIL;
}

esp_err_t l5_fw_from_manifest(l5_fw_t *fw, const char *json, char *why, size_t len)
{
    (void)fw, (void)json;
    snprintf(why, len, "not in the snapshots");
    return ESP_FAIL;
}

bool l5_fw_system_part(const l5_fw_part_t *p)
{
    (void)p;
    return false;
}

const char *l5_fw_subtype_name(uint8_t subtype)
{
    return subtype == 0x81 ? "FAT" : subtype == 0x82 ? "SPIFFS" : "data";
}

/* ---------- chip, NVS ---------- */

void esp_chip_info(esp_chip_info_t *out)
{
    *out = (esp_chip_info_t){.model = 18, .revision = 103, .cores = 2};
}

esp_err_t esp_efuse_mac_get_default(uint8_t *mac)
{
    static const uint8_t m[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01}; /* locally administered, not a device's */
    memcpy(mac, m, 6);
    return ESP_OK;
}

size_t esp_psram_get_size(void)
{
    return 32u << 20;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h)
{
    (void)ns, (void)mode;
    *h = 1;
    return ESP_OK;
}
esp_err_t nvs_erase_all(nvs_handle_t h) { (void)h; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

/* ---------- Wi-Fi UI, network, config.conf, system screens ---------- */

extern int shots_wifi_state; /* ui_stubs.c */
const char *l5_wifi_summary(char *buf, size_t len)
{
    snprintf(buf, len, shots_wifi_state == L5_WIFI_CONNECTED ? "Connected: Home" : "2 saved");
    return buf;
}
const char *l5_wifi_host_version(void) { return "2.12.6"; }
void l5_wifi_forget_all(void) {}
int shots_wifi_picks, shots_wifi_whys; /* CONNECT / WHY NO WI-FI */
void l5_wifi_pick(void) { shots_wifi_picks++; }
void l5_wifi_why(void) { shots_wifi_whys++; }
const char *l5_wifi_host_series(void) { return "2.12"; }

/* the Tab5 Keyboard on the PC: absent unless shots --keys plugs it in and types (shots_kbd_push) */
bool shots_kbd_present;
static l5_key_t s_keys[64];
static int s_key_head, s_key_tail;
void shots_kbd_push(l5_key_t k)
{
    s_keys[s_key_tail++ % 64] = k;
}
bool l5_kbd_present(void) { return shots_kbd_present; }
bool l5_kbd_get(l5_key_t *key)
{
    if (s_key_head == s_key_tail) return false;
    *key = s_keys[s_key_head++ % 64];
    return true;
}

esp_err_t l5_net_open(const char *url, uint32_t offset, uint32_t len, volatile bool *cancel, l5_net_stream_t **out,
                      int64_t *length, int64_t *total, l5_net_err_t *e)
{
    (void)url, (void)offset, (void)len, (void)cancel, (void)length, (void)total;
    *out = NULL;
    if (e) *e = (l5_net_err_t){.fail = L5_NET_NO_WIFI, .err = ESP_FAIL};
    return ESP_FAIL;
}
int l5_net_read(l5_net_stream_t *s, uint8_t *buf, size_t n, l5_net_err_t *e)
{
    (void)s, (void)buf, (void)n, (void)e;
    return -1;
}
void l5_net_close(l5_net_stream_t *s) { (void)s; }
esp_err_t l5_net_fetch(const char *url, size_t max, volatile bool *cancel, char **out, size_t *len, l5_net_err_t *e)
{
    (void)url, (void)max, (void)cancel;
    *out = NULL;
    *len = 0;
    if (e) *e = (l5_net_err_t){.fail = L5_NET_NO_WIFI, .err = ESP_FAIL};
    return ESP_FAIL;
}
void l5_net_urlencode(char *out, size_t len, const char *in) { snprintf(out, len, "%s", in); }

/* copy of l5_config.c's data.js colors (CFG's swatches) */
const l5_color_t l5_colors[] = {
    {"red", 0xe3171f}, {"orange", 0xff5f1a}, {"amber", 0xf2b400}, {"green", 0x18b25a},
    {"cyan", 0x0fb4c8}, {"blue", 0x2f6bff}, {"violet", 0x8a4dff}, {"magenta", 0xe0218a},
};
const int l5_color_count = sizeof(l5_colors) / sizeof(l5_colors[0]);

void l5_config_defaults(l5_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->bright = 70;
    cfg->dim = 30;
    cfg->screenOff = 120;
    strcpy(cfg->color, "red");
    strcpy(cfg->theme, "graphite");
    cfg->askSpiffs = cfg->onlyBins = cfg->wifiAuto = cfg->deepSleepStart = true;
    cfg->bootTime = 3;
    strcpy(cfg->scheme, "default");
}
esp_err_t l5_config_save(const l5_config_t *cfg) { (void)cfg; return ESP_OK; }
uint32_t l5_config_accent_hex(const l5_config_t *cfg) { (void)cfg; return 0xe3171f; }
int shots_favs; /* catalog favorites in config.conf (shots --keys: OTA's FAVORITES list) */
esp_err_t l5_favs_load(l5_favs_t *out)
{
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < shots_favs && i < L5_FAV_MAX; i++) {
        snprintf(out->item[i].name, sizeof(out->item[i].name), "Favorite %d", i);
        snprintf(out->item[i].fid, sizeof(out->item[i].fid), "fid%d", i);
    }
    out->n = shots_favs;
    return ESP_OK;
}
esp_err_t l5_favs_save(const l5_favs_t *favs) { (void)favs; return ESP_OK; }

void l5_board_set_rotation(uint16_t rot) { (void)rot; }
void l5_restart(void) {}
void l5_power_off(void) {}
