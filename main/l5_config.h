/* Launcher settings: mirrors `cfg` in tab5-launcher/app.js, stored on the SD card as
   /config.conf (JSON object with the same keys). HOME never edits these; the CFG tab will. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifndef L5_CONFIG_PATH /* the UI snapshots on the PC point it at their own card folder */
#define L5_CONFIG_PATH "/sdcard/config.conf"
#endif
#define L5_CONFIG_OLD L5_CONFIG_PATH ".old" /* the file being replaced, for the moment of the swap */

typedef struct {
    uint8_t bright;       /* backlight %, 5..100 in 5% steps */
    uint16_t dim;         /* idle seconds before dimming, 0 = off (0,10,20,30,60,120,300) */
    uint16_t screenOff;   /* idle seconds before the backlight goes off, 0 = never (0,60,120,300,600,1800) */
    char color[12];       /* UI accent id, see l5_colors */
    uint16_t rot;         /* 0 = landscape, 180 = landscape inverted */
    bool askSpiffs;       /* ask before installing a SPIFFS image */
    bool onlyBins;        /* SD manager lists only .bin files */
    uint8_t bootTime;     /* bootscreen countdown, 1..5 s */
    bool wifiAuto;        /* connect to saved networks at boot */
    bool deepSleepStart;  /* waking from deep sleep opens the launcher */
    bool btnOnlyStart;    /* enter the launcher only when the screen is touched at reset */
    char scheme[12];      /* partition scheme id (data.js schemes) */
} l5_config_t;

typedef struct {
    const char *id;
    uint32_t hex;
} l5_color_t;

extern l5_config_t g_cfg;
extern const l5_color_t l5_colors[];
extern const int l5_color_count;

void l5_config_defaults(l5_config_t *cfg);
/* Loads L5_CONFIG_PATH over the defaults. Missing keys keep their default value.
   Returns ESP_ERR_NOT_FOUND when the file does not exist (defaults are used). */
esp_err_t l5_config_load(l5_config_t *cfg);
esp_err_t l5_config_save(const l5_config_t *cfg);
/* deletes L5_CONFIG_PATH (SD manager), not while it is being read or written */
esp_err_t l5_config_remove(void);
/* the file src (on the card) takes the name L5_CONFIG_PATH, in place of the current one (SD manager: a copy of an old
   config.conf renamed or pasted back). ESP_FAIL: nothing changed, both files are as they were. */
esp_err_t l5_config_replace(const char *src);
uint32_t l5_config_accent_hex(const l5_config_t *cfg);

/* OTA favorites, kept in /config.conf as Launcher does: "favorite": [{name, fid, link}]
   (a catalog entry has its fid, a custom link has fid "" and the https URL in link) */
#define L5_FAV_MAX 32
typedef struct {
    char name[48];
    char fid[40];
    char link[200];
} l5_fav_t;
typedef struct {
    int n;
    l5_fav_t item[L5_FAV_MAX];
} l5_favs_t;
esp_err_t l5_favs_load(l5_favs_t *out);
esp_err_t l5_favs_save(const l5_favs_t *favs); /* rewrites only the "favorite" key */
