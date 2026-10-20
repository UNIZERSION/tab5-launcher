/* Tab5 Launcher: native LVGL 9 port of the tab5-launcher HTML prototype.
   Start-up follows L5.start(): load config.conf, paint the launcher, show the bootscreen. */
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "l5_board.h"
#include "l5_config.h"
#include "l5_kbd.h"
#include "l5_ptable.h"
#include "l5_slots.h"
#include "l5_wifi.h"
#include "ui/l5_ui.h"

static const char *TAG = "launcher";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    /* the bootloader's boot-loop guard: read and clear before anything else (the launcher runs) */
    int guard_slot = -1, guard_restarts = 0, guard_returns = 0;
    bool guarded = l5_slots_guard_take(&guard_slot, &guard_restarts, &guard_returns);
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "running from %s @0x%lx, reset reason %d, internal RAM %u bytes free", running->label,
             (unsigned long)running->address, (int)esp_reset_reason(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    l5_config_defaults(&g_cfg);
    /* the charger first (CHG_EN, as Aurora), before the panel's ~2 s start; again after it if the I2C bus was stuck
       (the panel start recovers the bus) */
    bool charger = l5_board_charger_on() == ESP_OK;
    lv_display_t *disp = l5_board_display_start(0);
    if (!disp) {
        return;
    }
    if (!charger && l5_board_charger_on() != ESP_OK) {
        ESP_LOGW(TAG, "charger: CHG_EN not set");
    }
    /* the BSP powered the I2C bus and IO expanders for the panel; SD, RTC and battery follow */
    if (l5_board_sd_mount() == ESP_OK) {
        l5_config_load(&g_cfg);
    }
    if (l5_board_sensors_init() != ESP_OK) {
        ESP_LOGW(TAG, "RTC / battery monitor not available");
    }
    l5_pt_aurora_sanitize(); /* aurora_cfg written over USB on old FAT bytes: make it usable for Aurora */
    l5_slots_scan();
    if (guarded) {
        ESP_LOGW(TAG, "app%d stopped by the boot-loop guard: %d quick restarts, %d early returns (reset reason %d)",
                 guard_slot, guard_restarts, guard_returns, (int)esp_reset_reason());
        l5_slots_mark_stopped(guard_slot);
    }
    /* a stopped app does not start by itself until the user chooses (l5_stopped_dialog) */
    int stopped = l5_slots_stopped();
    bool boot_stopped = stopped >= 0 && stopped == g_slots.boot;
    l5_board_set_rotation(g_cfg.rot);
    l5_kbd_init(); /* M5Stack Tab5 Keyboard on the side port, own I2C bus: looked for once a second */

    l5_board_lock(0);
    l5_theme_init(l5_config_accent_hex(&g_cfg));
    l5_ui_init();
    l5_wifi_ui_init();
    l5_ui_prepare(L5_VIEW_HOME); /* the bootscreen (or the app's start screen) is the first frame */
    /* cfg.btnOnlyStart: without a finger on the screen the app starts right away */
    if (g_cfg.btnOnlyStart && l5_slots_boot_app() && !boot_stopped && !l5_board_touch_pressed()) {
        l5_wifi_init(false);
        l5_boot_app_auto();
    } else {
        l5_wifi_init(g_cfg.wifiAuto); /* L5.start: saved networks join in the background */
        bool just_stopped = guarded && guard_slot == stopped;
        if (boot_stopped) {
            l5_stopped_dialog(stopped, just_stopped ? guard_restarts : -1, just_stopped ? guard_returns : -1);
        } else {
            l5_boot_screen();
        }
    }
    l5_board_reveal(g_cfg.bright); /* the backlight comes on only when the first frame is shown */
    l5_board_unlock();
}
