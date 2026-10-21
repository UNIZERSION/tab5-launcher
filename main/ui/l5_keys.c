/* Keys of the M5Stack Tab5 Keyboard in the whole launcher (l5_kbd.c reads them). Every 20 ms the LVGL task takes up
   to 8 and sends each where a tap would go:
   - a dimmed or dark screen: the first key only wakes it, like the first tap;
   - an open dialog, menu or keyboard (#layer): the topmost one gets the key as the LVGL event l5_key_event(), the key
     as its parameter (l5_dialogs.c, l5_keyboard.c);
   - a system screen (#sys): l5_sys_key(), the bootscreen enters the launcher, Charge Mode exits on Enter / Esc;
   - otherwise Tab / Shift+Tab and Ctrl+Right / Ctrl+Left switch tabs, everything else goes to the visible tab
     (HOME, SD, OTA, CFG; PMAN is touch only). */
#include "l5_kbd.h"
#include "l5_ui.h"

#define KEYS_PER_TICK 8

static uint32_t s_event;

uint32_t l5_key_event(void)
{
    if (!s_event) s_event = lv_event_register_id();
    return s_event;
}

static void dispatch(const l5_key_t *k)
{
    if (l5_ui_asleep()) {
        l5_ui_wake(); /* swallowed, like the waking tap on the veil */
        return;
    }
    lv_display_trigger_activity(NULL); /* a key counts as use: no dimming while typing */
    lv_obj_t *layer = l5_ui_layer();
    uint32_t n = lv_obj_get_child_count(layer);
    if (n) {
        lv_obj_send_event(lv_obj_get_child(layer, (int32_t)n - 1), l5_key_event(), (void *)k);
        return;
    }
    if (lv_obj_get_child_count(l5_ui_sys())) {
        l5_sys_key(k);
        return;
    }
    int dir = 0;
    if (k->kind == L5_KEY_TAB && !k->ctrl && !k->alt) {
        dir = k->shift ? -1 : 1;
    } else if (k->ctrl && (k->kind == L5_KEY_LEFT || k->kind == L5_KEY_RIGHT)) {
        dir = k->kind == L5_KEY_RIGHT ? 1 : -1;
    }
    if (dir) {
        l5_ui_go((l5_view_t)((l5_ui_view() + dir + L5_VIEW_COUNT) % L5_VIEW_COUNT));
        return;
    }
    switch (l5_ui_view()) {
    case L5_VIEW_HOME: l5_home_key(k); break;
    case L5_VIEW_SD: l5_sd_key(k); break;
    case L5_VIEW_OTA: l5_ota_key(k); break;
    case L5_VIEW_CFG: l5_cfg_key(k); break;
    default: break;
    }
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    l5_key_t k;
    for (int i = 0; i < KEYS_PER_TICK && l5_kbd_get(&k); i++) dispatch(&k);
}

void l5_keys_init(void)
{
    l5_key_event();
    lv_timer_create(timer_cb, 20, NULL);
}
