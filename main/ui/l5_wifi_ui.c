/* Wi-Fi dialogs shared by the top bar, CFG and OTA: L5.wifiMenu, L5.pickWifi and
   L5.connect from app.js, on top of l5_wifi.c (ESP32-C6 through ESP-Hosted). The dialogs are
   modal, so one pick runs at a time. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "l5_ui.h"
#include "l5_wifi.h"

#define ELLIPSIS "\xE2\x80\xA6"
#define DOT "\xC2\xB7"

static const char *const IDS[L5_WIFI_MAX_APS] = {"0", "1", "2",  "3",  "4",  "5",  "6",  "7",
                                                 "8", "9", "10", "11", "12", "13", "14", "15"};

static struct {
    l5_wifi_ap_t ap[L5_WIFI_MAX_APS];
    int n;
    l5_wifi_ap_t sel;
} s_pick;

/* ---------- progress while the worker runs (U.runSteps has fixed times, this follows the C6) ---------- */

static struct {
    l5_progress_t *p;
    lv_timer_t *timer;
    int pct;
    bool connect;
} s_busy;

static void busy_tick(lv_timer_t *t)
{
    (void)t;
    l5_wifi_status_t st;
    l5_wifi_get_status(&st);
    s_busy.pct += (95 - s_busy.pct + 9) / 10; /* creeps towards 95% */
    const char *label = s_busy.connect ? (st.associated ? "Waiting for DHCP" ELLIPSIS : "Associating" ELLIPSIS)
                                       : "ESP32-C6 scan";
    if (st.state == L5_WIFI_STARTING) label = "Starting the ESP32-C6" ELLIPSIS;
    l5_progress_set(s_busy.p, s_busy.pct, label);
}

static void busy_close(void)
{
    if (s_busy.timer) lv_timer_delete(s_busy.timer);
    l5_progress_close(s_busy.p);
    memset(&s_busy, 0, sizeof(s_busy));
}

static void busy_open(const char *title, bool connect)
{
    busy_close();
    s_busy.p = l5_progress_open("WI-FI", title);
    s_busy.connect = connect;
    s_busy.timer = lv_timer_create(busy_tick, 150, NULL);
    busy_tick(NULL);
}

/* ---------- L5.connect ---------- */

/* The C6 runs another ESP-Hosted series than the launcher's host (the first Tab5 units: 1.4.1): what to flash, and
   that the SD card works without it. */
static void mismatch_info(const l5_wifi_status_t *st)
{
    char runs[48], title[48], body[560];
    if (st->c6_fw[0] && strcmp(st->c6_fw, "unknown")) {
        snprintf(runs, sizeof(runs), "runs ESP-Hosted %s", st->c6_fw);
    } else {
        strlcpy(runs, "did not report its ESP-Hosted version", sizeof(runs));
    }
    snprintf(title, sizeof(title), "ESP32-C6 needs ESP-Hosted %s", l5_wifi_host_series());
    snprintf(body, sizeof(body),
             "The ESP32-C6 (the Wi-Fi chip) %s. This launcher talks to ESP-Hosted %s.x only, so Wi-Fi and OTA stay "
             "off.\n\nTo turn them on, flash the C6 with M5Burner: \"Tab5 ESP-Hosted Firmware Upgrade\" or \"Tab5 "
             "ESP32-C6 Wi-Fi SDIO\", version %s. Then restart the Tab5.\n\nInstalling from the SD card works without "
             "Wi-Fi: copy .bin files to the card and open the SD tab.",
             runs, l5_wifi_host_series(), l5_wifi_host_version());
    l5_info("WI-FI", title, body);
}

static void failed_info(void)
{
    l5_wifi_status_t st;
    l5_wifi_get_status(&st);
    if (st.c6_mismatch) {
        mismatch_info(&st);
        return;
    }
    char body[440];
    snprintf(body, sizeof(body), "%s\n\nLauncher: ESP-Hosted host %s. ESP32-C6: %s. Both sides need the same "
             "ESP-Hosted major.minor version.\n\nInstalling from the SD card works without Wi-Fi.", st.error,
             l5_wifi_host_version(), st.c6_fw[0] ? st.c6_fw : "not read");
    l5_info("WI-FI", "ESP32-C6 not available", body);
}

void l5_wifi_why(void)
{
    failed_info();
}

static void connect_cb(l5_wifi_result_t res, const char *detail, void *user)
{
    bool saved = user != NULL;
    busy_close();
    l5_wifi_status_t st;
    l5_wifi_get_status(&st);
    char body[320];
    switch (res) {
    case L5_WIFI_OK:
        l5_toast("Connected to %s " DOT " %s", st.ssid, st.ip);
        break;
    case L5_WIFI_ERR_PASSWORD:
        snprintf(body, sizeof(body),
                 saved ? "The access point rejected the saved password, so it was removed. Pick %s again to type "
                         "the new one."
                       : "The access point rejected the password for %s. WPA2/WPA3 passwords have at least 8 "
                         "characters.",
                 s_pick.sel.ssid);
        l5_info("WI-FI", "Wrong password", body);
        break;
    case L5_WIFI_ERR_NOT_FOUND:
        snprintf(body, sizeof(body), "%s did not answer. The ESP32-C6 only sees 2.4 GHz networks.", s_pick.sel.ssid);
        l5_info("WI-FI", "Network not found", body);
        break;
    case L5_WIFI_ERR_TIMEOUT:
        snprintf(body, sizeof(body), "%s accepted the password, but no IP address came from DHCP in 20 s.",
                 s_pick.sel.ssid);
        l5_info("WI-FI", "No IP address", body);
        break;
    case L5_WIFI_ERR_C6:
        if (st.state == L5_WIFI_FAILED) {
            failed_info();
        } else {
            l5_info("WI-FI", "Wi-Fi error", detail);
        }
        break;
    default:
        snprintf(body, sizeof(body), "%s refused the connection. %s", s_pick.sel.ssid, detail);
        l5_info("WI-FI", "Could not connect", body);
        break;
    }
}

static void connect(const char *pass)
{
    const l5_wifi_ap_t *ap = &s_pick.sel;
    if (pass && l5_wifi_password_too_short(ap->auth, pass)) {
        l5_info("WI-FI", "Password too short", "WPA2/WPA3 passwords have at least 8 characters.");
        return;
    }
    /* user data: the saved password is used */
    if (l5_wifi_connect(ap->ssid, pass, ap->auth, connect_cb, pass ? NULL : (void *)1) != ESP_OK) {
        l5_toast("Wi-Fi is busy, try again");
        return;
    }
    char title[64];
    snprintf(title, sizeof(title), "Connecting to %s", ap->ssid);
    busy_open(title, true);
}

/* ---------- L5.pickWifi ---------- */

static void password_cb(const char *text, void *user)
{
    (void)user;
    if (text) connect(text);
}

static void pick_cb(const char *id, void *user)
{
    (void)user;
    int i = id ? atoi(id) : -1;
    if (i < 0 || i >= s_pick.n) return;
    s_pick.sel = s_pick.ap[i];
    const l5_wifi_ap_t *ap = &s_pick.sel;
    if (!l5_wifi_auth_supported(ap->auth)) {
        char body[200];
        snprintf(body, sizeof(body), "%s uses enterprise (802.1X) sign-in, which needs a user name and certificates. "
                 "The launcher joins open and WPA/WPA2/WPA3-Personal networks.", ap->ssid);
        l5_info("WI-FI", "Not supported", body);
        return;
    }
    if (!l5_wifi_auth_needs_password(ap->auth)) {
        connect("");
    } else if (l5_wifi_saved_password(ap->ssid, NULL, 0)) {
        connect(NULL);
    } else {
        char title[64];
        snprintf(title, sizeof(title), "PASSWORD " DOT " %s", ap->ssid);
        const l5_keyboard_opts_t opts = {.title = title, .mask = true, .max_len = 64};
        l5_keyboard(&opts, password_cb, NULL);
    }
}

static void scan_cb(esp_err_t err, const l5_wifi_ap_t *aps, int n, void *user)
{
    (void)user;
    busy_close();
    l5_wifi_status_t st;
    l5_wifi_get_status(&st);
    if (st.state == L5_WIFI_FAILED) {
        failed_info();
        return;
    }
    if (err != ESP_OK) {
        char body[120];
        snprintf(body, sizeof(body), "The ESP32-C6 could not scan (%s).", esp_err_to_name(err));
        l5_info("WI-FI", "Scan failed", body);
        return;
    }
    if (n == 0) {
        l5_info("WI-FI", "No networks found", "The ESP32-C6 sees no 2.4 GHz network. Hidden networks are not listed.");
        return;
    }
    s_pick.n = n;
    memcpy(s_pick.ap, aps, n * sizeof(l5_wifi_ap_t));
    l5_menu_item_t items[L5_WIFI_MAX_APS];
    char labels[L5_WIFI_MAX_APS][72];
    for (int i = 0; i < n; i++) {
        const l5_wifi_ap_t *ap = &s_pick.ap[i];
        snprintf(labels[i], sizeof(labels[i]), "%s  " DOT "  %d dBm  " DOT "  %s%s", ap->ssid, ap->rssi,
                 l5_wifi_auth_name(ap->auth), l5_wifi_saved_password(ap->ssid, NULL, 0) ? "  " DOT "  saved" : "");
        items[i] = (l5_menu_item_t){IDS[i], l5_wifi_auth_needs_password(ap->auth) ? L5_ICON_LOCK : L5_ICON_WIFI,
                                    labels[i], false};
    }
    l5_menu("WI-FI SCAN", "Select a network", items, n, pick_cb, NULL);
}

void l5_wifi_pick(void)
{
    if (l5_wifi_scan(scan_cb, NULL) != ESP_OK) {
        l5_toast("Wi-Fi is busy, try again");
        return;
    }
    busy_open("Scanning Wi-Fi", false);
}

/* ---------- L5.wifiMenu ---------- */

static char s_saved[L5_WIFI_MAX_SAVED][33];

static void forget_cb(bool ok, void *user)
{
    (void)user;
    if (!ok) return;
    l5_wifi_forget_all();
    l5_toast("Saved networks removed");
}

static void forget_one_cb(bool ok, void *user)
{
    const char *ssid = s_saved[(intptr_t)user];
    if (!ok) return;
    l5_wifi_forget(ssid);
    l5_toast("%s forgotten", ssid);
}

static void saved_cb(const char *id, void *user)
{
    (void)user;
    int i = id ? atoi(id) : -1;
    if (i < 0 || i >= L5_WIFI_MAX_SAVED || !s_saved[i][0]) return;
    char title[64];
    snprintf(title, sizeof(title), "Forget %s?", s_saved[i]);
    l5_confirm("CONFIRM", title, "Its saved password is removed. A connection that is up stays up.", "FORGET", true,
               forget_one_cb, (void *)(intptr_t)i);
}

/* firmware addition: the prototype only forgets all networks at once */
static void saved_menu(void)
{
    memset(s_saved, 0, sizeof(s_saved));
    int n = l5_wifi_saved_ssids(s_saved, L5_WIFI_MAX_SAVED);
    l5_menu_item_t items[L5_WIFI_MAX_SAVED];
    for (int i = 0; i < n; i++) items[i] = (l5_menu_item_t){IDS[i], L5_ICON_LOCK, s_saved[i], false};
    l5_menu("WI-FI", "Saved networks", items, n, saved_cb, NULL);
}

static void menu_cb(const char *id, void *user)
{
    (void)user;
    if (!id) return;
    if (!strcmp(id, "scan")) {
        l5_wifi_pick();
    } else if (!strcmp(id, "disc")) {
        l5_wifi_disconnect();
        l5_toast("Wi-Fi disconnected");
    } else if (!strcmp(id, "saved")) {
        saved_menu();
    } else if (!strcmp(id, "forget")) {
        l5_confirm("CONFIRM", "Forget saved networks?",
                   "Saved passwords are removed from the launcher (NVS). The current connection stays up.", "FORGET",
                   true, forget_cb, NULL);
    }
}

void l5_wifi_menu(void)
{
    l5_wifi_status_t st;
    l5_wifi_get_status(&st);
    if (st.state == L5_WIFI_FAILED) {
        failed_info();
        return;
    }
    char title[96];
    bool linked = st.state == L5_WIFI_CONNECTED || st.state == L5_WIFI_CONNECTING;
    if (st.state == L5_WIFI_CONNECTED) {
        snprintf(title, sizeof(title), "Connected: %s (%s)", st.ssid, st.ip);
    } else if (st.state == L5_WIFI_CONNECTING) {
        snprintf(title, sizeof(title), "Connecting to %s" ELLIPSIS, st.ssid);
    } else if (st.state == L5_WIFI_STARTING) {
        snprintf(title, sizeof(title), "Starting the ESP32-C6" ELLIPSIS);
    } else {
        snprintf(title, sizeof(title), "Not connected");
    }
    char saved[40];
    int n = l5_wifi_saved_count();
    snprintf(saved, sizeof(saved), "Saved networks (%d)", n);
    const l5_menu_item_t items[] = {
        {linked ? NULL : "scan", L5_ICON_SEARCH, "Scan and connect", false},
        {linked ? "disc" : NULL, L5_ICON_X, "Disconnect", false},
        {n ? "saved" : NULL, L5_ICON_LOCK, saved, false},
        {"forget", L5_ICON_TRASH, "Forget saved networks", true},
    };
    l5_menu("WI-FI", title, items, 4, menu_cb, NULL);
}

/* ---------- status in the views ---------- */

const char *l5_wifi_summary(char *buf, size_t len)
{
    l5_wifi_status_t st;
    l5_wifi_get_status(&st);
    switch (st.state) {
    case L5_WIFI_CONNECTED: snprintf(buf, len, "Connected: %s", st.ssid); break;
    case L5_WIFI_CONNECTING: snprintf(buf, len, "Connecting to %s" ELLIPSIS, st.ssid); break;
    case L5_WIFI_STARTING: snprintf(buf, len, "Starting the ESP32-C6" ELLIPSIS); break;
    case L5_WIFI_FAILED: snprintf(buf, len, "ESP32-C6 not available"); break;
    default: snprintf(buf, len, "%d saved", l5_wifi_saved_count()); break;
    }
    return buf;
}

static void on_status(void)
{
    l5_topbar_paint_wifi();
    l5_cfg_paint_wifi();
    l5_ota_paint_wifi();
}

void l5_wifi_ui_init(void)
{
    l5_wifi_set_listener(on_status);
}
