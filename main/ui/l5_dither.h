/* Ordered dithering of the vertical gradients (l5_dither.c): RGB565 strips cached in PSRAM. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef struct l5_grad_key l5_grad_key_t; /* a gradient: top / bottom color and the stops' positions */

/* The cached gradient top -> bottom with the stops at 0 and `stop` (0..255); NULL when 64 are in use. */
const l5_grad_key_t *l5_grad_key(lv_color_t top, lv_color_t bottom, uint8_t stop);
/* Fill `area` with the gradient: dithered, else a plain lv_draw_rect gradient (black without a key). */
void l5_grad_draw(lv_layer_t *layer, const l5_grad_key_t *key, const lv_area_t *area);
/* Draw the object's opaque 2-stop vertical LV_PART_MAIN background gradient dithered. */
void l5_dither_hook(lv_obj_t *obj);
/* lv_obj_add_style(obj, dark ? &L5S.dark : &L5S.plate, 0) + l5_dither_hook(obj) */
void l5_panel(lv_obj_t *obj, bool dark);
/* Dithering off / on (on from the start). Nothing in the build of 2026-10-20 calls it. */
void l5_dither_set_enabled(bool on);
