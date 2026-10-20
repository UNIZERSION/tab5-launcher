/* Tear-free landscape output for the Tab5 MIPI-DSI panel: LVGL renders 1280x720 into partial
   PSRAM buffers, the PPA rotates every piece into a hidden 720x1280 DPI frame buffer, and the
   finished frame is published once per LVGL refresh (taken over at VSYNC). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

/* Powers the panel, starts the esp_lvgl_port task, creates the display and the touch input.
   rot = cfg.rot (0 or 180). *step: the start-up step that ran last, so on an error the one that failed
   ("LCD power (I2C bus, IO expander 0x43, LCD_EN)", or why the board was not recognised). */
esp_err_t l5_display_start(uint16_t rot, lv_display_t **display, const char **step);
lv_indev_t *l5_display_touch(void);
/* the later Tab5 display/touch the start-up probe found ("ST7121 + ST712x"), NULL for ILI9881C + GT911 */
const char *l5_display_new_panel(void);
/* Draw the first complete frame, wait until the panel shows it, then light the backlight.
   Call with the LVGL lock held. */
esp_err_t l5_display_reveal(uint8_t brightness);

/* Timing of one UI change (tab switch), logged 60 ms after its frame is published:
   tap->visible = build (UI code) + wait (until LVGL starts the refresh) + render (LVGL, incl.
   stalls waiting for the PPA) + last PPA + publish + wait for VSYNC.
   l5_display_mark() at the start of the event, l5_display_mark_built() when the UI is ready. */
void l5_display_mark(const char *what);
void l5_display_mark_built(void);
/* Start the next LVGL refresh right away instead of at the next 16 ms period. */
void l5_display_refresh_now(void);

/* Temporary: lost-tap diagnosis, log tag "l5_touch". Touch reports as LVGL reads them (down / up,
   position, gap between reads, ST7123 INT pulses), the pointer events LVGL sends (PRESSED,
   RELEASED, SHORT_CLICKED, CLICKED, LONG_PRESSED, scroll instead of click) and LVGL-task work
   that blocks the touch reads (renders, view builds). 0 removes it. */
#define L5_TOUCH_TRACE 1
#if L5_TOUCH_TRACE
/* logs "busy <what> N ms" when the LVGL task spent more than 30 ms since start_us */
void l5_touch_trace_busy(const char *what, int64_t start_us);
#endif
