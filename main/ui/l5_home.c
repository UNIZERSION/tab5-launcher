/* HOME: only the apps already in flash, one card per slot in a carousel (since 26.10.2026; a grid and a wide card for
   one slot before). A swipe, or a tap on a card at the side, brings the next card to the middle; a tap on the card in
   the middle makes it the app that boots after the bootscreen (BOOTS); BOOT under the carousel starts the card in the
   middle now. No settings here: they live in the rail's tabs. */
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

#define MAX_CARDS L5_MAX_SLOTS
#define CARD_W 440
#define CARD_H 490
#define CARD_GAP 28
#define COVER 300 /* the emblem on a card (l5_cover.c draws its big letters from 300 px) */
#define STRIP_W (L5_FULL_W - 2) /* inside the panel's border */

/* widgets that follow the selection and the card in the middle in place; all NULL while HOME is not built */
static struct {
    lv_obj_t *strip;            /* the carousel: scrolls sideways, snaps a card to the middle */
    lv_obj_t *card[MAX_CARDS];  /* every slot, used or empty */
    lv_obj_t *shade[MAX_CARDS]; /* darkens a card that is not in the middle */
    lv_obj_t *more[MAX_CARDS];  /* the card menu, only on the card in the middle */
    lv_obj_t *dot[MAX_CARDS];
    lv_obj_t *pick;             /* "BOOTS" on the selected card */
    lv_obj_t *line, *name, *boot;
    lv_obj_t *panel;            /* the whole view: the top of its gradient takes the tint of the app in the middle */
    int n, center;
} H;

/* the card in the middle, kept over rebuilds (an install or an erase rebuilds HOME); -1: the boot app */
static int s_center = -1;
static bool s_keep_place; /* l5_home_keep_place(): the next build keeps s_center in the middle */
static lv_style_t s_boots; /* the boot app's card: an accent frame (a glow would be drawn again on every scroll step) */

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
    l5_fmt_kb(kb, sizeof(kb), l5_slots_kb(i));
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

/* ---------- the card in the middle and the selection, in place ---------- */

static void set_label(lv_obj_t *l, const char *t)
{
    if (l && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

/* the footer follows the card in the middle: what it does after the bootscreen, its name, BOOT */
static void paint_foot(void)
{
    const int c = H.center;
    const l5_slot_t *s = c >= 0 && c < H.n ? &g_slots.slot[c] : NULL;
    const bool used = s && s->used;
    char line[96], name[40], btn[48];
    if (!used) {
        snprintf(line, sizeof(line), "APP%d IS EMPTY \xC2\xB7 INSTALL FROM SD OR OTA", c);
    } else if (c == g_slots.boot) {
        snprintf(line, sizeof(line), "APP%d STARTS AFTER THE %us BOOTSCREEN", c, g_cfg.bootTime);
    } else {
        snprintf(line, sizeof(line), "APP%d \xC2\xB7 TAP THE CARD TO START IT AFTER THE BOOTSCREEN", c);
    }
    set_label(H.line, line);
    set_label(H.name, used ? s->name : "Empty slot");
    upper(name, used ? s->name : "APP", sizeof(name));
    snprintf(btn, sizeof(btn), "BOOT %s", name);
    if (H.boot) {
        set_label(lv_obj_get_child(H.boot, -1), btn); /* l5_act_button(): [icon,] text */
        lv_obj_set_state(H.boot, LV_STATE_DISABLED, !used);
    }
}

/* the top of the panel's gradient (L5S.dark: PANEL_2 -> PANEL) with the icon's tint of the app in the middle over it */
static void paint_tint(int c)
{
    lv_color_t tint;
    const l5_slot_t *s = c >= 0 && c < H.n ? &g_slots.slot[c] : NULL;
    if (s && s->used && l5_appicon_colors(s->project, NULL, &tint)) {
        lv_obj_set_style_bg_color(H.panel, l5_mix(tint, L5_PANEL_2, 70), 0); /* 27 %: a glow, the cards stay apart */
    } else {
        lv_obj_remove_local_style_prop(H.panel, LV_STYLE_BG_COLOR, 0);
    }
}

static void paint_center(int c)
{
    H.center = c;
    s_center = c;
    paint_tint(c);
    for (int i = 0; i < H.n; i++) {
        if (H.shade[i]) lv_obj_set_flag(H.shade[i], LV_OBJ_FLAG_HIDDEN, i == c);
        if (H.more[i]) lv_obj_set_flag(H.more[i], LV_OBJ_FLAG_HIDDEN, i != c);
        if (H.dot[i]) {
            lv_obj_set_width(H.dot[i], i == c ? 30 : 10);
            lv_obj_set_style_bg_color(H.dot[i], i == c ? L5P.accent : L5_PANEL_LINE, 0);
        }
    }
    paint_foot();
}

static lv_obj_t *add_pick(lv_obj_t *card) /* .pick "BOOTS" */
{
    lv_obj_t *pick = l5_label(card, &l5_mono_12, L5P.accent_ink, "BOOTS");
    lv_obj_add_style(pick, &L5S.pick, 0);
    lv_obj_align(pick, LV_ALIGN_TOP_RIGHT, -(52 - 1), 10 - 1);
    return pick;
}

/* g_slots.boot -> the accent frame, "BOOTS" and the footer */
static void paint_selection(void)
{
    const l5_slot_t *sel = l5_slots_boot_app();
    for (int i = 0; i < H.n; i++) {
        if (!H.card[i] || !g_slots.slot[i].used) continue;
        lv_obj_remove_style(H.card[i], &s_boots, 0);
        if (sel && i == g_slots.boot) lv_obj_add_style(H.card[i], &s_boots, 0);
    }
    if (H.pick) lv_obj_delete(H.pick);
    H.pick = NULL;
    if (sel && g_slots.boot < H.n && H.card[g_slots.boot]) {
        H.pick = add_pick(H.card[g_slots.boot]);
        lv_obj_move_to_index(H.shade[g_slots.boot], -1); /* the shade stays on top of the badge */
    }
    paint_foot();
    l5_cfg_accent_follow(); /* CFG > UI color APP: another boot app, another accent */
}

static void home_deleted_cb(lv_event_t *e)
{
    (void)e;
    memset(&H, 0, sizeof(H));
}

/* the card nearest to the strip's middle */
static int middle_card(void)
{
    lv_area_t sa;
    lv_obj_get_coords(H.strip, &sa);
    const int32_t mid = (sa.x1 + sa.x2) / 2;
    int best = 0;
    int32_t best_d = INT32_MAX;
    for (int i = 0; i < H.n; i++) {
        lv_area_t a;
        lv_obj_get_coords(H.card[i], &a);
        int32_t d = LV_ABS((a.x1 + a.x2) / 2 - mid);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

static void scroll_end_cb(lv_event_t *e)
{
    (void)e;
    int c = middle_card();
    if (c != H.center) paint_center(c);
}

static void bring(int i, lv_anim_enable_t anim)
{
    if (i < 0 || i >= H.n) return;
    lv_obj_scroll_to_view(H.card[i], anim); /* the strip snaps to the middle: the card lands there */
    if (anim == LV_ANIM_OFF) paint_center(i);
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

/* the card in the middle: it becomes the boot app; a card at the side: it comes to the middle */
static void card_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i != H.center) {
        bring(i, LV_ANIM_ON);
        return;
    }
    if (!g_slots.slot[i].used) return;
    l5_slots_select(i);
    paint_selection();
}

static void dot_cb(lv_event_t *e)
{
    bring((int)(intptr_t)lv_event_get_user_data(e), LV_ANIM_ON);
}

/* BOOT: the app in the middle starts now and becomes the boot app (as "Boot now") */
static void boot_cb(lv_event_t *e)
{
    (void)e;
    if (H.center < 0 || H.center >= H.n || !g_slots.slot[H.center].used) return;
    l5_slots_select(H.center);
    l5_boot_app();
}

static void go_cb(lv_event_t *e)
{
    l5_ui_go((l5_view_t)(intptr_t)lv_event_get_user_data(e));
}

/* ---------- cards ---------- */

static lv_obj_t *slot_label(lv_obj_t *card, int i)
{
    char t[24];
    snprintf(t, sizeof(t), "APP%d \xC2\xB7 OTA_%d", i, i);
    lv_obj_t *l = l5_label(card, &l5_mono_12, L5_ON_PANEL_MUTED, t);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_set_pos(l, 14 - 1, 14 - 1);
    return l;
}

static lv_obj_t *inner_column(lv_obj_t *card)
{
    lv_obj_t *col = l5_box(card);
    lv_obj_set_size(col, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_top(col, 48, 0);
    lv_obj_set_style_pad_hor(col, 22, 0);
    lv_obj_set_style_pad_bottom(col, 20, 0);
    lv_obj_set_style_pad_row(col, 8, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
    return col;
}

static void used_card(lv_obj_t *card, int i)
{
    const l5_slot_t *s = &g_slots.slot[i];
    lv_obj_add_style(card, &L5S.slot, 0); /* the accent frame and "BOOTS": paint_selection() */
    l5_dither_hook(card);
    lv_obj_t *col = inner_column(card);
    char t[96];
    l5_cover_create(col, COVER, s->name, s->name, s->author[0] ? s->author : "ESP32-P4 APP", i, true, s->project);

    lv_obj_t *name = l5_label(col, &l5_chakra_b28, L5_ON_PANEL, s->name);
    lv_obj_set_style_pad_top(name, 6, 0);
    lv_obj_set_style_max_width(name, lv_pct(100), 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);

    /* "project · vVER · SRC · DATE": the project name only when the registry renamed the app; without a registry
       entry (flashed by hand) the build date and FLASH */
    const char *v = s->ver[0] == 'v' || s->ver[0] == 'V' ? "" : "v";
    int n = strcmp(s->name, s->project) ? snprintf(t, sizeof(t), "%s \xC2\xB7 %s%s", s->project, v, s->ver)
                                        : snprintf(t, sizeof(t), "%s%s", v, s->ver);
    if (n > 0 && (size_t)n < sizeof(t)) {
        snprintf(t + n, sizeof(t) - n, " \xC2\xB7 %s \xC2\xB7 %s", s->src[0] ? s->src : "FLASH",
                 s->installed[0] ? s->installed : s->date);
    }
    lv_obj_t *meta = l5w_line_label(col, &l5_mono_14, L5_ON_PANEL_MUTED, t);
    lv_obj_set_style_max_width(meta, lv_pct(100), 0);

    lv_obj_t *grow = l5_box(col); /* the meter at the bottom */
    lv_obj_set_flex_grow(grow, 1);

    lv_obj_t *use = l5_box(col);
    lv_obj_set_size(use, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(use, 6, 0);
    lv_obj_set_flex_flow(use, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *r = l5_box(use);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    uint64_t slot_bytes = (uint64_t)l5_slots_kb(i) * 1024; /* app0 may be bigger than the others */
    int pct = slot_bytes ? (int)((uint64_t)s->size * 100 / slot_bytes) : 0;
    char size[16], kb[16];
    l5_fmt_bytes(size, sizeof(size), s->size);
    l5_fmt_kb(kb, sizeof(kb), l5_slots_kb(i));
    snprintf(t, sizeof(t), "%s OF %sB", size, kb);
    l5_label(r, &l5_mono_13, L5_ON_PANEL_MUTED, t);
    snprintf(t, sizeof(t), "%d%%", pct);
    l5_label(r, &l5_mono_13, L5_ON_PANEL_MUTED, t);
    lv_obj_t *meter = lv_bar_create(use);
    lv_obj_remove_flag(meter, LV_OBJ_FLAG_CLICKABLE); /* a tap on the meter is a tap on the card */
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
    H.more[i] = more;
}

static void empty_card(lv_obj_t *card, int i)
{
    lv_obj_set_style_radius(card, 4, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, L5C.slot_bot, 0);
    l5_dashed_border(card, L5C.edge);
    lv_obj_t *col = inner_column(card);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    l5_icon(col, &l5_icon_64, L5C.dim, L5_ICON_BIN);
    lv_obj_t *b = l5_label(col, &l5_chakra_b28, L5_ON_PANEL, "Empty slot");
    lv_obj_set_style_pad_top(b, 6, 0);
    char kb[16], t[96];
    l5_fmt_kb(kb, sizeof(kb), l5_slots_kb(i));
    snprintf(t, sizeof(t), "%sB free for one firmware.\nInstall it from the SD card or OTA.", kb);
    lv_obj_t *meta = l5_label(col, &l5_mono_14, L5_ON_PANEL_MUTED, t);
    lv_obj_set_style_text_align(meta, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(meta, 4, 0);
    if (!l5_board_sd_ready()) { /* why "from SD" will not work right now; details in the SD tab */
        snprintf(t, sizeof(t), "SD CARD: %s", l5_board_sd_status(false));
        lv_obj_t *sd = l5_label(col, &l5_mono_13, L5_WARN, t);
        lv_obj_set_style_text_letter_space(sd, 1, 0);
    }
    lv_obj_t *row = l5_box(col); /* FROM SD CARD / FROM OTA */
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(row, 14, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    static const struct {
        const char *icon, *text;
        l5_view_t view;
    } GO[] = {{L5_ICON_SD, "FROM SD", L5_VIEW_SD}, {L5_ICON_OTA, "FROM OTA", L5_VIEW_OTA}};
    for (int k = 0; k < 2; k++) {
        lv_obj_t *g = l5_act_button(row, GO[k].icon, GO[k].text, k == 0);
        lv_obj_set_flex_grow(g, 1);
        lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(g, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(g, go_cb, LV_EVENT_CLICKED, (void *)(intptr_t)GO[k].view);
    }
    slot_label(card, i);
}

static lv_obj_t *card(lv_obj_t *strip, int i)
{
    lv_obj_t *c = l5_box(strip);
    lv_obj_set_size(c, CARD_W, CARD_H);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE); /* no pressed ring: every swipe starts with a press on a card */
    lv_obj_add_event_cb(c, card_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    H.card[i] = c;
    if (g_slots.slot[i].used) {
        used_card(c, i);
    } else {
        empty_card(c, i);
    }
    lv_obj_t *sh = l5_box(c); /* last child: over everything on the card */
    lv_obj_add_flag(sh, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(sh, -1, -1); /* over the border too */
    lv_obj_set_size(sh, CARD_W, CARD_H);
    lv_obj_set_style_radius(sh, 4, 0);
    lv_obj_set_style_bg_opa(sh, 150, 0);
    lv_obj_set_style_bg_color(sh, L5_PANEL, 0); /* darker in the dark themes, paler in Daylight */
    H.shade[i] = sh;
    return c;
}

/* keys of the Tab5 Keyboard (l5_keys.c). One slot: Enter = BOOT, on the empty card INSTALL FROM SD. Several:
   Left / Up and Right / Down make the previous / next installed app the one that boots and bring it to the middle,
   Enter = BOOT (the app in the middle) once an app was chosen to boot. */
bool l5_home_key(const l5_key_t *k)
{
    if (g_slots.n == 1) {
        if (k->kind != L5_KEY_ENTER) {
            return false;
        }
        if (g_slots.slot[0].used) {
            l5_slots_select(0);
            l5_boot_app();
        } else {
            l5_ui_go(L5_VIEW_SD);
        }
        return true;
    }
    if (k->kind == L5_KEY_ENTER) {
        if (l5_slots_boot_app()) {
            boot_cb(NULL); /* what the footer's BOOT shows: the app in the middle */
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
            bring(i, LV_ANIM_ON);
            break;
        }
    }
    return true;
}

void l5_home_keep_place(void)
{
    s_keep_place = true;
}

/* the card in the middle when HOME is built: the one before when only icons changed, else the boot app, else the
   one before the rebuild, else the first app */
static int first_center(int n)
{
    const bool keep = s_keep_place;
    s_keep_place = false;
    if (keep && s_center >= 0 && s_center < n) return s_center;
    if (l5_slots_boot_app() && g_slots.boot < n) return g_slots.boot;
    if (s_center >= 0 && s_center < n) return s_center;
    for (int i = 0; i < n; i++) {
        if (g_slots.slot[i].used) return i;
    }
    return 0;
}

void l5_home_render(lv_obj_t *view)
{
#if L5_TOUCH_TRACE
    int64_t t_build = esp_timer_get_time();
#endif
    static bool styled;
    if (!styled) {
        lv_style_init(&s_boots);
        lv_style_set_border_width(&s_boots, 2);
        styled = true;
    }
    lv_style_set_border_color(&s_boots, L5P.accent); /* CFG > UI color can change it */
    lv_obj_clean(view);
    memset(&H, 0, sizeof(H));
    l5_appicon_refresh(); /* icons and about.txt on the card: HOME is drawn again when one changed */
    const int n = g_slots.n < MAX_CARDS ? g_slots.n : MAX_CARDS;
    H.n = n;

    /* #hmMain.dark.vw-col.chamfer: the whole view right of the rail */
    lv_obj_t *panel = l5_box(view);
    H.panel = panel;
    lv_obj_add_event_cb(panel, home_deleted_cb, LV_EVENT_DELETE, NULL);
    l5_panel(panel, true);
    lv_obj_set_pos(panel, L5_GAP, L5_PANEL_Y);
    lv_obj_set_size(panel, L5_FULL_W, L5_PANEL_H);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    l5_chamfer(panel, L5_CHAMFER, L5_PLATE);

    char kb[16], kb0[16], scheme[24], tag[128];
    l5_fmt_kb(kb, sizeof(kb), g_slots.slot_kb);
    l5_fmt_kb(kb0, sizeof(kb0), g_slots.app0_kb);
    upper(scheme, g_slots.scheme, sizeof(scheme));
    if (g_slots.app0_kb) { /* "APP0 7.4MB + 2 x 2.5MB": app0 has its own size */
        snprintf(tag, sizeof(tag), "APP0 %sB + %d \xC3\x97 %sB \xC2\xB7 %s SCHEME%s", kb0, n - 1, kb, scheme,
                 n > 1 ? " \xC2\xB7 SWIPE" : "");
    } else {
        snprintf(tag, sizeof(tag), "%d SLOT%s \xC3\x97 %sB \xC2\xB7 %s SCHEME%s", n, n == 1 ? "" : "S", kb, scheme,
                 n > 1 ? " \xC2\xB7 SWIPE" : "");
    }
    l5_col_header(panel, "APPS IN FLASH", tag);

    if (n == 0) {
        lv_obj_t *e = l5_label(panel, &l5_chakra_r17, L5_ON_PANEL_MUTED,
                               "The partition table has no ota_N app slots.\nFlash partitions.csv from this project.");
        lv_obj_set_style_text_align(e, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(e, lv_pct(100));
        lv_obj_set_style_pad_top(e, 200, 0);
        return;
    }

    /* the carousel: a card is CARD_W wide, the padding lets the first and the last reach the middle */
    lv_obj_t *strip = l5_box(panel);
    H.strip = strip;
    lv_obj_set_width(strip, lv_pct(100));
    lv_obj_set_flex_grow(strip, 1);
    lv_obj_set_style_pad_hor(strip, (STRIP_W - CARD_W) / 2, 0);
    lv_obj_set_style_pad_column(strip, CARD_GAP, 0);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(strip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_ONE);
    lv_obj_set_scroll_dir(strip, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(strip, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(strip, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(strip, scroll_end_cb, LV_EVENT_SCROLL_END, NULL);
    for (int i = 0; i < n; i++) card(strip, i);

    lv_obj_t *dots = l5_box(panel); /* where the card in the middle is; a tap on a dot brings its card */
    lv_obj_set_size(dots, lv_pct(100), 30);
    lv_obj_set_style_pad_column(dots, 8, 0);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < n && n > 1; i++) {
        lv_obj_t *d = l5_box(dots);
        lv_obj_set_size(d, 10, 10);
        lv_obj_set_ext_click_area(d, 10);
        lv_obj_add_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(d, 5, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(d, dot_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        H.dot[i] = d;
    }

    /* .hm-foot: the card in the middle, BOOT */
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
    H.line = l5_label(tx, &l5_mono_13, L5_ON_PANEL_MUTED, "");
    lv_obj_set_style_text_letter_space(H.line, 2, 0);
    H.name = l5_label(tx, &l5_chakra_b26, L5_ON_PANEL, "");
    lv_obj_set_width(H.name, lv_pct(100));
    lv_label_set_long_mode(H.name, LV_LABEL_LONG_DOT);
    H.boot = l5_act_button(foot, L5_ICON_LAUNCH, "BOOT", true);
    lv_obj_set_size(H.boot, 380, 64);
    lv_obj_set_style_text_font(H.boot, &l5_mono_16, 0);
    lv_obj_set_flex_flow(H.boot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(H.boot, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_height(lv_obj_get_child(H.boot, -1), lv_font_get_line_height(&l5_mono_16)); /* one line, cut with dots */
    lv_obj_add_event_cb(H.boot, boot_cb, LV_EVENT_CLICKED, NULL);

    H.center = -1;
    paint_selection();
    lv_obj_update_layout(strip);
    bring(first_center(n), LV_ANIM_OFF);
#if L5_TOUCH_TRACE
    ESP_LOGI("l5_home", "HOME built in %lld ms", (esp_timer_get_time() - t_build) / 1000);
#endif
}
