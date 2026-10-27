/* Keys of the M5Stack Tab5 Keyboard in the whole launcher (l5_kbd.c reads them). Every 20 ms the LVGL task takes up
   to 8 and sends each where a tap would go:
   - a dimmed or dark screen: the first key only wakes it, like the first tap;
   - an open dialog, menu or keyboard (#layer): the topmost one gets the key as the LVGL event l5_key_event(), the key
     as its parameter (l5_dialogs.c, l5_keyboard.c);
   - a system screen (#sys): l5_sys_key(), the bootscreen enters the launcher, Charge Mode exits on Enter / Esc;
   - otherwise Tab / Shift+Tab and Ctrl+Right / Ctrl+Left switch tabs, 1..5 (with Alt too) open HOME, SD, OTA, PMAN,
     CFG and the visible tab's own digit builds it again as a tap on it does (also F5), ? or Ctrl+H (F1) shows the
     shortcuts (l5_keys_help), everything else goes to the visible tab (HOME, SD, OTA, PMAN, CFG). The Tab5 Keyboard
     has no F-keys and its Alt is far from the digits: single digits are the tabs (SD's type-to-find takes letters). */
#include <stdio.h>
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
    if (k->kind == L5_KEY_CHAR && !k->ctrl && k->ch >= '1' && k->ch < '1' + L5_VIEW_COUNT) {
        l5_ui_go((l5_view_t)(k->ch - '1')); /* the rail's order; the visible tab's digit builds it again */
        return;
    }
    if (k->kind == L5_KEY_F1 || (k->kind == L5_KEY_CHAR && k->ch == '?' && !k->ctrl && !k->alt)) {
        l5_keys_help(); /* '?' is not a letter of FAT names: SD's type-to-find never needs it */
        return;
    }
    if (k->kind == L5_KEY_F(5)) {
        l5_ui_go(l5_ui_view()); /* the active tab again: re-reads the card, the partition table, the catalog */
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
    case L5_VIEW_PMAN: l5_pman_key(k); break;
    case L5_VIEW_CFG: l5_cfg_key(k); break;
    default: break;
    }
}

/* ? / Ctrl+H: what the keys of the Tab5 Keyboard do everywhere, in this tab, in text fields and on the bootscreen */
void l5_keys_help(void)
{
    static const char *const TAB[L5_VIEW_COUNT] = {
        "HOME\n"
        "Left / Right, Up / Down: the previous / next app starts after the bootscreen\n"
        "Enter: BOOT the app in the middle",
        "SD\n"
        "Up / Down: select \xC2\xB7 Ctrl+Up / Down: 8 rows \xC2\xB7 Alt+Up / Down: first / last\n"
        "type letters: jump to a name \xC2\xB7 Enter: open, install a .bin, view a text\n"
        "Esc / Backspace: folder up \xC2\xB7 Del: delete \xC2\xB7 Ctrl+R rename \xC2\xB7 Ctrl+N new folder\n"
        "Ctrl+C copy \xC2\xB7 Ctrl+X cut \xC2\xB7 Ctrl+V paste",
        "OTA\n"
        "Up / Down: select \xC2\xB7 Ctrl+Up / Down: a page \xC2\xB7 Alt+Up / Down: first / last\n"
        "Enter: open \xC2\xB7 / or Ctrl+F: search",
        "PMAN\n"
        "Up / Down: select a partition \xC2\xB7 Alt+Up / Down: first / last \xC2\xB7 Enter: its actions\n"
        "Ctrl+R resize \xC2\xB7 Del: delete \xC2\xB7 Ctrl+S scheme \xC2\xB7 Ctrl+O optimize\n"
        "Ctrl+A auto partition \xC2\xB7 Ctrl+K sanity check",
        "CFG\n"
        "Up / Down: a row \xC2\xB7 Left / Right: change it \xC2\xB7 Enter: switch or open",
    };
    static const char FMT[] =
             "EVERYWHERE\n"
             "1 \xE2\x80\xA6 5: HOME, SD, OTA, PMAN, CFG (the same digit again: read the tab again)\n"
             "Tab or Ctrl+Left / Right: next / previous tab \xC2\xB7 ? or Ctrl+H: this help\n"
             "Enter: OK \xC2\xB7 Esc: close\n\n"
             "%s\n\n"
             "TEXT FIELDS\n"
             "Left / Right: move \xC2\xB7 Alt+Left / Right: start / end \xC2\xB7 Ctrl+Left / Right: a word\n"
             "Backspace / Del: erase \xC2\xB7 Ctrl+Backspace: a word \xC2\xB7 Ctrl+U: clear\n"
             "Ctrl+C / Ctrl+X: copy / cut all \xC2\xB7 Ctrl+V: paste \xC2\xB7 Ctrl+H: this help\n"
             "Alt + a c e l n o s x z: \xC4\x85 \xC4\x87 \xC4\x99 \xC5\x82 \xC5\x84 \xC3\xB3 \xC5\x9B \xC5\xBA "
             "\xC5\xBC (with Aa: capitals)\n\n"
             "BOOTSCREEN\n"
             "Enter: START NOW \xC2\xB7 Esc: LAUNCHER \xC2\xB7 Space: stop the countdown \xC2\xB7 1 \xE2\x80\xA6 4: the "
             "other apps";
    const char *tab = TAB[l5_ui_view() < L5_VIEW_COUNT ? l5_ui_view() : 0];
    const int len = snprintf(NULL, 0, FMT, tab); /* as long as the text is: nothing is cut */
    char *body = len > 0 ? lv_malloc((size_t)len + 1) : NULL;
    if (!body) return;
    snprintf(body, (size_t)len + 1, FMT, tab);
    l5_text_dialog("Shortcuts", "UP / DOWN, CTRL+UP / DOWN SCROLL", body, 1, lv_free); /* long: it scrolls */
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
