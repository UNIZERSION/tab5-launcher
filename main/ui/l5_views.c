/* Tab routing, plus the placeholder screen for WUI. SD is l5_sd.c, OTA l5_ota.c, PMAN l5_pman.c,
   CFG l5_cfg.c. The placeholder keeps the prototype's two-pane layout (.vw-main dark 816 px +
   .vw-side plate 428 px). */
#include <stdio.h>
#include <string.h>
#include "l5_ui.h"

static lv_obj_t *pane(lv_obj_t *view, bool dark, int32_t x, int32_t w)
{
    lv_obj_t *p = l5_box(view);
    lv_obj_add_style(p, dark ? &L5S.dark : &L5S.plate, 0);
    lv_obj_set_pos(p, x, L5_PANEL_Y);
    lv_obj_set_size(p, w, L5_PANEL_H);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    l5_chamfer(p, L5_CHAMFER, L5_PLATE);
    return p;
}

static lv_obj_t *header(lv_obj_t *pane, bool dark, const char *title, const char *tag)
{
    lv_obj_t *tg = l5_col_header(pane, title, tag);
    if (!dark) { /* .plate .col-h */
        lv_obj_t *h = lv_obj_get_parent(tg);
        lv_obj_set_style_border_color(h, L5_PLATE_LO, 0);
        lv_obj_set_style_text_color(h, L5_INK, 0);
        lv_obj_set_style_text_color(tg, L5_MUTED, 0);
    }
    return tg;
}

/* .empty block: bold line + description */
static void empty(lv_obj_t *parent, bool dark, const char *title, const char *body)
{
    lv_obj_t *box = l5_box(parent);
    lv_obj_set_width(box, lv_pct(100));
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_style_pad_hor(box, 40, 0);
    lv_obj_set_style_pad_row(box, 8, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    l5_label(box, &l5_chakra_b24, dark ? L5_ON_PANEL : L5_INK, title);
    lv_obj_t *b = l5_label(box, &l5_chakra_r17, dark ? L5_ON_PANEL_MUTED : L5_INK_SOFT, body);
    lv_obj_set_width(b, lv_pct(100));
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(b, 6, 0);
}

static void wui_view(lv_obj_t *view)
{
    lv_obj_t *m = pane(view, true, 12, 816);
    header(m, true, "WEB INTERFACE", "STOPPED");
    empty(m, true, "WebUI: not ported yet",
          "Prototype: STA or AP mode, user / password, QR code with the URL, request log. The device side "
          "is Launcher's web server (file manager, partitions, NVS editor, install from browser).");
    lv_obj_t *s = pane(view, false, 840, 428);
    header(s, false, "ACCESS", "");
    empty(s, false, "No URL yet", "Starts after Wi-Fi is up.");
}

void l5_views_render(l5_view_t v, lv_obj_t *view)
{
    lv_obj_clean(view);
    switch (v) {
    case L5_VIEW_SD: l5_sd_render(view); break;
    case L5_VIEW_OTA: l5_ota_render(view); break;
    case L5_VIEW_WUI: wui_view(view); break;
    case L5_VIEW_PMAN: l5_pman_render(view); break;
    case L5_VIEW_CFG: l5_cfg_render(view); break;
    default: break;
    }
}
