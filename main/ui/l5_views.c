/* Tab routing. SD is l5_sd.c, OTA l5_ota.c, PMAN l5_pman.c, CFG l5_cfg.c, HOME l5_home.c (built by l5_ui.c).
   The WUI tab and its placeholder were removed on 20.10.2026 (WUI is not ported; the tab only said so). */
#include "l5_ui.h"

void l5_views_render(l5_view_t v, lv_obj_t *view)
{
    lv_obj_clean(view);
    switch (v) {
    case L5_VIEW_SD: l5_sd_render(view); break;
    case L5_VIEW_OTA: l5_ota_render(view); break;
    case L5_VIEW_PMAN: l5_pman_render(view); break;
    case L5_VIEW_CFG: l5_cfg_render(view); break;
    default: break;
    }
}
