/* Snapshots of the SD manager and HOME rendered on the PC with the firmware's own LVGL configuration, fonts,
   theme, screen (l5_ui.c: views, top bar, layers, veil), views and dialogs: layout of the side panel and the actions,
   Polish letters in names, paths, the clipboard line and the text viewer; HOME with the device's slot (Custom, one
   11264 KB slot, Aurora from SD), an empty slot and two slots. tools/host_test/ui_shots.py builds and
   runs it: shots <output folder>; the card is the folder CONFIG_BSP_SD_MOUNT_POINT.
   shots --idle: dim, screen off and wake with l5_ui.c's own idle timer (run.py runs it).
   shots --taps: what a tap at the edges of the top bar's keys and buttons hits, and HOME's redraw bench (a long press
   on its header) putting every part back (run.py runs it). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "lvgl.h"
#include "l5_board.h"
#include "l5_config.h"
#include "l5_files.h"
#include "l5_sd.h"
#include "l5_slots.h"
#include "l5_ui.h"
#include "l5_wifi.h"

extern int shots_backlight; /* ui_stubs.c: last l5_board_backlight() */
extern bool shots_sd_ready;
extern int shots_wifi_state;
extern int shots_battery, shots_charging;
extern uint64_t l5_files_host_total, l5_files_host_free; /* l5_files.c on the PC */

#define W 1280
#define H 720
#define CARD CONFIG_BSP_SD_MOUNT_POINT

static uint32_t s_tick;
static uint16_t *s_fb;
static const char *s_out;
static int s_shots;

static uint32_t tick_cb(void)
{
    return s_tick;
}

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a, (void)px; /* direct mode: LVGL drew into s_fb */
    lv_display_flush_ready(d);
}

static void settle(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 10) {
        s_tick += 10;
        lv_timer_handler();
    }
}

static void shot(const char *name)
{
    settle(300); /* toast fade-in, layout */
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_refr_now(NULL);
    char p[400];
    snprintf(p, sizeof(p), "%s/%s.ppm", s_out, name);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    uint8_t *row = malloc(W * 3);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t c = s_fb[y * W + x];
            row[x * 3] = (uint8_t)((c >> 11) * 255 / 31);
            row[x * 3 + 1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
            row[x * 3 + 2] = (uint8_t)((c & 31) * 255 / 31);
        }
        fwrite(row, 1, W * 3, f);
    }
    free(row);
    fclose(f);
    s_shots++;
    printf("shot %s\n", name);
}

static void put(const char *rel, const char *text)
{
    char p[400];
    snprintf(p, sizeof(p), "%s/%s", CARD, rel);
    FILE *f = fopen(p, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void put_size(const char *rel, size_t n)
{
    char p[400];
    snprintf(p, sizeof(p), "%s/%s", CARD, rel);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    char *b = calloc(1, n ? n : 1);
    b[0] = (char)0xE9;
    fwrite(b, 1, n, f);
    free(b);
    fclose(f);
}

static void dir(const char *rel)
{
    char p[400];
    snprintf(p, sizeof(p), "%s/%s", CARD, rel);
    mkdir(p, 0777);
}

/* folder and file names avoid the Polish capitals whose UTF-8 bytes the Windows ANSI code page drops */
#define PHOTOS "Zdj\xC4\x99" "cia \xC5\x9Al\xC4\x85sk"
#define NOTES "notatki-\xC4\x85\xC4\x99\xC5\x9B\xC4\x87\xC5\xBC\xC5\xBA\xC5\x84\xC3\xB3\xC5\x82.txt"
#define ZOLC "\xC5\xBB\xC3\xB3\xC5\x82\xC4\x87 g\xC4\x99\xC5\x9Bl\xC4\x85 ja\xC5\xBA\xC5\x84.txt"

static void make_card(void)
{
    mkdir(CARD, 0777);
    dir(PHOTOS);
    dir(PHOTOS "/2024");
    dir("bins");
    dir("logs");
    dir("duzy-folder");
    put_size("Aurora-Player-1.3.21.bin", 5729872);
    put_size("bins/Aurora-Player-1.3.21.bin", 5729872);
    put_size(PHOTOS "/IMG_0001.jpg", 2400000);
    put_size(PHOTOS "/2024/IMG_0002.jpg", 2100000);
    put("config.conf", "{\n  \"bright\": 70,\n  \"dim\": 30,\n  \"color\": \"red\",\n  \"onlyBins\": false,\n"
                       "  \"favorite\": [{\"name\": \"Aurora Player\", \"fid\": \"\", \"link\": \"\"}]\n}\n");
    put("README.md", "# Karta Tab5\n\nPliki .bin instaluje zak\xC5\x82" "adka SD.\n");
    put(NOTES, "Lista zakup\xC3\xB3w:\n- mleko\n- \xC5\xBC\xC3\xB3\xC5\x82ty ser\n");
    put(PHOTOS "/" ZOLC,
        "\xC5\xBB\xC3\x93\xC5\x81\xC4\x86 G\xC4\x98\xC5\x9AL\xC4\x84 JA\xC5\xB9\xC5\x83 \xE2\x80\x94 pierwsza linia z wielkimi literami\n"
        "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87 g\xC4\x99\xC5\x9Bl\xC4\x85 ja\xC5\xBA\xC5\x84. Pchn\xC4\x85\xC4\x87 w t\xC4\x99 \xC5\x82\xC3\xB3" "d\xC5\xBA je\xC5\xBC" "a lub o\xC5\x9Bm skrzy\xC5\x84 fig.\n"
        "\tWci\xC4\x99" "cie tabulatorem, potem tekst.\n"
        "\x1b[0;32mI (1234) l5_sd: kolory terminala znikaj\xC4\x85\x1b[0m\n"
        "Bardzo d\xC5\x82uga linia: Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor "
        "incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco "
        "laboris nisi ut aliquip ex ea commodo consequat. Zako\xC5\x84" "czenie: \xC4\x84\xC4\x86\xC4\x98\xC5\x81\xC5\x83\xC3\x93\xC5\x9A\xC5\xB9\xC5\xBB.\n"
        "{\"json\": [1, 2, 3], \"\xC5\xBC\xC3\xB3\xC5\x82w\": true}\n");
    put("logs/tab5-ota.log", "\x1b[0;32mI (16000) l5_net: GET m5burner-cdn.m5stack.com: HTTP 206\x1b[0m\n");
    char name[64], body[200000];
    for (int i = 0; i < 230; i++) {
        snprintf(name, sizeof(name), "duzy-folder/plik-%03d.bin", i);
        put_size(name, 1000 + i * 37);
    }
    size_t n = 0;
    for (int i = 0; n < 100000; i++) {
        n += (size_t)snprintf(body + n, sizeof(body) - n, "%05d zak\xC5\x82" "adka SD \xE2\x80\x94 linia testowa ze znakami \xC4\x85\xC4\x99\xC5\x9B\n", i);
    }
    put("long.txt", body);
}

/* a key of the on-screen keyboard by its label, as if tapped */
static void key(const char *label)
{
    lv_obj_t *stack[64];
    int sp = 0;
    stack[sp++] = l5_ui_layer();
    while (sp) {
        lv_obj_t *o = stack[--sp];
        if (lv_obj_check_type(o, &lv_button_class) && lv_obj_get_child_count(o) &&
            lv_obj_check_type(lv_obj_get_child(o, 0), &lv_label_class) &&
            !strcmp(lv_label_get_text(lv_obj_get_child(o, 0)), label)) {
            lv_obj_send_event(o, LV_EVENT_CLICKED, NULL);
            return;
        }
        for (uint32_t i = 0; i < lv_obj_get_child_count(o) && sp < 64; i++) stack[sp++] = lv_obj_get_child(o, i);
    }
    printf("key %s not found\n", label);
}

static void close_all(void)
{
    l5_dialogs_close_all();
    lv_obj_clean(l5_ui_toasts());
    settle(50);
}

/* ---------- HOME ---------- */

static void slot_set(int i, const char *name, const char *project, const char *ver, const char *src,
                     const char *installed, const char *date, uint32_t size)
{
    l5_slot_t *s = &g_slots.slot[i];
    memset(s, 0, sizeof(*s));
    if (!name) return;
    s->used = true;
    strlcpy(s->name, name, sizeof(s->name));
    strlcpy(s->project, project, sizeof(s->project));
    strlcpy(s->ver, ver, sizeof(s->ver));
    strlcpy(s->src, src, sizeof(s->src));
    strlcpy(s->installed, installed, sizeof(s->installed));
    strlcpy(s->date, date, sizeof(s->date));
    s->size = size;
}

static void home_shots(void)
{
    /* the device on 19.10.2026: AUTO PARTITION for TAB5 Demo left a Custom table with one 11264 KB slot, then
       Aurora 1.3.21 went into app0 from the SD card (registry name = file name without .bin) */
    g_slots.n = 1;
    g_slots.slot_kb = 11264;
    g_slots.scheme = "Custom";
    g_slots.boot = 0;
    slot_set(0, "Aurora-Player-1.3.21-Application", "tab5_player", "1.3.21", "SD", "2026-10-19", "Oct 18 2026", 6344736);
    l5_ui_go(L5_VIEW_HOME);
    shot("20_home_device");
    /* a preview only, not in the firmware: the glow replaced by a 2 px accent outline (cheaper if the glow turns out
       to cost much on the device) */
    lv_obj_t *card = lv_obj_get_child(lv_obj_get_child(lv_obj_get_child(lv_obj_get_child(lv_screen_active(), L5_VIEW_HOME), 0), -1), 0);
    lv_obj_remove_style(card, &L5S.slot_sel, 0);
    lv_obj_set_style_border_color(card, L5P.accent, 0);
    lv_obj_set_style_outline_width(card, 2, 0);
    lv_obj_set_style_outline_color(card, L5P.accent, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    shot("20b_home_outline_preview");
    l5_ui_go(L5_VIEW_HOME); /* rebuilt as the firmware draws it */
    slot_set(0, NULL, NULL, NULL, NULL, NULL, NULL, 0);
    l5_ui_go(L5_VIEW_HOME); /* the active tab again: rebuilt */
    shot("21_home_empty");
    g_slots.n = 2; /* 2 Apps: 2 x 4096 KB */
    g_slots.slot_kb = 4096;
    g_slots.scheme = "2 Apps";
    slot_set(0, "Sensor-Hub-3.0.1", "sensor_hub", "3.0.1", "SD", "2026-10-09", "Oct 2 2026", 2726297);
    slot_set(1, "Pixel Paint", "pixel_paint", "1.2.0", "OTA", "2026-10-18", "Sep 30 2026", 1992294);
    l5_ui_go(L5_VIEW_HOME);
    shot("22_home_two_slots");
    /* one slot, the longest name the registry keeps, from OTA with a catalog author, no install date */
    g_slots.n = 1;
    g_slots.slot_kb = 8192;
    g_slots.scheme = "Default";
    slot_set(0, "M5Stack-UserDemo-Tab5-Factory-Firmware-2026-10", "tab5_user_demo", "v2.5.3", "OTA", "", "Sep 30 2026",
             3355443);
    strlcpy(g_slots.slot[0].author, "m5stack", sizeof(g_slots.slot[0].author));
    l5_ui_go(L5_VIEW_HOME);
    shot("23_home_long_name");
    slot_set(0, NULL, NULL, NULL, NULL, NULL, NULL, 0);
    shots_sd_ready = false;
    l5_ui_go(L5_VIEW_HOME);
    shot("24_home_empty_no_card");
    shots_sd_ready = true;

    /* the other tabs with the device's slot (Custom, 11264 KB, Aurora) */
    g_slots.n = 1;
    g_slots.slot_kb = 11264;
    g_slots.scheme = "Custom";
    g_slots.boot = 0;
    slot_set(0, "Aurora-Player-1.3.21-Application", "tab5_player", "1.3.21", "SD", "2026-10-19", "Oct 18 2026", 6344736);
    l5_ui_go(L5_VIEW_CFG);
    shot("30_cfg");
    l5_ui_go(L5_VIEW_PMAN);
    shot("31_pman");
    shots_wifi_state = L5_WIFI_OFF;
    l5_topbar_paint_wifi();
    l5_ui_go(L5_VIEW_OTA);
    shot("32_ota_no_wifi");
    shots_wifi_state = L5_WIFI_CONNECTED;
    l5_topbar_paint_wifi();
    l5_ui_go(L5_VIEW_WUI);
    shot("33_wui");

    /* Charge Mode: the real charge, the state line, the pulsing tip while charging */
    static const struct {
        int pct, chg;
        const char *name;
    } CH[] = {{78, L5_CHARGE_CHARGING, "40_charge_charging"},
              {15, L5_CHARGE_ON_BATTERY, "41_charge_low_on_battery"},
              {6, L5_CHARGE_CHARGING, "42_charge_critical_charging"},
              {-1, L5_CHARGE_UNKNOWN, "43_charge_usb_only"},
              {99, L5_CHARGE_EXTERNAL, "44_charge_full"},
              {60, L5_CHARGE_EXTERNAL, "45_charge_usb_not_charging"}};
    for (size_t i = 0; i < sizeof(CH) / sizeof(CH[0]); i++) {
        shots_battery = CH[i].pct;
        shots_charging = CH[i].chg;
        l5_charge_mode();
        settle(500); /* the tip's pulse near its top (0.8 s of 1.6 s) */
        shot(CH[i].name);
    }
    l5_sys_hide();
    l5_ui_set_booted(true);
    shots_battery = 76;
    shots_charging = L5_CHARGE_CHARGING;
    l5_ui_go(L5_VIEW_SD);
}

/* ---------- idle: dim, screen off and wake, timed by l5_ui.c's idle timer ---------- */

static int s_fails;
#define EXPECT(cond, ...)                              \
    do {                                               \
        if (!(cond)) {                                 \
            s_fails++;                                 \
            printf("  FAIL line %d: ", __LINE__);      \
            printf(__VA_ARGS__);                       \
            printf("\n");                              \
        }                                              \
    } while (0)

/* ms without touch; *dimmed: a dimmed backlight was seen on the way */
static void idle_for(uint32_t ms, bool *dimmed)
{
    for (uint32_t t = 0; t < ms; t += 10) {
        s_tick += 10;
        lv_timer_handler();
        if (dimmed && shots_backlight > 0 && shots_backlight < g_cfg.bright) *dimmed = true;
    }
}

static int idle_test(void)
{
    const int dim_level = 70 * 12 / 100 + 1;
    g_cfg.bright = 70;
    g_cfg.dim = 30;
    g_cfg.screenOff = 60;
    shots_backlight = 70;
    l5_ui_set_booted(true);
    l5_ui_wake();
    idle_for(29500, NULL);
    EXPECT(shots_backlight == 70, "29.5 s: bright, backlight %d", shots_backlight);
    idle_for(2000, NULL);
    EXPECT(shots_backlight == dim_level, "31.5 s: dimmed, backlight %d", shots_backlight);
    idle_for(30000, NULL);
    EXPECT(shots_backlight == 0 && !lv_display_is_invalidation_enabled(NULL), "61.5 s: dark and not drawing, backlight %d",
           shots_backlight);
    l5_ui_wake(); /* what the veil does with the first tap */
    EXPECT(shots_backlight == 70 && lv_display_is_invalidation_enabled(NULL), "a tap wakes, backlight %d", shots_backlight);

    bool dimmed = false;
    g_cfg.dim = 0; /* Dim time OFF: dark at 60 s without dimming first */
    idle_for(61500, &dimmed);
    EXPECT(shots_backlight == 0 && !dimmed, "Dim time OFF: backlight %d, dimmed on the way %d", shots_backlight, dimmed);
    l5_ui_wake();

    g_cfg.dim = 30;
    g_cfg.screenOff = 0; /* Screen off OFF: dimmed, never dark */
    idle_for(400000, NULL);
    EXPECT(shots_backlight == dim_level && lv_display_is_invalidation_enabled(NULL), "Screen off OFF: backlight %d",
           shots_backlight);
    l5_ui_wake();

    g_cfg.dim = 300;
    g_cfg.screenOff = 60; /* screen off before dim */
    idle_for(61500, NULL);
    EXPECT(shots_backlight == 0, "dim 300 s, off 60 s: dark at 60 s, backlight %d", shots_backlight);
    l5_info("TEST", "Install finished", "A dialog that opens on its own wakes the screen.");
    EXPECT(shots_backlight == 70 && lv_display_is_invalidation_enabled(NULL), "a dialog wakes, backlight %d", shots_backlight);
    l5_dialogs_close_all();

    idle_for(61500, NULL);
    EXPECT(shots_backlight == 0, "dark again, backlight %d", shots_backlight);
    l5_ui_set_booted(false); /* a system screen: bootscreen, restart, power off, charge mode */
    EXPECT(shots_backlight == 70 && lv_display_is_invalidation_enabled(NULL), "a system screen wakes, backlight %d",
           shots_backlight);
    idle_for(61500, NULL);
    EXPECT(shots_backlight == 70, "outside the launcher: no screen off, backlight %d", shots_backlight);
    l5_ui_set_booted(true);
    for (int i = 0; i < 3; i++) {
        idle_for(61500, NULL);
        l5_ui_wake();
    }
    EXPECT(lv_display_is_invalidation_enabled(NULL), "invalidation on after three dark periods");
    printf(s_fails ? "idle: FAILED %d\n" : "idle: dim, screen off, wake by tap / dialog / system screen PASSED\n", s_fails);
    return s_fails ? 1 : 0;
}

/* ---------- taps on the top bar: which key or button LVGL finds under a point ---------- */

/* "HOME".."CFG" for a tab key (icon label, text label[, divider]), "WIFI" / "POWER", "-" for nothing clickable
   (the screen itself is clickable in LVGL and has no handler) */
static const char *hit(int32_t x, int32_t y)
{
    lv_point_t p = {x, y};
    lv_obj_t *o = lv_indev_search_obj(lv_screen_active(), &p);
    if (!o || o == lv_screen_active()) return "-";
    int labels = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
        if (!lv_obj_check_type(c, &lv_label_class)) continue;
        const char *t = lv_label_get_text(c);
        if (!strcmp(t, L5_ICON_WIFI)) return "WIFI";
        if (!strcmp(t, L5_ICON_POWER)) return "POWER";
        if (++labels == 2) return t;
    }
    return "other";
}

#define EXPECT_HIT(x, y, name) EXPECT(!strcmp(hit((x), (y)), (name)), "tap %d,%d: %s, expected %s", (int)(x), (int)(y), \
                                      hit((x), (y)), (name))

static int taps_test(void)
{
    static const char *const TABS[] = {"HOME", "SD", "OTA", "WUI", "PMAN", "CFG"};
    lv_area_t key[6];
    for (int i = 0; i < 6; i++) { /* the keys' places: search at their middle */
        lv_point_t p = {(int32_t)(435 + 38 + i * 79.5), 34};
        lv_obj_t *o = lv_indev_search_obj(lv_screen_active(), &p);
        if (!o) {
            printf("  FAIL: no key under %d,%d\n", (int)p.x, (int)p.y);
            return 1;
        }
        lv_obj_get_coords(o, &key[i]);
    }
    EXPECT(lv_area_get_height(&key[0]) == 47, "keys are %d px high", (int)lv_area_get_height(&key[0]));
    for (int i = 0; i < 6; i++) {
        const lv_area_t *a = &key[i];
        int32_t cx = (a->x1 + a->x2) / 2, cy = 34;
        EXPECT_HIT(cx, cy, TABS[i]);
        EXPECT_HIT(a->x1, cy, TABS[i]);
        EXPECT_HIT(a->x2, cy, TABS[i]);
        EXPECT_HIT(cx, 0, TABS[i]);                 /* the top of the bar */
        EXPECT_HIT(cx, L5_TOPBAR_H - 4, TABS[i]);   /* just above the seam */
        EXPECT_HIT(cx, L5_TOPBAR_H - 3, "-");       /* the seam */
        if (i < 5) {                                /* the 3 px gap: 2 px left key, 1 px right key */
            EXPECT_HIT(a->x2 + 1, cy, TABS[i]);
            EXPECT_HIT(a->x2 + 2, cy, TABS[i]);
            EXPECT_HIT(a->x2 + 3, cy, TABS[i + 1]);
            EXPECT_HIT(a->x2 + 1, 2, TABS[i]);      /* in the gap, high up */
        }
    }
    EXPECT_HIT(key[0].x1 - 6, 34, "HOME");
    EXPECT_HIT(key[0].x1 - 7, 34, "-");
    EXPECT_HIT(key[5].x2 + 6, 34, "CFG");
    EXPECT_HIT(key[5].x2 + 7, 34, "-");

    /* Wi-Fi and power: 52 px, 6 px more around them */
    static const char *const BTN[] = {"WIFI", "POWER"};
    for (int b = 0; b < 2; b++) {
        lv_area_t a = {0};
        for (int32_t x = 924; x < L5_SCREEN_W && !a.x2; x++) { /* first column that hits the button */
            if (!strcmp(hit(x, 34), BTN[b])) {
                lv_point_t p = {x + 10, 34};
                lv_obj_get_coords(lv_indev_search_obj(lv_screen_active(), &p), &a);
            }
        }
        EXPECT(lv_area_get_width(&a) == 52 && lv_area_get_height(&a) == 52, "%s is %dx%d px", BTN[b],
               (int)lv_area_get_width(&a), (int)lv_area_get_height(&a));
        int32_t cy = (a.y1 + a.y2) / 2, cx = (a.x1 + a.x2) / 2;
        EXPECT_HIT(a.x1 - 6, cy, BTN[b]);
        EXPECT_HIT(a.x2 + 6, cy, BTN[b]);
        EXPECT_HIT(cx, a.y1 - 6, BTN[b]);
        EXPECT_HIT(cx, a.y2 + 6, BTN[b]);
        EXPECT(strcmp(hit(a.x1 - 7, cy), BTN[b]), "%s takes a tap 7 px left of it", BTN[b]);
        EXPECT(strcmp(hit(a.x2 + 7, cy), BTN[b]), "%s takes a tap 7 px right of it", BTN[b]);
    }
    printf(s_fails ? "taps: FAILED %d\n" : "taps: keys, gaps, bar height, Wi-Fi and power PASSED\n", s_fails);
    return s_fails ? 1 : 0;
}

/* Charge Mode's state line from the current readings (l5_board.h l5_charge_next) */
static int charge_state_test(void)
{
    static const struct {
        int ma;
        l5_charge_t want;
        const char *why;
    } R[] = {
        {-1, L5_CHARGE_EXTERNAL, "first reading on USB with a full pack (device 19.10.2026): at once"},
        {485, L5_CHARGE_CHARGING, "charging current (Aurora's log): beyond 100 mA, at once"},
        {15, L5_CHARGE_CHARGING, "end of charge, first reading: waits"},
        {15, L5_CHARGE_EXTERNAL, "second reading agrees"},
        {25, L5_CHARGE_EXTERNAL, "21..25 mA once: waits"},
        {-5, L5_CHARGE_EXTERNAL, "back in the band: the wait ends"},
        {25, L5_CHARGE_EXTERNAL, "25 mA again: waits"},
        {25, L5_CHARGE_CHARGING, "twice in a row: charging"},
        {21, L5_CHARGE_CHARGING, "over the threshold: stays"},
        {19, L5_CHARGE_CHARGING, "under it once: waits"},
        {21, L5_CHARGE_CHARGING, "over again: no change"},
        {19, L5_CHARGE_CHARGING, "a reading jumping at the threshold never flips"},
        {-250, L5_CHARGE_ON_BATTERY, "USB pulled out: at once"},
        {-15, L5_CHARGE_ON_BATTERY, "light load once: waits"},
        {-30, L5_CHARGE_ON_BATTERY, "on battery again"},
        {-15, L5_CHARGE_ON_BATTERY, "light load once more: waits"},
        {-12, L5_CHARGE_EXTERNAL, "twice in a row: external power"},
        {150, L5_CHARGE_CHARGING, "USB in: at once"},
    };
    l5_charge_t state = L5_CHARGE_UNKNOWN, pending = L5_CHARGE_UNKNOWN;
    int fails = 0;
    for (size_t i = 0; i < sizeof(R) / sizeof(R[0]); i++) {
        state = l5_charge_next(state, &pending, R[i].ma);
        if (state != R[i].want) {
            fails++;
            printf("  FAIL reading %u (%d mA, %s): state %d, expected %d\n", (unsigned)i, R[i].ma, R[i].why, (int)state,
                   (int)R[i].want);
        }
    }
    printf(fails ? "charge: FAILED %d\n" : "charge: state line from the current (20 mA, two readings, 100 mA) PASSED\n",
           fails);
    return fails ? 1 : 0;
}

/* HOME's bench (l5_home.c, long press on "APPS IN FLASH") leaves the view as it found it */
static int bench_restore_test(void)
{
    g_slots.n = 1;
    g_slots.slot_kb = 11264;
    g_slots.scheme = "Custom";
    g_slots.boot = 0;
    slot_set(0, "Aurora-Player-1.3.21-Application", "tab5_player", "1.3.21", "SD", "2026-10-19", "Oct 18 2026", 6344736);
    l5_ui_go(L5_VIEW_HOME); /* the active tab again: rebuilt with the slot */
    settle(300);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    const size_t row = W * 2, skip = L5_TOPBAR_H * row; /* the bar has the clock */
    uint8_t *before = malloc(W * H * 2);
    memcpy(before, s_fb, W * H * 2);
    lv_obj_t *header = lv_obj_get_child(lv_obj_get_child(lv_obj_get_child(lv_screen_active(), L5_VIEW_HOME), 0), 0);
    lv_obj_send_event(header, LV_EVENT_LONG_PRESSED, NULL);
    settle(12000); /* 7 variants x 3 frames, 400 ms apart */
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    int fails = memcmp(before + skip, (uint8_t *)s_fb + skip, W * H * 2 - skip) != 0;
    if (fails) printf("  FAIL: HOME looks different after its bench\n");
    free(before);
    printf(fails ? "bench: FAILED\n" : "bench: HOME's redraw bench runs and puts every part back PASSED\n");
    return fails;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    bool idle = !strcmp(argv[1], "--idle"), taps = !strcmp(argv[1], "--taps");
    s_out = argv[1];
    l5_files_root = CARD;
    l5_files_host_total = 29ull << 30;
    l5_files_host_free = 21ull << 30;
    if (!idle && !taps) make_card();

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *d = lv_display_create(W, H);
    s_fb = malloc(W * H * 2);
    lv_display_set_buffers(d, s_fb, NULL, W * H * 2, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, flush_cb);

    l5_theme_init(0xe3171f);
    l5_ui_init(); /* the launcher's screen: views, top bar, layers, veil, idle timer */
    if (idle) return idle_test();
    if (taps) {
        l5_ui_go(L5_VIEW_HOME);
        lv_obj_update_layout(lv_screen_active());
        int r = taps_test();
        r = charge_state_test() || r;
        return bench_restore_test() || r;
    }
    home_shots();
    l5_ui_go(L5_VIEW_SD);

    l5_sd_after_op(CARD, NULL);
    shot("01_root");
    l5_sd_after_op(CARD, "Aurora-Player-1.3.21.bin");
    shot("02_bin_selected");
    l5_sd_after_op(CARD, NOTES);
    shot("03_text_selected");
    l5_sd_action("copy");
    shot("04_copied_clipboard");
    l5_sd_after_op(CARD "/" PHOTOS, NULL);
    shot("05_folder_open_current");
    l5_sd_action("paste");
    shot("06_pasted");
    l5_sd_after_op(CARD "/" PHOTOS, ZOLC);
    l5_sd_action("view");
    shot("07_view_polish");
    close_all();
    l5_sd_after_op(CARD, "long.txt");
    l5_sd_action("view");
    shot("08_view_long_cut");
    close_all();
    l5_sd_after_op(CARD, PHOTOS);
    l5_sd_action("delete");
    shot("09_delete_folder_confirm");
    close_all();
    l5_sd_after_op(CARD, "config.conf");
    l5_sd_action("delete");
    shot("10_delete_config_confirm");
    close_all();
    l5_sd_after_op(CARD, NOTES);
    l5_sd_action("rename");
    shot("11_rename_keyboard");
    key("CLEAR");
    key("a");
    key(":");
    key("b");
    key("OK");
    shot("12_rename_refused");
    close_all();
    l5_sd_action("mkdir");
    shot("13_new_folder_keyboard");
    key("OK");
    shot("14_new_folder_created");
    close_all();
    l5_sd_after_op(CARD "/" PHOTOS, NULL);
    l5_sd_action("paste"); /* a second copy: name-copy2 */
    l5_sd_after_op(CARD "/" PHOTOS, NULL);
    l5_sd_action("copy"); /* nothing selected: COPY does nothing */
    l5_sd_after_op(CARD, PHOTOS);
    l5_sd_action("copy");
    l5_sd_after_op(CARD "/" PHOTOS "/2024", NULL);
    l5_sd_action("paste"); /* a folder into itself */
    shot("15_paste_into_itself");
    close_all();
    l5_sd_after_op(CARD "/duzy-folder", "plik-199.bin");
    shot("16_big_folder");
    printf("%d shots\n", s_shots);
    return 0;
}
