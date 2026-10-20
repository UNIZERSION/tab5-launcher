/* Wi-Fi worker: ESP32-C6 power, ESP-Hosted transport, scans, connections, saved networks.

   The C6 has to run the ESP-Hosted release of this host (see idf_component.yml). With another
   release the two sides misread each other's messages: a 1.4.1 host took the scan of a 2.12.6 C6
   for a second WIFI_EVENT_STA_START and lwIP asserted. So the version the C6 reports is checked
   before Wi-Fi starts on it, and a mismatch leaves Wi-Fi off with a message.
   ESP-Hosted's own restarts are off (CONFIG_ESP_HOSTED_TRANSPORT_RESTART_ON_FAILURE=n) and its
   restart hook is replaced as a backstop: a software restart of the launcher boots the app
   selected in otadata.
   Every request to the C6 comes from the worker task. Before esp_restart() the worker stops Wi-Fi,
   then ESP-Hosted's tasks, and only then is the C6 switched off (wifi_off_at_restart). */
#include "l5_wifi.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "l5_board.h"

static const char *TAG = "l5_wifi";

#define NVS_NS "l5_wifi"
#define NVS_KEY "nets"
#define NVS_KEY_START "starting" /* set while the C6 comes up: left over = that start crashed */
#define SCAN_RECORDS 20 /* records read from the C6 per scan */

#define EV_STA_START BIT2
#define EV_SCAN_DONE BIT3
#define EV_GOT_IP BIT4
#define EV_DISCONNECTED BIT5
#define EV_ABORT BIT6      /* restart or power-off: long waits end, no new requests to the C6 */
#define EV_QUIET_DONE BIT7 /* the worker finished CMD_SHUTDOWN / CMD_PAUSE */

#define STOP_BUDGET_MS 400 /* a hung C6 must not hold up a restart or the power-off */

typedef enum {
    CMD_AUTO,
    CMD_SCAN,
    CMD_CONNECT,
    CMD_DISCONNECT,
    CMD_RECONNECT,
    CMD_PAUSE,    /* OFF: Wi-Fi off on the C6, ESP-Hosted stays */
    CMD_RESUME,   /* the power stayed on after OFF */
    CMD_SHUTDOWN, /* esp_restart(): Wi-Fi off, then ESP-Hosted's tasks; the worker ends */
} cmd_kind_t;

typedef struct {
    cmd_kind_t kind;
    char ssid[33];
    char pass[65];
    bool saved_pass; /* CONNECT: use the saved password */
    uint8_t auth;
    l5_wifi_scan_cb_t scan_cb;
    l5_wifi_connect_cb_t connect_cb;
    void *user;
} cmd_t;

typedef struct {
    char ssid[33];
    char pass[65];
} saved_net_t;

typedef struct {
    uint8_t n;
    saved_net_t net[L5_WIFI_MAX_SAVED];
} saved_list_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED; /* s_st and s_saved */
static l5_wifi_status_t s_st;
static saved_list_t s_saved;
static QueueHandle_t s_cmds;
static EventGroupHandle_t s_ev;
static l5_wifi_listener_t s_listener;
static volatile bool s_notify_pending;
static volatile bool s_want_link; /* connected on purpose: reconnect when the link drops */
static volatile uint8_t s_reason; /* last WIFI_EVENT_STA_DISCONNECTED reason */
static l5_wifi_ap_t s_aps[L5_WIFI_MAX_APS]; /* worker only */
static volatile bool s_quiet;         /* restart or power-off running: no messages, no reconnects */
static volatile bool s_shutting_down; /* esp_restart(): the UI goes away, its caller may hold the UI lock */
static volatile bool s_hosted;        /* esp_hosted_init() done and not undone */
static bool s_wifi_init;              /* worker: esp_wifi_init() done */
static bool s_wifi_running;           /* worker: esp_wifi_start() done, Wi-Fi not stopped */
static bool s_resume_link;            /* worker: connected when OFF paused Wi-Fi */

/* ---------- status ---------- */

/* The UI lock, except during esp_restart(): its caller may hold the lock while it waits for us. */
static bool lock_ui(void)
{
    while (!s_shutting_down) {
        if (l5_board_lock(50)) return true;
    }
    return false;
}

static void listener_async(void *arg)
{
    (void)arg;
    s_notify_pending = false;
    if (s_listener) s_listener();
}

/* Status changed: the listener runs in the LVGL task (callers may sit on small stacks). */
static void notify(void)
{
    if (!s_listener || s_notify_pending || s_quiet) return;
    if (lock_ui()) {
        s_notify_pending = true;
        lv_async_call(listener_async, NULL);
        l5_board_unlock();
    }
}

static l5_wifi_state_t state(void)
{
    portENTER_CRITICAL(&s_mux);
    l5_wifi_state_t st = s_st.state;
    portEXIT_CRITICAL(&s_mux);
    return st;
}

static void set_state(l5_wifi_state_t st, const char *ssid)
{
    portENTER_CRITICAL(&s_mux);
    if (s_st.state == L5_WIFI_FAILED) { /* final until the next start */
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    s_st.state = st;
    s_st.associated = false;
    if (st != L5_WIFI_CONNECTED) {
        s_st.ip[0] = '\0';
        s_st.rssi = 0;
    }
    if (ssid) strlcpy(s_st.ssid, ssid, sizeof(s_st.ssid));
    portEXIT_CRITICAL(&s_mux);
    notify();
}

static void fail(const char *fmt, ...)
{
    char msg[sizeof(s_st.error)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    ESP_LOGE(TAG, "%s", msg);
    s_want_link = false;
    portENTER_CRITICAL(&s_mux);
    s_st.state = L5_WIFI_FAILED;
    s_st.associated = false;
    s_st.ssid[0] = s_st.ip[0] = '\0';
    strlcpy(s_st.error, msg, sizeof(s_st.error));
    portEXIT_CRITICAL(&s_mux);
    notify();
}

void l5_wifi_set_listener(l5_wifi_listener_t cb)
{
    s_listener = cb;
}

void l5_wifi_get_status(l5_wifi_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

/* ---------- saved networks (NVS "l5_wifi" / "nets") ---------- */

static void saved_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return; /* nothing saved yet */
    saved_list_t list;
    size_t len = sizeof(list);
    esp_err_t err = nvs_get_blob(h, NVS_KEY, &list, &len);
    nvs_close(h);
    if (err == ESP_OK && len == sizeof(list) && list.n <= L5_WIFI_MAX_SAVED) {
        for (int i = 0; i < list.n; i++) {
            list.net[i].ssid[sizeof(list.net[i].ssid) - 1] = '\0';
            list.net[i].pass[sizeof(list.net[i].pass) - 1] = '\0';
        }
        portENTER_CRITICAL(&s_mux);
        s_saved = list;
        portEXIT_CRITICAL(&s_mux);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "saved networks unreadable (%s, %u bytes): ignored", esp_err_to_name(err), (unsigned)len);
    }
}

static void saved_store(void)
{
    saved_list_t list;
    portENTER_CRITICAL(&s_mux);
    list = s_saved;
    portEXIT_CRITICAL(&s_mux);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = list.n ? nvs_set_blob(h, NVS_KEY, &list, sizeof(list)) : nvs_erase_key(h, NVS_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK; /* nothing to erase */
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "saving the networks failed: %s", esp_err_to_name(err));
}

/* most recently used first */
static void saved_put(const char *ssid, const char *pass)
{
    saved_net_t net = {0};
    strlcpy(net.ssid, ssid, sizeof(net.ssid));
    strlcpy(net.pass, pass, sizeof(net.pass));
    portENTER_CRITICAL(&s_mux);
    bool same = s_saved.n && !memcmp(&s_saved.net[0], &net, sizeof(net));
    portEXIT_CRITICAL(&s_mux);
    if (same) return; /* already first: no NVS write (auto-connect at every start) */
    portENTER_CRITICAL(&s_mux);
    int i = 0;
    while (i < s_saved.n && strcmp(s_saved.net[i].ssid, ssid)) i++;
    if (i == s_saved.n && s_saved.n < L5_WIFI_MAX_SAVED) s_saved.n++;
    if (i == L5_WIFI_MAX_SAVED) i--; /* full: the oldest one goes */
    memmove(&s_saved.net[1], &s_saved.net[0], i * sizeof(saved_net_t));
    s_saved.net[0] = net;
    portEXIT_CRITICAL(&s_mux);
    saved_store();
}

static void saved_remove(const char *ssid)
{
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_saved.n; i++) {
        if (!strcmp(s_saved.net[i].ssid, ssid)) {
            memmove(&s_saved.net[i], &s_saved.net[i + 1], (s_saved.n - i - 1) * sizeof(saved_net_t));
            memset(&s_saved.net[--s_saved.n], 0, sizeof(saved_net_t));
            break;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    saved_store();
}

int l5_wifi_saved_count(void)
{
    portENTER_CRITICAL(&s_mux);
    int n = s_saved.n;
    portEXIT_CRITICAL(&s_mux);
    return n;
}

bool l5_wifi_saved_password(const char *ssid, char *pass, size_t len)
{
    bool found = false;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_saved.n && !found; i++) {
        if (!strcmp(s_saved.net[i].ssid, ssid)) {
            if (pass) strlcpy(pass, s_saved.net[i].pass, len);
            found = true;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return found;
}

int l5_wifi_saved_ssids(char (*ssids)[33], int max)
{
    portENTER_CRITICAL(&s_mux);
    int n = s_saved.n < max ? s_saved.n : max;
    for (int i = 0; i < n; i++) strlcpy(ssids[i], s_saved.net[i].ssid, sizeof(ssids[i]));
    portEXIT_CRITICAL(&s_mux);
    return n;
}

void l5_wifi_forget(const char *ssid)
{
    saved_remove(ssid);
    notify();
}

void l5_wifi_forget_all(void)
{
    portENTER_CRITICAL(&s_mux);
    memset(&s_saved, 0, sizeof(s_saved));
    portEXIT_CRITICAL(&s_mux);
    saved_store();
    notify();
}

/* ---------- versions, auth modes ---------- */

#define L5_STR(x) #x
#define L5_XSTR(x) L5_STR(x)

const char *l5_wifi_host_version(void)
{
    return L5_XSTR(ESP_HOSTED_VERSION_MAJOR_1) "." L5_XSTR(ESP_HOSTED_VERSION_MINOR_1) "." L5_XSTR(
        ESP_HOSTED_VERSION_PATCH_1);
}

const char *l5_wifi_host_series(void)
{
    return L5_XSTR(ESP_HOSTED_VERSION_MAJOR_1) "." L5_XSTR(ESP_HOSTED_VERSION_MINOR_1);
}

const char *l5_wifi_auth_name(uint8_t auth)
{
    switch (auth) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    case WIFI_AUTH_OWE: return "OWE";
    case WIFI_AUTH_WAPI_PSK: return "WAPI";
    case WIFI_AUTH_ENTERPRISE:
    case WIFI_AUTH_WPA3_ENT_192:
    case WIFI_AUTH_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE: return "802.1X";
    default: return "?";
    }
}

bool l5_wifi_auth_needs_password(uint8_t auth)
{
    return auth != WIFI_AUTH_OPEN && auth != WIFI_AUTH_OWE;
}

bool l5_wifi_auth_supported(uint8_t auth)
{
    return auth != WIFI_AUTH_ENTERPRISE && auth != WIFI_AUTH_WPA3_ENT_192 && auth != WIFI_AUTH_WPA3_ENTERPRISE &&
           auth != WIFI_AUTH_WPA2_WPA3_ENTERPRISE && auth != WIFI_AUTH_DPP;
}

bool l5_wifi_password_too_short(uint8_t auth, const char *pass)
{
    /* WPA / WPA2 / WPA3 passphrases have 8..63 characters (WEP keys 5 or 13) */
    return l5_wifi_auth_needs_password(auth) && auth != WIFI_AUTH_WEP && strlen(pass) < 8;
}

/* ---------- ESP-Hosted ---------- */

/* replaces ESP-Hosted's esp_restart() (g_hosted_osi_funcs._h_restart_host) */
static int restart_hook(void)
{
    if (!s_quiet) { /* during a restart or the power-off the C6 goes down on purpose */
        fail("The ESP32-C6 stopped answering (ESP-Hosted wanted to restart the Tab5). Wi-Fi is off until the next "
             "start: press RESET.");
    }
    for (;;) vTaskSuspend(NULL);
    return 0;
}

static void on_hosted(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == ESP_HOSTED_EVENT_TRANSPORT_FAILURE && !s_quiet && state() != L5_WIFI_FAILED) {
        fail("The ESP32-C6 stopped answering on SDIO. Wi-Fi is off until the next start: press RESET.");
    }
}

/* ---------- events (default event loop task) ---------- */

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(s_ev, EV_STA_START);
    } else if (id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(s_ev, EV_SCAN_DONE);
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        portENTER_CRITICAL(&s_mux);
        s_st.associated = true;
        portEXIT_CRITICAL(&s_mux);
        notify();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        s_reason = d ? d->reason : 0;
        xEventGroupSetBits(s_ev, EV_DISCONNECTED);
        bool dropped = false;
        portENTER_CRITICAL(&s_mux);
        if (s_st.state == L5_WIFI_CONNECTED && !s_quiet) { /* not asked for: the access point went away */
            dropped = true;
            s_st.state = L5_WIFI_CONNECTING;
            s_st.associated = false;
            s_st.ip[0] = '\0';
        }
        portEXIT_CRITICAL(&s_mux);
        if (dropped) {
            ESP_LOGW(TAG, "link lost (reason %u)", s_reason);
            cmd_t c = {.kind = CMD_RECONNECT};
            if (!s_want_link || xQueueSend(s_cmds, &c, 0) != pdTRUE) set_state(L5_WIFI_IDLE, "");
            notify();
        }
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    const ip_event_got_ip_t *e = data;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));
    portENTER_CRITICAL(&s_mux);
    if (s_st.state != L5_WIFI_FAILED) {
        strlcpy(s_st.ip, ip, sizeof(s_st.ip));
        s_st.state = L5_WIFI_CONNECTED;
        s_st.associated = false;
    }
    portEXIT_CRITICAL(&s_mux);
    xEventGroupSetBits(s_ev, EV_GOT_IP);
    notify();
}

/* ---------- worker steps ---------- */

/* A panic during the C6 start would reboot into the selected app, and the next RESET would start the
   C6 again: the marker turns that into one skipped auto-connect. */
static void start_marker(bool set)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (set) {
        nvs_set_u8(h, NVS_KEY_START, 1);
    } else {
        nvs_erase_key(h, NVS_KEY_START);
    }
    nvs_commit(h);
    nvs_close(h);
}

static bool start_marker_left(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool left = nvs_get_u8(h, NVS_KEY_START, &v) == ESP_OK && v;
    nvs_close(h);
    return left;
}

/* a restart or the power-off asked the worker to stop: no new requests to the C6 */
static bool aborted(void)
{
    return (xEventGroupGetBits(s_ev) & EV_ABORT) != 0;
}

static esp_err_t bring_up(void)
{
    g_hosted_osi_funcs._h_restart_host = restart_hook;

    /* ESP-Hosted's own start-up constructor is left out of the build (top CMakeLists.txt) */
    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (esp_hosted_init() != ESP_OK) { /* SDIO slot 1, ESP-Hosted tasks and buffers */
        fail("ESP-Hosted could not start (no memory?).");
        return ESP_FAIL;
    }
    s_hosted = true;
    ESP_LOGI(TAG, "ESP-Hosted host %s started: internal RAM %u -> %u bytes free, largest block %u",
             l5_wifi_host_version(), (unsigned)before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (!heap_caps_check_integrity_all(true)) ESP_LOGE(TAG, "heap corrupted after esp_hosted_init()");

    /* event loop first: ESP-Hosted reports transport failures through it */
    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) {
        err = esp_event_loop_create_default();
        if (err == ESP_ERR_INVALID_STATE) err = ESP_OK; /* already there */
    }
    if (err != ESP_OK) {
        fail("Network stack start failed (%s).", esp_err_to_name(err));
        return err;
    }
    esp_netif_create_default_wifi_sta();
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL, NULL);
    esp_event_handler_instance_register(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, on_hosted, NULL, NULL);

    /* the C6 may still run what the previous app left on it (connected, half-way through a request) */
    l5_board_c6_power(false);
    vTaskDelay(pdMS_TO_TICKS(100));
    err = l5_board_c6_power(true);
    if (err != ESP_OK) {
        fail("The ESP32-C6 could not be switched on (IO expander 0x44: %s).", esp_err_to_name(err));
        return err;
    }
    /* reset pulse on GPIO 15, SDIO card init, INIT event from the C6: at most about 25 s, no abort */
    int64_t t0 = esp_timer_get_time();
    if (esp_hosted_connect_to_slave() != ESP_OK) {
        fail("The ESP32-C6 does not answer on SDIO (slot 1, GPIO 8-13, reset GPIO 15). Press RESET to try again.");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ESP-Hosted transport up after %lld ms", (esp_timer_get_time() - t0) / 1000);

    esp_hosted_coprocessor_fwver_t fw = {0};
    char ver[sizeof(s_st.c6_fw)] = "unknown";
    bool known = esp_hosted_get_coprocessor_fwversion(&fw) == ESP_OK;
    if (known) {
        snprintf(ver, sizeof(ver), "%u.%u.%u", (unsigned)fw.major1, (unsigned)fw.minor1, (unsigned)fw.patch1);
    }
    ESP_LOGI(TAG, "ESP32-C6 firmware: ESP-Hosted %s (host %d.%d.%d)", ver, ESP_HOSTED_VERSION_MAJOR_1,
             ESP_HOSTED_VERSION_MINOR_1, ESP_HOSTED_VERSION_PATCH_1);
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_st.c6_fw, ver, sizeof(s_st.c6_fw));
    portEXIT_CRITICAL(&s_mux);
    /* same major.minor as ESP-Hosted's own compare_fw_version(), but enforced */
    if (!known || fw.major1 != ESP_HOSTED_VERSION_MAJOR_1 || fw.minor1 != ESP_HOSTED_VERSION_MINOR_1) {
        portENTER_CRITICAL(&s_mux);
        s_st.c6_mismatch = true; /* the UI explains what to flash (l5_wifi_why) */
        portEXIT_CRITICAL(&s_mux);
        fail("The ESP32-C6 runs ESP-Hosted %s, this launcher talks ESP-Hosted %d.%d.x. Wi-Fi stays off: the two "
             "would misread each other's messages.", ver, ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1);
        ESP_LOGE(TAG, "flash the C6 with M5Burner (\"Tab5 ESP-Hosted Firmware Upgrade\" or \"Tab5 ESP32-C6 Wi-Fi SDIO\", "
                      "%s); installing from SD works without Wi-Fi", l5_wifi_host_version());
        return ESP_ERR_NOT_SUPPORTED;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    s_wifi_init = err == ESP_OK;
    if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM); /* the launcher keeps its own list */
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        fail("The ESP32-C6 did not start Wi-Fi (%s, ESP-Hosted %s).", esp_err_to_name(err), ver);
        return err;
    }
    s_wifi_running = true;
    xEventGroupWaitBits(s_ev, EV_STA_START | EV_ABORT, pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "station ready after %lld ms", (esp_timer_get_time() - t0) / 1000);
    set_state(L5_WIFI_IDLE, ""); /* stays FAILED when the transport broke meanwhile */
    return state() == L5_WIFI_FAILED ? ESP_FAIL : ESP_OK;
}

static esp_err_t start_c6(void)
{
    set_state(L5_WIFI_STARTING, "");
    start_marker(true);
    esp_err_t err = bring_up();
    start_marker(false);
    return err;
}

static int cmp_rssi(const void *a, const void *b)
{
    return ((const wifi_ap_record_t *)b)->rssi - ((const wifi_ap_record_t *)a)->rssi;
}

/* scan, then keep one entry per SSID (the strongest), hidden networks left out */
static esp_err_t scan(int *count)
{
    *count = 0;
    xEventGroupClearBits(s_ev, EV_SCAN_DONE);
    esp_err_t err = esp_wifi_scan_start(NULL, false);
    if (err != ESP_OK) return err;
    EventBits_t bits = xEventGroupWaitBits(s_ev, EV_SCAN_DONE | EV_ABORT, pdFALSE, pdFALSE, pdMS_TO_TICKS(12000));
    xEventGroupClearBits(s_ev, EV_SCAN_DONE);
    if (bits & EV_ABORT) return ESP_ERR_INVALID_STATE; /* esp_wifi_stop() ends the scan */
    if (!(bits & EV_SCAN_DONE)) {
        esp_wifi_scan_stop();
        return ESP_ERR_TIMEOUT;
    }
    uint16_t num = SCAN_RECORDS;
    wifi_ap_record_t *rec = calloc(SCAN_RECORDS, sizeof(wifi_ap_record_t));
    if (!rec) {
        esp_wifi_clear_ap_list();
        return ESP_ERR_NO_MEM;
    }
    err = esp_wifi_scan_get_ap_records(&num, rec);
    if (err == ESP_OK) {
        qsort(rec, num, sizeof(wifi_ap_record_t), cmp_rssi);
        int n = 0;
        for (int i = 0; i < num && n < L5_WIFI_MAX_APS; i++) {
            const char *ssid = (const char *)rec[i].ssid;
            int j = 0;
            while (j < n && strcmp(s_aps[j].ssid, ssid)) j++;
            if (!ssid[0] || j < n) continue;
            strlcpy(s_aps[n].ssid, ssid, sizeof(s_aps[n].ssid));
            s_aps[n].rssi = rec[i].rssi;
            s_aps[n].channel = rec[i].primary;
            s_aps[n].auth = rec[i].authmode;
            n++;
        }
        *count = n;
    }
    free(rec);
    return err;
}

static l5_wifi_result_t reason_result(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return L5_WIFI_ERR_PASSWORD;
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return L5_WIFI_ERR_NOT_FOUND;
    default:
        return L5_WIFI_ERR_REFUSED;
    }
}

/* one connection attempt, up to 20 s: association + DHCP */
static l5_wifi_result_t join(const char *ssid, const char *pass, uint8_t auth, char *detail, size_t len)
{
    wifi_config_t wc = {0};
    size_t n = strlen(ssid);
    memcpy(wc.sta.ssid, ssid, n < sizeof(wc.sta.ssid) ? n : sizeof(wc.sta.ssid));
    n = strlen(pass);
    memcpy(wc.sta.password, pass, n < sizeof(wc.sta.password) ? n : sizeof(wc.sta.password)); /* 64 hex digits fit */
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    if (auth == WIFI_AUTH_OWE) {
        wc.sta.threshold.authmode = WIFI_AUTH_OWE;
        wc.sta.owe_enabled = 1;
    } else if (!pass[0]) {
        wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    } else {
        /* the weakest mode this network offers, so WEP / WPA-only access points are not filtered out */
        wc.sta.threshold.authmode = auth == WIFI_AUTH_WEP || auth == WIFI_AUTH_WPA_PSK ? auth : WIFI_AUTH_WPA2_PSK;
    }
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    l5_wifi_state_t before = state();
    s_want_link = false;
    set_state(L5_WIFI_CONNECTING, ssid);
    xEventGroupClearBits(s_ev, EV_DISCONNECTED);
    if (esp_wifi_disconnect() == ESP_OK && (before == L5_WIFI_CONNECTED || before == L5_WIFI_CONNECTING)) {
        /* the old link's event must not end this attempt */
        xEventGroupWaitBits(s_ev, EV_DISCONNECTED | EV_ABORT, pdFALSE, pdFALSE, pdMS_TO_TICKS(1500));
        xEventGroupClearBits(s_ev, EV_DISCONNECTED);
    }
    if (aborted()) {
        set_state(L5_WIFI_IDLE, "");
        snprintf(detail, len, "Wi-Fi was stopped (restart or power-off).");
        return L5_WIFI_ERR_C6;
    }
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err == ESP_OK) {
        xEventGroupClearBits(s_ev, EV_GOT_IP | EV_DISCONNECTED);
        s_reason = 0;
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        snprintf(detail, len, "The ESP32-C6 refused the request (%s).", esp_err_to_name(err));
        set_state(L5_WIFI_IDLE, "");
        return L5_WIFI_ERR_C6;
    }
    EventBits_t bits =
        xEventGroupWaitBits(s_ev, EV_GOT_IP | EV_DISCONNECTED | EV_ABORT, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    if ((bits & EV_ABORT) && !(bits & EV_GOT_IP)) { /* stop_wifi() disconnects */
        set_state(L5_WIFI_IDLE, "");
        snprintf(detail, len, "Wi-Fi was stopped (restart or power-off).");
        return L5_WIFI_ERR_C6;
    }
    if (bits & EV_GOT_IP) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            portENTER_CRITICAL(&s_mux);
            s_st.rssi = ap.rssi;
            portEXIT_CRITICAL(&s_mux);
        }
        s_want_link = true;
        l5_wifi_status_t st;
        l5_wifi_get_status(&st);
        ESP_LOGI(TAG, "connected to %s, %s, %d dBm", ssid, st.ip, st.rssi);
        notify();
        return L5_WIFI_OK;
    }
    esp_wifi_disconnect();
    set_state(L5_WIFI_IDLE, "");
    if (bits & EV_DISCONNECTED) {
        snprintf(detail, len, "Disconnect reason %u.", s_reason);
        ESP_LOGW(TAG, "%s: connection failed, reason %u", ssid, s_reason);
        return reason_result(s_reason);
    }
    snprintf(detail, len, "No IP address after 20 s (DHCP).");
    ESP_LOGW(TAG, "%s: no IP address after 20 s", ssid);
    return L5_WIFI_ERR_TIMEOUT;
}

/* boot: the strongest saved network in range */
static void autoconnect(void)
{
    int n = 0;
    esp_err_t err = scan(&n);
    if (err != ESP_OK) {
        if (!aborted()) ESP_LOGW(TAG, "auto-connect scan failed: %s", esp_err_to_name(err));
        return;
    }
    char pass[65], detail[64];
    int tried = 0;
    for (int i = 0; i < n && tried < 3 && !aborted(); i++) { /* a request from the UI waits behind this */
        if (!l5_wifi_saved_password(s_aps[i].ssid, pass, sizeof(pass))) continue;
        tried++;
        if (join(s_aps[i].ssid, pass, s_aps[i].auth, detail, sizeof(detail)) == L5_WIFI_OK) {
            saved_put(s_aps[i].ssid, pass);
            memset(pass, 0, sizeof(pass));
            return;
        }
    }
    memset(pass, 0, sizeof(pass));
    if (!aborted()) ESP_LOGI(TAG, "auto-connect: no saved network answered (%d in range, %d tried)", n, tried);
}

/* the access point went away: three tries, then idle */
static void reconnect(void)
{
    for (int i = 0; i < 3 && s_want_link && !aborted(); i++) {
        if (i && (xEventGroupWaitBits(s_ev, EV_ABORT, pdFALSE, pdFALSE, pdMS_TO_TICKS(5000)) & EV_ABORT)) break;
        xEventGroupClearBits(s_ev, EV_GOT_IP | EV_DISCONNECTED);
        if (esp_wifi_connect() != ESP_OK) continue;
        EventBits_t bits = xEventGroupWaitBits(s_ev, EV_GOT_IP | EV_DISCONNECTED | EV_ABORT, pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(15000));
        if (bits & EV_GOT_IP) {
            ESP_LOGI(TAG, "reconnected");
            return;
        }
    }
    if (state() == L5_WIFI_CONNECTING) {
        s_want_link = false;
        set_state(L5_WIFI_IDLE, "");
    }
}

static void deliver_scan(const cmd_t *c, esp_err_t err, int n)
{
    if (c->scan_cb && lock_ui()) {
        c->scan_cb(err, s_aps, n, c->user);
        l5_board_unlock();
    }
}

static void deliver_connect(const cmd_t *c, l5_wifi_result_t res, const char *detail)
{
    if (c->connect_cb && lock_ui()) {
        c->connect_cb(res, detail, c->user);
        l5_board_unlock();
    }
}

/* Wi-Fi off on the C6: the access point sees the station leave, and nothing more is sent */
static void stop_wifi(void)
{
    s_resume_link = s_want_link;
    s_want_link = false;
    if (!s_wifi_running || state() == L5_WIFI_FAILED) return; /* not running, or the link is dead anyway */
    set_state(L5_WIFI_IDLE, ""); /* before the disconnect event: not a lost link */
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_wifi_running = false;
}

/* CMD_SHUTDOWN: what the C6 needs before it loses power. The worker ends here. */
static void shut_down(void)
{
    stop_wifi();
    if (s_hosted) {
        esp_hosted_deinit(); /* RPC and SDIO tasks end, SDIO host off: nothing is sent any more */
        s_hosted = false;
    }
    xEventGroupSetBits(s_ev, EV_QUIET_DONE);
    for (;;) vTaskSuspend(NULL); /* the restart follows; ESP-Hosted must not be used any more */
}

/* CMD_RESUME: OFF did not cut the power (USB). Wi-Fi starts again and rejoins if it was connected. */
static void resume_wifi(void)
{
    if (!s_wifi_init || s_wifi_running || state() == L5_WIFI_FAILED) return;
    xEventGroupClearBits(s_ev, EV_STA_START);
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        fail("The ESP32-C6 did not start Wi-Fi again (%s). Press RESET.", esp_err_to_name(err));
        return;
    }
    s_wifi_running = true;
    xEventGroupWaitBits(s_ev, EV_STA_START | EV_ABORT, pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));
    set_state(L5_WIFI_IDLE, "");
    if (s_resume_link) autoconnect();
}

static void worker(void *arg)
{
    (void)arg;
    static cmd_t c; /* big: off the task stack */
    for (;;) {
        if (xQueueReceive(s_cmds, &c, portMAX_DELAY) != pdTRUE) continue;
        if (c.kind == CMD_SHUTDOWN) shut_down(); /* does not return */
        if (c.kind == CMD_PAUSE) {
            stop_wifi();
            xEventGroupSetBits(s_ev, EV_QUIET_DONE);
            continue;
        }
        if (c.kind == CMD_RESUME) {
            resume_wifi();
            continue;
        }
        if (aborted()) { /* stopped for a restart or the power-off: requests fail, the C6 is not started */
            deliver_scan(&c, ESP_ERR_INVALID_STATE, 0);
            deliver_connect(&c, L5_WIFI_ERR_C6, "Wi-Fi was stopped (restart or power-off).");
            memset(c.pass, 0, sizeof(c.pass));
            continue;
        }
        if (state() == L5_WIFI_OFF && c.kind != CMD_DISCONNECT) start_c6();
        if (state() == L5_WIFI_FAILED) {
            l5_wifi_status_t st;
            l5_wifi_get_status(&st);
            deliver_scan(&c, ESP_FAIL, 0);
            deliver_connect(&c, L5_WIFI_ERR_C6, st.error);
            continue;
        }
        switch (c.kind) {
        case CMD_AUTO:
            autoconnect();
            break;
        case CMD_SCAN: {
            int n = 0;
            esp_err_t err = scan(&n);
            deliver_scan(&c, err, n);
            break;
        }
        case CMD_CONNECT: {
            char detail[96] = "";
            if (c.saved_pass && !l5_wifi_saved_password(c.ssid, c.pass, sizeof(c.pass))) c.pass[0] = '\0';
            l5_wifi_result_t res = join(c.ssid, c.pass, c.auth, detail, sizeof(detail));
            if (res == L5_WIFI_OK) {
                if (c.pass[0]) saved_put(c.ssid, c.pass); /* app.js: only networks with a password are saved */
            } else if (res == L5_WIFI_ERR_PASSWORD && c.saved_pass) {
                saved_remove(c.ssid); /* changed on the router: the next pick asks again */
                notify();
            }
            deliver_connect(&c, res, detail);
            break;
        }
        case CMD_DISCONNECT:
            s_want_link = false;
            if (state() != L5_WIFI_OFF) {
                set_state(L5_WIFI_IDLE, "");
                esp_wifi_disconnect();
            }
            break;
        case CMD_RECONNECT:
            reconnect();
            break;
        case CMD_PAUSE:
        case CMD_RESUME:
        case CMD_SHUTDOWN:
            break; /* handled above */
        }
        memset(c.pass, 0, sizeof(c.pass));
    }
}

/* ---------- public ---------- */

static esp_err_t post(const cmd_t *c)
{
    if (!s_cmds) return ESP_ERR_INVALID_STATE;
    return xQueueSend(s_cmds, c, 0) == pdTRUE ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/* CMD_PAUSE / CMD_SHUTDOWN ahead of everything queued; true when the worker finished it within budget_ms */
static bool quiet_request(cmd_kind_t kind, uint32_t budget_ms)
{
    xEventGroupClearBits(s_ev, EV_QUIET_DONE);
    xEventGroupSetBits(s_ev, EV_ABORT); /* a running scan or connection attempt ends now */
    cmd_t c = {.kind = kind};
    if (xQueueSendToFront(s_cmds, &c, 0) != pdTRUE) {
        if (kind != CMD_SHUTDOWN) return false;
        xQueueReset(s_cmds); /* full: the queued UI requests do not matter any more */
        if (xQueueSendToFront(s_cmds, &c, 0) != pdTRUE) return false;
    }
    return (xEventGroupWaitBits(s_ev, EV_QUIET_DONE, pdFALSE, pdFALSE, pdMS_TO_TICKS(budget_ms)) & EV_QUIET_DONE) != 0;
}

/* esp_restart() (BOOT, Restart, scheme change) runs this before the display blanking. Switching the C6
   off first left ESP-Hosted's SDIO write task sending to it: "sdmmc_send_cmd returned 0x107",
   "Unrecoverable host sdio state" and a false "ESP32-C6 stopped answering" before rst:0xc. */
static void wifi_off_at_restart(void)
{
    s_shutting_down = true; /* first: our caller may hold the UI lock */
    s_quiet = true;
    if (state() == L5_WIFI_STARTING) start_marker(false); /* a planned restart, not a crash */
    if (s_hosted) {
        int64_t t0 = esp_timer_get_time();
        if (quiet_request(CMD_SHUTDOWN, STOP_BUDGET_MS)) {
            ESP_LOGI(TAG, "Wi-Fi and ESP-Hosted stopped for the restart in %lld ms", (esp_timer_get_time() - t0) / 1000);
        } else {
            ESP_LOGW(TAG, "the ESP32-C6 did not stop within %d ms: switched off anyway", STOP_BUDGET_MS);
        }
    } else if (s_ev) {
        xEventGroupSetBits(s_ev, EV_ABORT); /* a queued request must not start the C6 now */
    }
    /* every app starts with the C6 as after power-on (off); apps that use Wi-Fi switch it on */
    l5_board_c6_power(false);
}

void l5_wifi_pause(void)
{
    if (!s_hosted || s_quiet) return; /* the C6 never started, or it is being stopped already */
    s_quiet = true;
    int64_t t0 = esp_timer_get_time();
    if (quiet_request(CMD_PAUSE, STOP_BUDGET_MS)) {
        ESP_LOGI(TAG, "Wi-Fi stopped for the power-off in %lld ms", (esp_timer_get_time() - t0) / 1000);
    } else {
        ESP_LOGW(TAG, "Wi-Fi did not stop within %d ms before the power-off", STOP_BUDGET_MS);
    }
}

void l5_wifi_resume(void)
{
    if (!s_quiet || s_shutting_down) return;
    xEventGroupClearBits(s_ev, EV_ABORT);
    s_quiet = false;
    cmd_t c = {.kind = CMD_RESUME};
    if (xQueueSend(s_cmds, &c, pdMS_TO_TICKS(200)) != pdTRUE) ESP_LOGW(TAG, "Wi-Fi stays stopped: worker busy");
    notify();
}

void l5_wifi_init(bool autoconnect_now)
{
    saved_load();
    s_ev = xEventGroupCreate();
    s_cmds = xQueueCreate(4, sizeof(cmd_t));
    if (!s_ev || !s_cmds || xTaskCreatePinnedToCore(worker, "l5_wifi", 8192, NULL, 3, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the Wi-Fi worker");
        s_cmds = NULL;
        return;
    }
    esp_register_shutdown_handler(wifi_off_at_restart);
    bool wanted = autoconnect_now; /* cfg.wifiAuto; false when the app starts right away */
    if (start_marker_left()) {
        start_marker(false);
        if (autoconnect_now) {
            ESP_LOGW(TAG, "the last ESP32-C6 start did not finish (crash?): auto-connect skipped this time");
            autoconnect_now = false;
        }
    }
    int saved = l5_wifi_saved_count();
    autoconnect_now = autoconnect_now && saved > 0; /* app.js L5.start */
    ESP_LOGI(TAG, "%d saved network(s)%s", saved,
             autoconnect_now ? ", auto-connect" : saved && !wanted ? ", auto-connect off" : "");
    if (autoconnect_now) {
        cmd_t c = {.kind = CMD_AUTO};
        post(&c);
    }
}

esp_err_t l5_wifi_scan(l5_wifi_scan_cb_t cb, void *user)
{
    cmd_t c = {.kind = CMD_SCAN, .scan_cb = cb, .user = user};
    return post(&c);
}

esp_err_t l5_wifi_connect(const char *ssid, const char *pass, uint8_t auth, l5_wifi_connect_cb_t cb, void *user)
{
    cmd_t c = {.kind = CMD_CONNECT, .auth = auth, .saved_pass = pass == NULL, .connect_cb = cb, .user = user};
    strlcpy(c.ssid, ssid, sizeof(c.ssid));
    if (pass) strlcpy(c.pass, pass, sizeof(c.pass));
    esp_err_t err = post(&c);
    memset(c.pass, 0, sizeof(c.pass));
    return err;
}

esp_err_t l5_wifi_disconnect(void)
{
    s_want_link = false; /* also ends a running reconnect */
    cmd_t c = {.kind = CMD_DISCONNECT};
    return post(&c);
}
