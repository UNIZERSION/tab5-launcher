/* #sys screens of app.js: app hand-over, restart, power (the bootscreen is in l5_boot.c). */
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "l5_board.h"
#include "l5_config.h"
#include "l5_display.h"
#include "l5_slots.h"
#include "l5_ui.h"
#include "l5_wifi.h"

static const char *TAG = "l5_sys";

/* L5.sysShow: clears #layer and fills #sys */
static lv_obj_t *sys_show(lv_color_t top, lv_color_t bottom)
{
    l5_boot_screen_stop();
    l5_dialogs_close_all();
    lv_obj_t *sys = l5_ui_sys();
    lv_obj_clean(sys);
    l5_ui_covered(true);
    lv_obj_t *el = l5_box(sys);
    lv_obj_set_size(el, L5_SCREEN_W, L5_SCREEN_H);
    lv_obj_add_flag(el, LV_OBJ_FLAG_CLICKABLE); /* blocks the launcher underneath */
    lv_obj_set_style_bg_opa(el, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(el, top, 0);
    if (!lv_color_eq(top, bottom)) { /* the bootscreen; the black screens have no gradient */
        lv_obj_set_style_bg_grad_color(el, bottom, 0);
        lv_obj_set_style_bg_grad_dir(el, LV_GRAD_DIR_VER, 0);
        l5_dither_hook(el);
    }
    return el;
}

lv_obj_t *l5_sys_show(lv_color_t top, lv_color_t bottom)
{
    return sys_show(top, bottom);
}

void l5_sys_hide(void)
{
    l5_boot_screen_stop();
    lv_obj_clean(l5_ui_sys());
    l5_ui_covered(false);
    lv_display_trigger_activity(NULL);
}

static lv_obj_t *sys_text_screen(const char *text)
{
    lv_obj_t *el = sys_show(L5_BLACK, L5_BLACK); /* .off */
    lv_obj_t *l = l5_label(el, &l5_mono_15, lv_color_hex(0x3a3c41), text);
    lv_obj_set_style_text_letter_space(l, 3, 0);
    lv_obj_center(l);
    return el;
}

/* a key of the Tab5 Keyboard while #sys shows a screen (l5_keys.c): on the bootscreen any key is ENTER LAUNCHER,
   another screen (Charge Mode) gets it as l5_key_event() */
bool l5_sys_key(const l5_key_t *k)
{
    if (l5_boot_screen_key(k)) return true;
    lv_obj_t *el = lv_obj_get_child(l5_ui_sys(), 0);
    if (el) lv_obj_send_event(el, l5_key_event(), (void *)k);
    return el != NULL;
}

/* ---------- hand-over to the selected app (L5.bootApp) ---------- */

static void handoff_task(void *arg)
{
    int slot = (int)(intptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(120)); /* let the "starting" frame reach the panel */
    l5_cfg_flush();                 /* a setting changed in the last 300 ms */
    esp_err_t err = l5_slots_handoff(slot, g_cfg.deepSleepStart); /* restarts on success */
    ESP_LOGE(TAG, "app%d hand-over failed: %s", slot, esp_err_to_name(err));
    l5_board_lock(0);
    l5_sys_hide();
    l5_ui_set_booted(true);
    l5_ui_show(L5_VIEW_HOME);
    char body[120];
    snprintf(body, sizeof(body), "app%d did not pass the image check (%s). Reinstall it from SD or OTA.",
             slot, esp_err_to_name(err));
    l5_info("BOOT", "Cannot start the app", body);
    l5_board_unlock();
    vTaskDelete(NULL);
}

/* .app-run: "APPn · OTA_n · STARTING" card while the hand-over task runs */
static void starting_screen(int slot, const char *name)
{
    lv_obj_t *el = sys_show(L5_BLACK, L5_BLACK); /* .app-run */
    lv_obj_t *card = l5_box(el);
    lv_obj_set_size(card, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(card, 40, 0);
    lv_obj_set_style_pad_hor(card, 56, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_style_bg_opa(card, 153, 0);
    lv_obj_set_style_bg_color(card, L5_BLACK, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(card);
    char t[160];
    snprintf(t, sizeof(t), "APP%d \xC2\xB7 OTA_%d \xC2\xB7 STARTING", slot, slot);
    lv_obj_t *k = l5_label(card, &l5_mono_14, lv_color_hex(0xb9bcc2), t);
    lv_obj_set_style_text_letter_space(k, 3, 0);
    l5_label(card, &l5_chakra_b40, L5_WHITE, name);
    snprintf(t, sizeof(t), "Launcher selects app%d in otadata and restarts into it.\nPress RESET on the side to come back to the "
             "Launcher boot screen.", slot);
    lv_obj_t *p = l5_label(card, &l5_chakra_r17, lv_color_hex(0xc9ccd2), t);
    lv_obj_set_style_text_align(p, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(p, 6, 0);
}

static void boot_app(bool by_user)
{
    const l5_slot_t *a = l5_slots_boot_app();
    if (!a) {
        l5_toast("No app installed");
        return;
    }
    if (by_user) { /* the user starts it on purpose: no stop mark, the boot-loop guard starts over */
        if (l5_slots_stopped() == g_slots.boot) l5_slots_clear_stopped();
        l5_slots_guard_clear();
    }
    l5_ui_set_booted(false);
    starting_screen(g_slots.boot, a->name);
    xTaskCreatePinnedToCore(handoff_task, "l5_handoff", 4096, (void *)(intptr_t)g_slots.boot, 3, NULL, 1);
}

void l5_boot_app(void)
{
    boot_app(true);
}

void l5_boot_app_auto(void)
{
    boot_app(false);
}

static struct {
    uint32_t index, count, addr, size;
} s_at;

static void handoff_at_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(120)); /* let the "starting" frame reach the panel */
    l5_cfg_flush();
    esp_err_t err = l5_slots_handoff_at(s_at.index, s_at.count, s_at.addr, s_at.size, g_cfg.deepSleepStart);
    ESP_LOGE(TAG, "ota_%lu hand-over failed: %s", (unsigned long)s_at.index, esp_err_to_name(err));
    l5_restart_to_launcher(); /* the table changed since boot: the launcher starts over with it */
    vTaskDelete(NULL);
}

void l5_boot_at(uint32_t ota_index, uint32_t ota_count, uint32_t addr, uint32_t size, const char *name)
{
    s_at.index = ota_index;
    s_at.count = ota_count;
    s_at.addr = addr;
    s_at.size = size;
    l5_slots_guard_clear(); /* START APP after AUTO PARTITION: the user's choice */
    l5_ui_set_booted(false);
    starting_screen((int)ota_index, name);
    xTaskCreatePinnedToCore(handoff_at_task, "l5_handoff", 4096, NULL, 3, NULL, 1);
}

/* ---------- an app the boot-loop guard stopped ---------- */

static int s_stopped_slot;

static void stopped_cb(int index, void *user)
{
    (void)user;
    char label[8];
    snprintf(label, sizeof(label), "app%d", s_stopped_slot);
    if (index == 0) { /* ERASE APP: PMAN's FORMAT asks first */
        l5_pman_format(label);
    } else if (index == 1) { /* BOOT AGAIN */
        l5_slots_clear_stopped();
        l5_slots_select(s_stopped_slot);
        l5_boot_app();
    } /* OK: it stays stopped until it is booted from HOME */
}

void l5_stopped_dialog(int slot, int restarts, int returns)
{
    l5_enter_launcher();
    if (slot < 0 || slot >= g_slots.n || !g_slots.slot[slot].used) return;
    s_stopped_slot = slot;
    const char *name = g_slots.slot[slot].name;
    char reason[40], title[96], body[400];
    switch (esp_reset_reason()) { /* the app's last reset, as its esp_restart / panic handler noted it */
    case ESP_RST_PANIC: snprintf(reason, sizeof(reason), "panic"); break;
    case ESP_RST_INT_WDT: snprintf(reason, sizeof(reason), "interrupt watchdog"); break;
    case ESP_RST_TASK_WDT: snprintf(reason, sizeof(reason), "task watchdog"); break;
    case ESP_RST_WDT: snprintf(reason, sizeof(reason), "watchdog"); break;
    case ESP_RST_SW: snprintf(reason, sizeof(reason), "esp_restart"); break;
    case ESP_RST_BROWNOUT: snprintf(reason, sizeof(reason), "brown-out"); break;
    default: snprintf(reason, sizeof(reason), "reset reason %d", (int)esp_reset_reason()); break;
    }
    snprintf(title, sizeof(title), "%.60s was stopped", name);
    if (restarts > 0) {
        snprintf(body, sizeof(body), "%.60s restarted %d times right after starting (%s) and was stopped. It may not "
                 "support this Tab5. The bootscreen does not start it until you boot it again.", name, restarts, reason);
    } else if (returns > 0) {
        snprintf(body, sizeof(body), "%.60s came back to the Launcher %d times within 15 s of starting (%s) and was "
                 "stopped. It may not support this Tab5. The bootscreen does not start it until you boot it again.",
                 name, returns, reason);
    } else {
        snprintf(body, sizeof(body), "%.60s was stopped on an earlier start: it restarted again and again right after "
                 "starting. It may not support this Tab5. The bootscreen does not start it until you boot it again.", name);
    }
    static const char *const labels[] = {"ERASE APP", "BOOT AGAIN", "OK"};
    l5_choose("BOOT", title, body, labels, 3, L5_ICON_CHECK, stopped_cb, NULL);
}

/* ---------- restart / power ---------- */

static void restart_cb(lv_timer_t *t)
{
    (void)t;
    l5_restart_to_launcher();
}

void l5_restart(void)
{
    l5_cfg_flush(); /* a setting changed in the last 300 ms */
    l5_ui_set_booted(false);
    sys_text_screen("RESTARTING\xE2\x80\xA6");
    lv_timer_t *t = lv_timer_create(restart_cb, 300, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void power_off_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(150));
    l5_board_backlight(0);
    l5_cfg_flush();  /* a setting changed in the last 300 ms */
    l5_wifi_pause(); /* the access point sees the Tab5 leave; the C6 loses power with the board */
    esp_err_t err = l5_board_power_off(); /* returns only if power stayed on */
    l5_board_charger_on(); /* the power-off reset the expander 0x44 (BSP driver): on USB the pack charges again */
    l5_wifi_resume(); /* still powered (USB): Wi-Fi comes back and rejoins */
    l5_board_lock(0);
    l5_board_backlight(g_cfg.bright);
    l5_sys_hide();
    l5_ui_set_booted(true);
    l5_info("POWER", "Power-off failed", esp_err_to_name(err));
    l5_board_unlock();
    vTaskDelete(NULL);
}

void l5_power_off(void)
{
    l5_ui_set_booted(false);
    sys_text_screen("POWERED OFF \xC2\xB7 PRESS POWER");
    xTaskCreatePinnedToCore(power_off_task, "l5_poweroff", 4096, NULL, 3, NULL, 1);
}

static void power_menu_cb(const char *id, void *user)
{
    (void)user;
    if (!id) {
        return;
    }
    if (strcmp(id, "app") == 0) {
        l5_boot_app();
    } else if (strcmp(id, "restart") == 0) {
        l5_restart();
    } else if (strcmp(id, "charge") == 0) {
        l5_charge_mode();
    } else if (strcmp(id, "off") == 0) {
        l5_power_off();
    }
}

void l5_power_menu(void)
{
    if (l5_pman_busy()) { /* the partition table is being rewritten */
        l5_toast("Wait: the partition table is being written");
        return;
    }
    if (l5_install_busy()) {
        l5_toast("Wait: an install is running");
        return;
    }
    const l5_slot_t *a = l5_slots_boot_app();
    char app[64];
    snprintf(app, sizeof(app), "APP \xC2\xB7 Boot %s", a ? a->name : "");
    const l5_menu_item_t items[] = {
        {a ? "app" : NULL, L5_ICON_LAUNCH, app, false},
        {"restart", L5_ICON_RESTART, "Restart", false},
        {"charge", L5_ICON_BOLT, "Charge Mode", false},
        {"off", L5_ICON_POWER, "OFF \xC2\xB7 Turn off", true},
    };
    l5_menu("POWER", "Power", items, 4, power_menu_cb, NULL);
}
