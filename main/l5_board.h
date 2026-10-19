/* Tab5 hardware used by the launcher: MIPI-DSI panel + touch (GT911 or ST7123 via the
   M5Stack Tab5 BSP), backlight, SD card, RX8130 RTC, INA226 battery monitor, power-off. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

/* Starts LVGL (esp_lvgl_port task) on the panel in landscape: 1280x720 logical,
   rotated by the PPA into the 720x1280 panel. rot = cfg.rot (0 or 180). */
lv_display_t *l5_board_display_start(uint16_t rot);
void l5_board_set_rotation(uint16_t rot);
void l5_board_backlight(uint8_t percent);
/* first complete frame on the panel, then the backlight (LVGL lock held) */
void l5_board_reveal(uint8_t percent);
bool l5_board_lock(uint32_t timeout_ms);
void l5_board_unlock(void);
/* true while a finger is on the screen (call with the LVGL lock held) */
bool l5_board_touch_pressed(void);
/* "ST7121 + ST712x" on the later Tab5 boards, NULL on the first ones (ILI9881C + GT911) */
const char *l5_board_new_panel(void);

typedef enum {
    L5_SD_UNCHECKED,
    L5_SD_MOUNTED,
    L5_SD_NO_CARD,    /* no answer: no card, or it is not seated */
    L5_SD_UNREADABLE, /* a card answered, FatFs found no FAT32/exFAT volume (FR_NO_FILESYSTEM) */
    L5_SD_ERROR,      /* any other driver error, see l5_board_sd_error() */
} l5_sd_state_t;

/* Mounts the card at /sdcard (never formats it). Safe to call again after a failure. */
esp_err_t l5_board_sd_mount(void);
bool l5_board_sd_ready(void);
l5_sd_state_t l5_board_sd_state(void);
esp_err_t l5_board_sd_error(void);
/* Status for the UI: short tag ("NO CARD") or a full sentence with what to do. */
const char *l5_board_sd_status(bool detail);
uint64_t l5_board_sd_total_bytes(void);
/* FatFs drive number of the mounted card ("0:" paths for f_readdir), -1 when not mounted */
int l5_board_sd_drive(void);

/* RTC -> system time; battery from INA226 bus voltage (2S pack). */
esp_err_t l5_board_sensors_init(void);
int l5_board_battery_percent(void); /* -1 when unknown */
typedef enum {
    L5_CHARGE_UNKNOWN = -1, /* no pack, or no reading */
    L5_CHARGE_ON_BATTERY,   /* current out of the pack */
    L5_CHARGE_CHARGING,     /* current into the pack */
    L5_CHARGE_EXTERNAL,     /* about no current: external power, the charger does not charge (full or stopped) */
} l5_charge_t;
/* from the current through the pack, see l5_board.c; fresh: forget the last state (Charge Mode just opened), so the
   first reading decides at once and goes to the log */
l5_charge_t l5_board_battery_state(bool fresh);

/* One reading (mA into the pack) on top of the state: more than 20 mA in charging, more than 20 mA out on battery,
   in between external power without charging. A change needs two readings in a row that agree (*pending keeps the
   first one), except from no state and beyond 100 mA either way (USB plugged in or pulled out). */
static inline l5_charge_t l5_charge_next(l5_charge_t state, l5_charge_t *pending, int ma)
{
    l5_charge_t now = ma > 20 ? L5_CHARGE_CHARGING : ma < -20 ? L5_CHARGE_ON_BATTERY : L5_CHARGE_EXTERNAL;
    if (now == state || state == L5_CHARGE_UNKNOWN || now == *pending || ma > 100 || ma < -100) {
        *pending = now;
        return now;
    }
    *pending = now; /* a second reading decides */
    return state;
}
/* Battery charger on: CHG_EN (PI4IOE 0x44 pin 7) high, CHG_STAT (pin 6) an input, register by register (no
   expander reset), as Aurora. At start-up and after a failed power-off; nothing turns it off. */
esp_err_t l5_board_charger_on(void);
/* Hardware power-off (PI4IOE 0x44 pin 4 pulses, as M5Stack's Tab5 UserDemo). Returns on failure. */
esp_err_t l5_board_power_off(void);
/* ESP32-C6 (Wi-Fi) supply, PI4IOE 0x44 pin 0 (WLAN_PWR_EN). Off after power-on; the expander keeps
   its state across a RESET of the P4. */
esp_err_t l5_board_c6_power(bool on);
