/* Stand-ins for the parts of the launcher the snapshots do not need: board (the backlight value is kept for the
   idle test), display driver, installer, slot hand-over and erase (shots.c sets g_slots itself); views_stubs.c
   has what CFG, PMAN and OTA call. The screen itself (views, top
   bar, layers, veil, idle timer) is the real l5_ui.c. The card is a folder on the PC (CONFIG_BSP_SD_MOUNT_POINT). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/task.h"
#include "l5_board.h"
#include "l5_config.h"
#include "l5_inst.h"
#include "l5_slots.h"
#include "l5_ui.h"
#include "l5_wifi.h"

l5_config_t g_cfg = {.bright = 70, .dim = 30, .screenOff = 120, .color = "red", .askSpiffs = true, .onlyBins = false,
                     .bootTime = 3, .wifiAuto = true, .deepSleepStart = true, .scheme = "custom"};
l5_slots_t g_slots = {.n = 1, .slot_kb = 11264, .boot = 0, .scheme = "Custom"}; /* l5_slots_scan() always names it */
int shots_backlight = -1; /* last l5_board_backlight() value */
int shots_wifi_state = L5_WIFI_CONNECTED;
int shots_battery = 76, shots_charging = L5_CHARGE_CHARGING; /* -1: no pack */
bool shots_sd_ready = true;

const char *esp_err_to_name(esp_err_t e)
{
    static char b[16];
    snprintf(b, sizeof(b), "0x%x", e);
    return b;
}

int64_t esp_timer_get_time(void)
{
    return (int64_t)clock() * 1000000 / CLOCKS_PER_SEC;
}

esp_err_t esp_vfs_fat_info(const char *base_path, uint64_t *total, uint64_t *free_bytes)
{
    (void)base_path;
    *total = 29ull << 30;
    *free_bytes = 21ull << 30;
    return ESP_OK;
}

BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg, int prio,
                                   TaskHandle_t *handle, int core)
{
    (void)stack, (void)prio, (void)handle, (void)core;
    if (!strcmp(name, "l5_cfg_save") || !strcmp(name, "l5_ota")) return pdFAIL; /* they wait for work forever */
    fn(arg); /* the worker runs to its end, its result dialog included */
    return pdPASS;
}

void vTaskDelete(TaskHandle_t t)
{
    (void)t;
}

bool l5_board_lock(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return true;
}
void l5_board_unlock(void) {}
bool l5_board_sd_ready(void) { return shots_sd_ready; }
l5_sd_state_t l5_board_sd_state(void) { return L5_SD_MOUNTED; }
const char *l5_board_sd_status(bool detail)
{
    if (!shots_sd_ready) return detail ? "No SD card." : "NO CARD";
    return detail ? "SD card mounted." : "MOUNTED";
}
esp_err_t l5_board_sd_error(void) { return ESP_OK; }
esp_err_t l5_board_sd_mount(void) { return ESP_OK; }
int l5_board_sd_drive(void) { return -1; }
void l5_board_backlight(uint8_t percent) { shots_backlight = percent; }

/* l5_display.c: no panel, no touch input */
lv_indev_t *l5_display_touch(void) { return NULL; }
void l5_display_mark(const char *what) { (void)what; }
void l5_display_mark_built(void) {}
void l5_display_refresh_now(void) {}
void l5_touch_trace_busy(const char *what, int64_t start_us) { (void)what, (void)start_us; }

esp_err_t l5_config_remove(void) { return ESP_FAIL; }

bool l5_install_busy(void) { return false; }

/* top bar */
const esp_app_desc_t *esp_app_get_description(void)
{
    static esp_app_desc_t d = {.version = "0.1.0", .project_name = "tab5_launcher"};
    return &d;
}
void l5_wifi_get_status(l5_wifi_status_t *st)
{
    memset(st, 0, sizeof(*st));
    st->state = (l5_wifi_state_t)shots_wifi_state;
}
void l5_wifi_menu(void) {}
void l5_power_menu(void) {}
int l5_board_battery_percent(void) { return shots_battery; }
l5_charge_t l5_board_battery_state(bool fresh)
{
    (void)fresh;
    return shots_battery < 0 ? L5_CHARGE_UNKNOWN : (l5_charge_t)shots_charging;
}

/* l5_sys.c: a full-screen system screen on #sys */
lv_obj_t *l5_sys_show(lv_color_t top, lv_color_t bottom)
{
    lv_obj_t *sys = l5_ui_sys();
    lv_obj_clean(sys);
    l5_ui_covered(true);
    lv_obj_t *el = l5_box(sys);
    lv_obj_set_size(el, L5_SCREEN_W, L5_SCREEN_H);
    lv_obj_add_flag(el, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(el, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(el, top, 0);
    lv_obj_set_style_bg_grad_color(el, bottom, 0);
    lv_obj_set_style_bg_grad_dir(el, LV_GRAD_DIR_VER, 0);
    return el;
}
void l5_sys_hide(void)
{
    lv_obj_clean(l5_ui_sys());
    l5_ui_covered(false);
}
void l5_install_fw(l5_fw_t *fw, const char *name, const char *author, const char *src)
{
    (void)name, (void)author, (void)src;
    free(fw);
}
void l5_fw_init(l5_fw_t *fw, l5_fw_format_t format) { (void)fw, (void)format; }
esp_err_t l5_fw_from_probe(l5_fw_t *fw, const uint8_t *head, size_t len, uint32_t file_size, char *why, size_t wlen)
{
    (void)fw, (void)head, (void)len, (void)file_size;
    snprintf(why, wlen, "not in the snapshots");
    return ESP_FAIL;
}
bool l5_fw_set_source(l5_fw_t *fw, const char *key, const char *url) { (void)fw, (void)key, (void)url; return false; }
uint32_t l5_fw_app_bound(const l5_fw_t *fw) { (void)fw; return 0; }
esp_err_t l5_inst_probe(const char *url, volatile bool *cancel, uint8_t *head, size_t *len, uint32_t *size, char *why,
                        size_t wlen, bool *net)
{
    (void)url, (void)cancel, (void)head, (void)len, (void)size, (void)net;
    snprintf(why, wlen, "not in the snapshots");
    return ESP_FAIL;
}
esp_err_t l5_inst_app_len_file(const char *path, uint32_t offset, uint32_t max, uint32_t *len)
{
    (void)path, (void)offset, (void)max, (void)len;
    return ESP_FAIL;
}

const l5_slot_t *l5_slots_boot_app(void)
{
    return g_slots.boot >= 0 && g_slots.slot[g_slots.boot].used ? &g_slots.slot[g_slots.boot] : NULL;
}
void l5_slots_select(int index) { g_slots.boot = index; }
esp_err_t l5_slots_erase(int index, void (*progress)(int pct, void *user), void *user)
{
    (void)progress, (void)user;
    memset(&g_slots.slot[index], 0, sizeof(g_slots.slot[index]));
    return ESP_OK;
}
void l5_boot_app(void) {}

int l5_slots_list(char *out, size_t len)
{
    snprintf(out, len, "Aurora-Player-1.3.21 in app0");
    return 1;
}
const char *l5_chip_name(uint16_t chip_id) { (void)chip_id; return "ESP32-S3"; }

/* every .bin is the Aurora image that app0 holds: the longest side panel (STATUS row included) */
esp_err_t l5_bin_probe(const char *path, l5_bin_info_t *out)
{
    struct stat st;
    memset(out, 0, sizeof(*out));
    if (stat(path, &st) != 0) return ESP_FAIL;
    out->size = (uint32_t)st.st_size;
    out->kind = L5_BIN_APP;
    strcpy(out->desc.project_name, "Aurora-Player");
    strcpy(out->desc.version, "1.3.21");
    strcpy(out->desc.date, "Oct 12 2026");
    strcpy(out->desc.time, "10:22:31");
    strcpy(out->desc.idf_ver, "v5.5.2");
    g_slots.slot[0].used = true;
    return ESP_OK;
}
