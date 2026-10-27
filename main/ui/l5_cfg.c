/* CFG: port of renderCFG / cfgAction (views2.js). Every change applies at once where it can
   (backlight, dim timer, accent, rotation) and is saved to /config.conf on the SD card.
   A change updates only its own widgets (value, toggle, swatch, title): rebuilding the view and
   redrawing all of it took 250-290 ms after every tap.
   Keys of the Tab5 Keyboard (l5_cfg_key): Up / Down, Home / End move the ring over the rows of the three columns,
   Enter = a tap on the row, Left / Right = - / + on the stepper rows, the previous / next UI color or theme. */
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "l5_board.h"
#include "l5_config.h"
#include "l5_slots.h"
#include "l5_ui.h"
#include "l5_wifi.h"

static const uint16_t DIMS[] = {0, 10, 20, 30, 60, 120, 300}; /* L5.DIMS */
#define NDIMS (sizeof(DIMS) / sizeof(DIMS[0]))
static const uint16_t OFFS[] = {0, 60, 120, 300, 600, 1800}; /* L5.OFFS */
#define NOFFS (sizeof(OFFS) / sizeof(OFFS[0]))
#define MAX_SWATCH 16
#define CFG_COL_W ((L5_FULL_W - 2 * 13) / 3) /* three columns: 372 px since the rail (410 under the top bar) */

static lv_obj_t *s_wifi_sub; /* "Wi-Fi networks" subtitle, follows the Wi-Fi status */

/* widgets that follow g_cfg in place; all NULL while CFG is not built */
static struct {
    lv_obj_t *bright, *dim, *off, *rot, *boot; /* value labels */
    lv_obj_t *ask_title, *bins_title;     /* row titles that name the state */
    lv_obj_t *ask, *bins, *wifi, *deep, *btn, *clock; /* toggles */
    lv_obj_t *swatch[MAX_SWATCH];
    lv_obj_t *theme[L5_THEME_COUNT]; /* the previews of Graphite, Midnight, Daylight */
    lv_obj_t *theme_sub;             /* the Theme row's subtitle: the theme's name */
} W;

/* every row in column order, for the keyboard; empty while CFG is not built */
#define MAX_KROWS 32
static struct {
    lv_obj_t *row[MAX_KROWS];
    const char *key[MAX_KROWS]; /* Enter: the row's action (NULL: the row has none) */
    const char *dec[MAX_KROWS]; /* Left / Right on a stepper row */
    const char *inc[MAX_KROWS];
    bool color[MAX_KROWS];      /* the UI color row: Left / Right pick the previous / next swatch */
    bool theme[MAX_KROWS];      /* the Theme row: Left / Right the previous / next theme */
    int n;
} K;
static int s_focus = -1; /* the row with the ring; kept while CFG is rebuilt, -1 = none yet */

/* ---------- saving /config.conf ---------- */

/* Saving reads, merges and writes config.conf on the SD card: tens of ms that delayed every tap.
   A low-priority task writes the latest values 300 ms after the last change; restart, power-off and
   the hand-over to an app write a pending change first (l5_cfg_flush). */
static TaskHandle_t s_saver;
static SemaphoreHandle_t s_save_lock;
static volatile bool s_dirty;

static esp_err_t write_now(void)
{
    if (!s_save_lock) s_save_lock = xSemaphoreCreateMutex();
    if (!s_save_lock) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s_dirty) {
        s_dirty = false;
        l5_config_t snap = g_cfg;
        err = l5_config_save(&snap);
    }
    xSemaphoreGive(s_save_lock);
    return err;
}

static void saver_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(300))) {
        } /* more changes: wait until they stop */
        if (write_now() != ESP_OK && l5_board_lock(0)) {
            l5_toast("Could not write /config.conf");
            l5_board_unlock();
        }
    }
}

void l5_cfg_save(void)
{
    static bool warned;
    if (!l5_board_sd_ready()) {
        if (!warned) l5_toast("No SD card: changes last until restart");
        warned = true;
        return;
    }
    s_dirty = true;
    if (!s_save_lock) s_save_lock = xSemaphoreCreateMutex();
    if (!s_saver && xTaskCreatePinnedToCore(saver_task, "l5_cfg_save", 6144, NULL, 2, &s_saver, 1) != pdPASS) {
        s_saver = NULL;
    }
    if (s_saver) {
        xTaskNotifyGive(s_saver);
    } else if (write_now() != ESP_OK) { /* no task: write it now */
        l5_toast("Could not write /config.conf");
    }
}

esp_err_t l5_cfg_flush(void)
{
    return s_dirty ? write_now() : ESP_OK;
}

esp_err_t l5_cfg_save_now(void)
{
    s_dirty = true;
    return write_now();
}

void l5_cfg_discard_pending(void)
{
    if (!s_save_lock) s_save_lock = xSemaphoreCreateMutex();
    if (!s_save_lock) return;
    xSemaphoreTake(s_save_lock, portMAX_DELAY); /* a write in progress finishes first */
    s_dirty = false;
    xSemaphoreGive(s_save_lock);
}

static void save(void)
{
    l5_cfg_save();
}

/* config.conf's theme as an index; an unknown id is Graphite */
static int cfg_theme(void)
{
    const int t = l5_theme_find(g_cfg.theme);
    return t < 0 ? L5_THEME_GRAPHITE : t;
}

#define APP l5_color_count /* the swatch after the colours: APP, the boot app's icon colour */

static bool app_color(void)
{
    return !strcmp(g_cfg.color, "app");
}

uint32_t l5_cfg_accent_hex(void)
{
    return app_color() ? l5_appicon_boot_accent(l5_colors[0].hex) : l5_config_accent_hex(&g_cfg);
}

bool l5_cfg_accent_follow(void)
{
    if (!app_color()) return false;
    const uint32_t hex = l5_cfg_accent_hex();
    if (hex == (lv_color_to_u32(L5P.accent) & 0xFFFFFF)) return false;
    l5_theme_set_accent(hex);
    l5_rail_apply_accent();
    l5_cfg_paint();
    l5_boot_screen_icons(); /* the bootscreen's ring, tile and seconds */
    /* the views bake the accent into local styles; after the current event (a tap in HOME may have chosen the app) */
    l5_ui_render_views_async(L5_VIEWS_ALL);
    return true;
}

static void apply_look(void) /* L5.applyLook */
{
    l5_theme_set_accent(l5_cfg_accent_hex());
    l5_rail_apply_accent();
    if (cfg_theme() != l5_theme_current()) l5_ui_retheme(cfg_theme()); /* Reset config, config.conf replaced */
    l5_board_set_rotation(g_cfg.rot);
    l5_board_backlight(g_cfg.bright);
}

static const char *part_label(uint8_t subtype)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, subtype, NULL);
    return p ? p->label : NULL;
}

/* ---------- in-place updates ---------- */

static void set_text(lv_obj_t *l, const char *t)
{
    if (l && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t); /* same text: nothing to redraw */
}

static void set_toggle(lv_obj_t *t, bool on)
{
    if (!t) return;
    lv_obj_set_state(t, LV_STATE_CHECKED, on);
    lv_obj_set_state(lv_obj_get_child(t, 0), LV_STATE_CHECKED, on);
}

/* g_cfg -> widgets: LVGL redraws only what changed */
void l5_cfg_paint(void)
{
    char t[16];
    snprintf(t, sizeof(t), "%u%%", g_cfg.bright);
    set_text(W.bright, t);
    if (g_cfg.dim) {
        snprintf(t, sizeof(t), "%us", g_cfg.dim);
    } else {
        strlcpy(t, "OFF", sizeof(t));
    }
    set_text(W.dim, t);
    if (g_cfg.screenOff) {
        snprintf(t, sizeof(t), "%u min", g_cfg.screenOff / 60);
    } else {
        strlcpy(t, "OFF", sizeof(t));
    }
    set_text(W.off, t);
    snprintf(t, sizeof(t), "%u\xC2\xB0", g_cfg.rot);
    set_text(W.rot, t);
    snprintf(t, sizeof(t), "%us", g_cfg.bootTime);
    set_text(W.boot, t);
    set_text(W.ask_title, g_cfg.askSpiffs ? "Ask SPIFFS" : "Avoid SPIFFS");
    set_text(W.bins_title, g_cfg.onlyBins ? "Only Bins" : "All files");
    set_toggle(W.ask, g_cfg.askSpiffs);
    set_toggle(W.bins, !g_cfg.onlyBins);
    set_toggle(W.wifi, g_cfg.wifiAuto);
    set_toggle(W.deep, g_cfg.deepSleepStart);
    set_toggle(W.btn, g_cfg.btnOnlyStart);
    set_toggle(W.clock, g_cfg.idleClock);
    for (int i = 0; i < l5_color_count && i < MAX_SWATCH; i++) {
        if (W.swatch[i]) lv_obj_set_state(W.swatch[i], LV_STATE_CHECKED, !strcmp(g_cfg.color, l5_colors[i].id));
    }
    if (W.swatch[APP]) { /* the APP swatch shows the colour it gives now */
        const uint32_t hex = l5_appicon_boot_accent(0x1000000);
        const lv_color_t c = hex >> 24 ? L5_PANEL_3 : lv_color_hex(hex);
        lv_obj_set_state(W.swatch[APP], LV_STATE_CHECKED, app_color());
        lv_obj_set_style_bg_color(W.swatch[APP], c, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(W.swatch[APP], 0),
                                    hex >> 24 ? L5_ON_PANEL : (c.red * 30 + c.green * 59 + c.blue * 11) / 100 > 165
                                                                  ? lv_color_hex(0x111111) : L5_WHITE, 0);
    }
    for (int i = 0; i < L5_THEME_COUNT; i++) {
        if (!W.theme[i]) continue;
        lv_obj_set_state(W.theme[i], LV_STATE_CHECKED, i == cfg_theme());
        lv_obj_set_style_bg_color(lv_obj_get_child(lv_obj_get_child(W.theme[i], 1), 0), L5P.accent, 0); /* UI color */
    }
    set_text(W.theme_sub, l5_themes[cfg_theme()].label);
}

static void widgets_deleted_cb(lv_event_t *e)
{
    (void)e;
    memset(&W, 0, sizeof(W));
    memset(&K, 0, sizeof(K));
}

/* LV_STATE_FOCUS_KEY on the focused row only (L5S.focus draws the ring), scrolled into view */
static void paint_focus(void)
{
    for (int i = 0; i < K.n; i++) lv_obj_set_state(K.row[i], LV_STATE_FOCUS_KEY, i == s_focus);
    if (s_focus >= 0 && s_focus < K.n) lv_obj_scroll_to_view(K.row[s_focus], LV_ANIM_OFF);
}

/* ---------- actions ---------- */

static void deep_sleep_cb(bool ok, void *user)
{
    (void)user;
    if (!ok) return;
    g_cfg.deepSleepStart = false;
    save();
    l5_cfg_paint();
}

static void erase_ns(const char *ns)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

esp_err_t l5_cfg_reload_now(void)
{
    l5_config_t cfg;
    esp_err_t err = l5_config_load(&cfg);
    if (err != ESP_OK) return err; /* not a JSON object: g_cfg stays */
    strlcpy(cfg.scheme, g_cfg.scheme, sizeof(cfg.scheme)); /* the scheme describes flash, not the file */
    g_cfg = cfg;
    apply_look();
    l5_cfg_paint();
    l5_ui_render();
    return ESP_OK;
}

void l5_cfg_defaults_now(void)
{
    char scheme[sizeof(g_cfg.scheme)];
    strlcpy(scheme, g_cfg.scheme, sizeof(scheme)); /* the scheme describes flash, it stays */
    l5_config_defaults(&g_cfg);
    strlcpy(g_cfg.scheme, scheme, sizeof(g_cfg.scheme));
    apply_look();
    l5_cfg_paint();
    l5_ui_render();
}

static void reset_cb(const char *id, void *user)
{
    (void)user;
    if (!id) return;
    char scheme[sizeof(g_cfg.scheme)];
    strlcpy(scheme, g_cfg.scheme, sizeof(scheme)); /* the scheme describes flash, it stays */
    l5_config_defaults(&g_cfg);
    strlcpy(g_cfg.scheme, scheme, sizeof(g_cfg.scheme));
    bool dev = strcmp(id, "dev") == 0;
    if (dev) { /* saved networks: the launcher's store and the IDF Wi-Fi driver's */
        l5_wifi_forget_all();
        erase_ns("nvs.net80211");
    }
    apply_look();
    save();
    l5_toast(dev ? "Config and Wi-Fi reset" : "Config reset");
    l5_ui_render();
}

static void about(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    const esp_app_desc_t *app = esp_app_get_description();
    l5_wifi_status_t wifi;
    l5_wifi_get_status(&wifi);
    char body[560];
    snprintf(body, sizeof(body),
             "Native LVGL launcher for M5Stack Tab5 (ESP32-P4, 1280\xC3\x97" "720). Menus follow the open-source "
             "Launcher by bmorcelli (formerly M5Stick-Launcher): OTA, SD, CFG, PMan, APP, OFF.\n\n"
             "Launcher %s \xC2\xB7 ESP-IDF %s \xC2\xB7 LVGL %d.%d.%d\nESP32-P4 rev v%d.%d \xC2\xB7 %u MB PSRAM \xC2\xB7 "
             "MAC %02X:%02X:%02X:%02X:%02X:%02X\nESP32-C6 Wi-Fi: ESP-Hosted %s (launcher: host %s)",
             app->version, app->idf_ver, LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, chip.revision / 100,
             chip.revision % 100, (unsigned)(esp_psram_get_size() >> 20), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             wifi.c6_fw[0] ? wifi.c6_fw : "not started yet", l5_wifi_host_version());
    l5_info("ABOUT", "Tab5 Launcher", body);
}

static void action(const char *k)
{
    if (!strcmp(k, "askSpiffs")) g_cfg.askSpiffs = !g_cfg.askSpiffs;
    else if (!strcmp(k, "onlyBins")) {
        g_cfg.onlyBins = !g_cfg.onlyBins;
        l5_ui_render_views(L5_VIEW_BIT(L5_VIEW_SD)); /* the SD list changes: rebuilt when shown */
    } else if (!strcmp(k, "wifiAuto")) g_cfg.wifiAuto = !g_cfg.wifiAuto;
    else if (!strcmp(k, "idleClock")) g_cfg.idleClock = !g_cfg.idleClock;
    else if (!strcmp(k, "btnOnlyStart")) g_cfg.btnOnlyStart = !g_cfg.btnOnlyStart;
    else if (!strcmp(k, "deepSleepStart")) {
        if (g_cfg.deepSleepStart) {
            l5_confirm("CONFIRM", "Turn off start from DeepSleep?",
                       "Some apps sleep instead of restarting. With this off, reaching Launcher can need a full power "
                       "cycle.", "TURN OFF", false, deep_sleep_cb, NULL);
            return;
        }
        g_cfg.deepSleepStart = true;
    } else if (!strcmp(k, "rot")) {
        g_cfg.rot = g_cfg.rot ? 0 : 180;
        l5_board_set_rotation(g_cfg.rot);
    } else if (!strcmp(k, "charge")) {
        l5_charge_mode();
        return;
    } else if (!strcmp(k, "scheme")) {
        l5_pman_pick_scheme();
        return;
    } else if (!strcmp(k, "plist")) {
        l5_ui_go(L5_VIEW_PMAN);
        return;
    } else if (!strcmp(k, "clearfat") || !strcmp(k, "savesp") || !strcmp(k, "restsp")) {
        const char *label = part_label(!strcmp(k, "clearfat") ? ESP_PARTITION_SUBTYPE_DATA_FAT : ESP_PARTITION_SUBTYPE_DATA_SPIFFS);
        if (!label) {
            l5_info("CFG", "Not in this scheme", !strcmp(k, "clearfat") ? "The partition table has no FAT (vfs) partition."
                                                                          : "The partition table has no SPIFFS partition.");
        } else if (!strcmp(k, "clearfat")) {
            l5_pman_format(label);
        } else if (!strcmp(k, "savesp")) {
            l5_pman_backup(label);
        } else {
            l5_pman_restore(label);
        }
        return;
    } else if (!strcmp(k, "wifi")) {
        l5_wifi_menu();
        return;
    } else if (!strcmp(k, "reset")) {
        const l5_menu_item_t items[] = {
            {"cfg", L5_ICON_RESTART, "Reset config", false},
            {"dev", L5_ICON_TRASH, "Reset config + Wi-Fi credentials (dev)", true},
        };
        l5_menu("CFG", "Reset config", items, 2, reset_cb, NULL);
        return;
    } else if (!strcmp(k, "about")) {
        about();
        return;
    } else if (!strcmp(k, "restart")) {
        l5_restart();
        return;
    } else if (!strcmp(k, "off")) {
        l5_power_off();
        return;
    }
    save();
    l5_cfg_paint();
}

static void row_cb(lv_event_t *e)
{
    action(lv_event_get_user_data(e));
}

static void step(const char *k)
{
    int d = k[strlen(k) - 1] == '+' ? 1 : -1;
    if (!strncmp(k, "bright", 6)) {
        int v = g_cfg.bright + 5 * d; /* L5.setBright */
        g_cfg.bright = v < 5 ? 5 : v > 100 ? 100 : v;
        l5_board_backlight(g_cfg.bright);
    } else if (!strncmp(k, "dim", 3)) {
        int i = 0;
        while (i < (int)NDIMS - 1 && DIMS[i] != g_cfg.dim) i++;
        i += d;
        g_cfg.dim = DIMS[i < 0 ? 0 : i >= (int)NDIMS ? NDIMS - 1 : i];
    } else if (!strncmp(k, "scr", 3)) {
        int i = 0;
        while (i < (int)NOFFS - 1 && OFFS[i] != g_cfg.screenOff) i++;
        i += d;
        g_cfg.screenOff = OFFS[i < 0 ? 0 : i >= (int)NOFFS ? NOFFS - 1 : i];
    } else if (!strncmp(k, "boot", 4)) {
        int v = g_cfg.bootTime + d;
        g_cfg.bootTime = v < 1 ? 1 : v > 5 ? 5 : v;
        l5_ui_render_views(L5_VIEW_BIT(L5_VIEW_HOME)); /* "APPn STARTS AFTER THE Ns BOOTSCREEN" */
    }
    save();
    l5_cfg_paint();
}

static void step_cb(lv_event_t *e)
{
    step(lv_event_get_user_data(e));
}

static void set_color(int i)
{
    strlcpy(g_cfg.color, i == APP ? "app" : l5_colors[i].id, sizeof(g_cfg.color));
    apply_look(); /* shared styles and the top bar follow at once; CFG uses only shared accent styles */
    save();
    l5_cfg_paint();
    /* the other views bake the accent into local styles: rebuilt when they are shown next */
    l5_ui_render_views(L5_VIEWS_ALL & ~L5_VIEW_BIT(L5_VIEW_CFG));
}

static void color_cb(lv_event_t *e)
{
    set_color((int)(intptr_t)lv_event_get_user_data(e));
}

/* CFG > Theme: saved, then every colour of the launcher changes at once (CFG itself is rebuilt after this event) */
static void set_theme(int i)
{
    if (i < 0 || i >= L5_THEME_COUNT || i == cfg_theme()) return;
    strlcpy(g_cfg.theme, l5_themes[i].id, sizeof(g_cfg.theme));
    save();
    l5_ui_retheme(i);
}

static void theme_cb(lv_event_t *e)
{
    set_theme((int)(intptr_t)lv_event_get_user_data(e));
}

/* ---------- building blocks (.crow, .tgl, .step, .swatches) ---------- */

static lv_obj_t *column(lv_obj_t *view, int i, const char *title)
{
    lv_obj_t *c = l5_box(view);
    l5_panel(c, true);
    lv_obj_set_pos(c, L5_GAP + i * (CFG_COL_W + 13), L5_PANEL_Y); /* 13 px between the columns */
    lv_obj_set_size(c, CFG_COL_W, L5_PANEL_H);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    l5_chamfer(c, L5_CHAMFER, L5_PLATE);
    l5_col_header(c, title, "");
    lv_obj_t *list = l5_box(c);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE); /* clickable: a drag between rows scrolls too */
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    return list;
}

static lv_obj_t *crow(lv_obj_t *list, const char *key, const char *title, const char *sub, bool danger)
{
    lv_obj_t *r = l5_box(list);
    lv_obj_set_width(r, lv_pct(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(r, 64, 0);
    lv_obj_set_style_pad_ver(r, 6, 0);
    lv_obj_set_style_pad_hor(r, 16, 0);
    lv_obj_set_style_pad_column(r, 12, 0);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, L5_PANEL_LINE, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (key) {
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_style(r, &L5S.press_tint, LV_STATE_PRESSED); /* seen at touch-down, the action runs at release */
        lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, (void *)key);
    }
    lv_obj_add_style(r, &L5S.focus, LV_STATE_FOCUS_KEY);
    if (K.n < MAX_KROWS) {
        K.row[K.n] = r;
        K.key[K.n++] = key;
    }
    lv_obj_t *tx = l5_box(r);
    lv_obj_set_height(tx, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(tx, 1);
    lv_obj_set_style_pad_row(tx, 2, 0);
    lv_obj_set_flex_flow(tx, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *b = l5_label(tx, &l5_chakra_sb17, danger ? L5_DANGER : L5_ON_PANEL, title);
    lv_obj_set_width(b, lv_pct(100));
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_t *s = l5_label(tx, &l5_mono_13, L5_ON_PANEL_MUTED, sub);
    lv_obj_set_width(s, lv_pct(100));
    lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
    return r;
}

/* .tgl / .tgl.on as LV_STATE_CHECKED: switching it redraws only the toggle (l5_cfg_paint) */
static lv_obj_t *toggle(lv_obj_t *row)
{
    lv_obj_t *t = l5_box(row);
    lv_obj_set_size(t, 64, 34);
    lv_obj_set_style_radius(t, 17, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(t, L5C.edge, 0);
    lv_obj_add_style(t, &L5S.tgl_on, LV_STATE_CHECKED); /* accent: follows an accent change */
    lv_obj_t *k = l5_box(t);
    lv_obj_set_size(k, 26, 26);
    lv_obj_set_pos(k, 4, 4);
    lv_obj_set_style_radius(k, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(k, L5C.knob, 0);
    lv_obj_set_style_bg_color(k, L5_WHITE, LV_STATE_CHECKED);
    lv_obj_set_style_translate_x(k, 30, LV_STATE_CHECKED);
    return t;
}

static lv_obj_t *row_title(lv_obj_t *row)
{
    return lv_obj_get_child(lv_obj_get_child(row, 0), 0); /* crow(): row > text column > title */
}

static void chev(lv_obj_t *row)
{
    l5_icon(row, &l5_icon_20, L5_ON_PANEL_MUTED, L5_ICON_CHEV);
}

static lv_obj_t *value(lv_obj_t *row)
{
    return l5_label(row, &l5_mono_15, L5_ON_PANEL, "");
}

static lv_obj_t *step_btn(lv_obj_t *parent, const char *txt, const char *key)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 44, 44);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, L5_PANEL_3, 0);
    lv_obj_add_style(b, &L5S.press_fill, LV_STATE_PRESSED);
    lv_obj_center(l5_label(b, &l5_chakra_b24, L5_ON_PANEL, txt));
    lv_obj_add_event_cb(b, step_cb, LV_EVENT_CLICKED, (void *)key);
    return b;
}

/* .step: returns the value label */
static lv_obj_t *stepper(lv_obj_t *row, const char *dec, const char *inc)
{
    lv_obj_t *s = l5_box(row);
    lv_obj_set_size(s, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(s, 4, 0);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    step_btn(s, "-", dec);
    if (K.n && K.row[K.n - 1] == row) { /* the row crow() just registered */
        K.dec[K.n - 1] = dec;
        K.inc[K.n - 1] = inc;
    }
    lv_obj_t *o = l5_label(s, &l5_mono_16, L5_ON_PANEL, "");
    lv_obj_set_width(o, 66);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    step_btn(s, "+", inc);
    return o;
}

static void swatches(lv_obj_t *row)
{
    if (K.n && K.row[K.n - 1] == row) K.color[K.n - 1] = true;
    lv_obj_t *w = l5_box(row);
    lv_obj_set_size(w, 5 * 38 + 4 * 6, LV_SIZE_CONTENT); /* five a row: the eight colours and APP */
    lv_obj_set_style_pad_row(w, 6, 0);
    lv_obj_set_style_pad_column(w, 6, 0);
    lv_obj_set_flex_flow(w, LV_FLEX_FLOW_ROW_WRAP);
    for (int i = 0; i <= APP && i < MAX_SWATCH; i++) {
        lv_obj_t *b = lv_button_create(w);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 38, 38);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        if (i == APP) { /* its colour and text: l5_cfg_paint() */
            lv_obj_center(l5_label(b, &l5_mono_12, L5_ON_PANEL, "APP"));
        } else {
            lv_obj_set_style_bg_color(b, lv_color_hex(l5_colors[i].hex), 0);
        }
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_border_color(b, L5_BLACK, 0);
        lv_obj_set_style_border_opa(b, 102, 0);
        lv_obj_add_style(b, &L5S.swatch_sel, LV_STATE_CHECKED); /* [aria-pressed]: white ring + black ring */
        lv_obj_add_style(b, &L5S.press_ring, LV_STATE_PRESSED);
        lv_obj_add_event_cb(b, color_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        W.swatch[i] = b;
    }
}

/* one preview per theme: its plate, a strip of its rail on the left, one of its panels */
static void themes(lv_obj_t *row)
{
    if (K.n && K.row[K.n - 1] == row) K.theme[K.n - 1] = true;
    lv_obj_t *w = l5_box(row);
    lv_obj_set_size(w, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(w, 8, 0);
    lv_obj_set_flex_flow(w, LV_FLEX_FLOW_ROW);
    for (int i = 0; i < L5_THEME_COUNT; i++) {
        const l5_colors_t *c = l5_theme_colors(i);
        lv_obj_t *b = lv_button_create(w);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 56, 44);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, c->plate, 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_border_color(b, L5_BLACK, 0);
        lv_obj_set_style_border_opa(b, 102, 0);
        lv_obj_add_style(b, &L5S.swatch_sel, LV_STATE_CHECKED);
        lv_obj_add_style(b, &L5S.press_ring, LV_STATE_PRESSED);
        lv_obj_add_event_cb(b, theme_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *rail = l5_box(b);
        lv_obj_set_pos(rail, 2, 2);
        lv_obj_set_size(rail, 10, 36);
        lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(rail, c->rail, 0);
        lv_obj_t *panel = l5_box(b);
        lv_obj_set_pos(panel, 17, 7);
        lv_obj_set_size(panel, 30, 26);
        lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(panel, c->panel_2, 0);
        lv_obj_set_style_border_width(panel, 1, 0);
        lv_obj_set_style_border_color(panel, c->panel_edge, 0);
        lv_obj_t *dot = l5_box(panel); /* the accent, as on the real panels */
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_pos(dot, 3, 3);
        lv_obj_set_style_radius(dot, 3, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, L5P.accent, 0);
        W.theme[i] = b;
    }
}

static void wifi_sub_deleted_cb(lv_event_t *e)
{
    (void)e;
    s_wifi_sub = NULL;
}

void l5_cfg_paint_wifi(void)
{
    char ws[64];
    if (s_wifi_sub) lv_label_set_text(s_wifi_sub, l5_wifi_summary(ws, sizeof(ws)));
}

/* keys of the Tab5 Keyboard (l5_keys.c) */
bool l5_cfg_key(const l5_key_t *k)
{
    if (!K.n) return false; /* CFG not built */
    int f = s_focus < K.n ? s_focus : -1;
    switch (k->kind) {
    case L5_KEY_UP:
    case L5_KEY_DOWN:
        if (f < 0) f = 0; /* the first key shows the ring on the first row */
        else if (k->kind == L5_KEY_DOWN) f = f + 1 < K.n ? f + 1 : K.n - 1;
        else f = f > 0 ? f - 1 : 0;
        s_focus = f;
        break;
    case L5_KEY_HOME: s_focus = 0; break;
    case L5_KEY_END: s_focus = K.n - 1; break;
    case L5_KEY_ENTER:
        if (f < 0 || !K.key[f]) return false;
        action(K.key[f]);
        return true;
    case L5_KEY_LEFT:
    case L5_KEY_RIGHT:
        if (f < 0) return false;
        if (K.dec[f]) {
            step(k->kind == L5_KEY_RIGHT ? K.inc[f] : K.dec[f]);
            return true;
        }
        if (K.theme[f]) {
            set_theme((cfg_theme() + (k->kind == L5_KEY_RIGHT ? 1 : L5_THEME_COUNT - 1)) % L5_THEME_COUNT);
            return true;
        }
        if (!K.color[f]) return false;
        { /* the colours, then APP */
            int i = 0;
            while (i < APP && strcmp(l5_colors[i].id, g_cfg.color)) i++;
            set_color((i + (k->kind == L5_KEY_RIGHT ? 1 : APP)) % (APP + 1));
        }
        return true;
    default: return false;
    }
    paint_focus();
    return true;
}

void l5_cfg_render(lv_obj_t *view)
{
    lv_obj_clean(view);
    memset(&W, 0, sizeof(W));
    memset(&K, 0, sizeof(K));
    char t[48];

    /* values, toggle states and titles come from l5_cfg_paint() at the end */
    lv_obj_t *d = column(view, 0, "DISPLAY");
    lv_obj_add_event_cb(d, widgets_deleted_cb, LV_EVENT_DELETE, NULL);
    W.bright = stepper(crow(d, NULL, "Brightness", "5% steps", false), "bright-", "bright+");
    W.dim = stepper(crow(d, NULL, "Dim time", "Screen dims when idle", false), "dim-", "dim+");
    W.off = stepper(crow(d, NULL, "Screen off", "Backlight off when idle", false), "scr-", "scr+");
    lv_obj_t *c = crow(d, NULL, "UI color", "Accent; APP: app icon", false);
    lv_obj_set_style_min_height(c, 104, 0);
    swatches(c);
    lv_obj_t *th = crow(d, NULL, "Theme", l5_themes[cfg_theme()].label, false);
    W.theme_sub = lv_obj_get_child(lv_obj_get_child(th, 0), 1);
    themes(th);
    W.rot = value(crow(d, "rot", "Rotation", "Landscape / landscape inverted", false));
    W.boot = stepper(crow(d, NULL, "Bootscreen time", "Wait before the app starts", false), "boot-", "boot+");
    chev(crow(d, "charge", "Charge Mode", "Screen off, shows battery", false));
    W.clock = toggle(crow(d, "idleClock", "Idle clock", "A clock on the dimmed screen", false));

    lv_obj_t *s = column(view, 1, "INSTALL & STORAGE");
    lv_obj_t *r = crow(s, "askSpiffs", "", "Ask before installing a SPIFFS image", false);
    W.ask_title = row_title(r);
    W.ask = toggle(r);
    r = crow(s, "onlyBins", "", "What the SD manager lists", false);
    W.bins_title = row_title(r);
    W.bins = toggle(r);
    chev(crow(s, "scheme", "Partition scheme", g_slots.scheme, false));
    chev(crow(s, "plist", "List of partitions", "Opens PMan", false));
    chev(crow(s, "clearfat", "Clear FAT partition", "Erase the vfs partition", true));
    chev(crow(s, "savesp", "Save SPIFFS", "Copy SPIFFS to /backups on the SD card", false));
    chev(crow(s, "restsp", "Restore SPIFFS", "Write a saved copy back", false));

    lv_obj_t *y = column(view, 2, "SYSTEM");
    W.wifi = toggle(crow(y, "wifiAuto", "Wi-Fi auto-connect", "Connect to saved networks at boot", false));
    W.deep = toggle(crow(y, "deepSleepStart", "Start from DeepSleep", "Open Launcher when waking up", false));
    W.btn = toggle(crow(y, "btnOnlyStart", "Only with button", "Enter Launcher only if touched at restart", false));
    char ws[64];
    lv_obj_t *w = crow(y, "wifi", "Wi-Fi networks", l5_wifi_summary(ws, sizeof(ws)), false);
    s_wifi_sub = lv_obj_get_child(lv_obj_get_child(w, 0), 1);
    lv_obj_add_event_cb(s_wifi_sub, wifi_sub_deleted_cb, LV_EVENT_DELETE, NULL);
    chev(w);
    chev(crow(y, "reset", "Reset config", "Restore config.conf defaults", true));
    snprintf(t, sizeof(t), "Launcher %.16s \xC2\xB7 LVGL UI", esp_app_get_description()->version);
    chev(crow(y, "about", "About", t, false));
    chev(crow(y, "restart", "Restart", "Back to the boot screen", false));
    chev(crow(y, "off", "Turn off", "Press POWER to start again", true));
    l5_cfg_paint();
    if (s_focus >= 0) paint_focus(); /* the ring stays on its row */
}
