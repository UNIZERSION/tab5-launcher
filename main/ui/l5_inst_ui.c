/* The install flow shared by SD and OTA: L5.install / L5.tooBig (app.js) on top of l5_inst.c.
   1. the firmware is compared with the partition table (l5_fw_check); missing, smaller or other
      data partitions, or images that cannot be written, open the check dialog with
      INSTALL ANYWAY / AUTO PARTITION (l5_fw_plan: the layout the firmware's own table asks for)
   2. the flash slot (empty ones first), APP ONLY / APP + DATA when Ask SPIFFS is on
   3. the install task with progress and CANCEL; AUTO PARTITION writes the table first
      (l5_pt_apply) and installs into it before the one restart
   4. RESTART NOW (otadata -> ota_N, the ordered Wi-Fi shutdown runs in esp_restart), or RETRY
      after a network failure */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "l5_board.h"
#include "l5_config.h"
#include "l5_inst.h"
#include "l5_slots.h"
#include "l5_ui.h"

static const char *TAG = "l5_inst_ui";

enum { ACT_CANCEL, ACT_ANYWAY, ACT_AUTO };

static struct {
    l5_fw_t *fw;
    char name[64], author[48], src[8];
    l5_flash_map_t *map;  /* the table read when the flow started */
    l5_ptable_t *next;    /* AUTO PARTITION */
    l5_layout_t plan;
    l5_fw_report_t rep;
    char notes[400];      /* plan notes, or why AUTO PARTITION is not possible */
    int slot;
    int acts[3];
    bool busy, plan_ok, auto_part, table_written, data;
    volatile bool cancel;
    l5_progress_t *progress;
    l5_inst_result_t res;
    char why[220];
} F;

/* after AUTO PARTITION: where START APP hands over to */
static struct {
    uint32_t addr, size, count;
    char name[64];
} s_boot;

static char s_body[1200]; /* dialog text, built on the LVGL task only */

bool l5_install_busy(void)
{
    return F.busy;
}

static void flow_end(void)
{
    heap_caps_free(F.fw);
    heap_caps_free(F.map);
    heap_caps_free(F.next);
    F.fw = NULL;
    F.map = NULL;
    F.next = NULL;
    F.busy = false;
}

/* ---------- 3. the install task ---------- */

static void progress_cb(int pct, const char *text, void *user)
{
    (void)user;
    if (l5_board_lock(0)) {
        l5_progress_set(F.progress, F.auto_part ? 30 + pct * 70 / 100 : pct, text);
        l5_board_unlock();
    }
}

static void pt_progress(int pct, const char *label, void *user)
{
    (void)user;
    if (l5_board_lock(0)) {
        l5_progress_set(F.progress, pct * 30 / 100, label);
        l5_board_unlock();
    }
}

static void start(void);

static void done_cb(bool restart, void *user)
{
    (void)user;
    if (restart) l5_boot_app(); /* RESTART NOW: the installed slot is S.boot */
}

static void auto_done_cb(bool start_app, void *user)
{
    (void)user;
    if (start_app) l5_boot_at(0, s_boot.count, s_boot.addr, s_boot.size, s_boot.name);
    else l5_restart();
}

static void retry_cb(bool retry, void *user)
{
    (void)user;
    if (retry) {
        start();
        return;
    }
    bool restart = F.table_written; /* the running table is stale: nothing else may use flash before */
    flow_end();
    if (restart) l5_restart();
}

static void restart_cb(bool ok, void *user)
{
    (void)user;
    if (ok) l5_restart();
}

static void result(esp_err_t err)
{
    char size[16], title[96];
    if (err == ESP_OK) {
        l5_fmt_bytes(size, sizeof(size), F.res.image_len);
        snprintf(title, sizeof(title), "%s installed", F.name);
        int n = snprintf(s_body, sizeof(s_body), "Version %s \xC2\xB7 %s written to app%d", F.fw->version[0] ? F.fw->version : "-",
                         size, F.slot);
        if (F.res.data_written) {
            n += snprintf(s_body + n, sizeof(s_body) - n, " with %d data image%s", F.res.data_written,
                          F.res.data_written == 1 ? "" : "s");
        } else if (F.rep.images && !F.data) {
            n += snprintf(s_body + n, sizeof(s_body) - n, " without its data image%s (%s)", F.rep.images == 1 ? "" : "s",
                          g_cfg.askSpiffs ? "APP ONLY" : "Avoid SPIFFS");
        }
        if (F.table_written) { /* AUTO PARTITION: esp_partition still has the old table until the restart */
            const l5_part_t *p = NULL;
            for (int i = 0; i < F.next->n; i++) {
                if (F.next->p[i].type == ESP_PARTITION_TYPE_APP && F.next->p[i].subtype == ESP_PARTITION_SUBTYPE_APP_OTA_MIN + F.slot)
                    p = &F.next->p[i];
            }
            s_boot.addr = p ? p->offset : 0;
            s_boot.size = p ? p->size : 0;
            s_boot.count = (uint32_t)F.plan.n;
            strlcpy(s_boot.name, F.name, sizeof(s_boot.name));
            l5_slots_set_boot(F.slot);
            snprintf(s_body + n, sizeof(s_body) - n, ", in the new partition table. The Tab5 restarts to use the new table.");
            l5_ask("DONE", title, s_body, "TO LAUNCHER", "START APP", L5_ICON_RESTART, auto_done_cb, NULL);
        } else {
            l5_slots_scan();
            l5_slots_select(F.slot); /* "Setting boot partition": it is now the app that boots */
            l5_ui_render();
            snprintf(s_body + n, sizeof(s_body) - n, ". It is now the app that boots.");
            l5_ask("DONE", title, s_body, "STAY IN LAUNCHER", "RESTART NOW", L5_ICON_RESTART, done_cb, NULL);
        }
        flow_end();
        return;
    }
    if (F.auto_part && !F.table_written) {
        l5_info("PARTITION", "Partition table not changed", F.why);
        flow_end();
        return;
    }
    if (F.auto_part && F.res.stage == L5_INST_CHECK && !F.res.text[0]) { /* the table is written, erasing stopped */
        l5_ask("PARTITION", "Restart needed", F.why, "LATER", "RESTART NOW", L5_ICON_RESTART, restart_cb, NULL);
        flow_end();
        return;
    }
    snprintf(s_body, sizeof(s_body), "%s %s%s", F.res.text,
             F.res.erased ? "The app slot is left empty." : "Nothing was written.",
             F.table_written ? " The new partition table is in place: the launcher restarts before anything else." : "");
    s_body[sizeof(s_body) - 1] = 0;
    if (F.res.net && !F.res.cancelled) {
        l5_ask("INSTALL", F.res.cancelled ? "Install cancelled" : "Install failed", s_body,
               F.table_written ? "RESTART" : "CLOSE", "RETRY", L5_ICON_RESTART, retry_cb, NULL);
        return; /* the flow stays for RETRY */
    }
    if (F.table_written) {
        l5_ask("INSTALL", F.res.cancelled ? "Install cancelled" : "Install failed", s_body, "LATER", "RESTART NOW",
               L5_ICON_RESTART, restart_cb, NULL);
    } else {
        l5_info("INSTALL", F.res.cancelled ? "Install cancelled" : "Install failed", s_body);
    }
    flow_end();
}

/* the app names only the first Tab5's display and touch: asked from the install task, which waits */
static SemaphoreHandle_t s_hw_sem;
static bool s_hw_go;

static void hw_cb(int index, void *user)
{
    (void)user;
    s_hw_go = index == 1;
    xSemaphoreGive(s_hw_sem);
}

static bool hw_warn(void *user)
{
    (void)user;
    const char *panel = l5_board_new_panel();
    if (!panel) return true; /* a first Tab5 (ILI9881C + GT911): that is what the app expects */
    if (!s_hw_sem && !(s_hw_sem = xSemaphoreCreateBinary())) return true;
    static char body[300];
    static const char *const labels[] = {"CANCEL", "INSTALL ANYWAY"};
    snprintf(body, sizeof(body), "This firmware seems to support only the older Tab5 display/touch (ILI9881C + GT911). "
             "This Tab5 has %s, so it may not start.", panel);
    l5_board_lock(0);
    l5_choose("INSTALL", "Older Tab5 hardware", body, labels, 2, L5_ICON_INSTALL, hw_cb, NULL);
    l5_board_unlock();
    xSemaphoreTake(s_hw_sem, portMAX_DELAY);
    return s_hw_go;
}

static void task(void *arg)
{
    (void)arg;
    esp_err_t err = ESP_OK;
    memset(&F.res, 0, sizeof(F.res));
    if (F.auto_part && !F.table_written) {
        err = l5_pt_apply(F.map, F.next, pt_progress, NULL, &F.table_written, F.why, sizeof(F.why));
        if (F.table_written) { /* cfg.scheme follows what flash holds now */
            strlcpy(g_cfg.scheme, "custom", sizeof(g_cfg.scheme));
            if (l5_board_sd_ready()) l5_cfg_save_now();
        }
    }
    if (err == ESP_OK) {
        const l5_inst_job_t job = {
            .fw = F.fw, .table = F.table_written ? F.next : &F.map->table, .slot = F.slot, .data = F.data,
            .name = F.name, .author = F.author, .src = F.src, .cancel = &F.cancel, .progress = progress_cb,
            .hw_warn = hw_warn,
        };
        err = l5_inst_run(&job, &F.res);
    }
    l5_board_lock(0);
    l5_progress_close(F.progress);
    F.progress = NULL;
    result(err);
    l5_board_unlock();
    vTaskDelete(NULL);
}

static void start(void)
{
    char title[96];
    F.cancel = false;
    snprintf(title, sizeof(title), "Installing %s", F.name);
    F.progress = l5_progress_open("INSTALL", title);
    l5_progress_cancelable(F.progress, &F.cancel);
    l5_progress_set(F.progress, 0, F.auto_part && !F.table_written ? "Checking the new table" : "Starting");
    if (xTaskCreatePinnedToCore(task, "l5_install", 10240, NULL, 3, NULL, 1) != pdPASS) {
        l5_progress_close(F.progress);
        F.progress = NULL;
        l5_info("INSTALL", "Install failed", "No memory for the install task.");
        flow_end();
    }
}

/* ---------- 2. slot, APP ONLY / APP + DATA ---------- */

static void data_cb(bool ok, void *user)
{
    (void)user;
    F.data = ok;
    start();
}

static void slot_chosen(int slot)
{
    F.slot = slot;
    /* Ask SPIFFS off ("Avoid SPIFFS"): the app alone, its data partitions are not written (app.js L5.install:
       withSpiffs stays false unless Ask SPIFFS is on and the user picks it) */
    F.data = F.rep.images > 0 && g_cfg.askSpiffs;
    if (!F.data) {
        start();
        return;
    }
    char sz[16];
    int n = 0;
    for (int i = 0; i < F.rep.n && n < (int)sizeof(s_body) - 80; i++) {
        const l5_fw_item_t *it = &F.rep.item[i];
        if (!it->writable) continue;
        l5_fmt_bytes(sz, sizeof(sz), it->want);
        n += snprintf(s_body + n, sizeof(s_body) - n, "%s%s (%s %s)", n ? ", " : "", it->label, l5_fw_subtype_name(it->subtype), sz);
    }
    snprintf(s_body + n, sizeof(s_body) - n, " come%s with the firmware. Writing %s replaces what %s now.",
             F.rep.images == 1 ? "s" : "", F.rep.images == 1 ? "it" : "them",
             F.rep.images == 1 ? "that partition holds" : "these partitions hold");
    l5_ask("DATA", "Install the data images too?", s_body, "APP ONLY", "APP + DATA", L5_ICON_INSTALL, data_cb, NULL);
}

static void slot_menu_cb(const char *id, void *user)
{
    (void)user;
    if (id) slot_chosen(atoi(id));
    else flow_end();
}

static void replace_cb(bool ok, void *user)
{
    (void)user;
    if (ok) slot_chosen(0);
    else flow_end();
}

static void pick_slot(void)
{
    if (g_slots.n == 1) {
        const l5_slot_t *s = &g_slots.slot[0];
        if (!s->used) {
            slot_chosen(0);
            return;
        }
        char title[96];
        snprintf(title, sizeof(title), "Replace %s?", s->name);
        snprintf(s_body, sizeof(s_body), "app0 is the only app slot. %s is erased and %s is written in its place.", s->name, F.name);
        l5_confirm("FLASH SLOT", title, s_body, "REPLACE", true, replace_cb, NULL);
        return;
    }
    /* empty slots first, a used one gets replaced */
    l5_menu_item_t items[L5_MAX_SLOTS];
    static char ids[L5_MAX_SLOTS][4], labels[L5_MAX_SLOTS][96];
    char kb[16], title[96];
    l5_fmt_kb(kb, sizeof(kb), g_slots.slot_kb);
    int k = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < g_slots.n && i < L5_MAX_SLOTS; i++) {
            const l5_slot_t *s = &g_slots.slot[i];
            if (s->used != (pass == 1)) continue;
            snprintf(ids[k], sizeof(ids[k]), "%d", i);
            if (s->used) snprintf(labels[k], sizeof(labels[k]), "APP%d \xC2\xB7 %sB \xC2\xB7 replace %s", i, kb, s->name);
            else snprintf(labels[k], sizeof(labels[k]), "APP%d \xC2\xB7 %sB \xC2\xB7 empty", i, kb);
            items[k] = (l5_menu_item_t){ids[k], s->used ? L5_ICON_TRASH : L5_ICON_INSTALL, labels[k], s->used};
            k++;
        }
    }
    snprintf(title, sizeof(title), "Install %s to", F.name);
    l5_menu("FLASH SLOT", title, items, k, slot_menu_cb, NULL);
}

/* ---------- 1. the check against the partition table ---------- */

static const char *s_best_scheme;

static void use_scheme_cb(bool ok, void *user)
{
    (void)user;
    if (ok && s_best_scheme) l5_pman_use_scheme(s_best_scheme); /* its own confirmation lists what is erased */
}

/* L5.tooBig: an app without a known table that does not fit one slot */
static void too_big(void)
{
    char need[16], slot[16], ok[32];
    const char *best = l5_slots_best_scheme(F.rep.app_need);
    l5_fmt_bytes(need, sizeof(need), F.rep.app_need);
    l5_fmt_kb(slot, sizeof(slot), g_slots.slot_kb);
    snprintf(s_body, sizeof(s_body), "%s needs %s, one app slot has %sB in the %s scheme.%s%s%s", F.name, need, slot,
             g_slots.scheme, best ? " The " : " No scheme on 16 MB flash fits this binary.", best ? best : "",
             best ? " scheme fits it." : "");
    flow_end();
    if (!best) {
        l5_info("PARTITION", "App slot too small", s_body);
        return;
    }
    size_t i = snprintf(ok, sizeof(ok), "USE ");
    for (const char *c = best; *c && i + 1 < sizeof(ok); c++) ok[i++] = (char)toupper((unsigned char)*c);
    ok[i] = 0;
    s_best_scheme = best;
    l5_ask("PARTITION", "App slot too small", s_body, "CANCEL", ok, L5_ICON_PMAN, use_scheme_cb, NULL);
}

static void auto_cb(bool ok, void *user)
{
    (void)user;
    if (!ok) {
        flow_end();
        return;
    }
    F.auto_part = true;
    F.slot = 0;
    /* The partitions are new and empty: with Ask SPIFFS their images go in without asking, with Avoid SPIFFS
       they stay empty and only the app is installed. */
    F.data = g_cfg.askSpiffs;
    start();
}

static void confirm_auto(void)
{
    char lay[220], apps[300] = "";
    l5_pt_diff_t d;
    l5_pt_diff(F.map, F.next, &d);
    l5_layout_text(&F.plan, lay, sizeof(lay));
    if (d.apps) l5w_apps_erased(apps, sizeof(apps));
    int n = snprintf(s_body, sizeof(s_body), "New table: %s.\n\n%s%s", lay, apps, apps[0] ? " " : "");
    if (d.erased[0]) n += snprintf(s_body + n, sizeof(s_body) - n, "Erased: %s. ", d.erased);
    if (d.removed[0]) n += snprintf(s_body + n, sizeof(s_body) - n, "Removed with their data: %s. ", d.removed);
    snprintf(s_body + n, sizeof(s_body) - n, "\n\n%s goes into app0 %s, then the Tab5 restarts. "
             "Do not turn it off or press RESET before that.", F.name,
             g_cfg.askSpiffs ? "with the data images that fit"
                             : "without its data images (Avoid SPIFFS: the new data partitions stay empty)");
    l5_confirm("PARTITION", "Change the partition table?", s_body, "APPLY + INSTALL", true, auto_cb, NULL);
}

static void compat_cb(int index, void *user)
{
    (void)user;
    switch (F.acts[index]) {
    case ACT_ANYWAY: pick_slot(); break;
    case ACT_AUTO: confirm_auto(); break;
    default: flow_end(); break;
    }
}

static void compat_dialog(void)
{
    char lay[220], title[96];
    l5_fw_report_text(&F.rep, s_body, sizeof(s_body));
    size_t n = strlen(s_body);
    if (F.plan_ok) {
        char apps[300] = "";
        l5_pt_diff_t d;
        l5_pt_diff(F.map, F.next, &d);
        l5_layout_text(&F.plan, lay, sizeof(lay));
        if (d.apps) l5w_apps_erased(apps, sizeof(apps));
        snprintf(s_body + n, sizeof(s_body) - n, "\n\nAUTO PARTITION: %s.%s%s%s%s", lay, F.notes[0] ? " " : "", F.notes,
                 apps[0] ? " " : "", apps);
    } else {
        snprintf(s_body + n, sizeof(s_body) - n, "\n\nAUTO PARTITION is not possible: %s", F.notes);
    }
    if (!F.rep.app_fits) snprintf(title, sizeof(title), "App slot too small");
    else if (F.rep.problems) snprintf(title, sizeof(title), "%.60s expects other partitions", F.name);
    else snprintf(title, sizeof(title), "Data images cannot be written");
    const char *labels[3];
    int k = 0;
    F.acts[k] = ACT_CANCEL, labels[k++] = "CANCEL";
    if (F.rep.app_fits) F.acts[k] = ACT_ANYWAY, labels[k++] = "INSTALL ANYWAY";
    if (F.plan_ok) F.acts[k] = ACT_AUTO, labels[k++] = "AUTO PARTITION";
    if (k == 1) {
        l5_info("INSTALL", title, s_body);
        flow_end();
        return;
    }
    l5_choose("INSTALL", title, s_body, labels, k, F.plan_ok ? L5_ICON_PMAN : L5_ICON_INSTALL, compat_cb, NULL);
}

void l5_install_fw(l5_fw_t *fw, const char *name, const char *author, const char *src)
{
    if (F.busy) {
        heap_caps_free(fw);
        l5_toast("An install is already running");
        return;
    }
    memset(&F, 0, sizeof(F));
    F.busy = true;
    F.fw = fw;
    strlcpy(F.name, name && *name ? name : fw->name[0] ? fw->name : "Firmware", sizeof(F.name));
    strlcpy(F.author, author && *author ? author : fw->author, sizeof(F.author));
    strlcpy(F.src, src, sizeof(F.src));
    F.map = heap_caps_calloc(1, sizeof(*F.map), MALLOC_CAP_SPIRAM);
    F.next = heap_caps_calloc(1, sizeof(*F.next), MALLOC_CAP_SPIRAM);
    if (!F.map || !F.next) {
        l5_info("INSTALL", "Cannot install", "Not enough memory.");
        flow_end();
        return;
    }
    if (l5_pt_read_map(F.map) != ESP_OK || !g_slots.n) {
        l5_info("INSTALL", "Cannot install", g_slots.n ? F.map->why : "The partition table has no ota_N app slots. "
                "Flash partitions.csv from this project.");
        flow_end();
        return;
    }
    l5_fw_check(fw, &F.map->table, g_slots.slot_kb * 1024, &F.rep);
    ESP_LOGI(TAG, "%s: app %lu B (%s), %d data partition(s), %d problem(s), %d image(s) not writable", F.name,
             (unsigned long)F.rep.app_need, F.rep.app_known ? "known" : "bound", fw->nparts, F.rep.problems, F.rep.lost);
    if (!fw->nparts) { /* only an app is known: classic slot check */
        if (F.rep.app_fits) pick_slot();
        else too_big();
        return;
    }
    if (F.rep.app_fits && !F.rep.problems && !F.rep.lost) {
        pick_slot();
        return;
    }
    if (!F.map->managed) {
        strlcpy(F.notes, F.map->why, sizeof(F.notes));
    } else if (l5_fw_plan(fw, &F.plan, F.notes, sizeof(F.notes)) != ESP_OK) {
        /* notes says why */
    } else if (l5_pt_build(F.map, &F.plan, F.next, F.why, sizeof(F.why)) != ESP_OK) {
        strlcpy(F.notes, F.why, sizeof(F.notes));
    } else if (F.map->aurora && l5_layout_equal(&F.map->layout, &F.plan)) {
        strlcpy(F.notes, "the table already has the layout it would make.", sizeof(F.notes));
    } else {
        F.plan_ok = true;
    }
    compat_dialog();
}
