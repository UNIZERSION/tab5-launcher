/* HOME: only the apps already in flash. Pick the one that boots after the bootscreen, or boot
   it now. No settings here: they live in the top-bar tabs. Port of renderHome() in app.js, with
   hero() for one slot. */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "l5_board.h"
#include "l5_config.h"
#include "l5_display.h" /* L5_TOUCH_TRACE */
#include "l5_slots.h"
#include "l5_ui.h"
#include "l5_wifi.h"
#if L5_TOUCH_TRACE
#include "esp_log.h"
#include "esp_timer.h"
#endif
/* 1: a long press on "APPS IN FLASH" times the big card's parts (with L5_TOUCH_TRACE; measured 19.10.2026, README) */
#ifndef L5_HOME_BENCH
#define L5_HOME_BENCH 0
#endif

#define PANEL_W 1256
#define PANEL_H L5_PANEL_H
#define MAX_CARDS 8

/* widgets that follow the boot selection in place (a tap redraws two cards and the footer, not
   the view); all NULL while HOME is not built */
static struct {
    lv_obj_t *card[MAX_CARDS]; /* installed slots */
    lv_obj_t *pick;            /* "BOOTS" on the selected card */
    lv_obj_t *line, *name, *boot;
    lv_obj_t *hero_card, *hero_cover, *hero_name; /* one slot: the big card, its emblem and name */
    bool panel_full;                              /* the panel paints its background under the card too */
} H;

static void upper(char *dst, const char *src, size_t len)
{
    size_t i = 0;
    for (; src[i] && i + 1 < len; i++) {
        dst[i] = (char)toupper((unsigned char)src[i]);
    }
    dst[i] = 0;
}

/* ---------- erase (L5.eraseSlot) ---------- */

typedef struct {
    int slot;
    l5_progress_t *progress;
    char label[40];
} erase_job_t;

static void erase_progress(int pct, void *user)
{
    erase_job_t *job = user;
    if (l5_board_lock(0)) {
        l5_progress_set(job->progress, pct, job->label);
        l5_board_unlock();
    }
}

static void erase_task(void *arg)
{
    erase_job_t *job = arg;
    esp_err_t err = l5_slots_erase(job->slot, erase_progress, job);
    l5_board_lock(0);
    l5_progress_close(job->progress);
    if (err == ESP_OK) {
        l5_toast("app%d erased", job->slot);
    } else {
        l5_info("FLASH SLOT", "Erase failed", esp_err_to_name(err));
    }
    l5_ui_render();
    l5_board_unlock();
    lv_free(job);
    vTaskDelete(NULL);
}

static void erase_confirmed(bool ok, void *user)
{
    if (!ok) {
        return;
    }
    int i = (int)(intptr_t)user;
    erase_job_t *job = lv_zalloc(sizeof(erase_job_t));
    job->slot = i;
    char title[24], kb[16];
    snprintf(title, sizeof(title), "Erasing app%d", i);
    l5_fmt_kb(kb, sizeof(kb), g_slots.slot_kb);
    snprintf(job->label, sizeof(job->label), "Erasing %sB", kb);
    job->progress = l5_progress_open("FLASH SLOT", title);
    l5_progress_set(job->progress, 0, job->label);
    xTaskCreatePinnedToCore(erase_task, "l5_erase", 4096, job, 3, NULL, 1);
}

static void erase_slot(int i)
{
    const l5_slot_t *s = &g_slots.slot[i];
    if (!s->used) {
        return;
    }
    char title[24], body[160];
    snprintf(title, sizeof(title), "Erase app%d?", i);
    snprintf(body, sizeof(body), "%s is removed from flash. The .bin on the SD card is not touched.", s->name);
    l5_confirm("FLASH SLOT", title, body, "ERASE", true, erase_confirmed, (void *)(intptr_t)i);
}

/* ---------- selection, in place ---------- */

static lv_obj_t *add_pick(lv_obj_t *card) /* .pick "BOOTS" */
{
    lv_obj_t *pick = l5_label(card, &l5_mono_12, L5P.accent_ink, "BOOTS");
    lv_obj_add_style(pick, &L5S.pick, 0);
    lv_obj_align(pick, LV_ALIGN_TOP_RIGHT, -(52 - 1), 10 - 1);
    return pick;
}

static void set_label(lv_obj_t *l, const char *t)
{
    if (l && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

/* g_slots.boot -> .slot.sel, "BOOTS" and the footer */
static void paint_selection(void)
{
    const l5_slot_t *sel = l5_slots_boot_app();
    for (int i = 0; i < MAX_CARDS; i++) {
        if (!H.card[i]) continue;
        lv_obj_remove_style(H.card[i], &L5S.slot_sel, 0);
        if (sel && i == g_slots.boot) lv_obj_add_style(H.card[i], &L5S.slot_sel, 0);
    }
    if (H.pick) lv_obj_delete(H.pick);
    H.pick = sel && g_slots.boot < MAX_CARDS && H.card[g_slots.boot] ? add_pick(H.card[g_slots.boot]) : NULL;

    char line[96], name[40], btn[48];
    const bool unchosen = !sel && l5_slots_used() > 0; /* flashed by hand: waits for a tap (l5_slots_scan) */
    if (sel) {
        snprintf(line, sizeof(line), "APP%d STARTS AFTER THE %us BOOTSCREEN", g_slots.boot, g_cfg.bootTime);
    } else {
        snprintf(line, sizeof(line), unchosen ? "NO APP CHOSEN TO START" : "NO APP IN FLASH");
    }
    set_label(H.line, line);
    set_label(H.name, sel ? sel->name : unchosen ? "Tap an app to choose it" : "Install a firmware first");
    upper(name, sel ? sel->name : "APP", sizeof(name));
    snprintf(btn, sizeof(btn), "BOOT %s", name);
    if (H.boot) {
        set_label(lv_obj_get_child(H.boot, -1), btn); /* l5_act_button(): [icon,] text */
        lv_obj_set_state(H.boot, LV_STATE_DISABLED, !sel);
    }
}

static void home_deleted_cb(lv_event_t *e)
{
    (void)e;
    memset(&H, 0, sizeof(H));
}

/* ---------- card events ---------- */

static void card_menu_cb(const char *id, void *user)
{
    int i = (int)(intptr_t)user;
    if (!id) {
        return;
    }
    if (strcmp(id, "boot") == 0) {
        l5_slots_select(i);
        l5_boot_app();
    } else if (strcmp(id, "pick") == 0) {
        l5_slots_select(i);
        paint_selection();
    } else if (strcmp(id, "erase") == 0) {
        erase_slot(i);
    }
}

static void more_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    char erase[32], kind[8];
    snprintf(erase, sizeof(erase), "Erase app%d from flash", i);
    snprintf(kind, sizeof(kind), "APP%d", i);
    const l5_menu_item_t items[] = {
        {"boot", L5_ICON_LAUNCH, "Boot now", false},
        {i != g_slots.boot ? "pick" : NULL, L5_ICON_CHECK, "Boot this app after the bootscreen", false},
        {"erase", L5_ICON_TRASH, erase, true},
    };
    l5_menu(kind, g_slots.slot[i].name, items, 3, card_menu_cb, (void *)(intptr_t)i);
}

static void card_cb(lv_event_t *e)
{
    l5_slots_select((int)(intptr_t)lv_event_get_user_data(e));
    paint_selection();
}

static void boot_cb(lv_event_t *e)
{
    (void)e;
    l5_boot_app();
}

/* BOOT on the one-slot card: also when app0 is not chosen yet (flashed by hand), it becomes the boot app */
static void hero_boot_cb(lv_event_t *e)
{
    (void)e;
    l5_slots_select(0);
    l5_boot_app();
}

/* ---------- building blocks ---------- */

static lv_obj_t *slot_label(lv_obj_t *card, int i)
{
    char t[24];
    snprintf(t, sizeof(t), "APP%d \xC2\xB7 OTA_%d", i, i);
    lv_obj_t *l = l5_label(card, &l5_mono_12, L5_ON_PANEL_MUTED, t);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_set_pos(l, 14 - 1, 12 - 1);
    return l;
}

static lv_obj_t *inner_column(lv_obj_t *card)
{
    /* .slot padding 40px 18px 16px, flex column, gap 8 */
    lv_obj_t *col = l5_box(card);
    lv_obj_set_size(col, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_top(col, 40, 0);
    lv_obj_set_style_pad_hor(col, 18, 0);
    lv_obj_set_style_pad_bottom(col, 16, 0);
    lv_obj_set_style_pad_row(col, 8, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
    return col;
}

static void used_card(lv_obj_t *card, int i)
{
    const l5_slot_t *s = &g_slots.slot[i];
    lv_obj_add_style(card, &L5S.slot, 0); /* .slot.sel and "BOOTS": paint_selection() */
    lv_obj_add_style(card, &L5S.press_ring, LV_STATE_PRESSED);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, card_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    if (i < MAX_CARDS) H.card[i] = card;

    lv_obj_t *col = inner_column(card);
    char t[96];
    l5_cover_create(col, 200, s->name, s->name, "ESP32-P4 APP", i, true);

    lv_obj_t *name = l5_label(col, &l5_chakra_b24, L5_ON_PANEL, s->name);
    lv_obj_set_style_pad_top(name, 6, 0);
    lv_obj_set_style_max_width(name, lv_pct(100), 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);

    /* "author · vVER / SRC · DATE": the image has no author, so the project name stands in when the
       registry renamed the app; without a registry entry (flashed by hand) the build date is shown */
    int n = strcmp(s->name, s->project) ? snprintf(t, sizeof(t), "%s \xC2\xB7 v%s", s->project, s->ver)
                                        : snprintf(t, sizeof(t), "v%s", s->ver);
    if (n > 0 && (size_t)n < sizeof(t)) {
        if (s->src[0]) {
            snprintf(t + n, sizeof(t) - n, "\n%s \xC2\xB7 %s", s->src, s->installed[0] ? s->installed : s->date);
        } else {
            snprintf(t + n, sizeof(t) - n, "\nFLASH \xC2\xB7 %s", s->date);
        }
    }
    lv_obj_t *meta = l5_label(col, &l5_mono_14, L5_ON_PANEL_MUTED, t);
    lv_obj_set_style_max_width(meta, lv_pct(100), 0);
    lv_label_set_long_mode(meta, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(meta, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(meta, 4, 0);

    lv_obj_t *grow = l5_box(col); /* .use { margin-top: auto } */
    lv_obj_set_flex_grow(grow, 1);

    lv_obj_t *use = l5_box(col);
    lv_obj_set_size(use, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(use, 6, 0);
    lv_obj_set_flex_flow(use, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *r = l5_box(use);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    uint64_t slot_bytes = (uint64_t)g_slots.slot_kb * 1024;
    int pct = slot_bytes ? (int)((uint64_t)s->size * 100 / slot_bytes) : 0;
    l5_fmt_bytes(t, sizeof(t), s->size);
    l5_label(r, &l5_mono_13, L5_ON_PANEL_MUTED, t);
    snprintf(t, sizeof(t), "%d%%", pct);
    l5_label(r, &l5_mono_13, L5_ON_PANEL_MUTED, t);
    lv_obj_t *meter = lv_bar_create(use);
    lv_obj_remove_flag(meter, LV_OBJ_FLAG_CLICKABLE); /* a tap on the meter selects the card */
    lv_obj_remove_style_all(meter);
    lv_obj_add_style(meter, &L5S.meter, LV_PART_MAIN);
    lv_obj_add_style(meter, &L5S.meter_ind, LV_PART_INDICATOR);
    lv_obj_set_width(meter, lv_pct(100));
    lv_bar_set_range(meter, 0, 100);
    lv_bar_set_value(meter, pct > 100 ? 100 : pct, LV_ANIM_OFF);

    slot_label(card, i);
    lv_obj_t *more = lv_button_create(card); /* .more */
    lv_obj_remove_style_all(more);
    lv_obj_set_size(more, 48, 48);
    lv_obj_align(more, LV_ALIGN_TOP_RIGHT, -(2 - 1), -1);
    lv_obj_set_style_bg_color(more, L5_PANEL_3, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(more, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(more, 4, 0);
    lv_obj_center(l5_icon(more, &l5_icon_22, L5_ON_PANEL, L5_ICON_MORE));
    lv_obj_add_event_cb(more, more_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
}

static void empty_card(lv_obj_t *card, int i)
{
    lv_obj_set_style_radius(card, 4, 0);
    l5_dashed_border(card, lv_color_hex(0x3a3d43));
    lv_obj_t *col = inner_column(card);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    l5_icon(col, &l5_icon_64, lv_color_hex(0x4a4d54), L5_ICON_BIN);
    lv_obj_t *b = l5_label(col, &l5_chakra_b24, L5_ON_PANEL, "Empty slot");
    lv_obj_set_style_pad_top(b, 6, 0);
    char kb[16], t[96];
    l5_fmt_kb(kb, sizeof(kb), g_slots.slot_kb);
    snprintf(t, sizeof(t), "%sB free for one firmware.\nInstall it from SD or OTA.", kb);
    lv_obj_t *meta = l5_label(col, &l5_mono_14, L5_ON_PANEL_MUTED, t);
    lv_obj_set_style_text_align(meta, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(meta, 4, 0);
    if (!l5_board_sd_ready()) { /* why "from SD" will not work right now; details in the SD tab */
        snprintf(t, sizeof(t), "SD CARD: %s", l5_board_sd_status(false));
        lv_obj_t *sd = l5_label(col, &l5_mono_13, L5_WARN, t);
        lv_obj_set_style_text_letter_space(sd, 1, 0);
        lv_obj_set_style_pad_top(sd, 4, 0);
    }
    slot_label(card, i);
}

/* ---------- one slot: one wide card instead of a narrow column (app.js hero(), .hero) ---------- */

#define HERO_ART 420 /* .hero grid-template-columns: 420px 1fr */
/* the details column: panel 1256 - borders 2 - .slots padding 2 x 18 - card border 2 - padding 26 + 34 -
   emblem 420 - gap 40 */
#define HERO_INFO_W (PANEL_W - 2 - 36 - 2 - 60 - HERO_ART - 40)

static void go_cb(lv_event_t *e)
{
    l5_ui_go((l5_view_t)(intptr_t)lv_event_get_user_data(e));
}

static void erase_cb(lv_event_t *e)
{
    (void)e;
    erase_slot(0);
}

static lv_obj_t *column_box(lv_obj_t *parent, int32_t gap) /* full-width flex column */
{
    lv_obj_t *c = l5_box(parent);
    lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(c, gap, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    return c;
}

static lv_obj_t *spaced(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = l5_label(parent, font, color, text);
    lv_obj_set_style_text_letter_space(l, 2, 0); /* letter-spacing .14em */
    return l;
}

/* .hero .top: "APP0 · OTA_0 · 11MB SLOT" and BOOTS */
static void hero_top(lv_obj_t *info, bool boots)
{
    lv_obj_t *top = l5_box(info);
    lv_obj_set_size(top, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(top, 12, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    char kb[16], t[48];
    l5_fmt_kb(kb, sizeof(kb), g_slots.slot_kb);
    snprintf(t, sizeof(t), "APP0 \xC2\xB7 OTA_0 \xC2\xB7 %sB SLOT", kb);
    spaced(top, &l5_mono_13, L5_ON_PANEL_MUTED, t);
    if (boots) {
        lv_obj_t *pick = l5_label(top, &l5_mono_12, L5P.accent_ink, "BOOTS");
        lv_obj_add_style(pick, &L5S.pick, 0);
    }
}

/* .hero h2 (700 46px, one line) and .by under it (margin-top -8 against the gap of 16) */
static lv_obj_t *hero_head(lv_obj_t *info, const char *title, const char *by)
{
    lv_obj_t *head = column_box(info, 8);
    lv_obj_t *h = l5w_line_label(head, &l5_chakra_b40, L5_ON_PANEL, title);
    lv_obj_set_width(h, lv_pct(100));
    lv_obj_t *b = l5_label(head, &l5_mono_16, L5_ON_PANEL_MUTED, by);
    lv_obj_set_width(b, lv_pct(100));
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(b, 5, 0); /* line-height 1.4 */
    return head;
}

/* .hero .go: the line above the buttons and the button row; returns the row */
static lv_obj_t *hero_go(lv_obj_t *info, const char *line)
{
    lv_obj_t *go = column_box(info, 10);
    spaced(go, &l5_mono_13, L5_ON_PANEL_MUTED, line);
    lv_obj_t *row = l5_box(go);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    return row;
}

/* .hero .btns: the plain button keeps its width, the primary one takes the rest of the row and cuts its
   label with dots (mono 16, 64 px, padding 0 26) */
static lv_obj_t *hero_buttons(lv_obj_t *row, const char *icon1, const char *text1, const char *icon2,
                              const char *text2)
{
    lv_obj_t *b[2];
    const char *icons[2] = {icon1, icon2}, *texts[2] = {text1, text2};
    int32_t plain_w = 0;
    for (int i = 0; i < 2; i++) {
        b[i] = l5_act_button(row, icons[i], texts[i], i == 0);
        lv_obj_set_height(b[i], 64);
        lv_obj_set_style_pad_hor(b[i], 26, 0);
        lv_obj_set_style_text_font(b[i], &l5_mono_16, 0);
        lv_obj_t *l = lv_obj_get_child(b[i], -1);
        lv_obj_set_height(l, lv_font_get_line_height(&l5_mono_16));
        if (i == 1) {
            plain_w = 2 * 26 + 20 + 10 + lv_text_get_width(texts[i], strlen(texts[i]), &l5_mono_16, 1);
        }
    }
    lv_obj_set_flex_grow(b[0], 1);
    /* label room: the row minus the plain button, the gap, the padding, the icon and its gap */
    lv_obj_set_style_max_width(lv_obj_get_child(b[0], -1), HERO_INFO_W - 12 - plain_w - 2 * 26 - 20 - 10, 0);
    return b[0];
}

/* .hero .facts: SOURCE / INSTALLED / SIZE / FREE IN SLOT between two lines, divided by vertical lines */
static void hero_facts(lv_obj_t *info, const char *const labels[4], const char *const values[4])
{
    lv_obj_t *f = l5_box(info);
    lv_obj_set_size(f, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_side(f, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(f, 1, 0);
    lv_obj_set_style_border_color(f, L5_PANEL_LINE, 0);
    lv_obj_set_flex_flow(f, LV_FLEX_FLOW_ROW);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *c = l5_box(f);
        lv_obj_set_size(c, 0, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(c, 1); /* four equal columns */
        lv_obj_set_style_pad_ver(c, 12, 0);
        lv_obj_set_style_pad_left(c, i ? 14 : 0, 0);
        lv_obj_set_style_pad_right(c, 14, 0);
        lv_obj_set_style_pad_row(c, 5, 0);
        if (i) {
            lv_obj_set_style_border_side(c, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_width(c, 1, 0);
            lv_obj_set_style_border_color(c, L5_PANEL_LINE, 0);
        }
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        spaced(c, &l5_mono_12, L5_ON_PANEL_MUTED, labels[i]);
        lv_obj_t *v = l5w_line_label(c, &l5_chakra_sb19, L5_ON_PANEL, values[i]);
        lv_obj_set_width(v, lv_pct(100));
    }
}

static void hero_used(lv_obj_t *card)
{
    const l5_slot_t *s = &g_slots.slot[0];
    const bool boots = g_slots.boot == 0; /* not when it was flashed by hand and nobody chose it yet */
    lv_obj_add_style(card, &L5S.slot, 0);
    if (boots) lv_obj_add_style(card, &L5S.slot_sel, 0); /* .hero.sel: the only app is the one that boots */
    /* the image has no author: the catalog author (OTA), else the project name when the registry renamed the app */
    const char *who = s->author[0] ? s->author : strcmp(s->name, s->project) ? s->project : NULL;
#if L5_TOUCH_TRACE
    int64_t t0 = esp_timer_get_time();
#endif
    H.hero_cover = l5_cover_create(card, HERO_ART, s->name, s->name, s->author[0] ? s->author : s->project, 0, true);
#if L5_TOUCH_TRACE
    ESP_LOGI("l5_home", "emblem 420 px drawn in %lld ms", (esp_timer_get_time() - t0) / 1000);
#endif

    lv_obj_t *info = l5_box(card); /* .info */
    lv_obj_set_size(info, HERO_INFO_W, lv_pct(100));
    lv_obj_set_style_pad_row(info, 16, 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    hero_top(info, boots);

    char by[96];
    const char *v = s->ver[0] == 'v' || s->ver[0] == 'V' ? "" : "v";
    if (who) {
        snprintf(by, sizeof(by), "%s \xC2\xB7 %s%s", who, v, s->ver);
    } else {
        snprintf(by, sizeof(by), "%s%s", v, s->ver);
    }
    H.hero_name = lv_obj_get_child(hero_head(info, s->name, by), 0);

    /* without an install date (flashed by hand, or no clock at the install) the build date stands in */
    char size[16], free_b[16];
    uint64_t slot_bytes = (uint64_t)g_slots.slot_kb * 1024;
    l5_fmt_bytes(size, sizeof(size), s->size);
    l5_fmt_bytes(free_b, sizeof(free_b), slot_bytes > s->size ? slot_bytes - s->size : 0);
    const char *const labels[4] = {"SOURCE", s->installed[0] ? "INSTALLED" : "BUILT", "SIZE", "FREE IN SLOT"};
    const char *const values[4] = {s->src[0] ? s->src : "FLASH", s->installed[0] ? s->installed : s->date, size, free_b};
    hero_facts(info, labels, values);

    lv_obj_t *use = column_box(info, 8); /* .hero .use */
    lv_obj_t *r = l5_box(use);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    int pct = slot_bytes ? (int)((uint64_t)s->size * 100 / slot_bytes) : 0;
    char t[64];
    spaced(r, &l5_mono_13, L5_ON_PANEL_MUTED, "SLOT USE");
    snprintf(t, sizeof(t), "%d%%", pct);
    spaced(r, &l5_mono_13, L5_ON_PANEL_MUTED, t);
    lv_obj_t *meter = lv_bar_create(use);
    lv_obj_remove_style_all(meter);
    lv_obj_add_style(meter, &L5S.meter, LV_PART_MAIN);
    lv_obj_add_style(meter, &L5S.meter_ind, LV_PART_INDICATOR);
    lv_obj_set_size(meter, lv_pct(100), 10);
    lv_obj_set_style_radius(meter, 1, LV_PART_MAIN);
    lv_bar_set_range(meter, 0, 100);
    lv_bar_set_value(meter, pct > 100 ? 100 : pct, LV_ANIM_OFF);

    if (boots) {
        snprintf(t, sizeof(t), "STARTS AFTER THE %us BOOTSCREEN", g_cfg.bootTime);
    } else {
        strlcpy(t, "NOT STARTED BY ITSELF UNTIL YOU BOOT IT", sizeof(t));
    }
    char name[L5_NAME_LEN], btn[L5_NAME_LEN + 8]; /* the whole name: the label cuts it with dots */
    upper(name, s->name, sizeof(name));
    snprintf(btn, sizeof(btn), "BOOT %s", name);
    lv_obj_t *row = hero_go(info, t);
    lv_obj_t *boot = hero_buttons(row, L5_ICON_LAUNCH, btn, L5_ICON_TRASH, "ERASE");
    lv_obj_add_event_cb(boot, hero_boot_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(lv_obj_get_child(row, 1), erase_cb, LV_EVENT_CLICKED, NULL);
}

/* .hero.empty .hole: repeating-linear-gradient(135deg, rgba(255,255,255,.025) 0 12px, transparent 12px 24px) */
static void hatch_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = L5_WHITE;
    d.opa = 6;
    d.width = 12;
    /* stripes run "/" (x + y = c), 24 px apart across them = 34 px apart in c; the first one starts at the
       top-left corner. The layer clips them to the box. */
    for (int32_t c = a.x1 + a.y1 + 8; c < a.x2 + a.y2 + 12; c += 34) {
        d.p1.x = c - a.y2 - 12;
        d.p1.y = a.y2 + 12;
        d.p2.x = c - a.y1 + 12;
        d.p2.y = a.y1 - 12;
        lv_draw_line(layer, &d);
    }
}

static void hero_empty(lv_obj_t *card)
{
    lv_obj_set_style_radius(card, 4, 0);
    l5_dashed_border(card, lv_color_hex(0x3a3d43));
    lv_obj_t *hole = l5_box(card);
    lv_obj_set_size(hole, HERO_ART, HERO_ART);
    lv_obj_set_style_radius(hole, 4, 0);
    lv_obj_set_style_pad_row(hole, 14, 0);
    lv_obj_set_flex_flow(hole, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hole, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(hole, hatch_cb, LV_EVENT_DRAW_MAIN, NULL);
    l5_dashed_border(hole, lv_color_hex(0x3a3d43));
    l5_icon(hole, &l5_icon_64, lv_color_hex(0x4a4d54), L5_ICON_BIN);
    spaced(hole, &l5_mono_13, lv_color_hex(0x5d616a), "NO FIRMWARE");

    lv_obj_t *info = l5_box(card);
    lv_obj_set_size(info, HERO_INFO_W, lv_pct(100));
    lv_obj_set_style_pad_row(info, 16, 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    hero_top(info, false);
    char kb[16], t[128];
    l5_fmt_kb(kb, sizeof(kb), g_slots.slot_kb);
    snprintf(t, sizeof(t), "%sB free for one firmware. Until one is installed, the bootscreen opens the Launcher.", kb);
    lv_obj_t *head = hero_head(info, "Empty slot", t);
    if (!l5_board_sd_ready()) { /* why FROM SD CARD will not work right now; details in the SD tab */
        snprintf(t, sizeof(t), "SD CARD: %s", l5_board_sd_status(false));
        spaced(head, &l5_mono_13, L5_WARN, t);
    }
    /* the two ways in, and what each needs (a Tab5 whose C6 has another ESP-Hosted has only the first) */
    char how[160];
    snprintf(how, sizeof(how), "SD card: copy .bin files (ESP32-P4 apps) to a FAT32 or exFAT card.\nOTA: the online catalog. "
             "Needs Wi-Fi: the ESP32-C6 with ESP-Hosted %s.x.", l5_wifi_host_series());
    lv_obj_t *hw = l5_label(info, &l5_mono_14, L5_ON_PANEL_MUTED, how);
    lv_obj_set_width(hw, lv_pct(100));
    lv_label_set_long_mode(hw, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(hw, 6, 0);
    lv_obj_t *row = hero_go(info, "INSTALL A FIRMWARE");
    lv_obj_t *sd = hero_buttons(row, L5_ICON_SD, "FROM SD CARD", L5_ICON_OTA, "FROM OTA");
    lv_obj_add_event_cb(sd, go_cb, LV_EVENT_CLICKED, (void *)(intptr_t)L5_VIEW_SD);
    lv_obj_add_event_cb(lv_obj_get_child(row, 1), go_cb, LV_EVENT_CLICKED, (void *)(intptr_t)L5_VIEW_OTA);
}

static bool clip_to(lv_area_t *out, const lv_area_t *a, const lv_area_t *b)
{
    out->x1 = LV_MAX(a->x1, b->x1);
    out->y1 = LV_MAX(a->y1, b->y1);
    out->x2 = LV_MIN(a->x2, b->x2);
    out->y2 = LV_MIN(a->y2, b->y2);
    return out->x1 <= out->x2 && out->y1 <= out->y2;
}

/* The panel's background, L5S.dark's gradient dithered (l5_dither.c), for every layout. The big card covers most of
   the panel and paints its own background, so with it the panel paints its gradient only around it: one pass over the
   ~1218 x 542 card instead of two (the draw buffers are in PSRAM). The hole is 4 px smaller than the card, so its
   round corners still show the panel. The gradient spans the whole panel, each part clipped. */
static void panel_bg_cb(lv_event_t *e)
{
    lv_obj_t *panel = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t pa, hole;
    lv_obj_get_coords(panel, &pa);
    const l5_grad_key_t *g = l5_grad_key(L5_PANEL_2, L5_PANEL, 255);
    if (H.panel_full || !H.hero_card || lv_obj_has_flag(H.hero_card, LV_OBJ_FLAG_HIDDEN)) {
        l5_grad_draw(layer, g, &pa);
        return;
    }
    lv_obj_get_coords(H.hero_card, &hole);
    lv_area_increase(&hole, -4, -4);
    const lv_area_t clip = layer->_clip_area; /* lv_draw.h: set before adding a task, then put back */
    const lv_area_t parts[4] = {
        {pa.x1, pa.y1, pa.x2, hole.y1 - 1},
        {pa.x1, hole.y2 + 1, pa.x2, pa.y2},
        {pa.x1, hole.y1, hole.x1 - 1, hole.y2},
        {hole.x2 + 1, hole.y1, pa.x2, hole.y2},
    };
    for (int i = 0; i < 4; i++) {
        lv_area_t c;
        if (!clip_to(&c, &clip, &parts[i])) continue;
        layer->_clip_area = c;
        l5_grad_draw(layer, g, &pa);
    }
    layer->_clip_area = clip;
}

static void hero(lv_obj_t *panel)
{
    lv_obj_t *slots = l5_box(panel); /* .slots.one: padding 18, the card fills the rest of the panel */
    lv_obj_set_width(slots, lv_pct(100));
    lv_obj_set_flex_grow(slots, 1);
    lv_obj_set_style_pad_all(slots, 18, 0);
    lv_obj_set_flex_flow(slots, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *card = l5_box(slots); /* .hero: emblem 420 + details, gap 40, padding 26 34 26 26 */
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_pad_top(card, 26, 0);
    lv_obj_set_style_pad_bottom(card, 26, 0);
    lv_obj_set_style_pad_left(card, 26, 0);
    lv_obj_set_style_pad_right(card, 34, 0);
    lv_obj_set_style_pad_column(card, 40, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (g_slots.slot[0].used) {
        hero_used(card);
        H.hero_card = card; /* panel_bg_cb leaves out its area */
    } else {
        hero_empty(card);
    }
}

#if L5_TOUCH_TRACE && L5_HOME_BENCH
/* ---------- what the big card's redraw costs on the device ----------
   A long press on "APPS IN FLASH" redraws the view three times as it is and three times with each part left out;
   l5_display logs every frame ("HOME no glow: tap->visible … render N ms"). The screen shows the variants. */

static const struct {
    const char *what;
    int part;
} BENCH[] = {
    {"HOME as built", 0},  {"HOME no glow", 1},          {"HOME no emblem shadow", 2}, {"HOME no emblem", 3},
    {"HOME no name", 4},   {"HOME panel under card", 5}, {"HOME panel only", 6}, /* l5_display keeps 23 characters */
};
#define BENCH_RUNS 3
static int s_bench = -1;

static void bench_part(int part, bool on)
{
    switch (part) {
    case 1: lv_obj_set_style_shadow_opa(H.hero_card, on ? LV_OPA_TRANSP : 90, 0); break;
    case 2: lv_obj_set_style_shadow_width(H.hero_cover, on ? 0 : 16, 0); break;
    case 3: lv_obj_set_flag(H.hero_cover, LV_OBJ_FLAG_HIDDEN, on); break;
    case 4: lv_obj_set_flag(H.hero_name, LV_OBJ_FLAG_HIDDEN, on); break;
    case 5: H.panel_full = on; break;
    case 6: lv_obj_set_flag(H.hero_card, LV_OBJ_FLAG_HIDDEN, on); break;
    default: break;
    }
}

static void bench_cb(lv_timer_t *t)
{
    const int n = (int)(sizeof(BENCH) / sizeof(BENCH[0])) * BENCH_RUNS;
    if (s_bench > 0) bench_part(BENCH[(s_bench - 1) / BENCH_RUNS].part, false);
    if (!H.hero_card || s_bench >= n || l5_ui_view() != L5_VIEW_HOME) {
        if (s_bench >= 0) ESP_LOGI("l5_home", "bench: done");
        s_bench = -1;
        lv_timer_delete(t);
        if (H.hero_card) lv_obj_invalidate(lv_obj_get_parent(lv_obj_get_parent(H.hero_card)));
        return;
    }
    int v = s_bench / BENCH_RUNS;
    bench_part(BENCH[v].part, true);
    lv_obj_invalidate(lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(H.hero_card)))); /* the view */
    l5_display_mark(BENCH[v].what);
    l5_display_mark_built();
    l5_display_refresh_now();
    s_bench++;
}

static void bench_start_cb(lv_event_t *e)
{
    (void)e;
    if (s_bench >= 0 || !H.hero_card) return;
    ESP_LOGI("l5_home", "bench: %d variants x %d frames, timings in the l5_display lines",
             (int)(sizeof(BENCH) / sizeof(BENCH[0])), BENCH_RUNS);
    s_bench = 0;
    lv_timer_create(bench_cb, 400, NULL);
}
#endif

/* keys of the Tab5 Keyboard (l5_keys.c). One slot: Enter = BOOT, on the empty card INSTALL FROM SD. Several:
   Left / Up and Right / Down make the previous / next installed app the one that boots, Enter = BOOT. */
bool l5_home_key(const l5_key_t *k)
{
    if (g_slots.n == 1) {
        if (k->kind != L5_KEY_ENTER) {
            return false;
        }
        if (g_slots.slot[0].used) {
            hero_boot_cb(NULL);
        } else {
            l5_ui_go(L5_VIEW_SD);
        }
        return true;
    }
    if (k->kind == L5_KEY_ENTER) {
        if (l5_slots_boot_app()) {
            l5_boot_app();
        }
        return true;
    }
    if (k->kind < L5_KEY_LEFT || k->kind > L5_KEY_DOWN) {
        return false;
    }
    int dir = k->kind == L5_KEY_RIGHT || k->kind == L5_KEY_DOWN ? 1 : -1;
    int i = g_slots.boot >= 0 ? g_slots.boot + dir : dir > 0 ? 0 : g_slots.n - 1;
    for (; i >= 0 && i < g_slots.n; i += dir) { /* the empty slots are skipped */
        if (g_slots.slot[i].used) {
            l5_slots_select(i);
            paint_selection();
            break;
        }
    }
    return true;
}

void l5_home_render(lv_obj_t *view)
{
#if L5_TOUCH_TRACE
    int64_t t_build = esp_timer_get_time();
#endif
    lv_obj_clean(view);
    memset(&H, 0, sizeof(H));
    const int n = g_slots.n;
    const l5_slot_t *sel = l5_slots_boot_app();

    /* #hmMain.dark.vw-col.chamfer: left 12, top 8, 1256 x 632 */
    lv_obj_t *panel = l5_box(view);
    lv_obj_add_event_cb(panel, home_deleted_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_style(panel, &L5S.dark, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0); /* panel_bg_cb paints the gradient, dithered */
    lv_obj_add_event_cb(panel, panel_bg_cb, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
    lv_obj_set_pos(panel, 12, L5_PANEL_Y);
    lv_obj_set_size(panel, PANEL_W, PANEL_H);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    l5_chamfer(panel, L5_CHAMFER, L5_PLATE);

    char kb[16], scheme[24], tag[80];
    l5_fmt_kb(kb, sizeof(kb), g_slots.slot_kb);
    upper(scheme, g_slots.scheme, sizeof(scheme));
    snprintf(tag, sizeof(tag), "%d SLOT%s \xC3\x97 %sB \xC2\xB7 %s SCHEME", n, n == 1 ? "" : "S", kb, scheme);
    lv_obj_t *header = lv_obj_get_parent(l5_col_header(panel, "APPS IN FLASH", tag));
    if (n == 1) { /* the card carries the bootscreen line and BOOT itself: no .hm-foot */
        hero(panel);
#if L5_TOUCH_TRACE && L5_HOME_BENCH
        lv_obj_add_flag(header, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(header, bench_start_cb, LV_EVENT_LONG_PRESSED, NULL);
#endif
#if L5_TOUCH_TRACE
        ESP_LOGI("l5_home", "HOME built in %lld ms", (esp_timer_get_time() - t_build) / 1000);
#endif
        return;
    }
    (void)header;

    /* .slots grid: min(n, 4) equal columns */
    static int32_t cols[5], rows[3];
    int ncol = n <= 1 ? 1 : n < 4 ? n : 4;
    int nrow = n <= 0 ? 1 : (n + 3) / 4;
    for (int c = 0; c < ncol; c++) {
        cols[c] = LV_GRID_FR(1);
    }
    cols[ncol] = LV_GRID_TEMPLATE_LAST;
    for (int r = 0; r < nrow && r < 2; r++) {
        rows[r] = LV_GRID_FR(1);
    }
    rows[nrow < 2 ? nrow : 2] = LV_GRID_TEMPLATE_LAST;

    lv_obj_t *grid = l5_box(panel);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_style_pad_all(grid, 18, 0);
    lv_obj_set_style_pad_row(grid, 14, 0);
    lv_obj_set_style_pad_column(grid, 14, 0);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(grid, cols, rows);
    /* LV_GRID_ALIGN_STRETCH is no track alignment: LVGL 9.3 grid_align() leaves the first row's position
       uninitialised with it, and the cards landed anywhere (with one slot usually off the screen). The FR rows
       fill the height, so START puts them in place. */
    lv_obj_set_grid_align(grid, LV_GRID_ALIGN_CENTER, LV_GRID_ALIGN_START);

    if (n == 0) {
        lv_obj_t *e = l5_label(grid, &l5_chakra_r17, L5_ON_PANEL_MUTED,
                               "The partition table has no ota_N app slots.\nFlash partitions.csv from this project.");
        lv_obj_set_style_text_align(e, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_grid_cell(e, LV_GRID_ALIGN_CENTER, 0, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    }
    for (int i = 0; i < n && i < 8; i++) {
        lv_obj_t *card = l5_box(grid);
        lv_obj_set_grid_cell(card, LV_GRID_ALIGN_STRETCH, i % 4, 1, LV_GRID_ALIGN_STRETCH, i / 4, 1);
        if (g_slots.slot[i].used) {
            used_card(card, i);
        } else {
            empty_card(card, i);
        }
    }

    /* .hm-foot */
    lv_obj_t *foot = l5_box(panel);
    lv_obj_set_size(foot, lv_pct(100), 92);
    lv_obj_set_style_pad_left(foot, 24, 0);
    lv_obj_set_style_pad_right(foot, 18, 0);
    lv_obj_set_style_pad_column(foot, 20, 0);
    lv_obj_set_style_border_side(foot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(foot, 1, 0);
    lv_obj_set_style_border_color(foot, L5_PANEL_LINE, 0);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *tx = l5_box(foot);
    lv_obj_set_height(tx, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(tx, 1);
    lv_obj_set_style_pad_row(tx, 4, 0);
    lv_obj_set_flex_flow(tx, LV_FLEX_FLOW_COLUMN);
    char line[96];
    if (sel) {
        snprintf(line, sizeof(line), "APP%d STARTS AFTER THE %us BOOTSCREEN", g_slots.boot, g_cfg.bootTime);
    } else {
        snprintf(line, sizeof(line), "NO APP IN FLASH");
    }
    lv_obj_t *span = l5_label(tx, &l5_mono_13, L5_ON_PANEL_MUTED, line);
    lv_obj_set_style_text_letter_space(span, 2, 0);
    lv_obj_t *b = l5_label(tx, &l5_chakra_b26, L5_ON_PANEL, sel ? sel->name : "Install a firmware first");
    lv_obj_set_width(b, lv_pct(100));
    lv_label_set_long_mode(b, LV_LABEL_LONG_DOT);

    char name[40], btn[48];
    upper(name, sel ? sel->name : "APP", sizeof(name));
    snprintf(btn, sizeof(btn), "BOOT %s", name);
    lv_obj_t *boot = l5_act_button(foot, L5_ICON_LAUNCH, btn, true);
    lv_obj_set_size(boot, 380, 64);
    lv_obj_set_style_text_font(boot, &l5_mono_16, 0);
    lv_obj_add_event_cb(boot, boot_cb, LV_EVENT_CLICKED, NULL);
    if (!sel) {
        lv_obj_add_state(boot, LV_STATE_DISABLED);
    }
    H.line = span;
    H.name = b;
    H.boot = boot;
    paint_selection(); /* .slot.sel and "BOOTS" on the selected card */
}
