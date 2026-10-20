/* Wi-Fi through the Tab5's ESP32-C6: ESP-Hosted 1.4.1 host over SDIO + esp_wifi_remote, so the
   standard esp_wifi / esp_netif API drives the radio on the C6. Port of the Wi-Fi state in app.js
   (R.wifi, S.savedWifi, L5.connect). One worker task owns the C6; requests return at once and
   their results arrive in callbacks that run with the LVGL lock held. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define L5_WIFI_MAX_SAVED 8 /* saved networks, most recently used first (NVS namespace "l5_wifi") */
#define L5_WIFI_MAX_APS 16  /* scan results kept, strongest first, one entry per SSID */

typedef enum {
    L5_WIFI_OFF,        /* the C6 is not started: nothing asked for Wi-Fi yet */
    L5_WIFI_STARTING,   /* powering the C6, ESP-Hosted transport, esp_wifi_init */
    L5_WIFI_IDLE,       /* station running, not connected */
    L5_WIFI_CONNECTING, /* associating or waiting for DHCP (also while reconnecting) */
    L5_WIFI_CONNECTED,  /* IPv4 address assigned */
    L5_WIFI_FAILED,     /* the C6 did not start, see l5_wifi_status_t.error; final until restart */
} l5_wifi_state_t;

typedef struct {
    l5_wifi_state_t state;
    bool associated;  /* CONNECTING: the access point accepted us, DHCP is next */
    char ssid[33];    /* CONNECTING / CONNECTED */
    char ip[16];      /* CONNECTED */
    int rssi;         /* CONNECTED, dBm */
    char c6_fw[16];   /* ESP-Hosted version reported by the C6, "" until asked, "unknown" when it did not say */
    char error[160];  /* FAILED */
    bool c6_mismatch; /* FAILED at the version gate: the C6 runs another ESP-Hosted major.minor (or none it reports) */
} l5_wifi_status_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    uint8_t auth; /* wifi_auth_mode_t */
} l5_wifi_ap_t;

typedef enum {
    L5_WIFI_OK,
    L5_WIFI_ERR_PASSWORD,  /* the access point rejected the password */
    L5_WIFI_ERR_NOT_FOUND, /* no access point with that SSID answered */
    L5_WIFI_ERR_REFUSED,   /* association refused */
    L5_WIFI_ERR_TIMEOUT,   /* no IP address in time */
    L5_WIFI_ERR_C6,        /* the C6 is not running or did not answer */
} l5_wifi_result_t;

/* Callbacks run with the LVGL lock held. aps / detail are only valid during the call. */
typedef void (*l5_wifi_scan_cb_t)(esp_err_t err, const l5_wifi_ap_t *aps, int n, void *user);
typedef void (*l5_wifi_connect_cb_t)(l5_wifi_result_t res, const char *detail, void *user);
typedef void (*l5_wifi_listener_t)(void);

/* Loads the saved networks and starts the worker. With autoconnect (cfg.wifiAuto) and at least one
   saved network the C6 comes up right away and joins the strongest saved network in range, in the
   background. Otherwise the C6 stays off until the first scan or connect. At every software
   restart (hand-over to an app included) Wi-Fi is stopped, then ESP-Hosted's tasks, and then the
   C6 is switched off, as after power-on (at most 400 ms wait for a C6 that does not answer). */
void l5_wifi_init(bool autoconnect);
/* OFF, before the Tab5 cuts its own supply: Wi-Fi stops on the C6 (at most 400 ms). ESP-Hosted
   keeps running: stopping it switches off the SDMMC controller, which the SD card shares. */
void l5_wifi_pause(void);
/* The power stayed on after OFF (USB): Wi-Fi starts again and rejoins if it was connected. */
void l5_wifi_resume(void);
/* Called with the LVGL lock held whenever l5_wifi_get_status() changes. */
void l5_wifi_set_listener(l5_wifi_listener_t cb);
void l5_wifi_get_status(l5_wifi_status_t *out);

/* Requests for the worker. ESP_ERR_INVALID_STATE: another request is waiting. */
esp_err_t l5_wifi_scan(l5_wifi_scan_cb_t cb, void *user);
/* pass NULL: use the saved password. A network that connects is saved (moved to the front). */
esp_err_t l5_wifi_connect(const char *ssid, const char *pass, uint8_t auth, l5_wifi_connect_cb_t cb, void *user);
esp_err_t l5_wifi_disconnect(void);

int l5_wifi_saved_count(void);
int l5_wifi_saved_ssids(char (*ssids)[33], int max); /* most recently used first */
bool l5_wifi_saved_password(const char *ssid, char *pass, size_t len); /* pass may be NULL */
void l5_wifi_forget(const char *ssid); /* the current connection stays up */
void l5_wifi_forget_all(void);

const char *l5_wifi_host_version(void);      /* ESP-Hosted host release, "2.12.6" */
const char *l5_wifi_host_series(void);       /* its major.minor, "2.12": what the C6 has to run */
const char *l5_wifi_auth_name(uint8_t auth); /* "WPA2", "OPEN", ... */
bool l5_wifi_auth_needs_password(uint8_t auth);
bool l5_wifi_auth_supported(uint8_t auth); /* enterprise (802.1X) networks need more than a password */
bool l5_wifi_password_too_short(uint8_t auth, const char *pass);
