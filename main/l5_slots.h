/* Flash app slots (app0..appN-1 = ota_0..ota_N-1) and the hand-over to an installed app.
   Mirrors S.slots / S.boot / L5.table / L5.bootApp / L5.eraseSlot / L5.install in tab5-launcher/app.js,
   but reads the real partition table instead of a scheme description. */
#pragma once
#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_partition.h"

#define L5_MAX_SLOTS 8
#define L5_NAME_LEN 49   /* display name buffer (UTF-8, cut on a character boundary) */

typedef struct {
    const esp_partition_t *part;
    bool used;          /* a valid ESP32-P4 app image header was found */
    char name[L5_NAME_LEN]; /* display name: NVS registry (install file name), else project */
    char project[33];   /* esp_app_desc_t.project_name */
    char ver[33];       /* esp_app_desc_t.version */
    char date[17];      /* esp_app_desc_t.date (build date) */
    char src[8];        /* where it was installed from ("SD"), "" when flashed by hand */
    char installed[11]; /* install date YYYY-MM-DD, "" when unknown */
    uint8_t elf_sha[32]; /* esp_app_desc_t.app_elf_sha256: ties the registry entry to the image */
    uint32_t size;      /* image length in bytes */
    char author[33];    /* catalog author (OTA), "" otherwise */
    char fid[33];       /* launcherhub firmware id (OTA), "" otherwise */
    char cver[24];      /* catalog version ("v2.5.3"), "" otherwise */
} l5_slot_t;

typedef struct {
    int n;                 /* number of app slots in the partition table */
    uint32_t slot_kb;      /* size of the slots after app0 (all of them when app0_kb is 0) */
    uint32_t app0_kb;      /* app0's own size when it differs ("1 Big + 2 Small"), else 0 */
    uint32_t spiffs_kb;
    uint32_t fat_kb;
    const char *scheme;    /* scheme label matched against data.js, "Custom" otherwise */
    l5_slot_t slot[L5_MAX_SLOTS];
    int boot;              /* S.boot: slot started after the bootscreen, -1 = none */
} l5_slots_t;

extern l5_slots_t g_slots;
/* size of app slot `index` in KB, 0 for no such slot (from slot_kb / app0_kb, which l5_slots_scan reads) */
static inline uint32_t l5_slots_kb(int index)
{
    if (index < 0 || index >= g_slots.n) return 0;
    return index == 0 && g_slots.app0_kb ? g_slots.app0_kb : g_slots.slot_kb;
}
/* the largest app slot: whether an app fits anywhere */
static inline uint32_t l5_slots_max_kb(void)
{
    return g_slots.app0_kb > g_slots.slot_kb ? g_slots.app0_kb : g_slots.slot_kb;
}
/* "1 x 8M", "2 x 4M", "app0 7.4M + 2 x 2.5M" with a multiplication sign (the l5_fmt_kb sizes) */
static inline void l5_slots_text(char *out, size_t len)
{
    char a[16], b[16];
    const uint32_t kb[2] = {g_slots.app0_kb, g_slots.slot_kb};
    char *t[2] = {a, b};
    for (int i = 0; i < 2; i++) {
        if (kb[i] >= 1024 && kb[i] % 1024) snprintf(t[i], 16, "%.1fM", kb[i] / 1024.0);
        else if (kb[i] >= 1024) snprintf(t[i], 16, "%uM", (unsigned)(kb[i] / 1024));
        else snprintf(t[i], 16, "%uK", (unsigned)kb[i]);
    }
    if (g_slots.app0_kb) snprintf(out, len, "app0 %s + %d \xC3\x97 %s", a, g_slots.n - 1, b);
    else snprintf(out, len, "%d \xC3\x97 %s", g_slots.n, b);
}

void l5_slots_scan(void);
int l5_slots_used(void);

const l5_slot_t *l5_slots_boot_app(void);   /* S.installed, NULL when nothing is in flash */
void l5_slots_select(int index);            /* persists S.boot in NVS */
/* "Aurora-Player-1.3.21 in app0, Bruce in app1": the installed apps, for erase confirmations.
   Returns how many there are. */
int l5_slots_list(char *out, size_t len);

/* Verify the image in `index`, select its ota_N in otadata and restart into it.
   deep_sleep_starts_launcher is cfg.deepSleepStart, read by the bootloader override.
   Returns only on failure. */
esp_err_t l5_slots_handoff(int index, bool deep_sleep_starts_launcher);
/* Restart into the launcher bootscreen: otadata is cleared, the selection stays in NVS. */
void l5_restart_to_launcher(void);
/* After the app slots moved (scheme change): drop the NVS app registry and S.boot. */
void l5_slots_forget_all(void);
/* Erase a whole slot (blocking, call from a worker task). progress gets 0..100. */
esp_err_t l5_slots_erase(int index, void (*progress)(int pct, void *user), void *user);

/* ---------- used by the installer (l5_inst.c) ---------- */

/* registry entry of a freshly installed image (NVS "l5"/"appN"); strings may be NULL */
typedef struct {
    const char *name, *author, *src, *fid, *ver;
} l5_slot_reg_t;
esp_err_t l5_slots_register(int index, const uint8_t elf_sha[32], uint32_t image_len, const l5_slot_reg_t *r);
/* before an install overwrites slot `index`: otadata stops selecting it, its registry entry goes */
void l5_slots_release(int index);
/* S.boot without a scan (after AUTO PARTITION the running table is stale until the restart) */
void l5_slots_set_boot(int index);
/* Hand-over to an image at a flash address of a table written since boot (AUTO PARTITION):
   verify, select ota_index of ota_count slots in otadata, restart. Returns only on failure. */
esp_err_t l5_slots_handoff_at(uint32_t ota_index, uint32_t ota_count, uint32_t addr, uint32_t size,
                              bool deep_sleep_starts_launcher);
bool l5_chip_rev_ok(uint16_t rev_min_full, uint16_t rev_max_full); /* this chip runs an image built for it */

/* ---------- boot-loop guard (bootloader_components/main/l5_guard.h) ---------- */

/* Call first thing in app_main: true when the bootloader stopped an app (slot) that restarted 3 times
   right after starting (restarts) or came back to the launcher 3 times within 15 s of starting
   (returns); the registers are cleared then. Otherwise they stay as they are: the bootscreen's own
   boot must not end a row of early returns. */
bool l5_slots_guard_take(int *slot, int *restarts, int *returns);
/* The user boots an app on purpose, or the slot's image goes: both rows start over. */
void l5_slots_guard_clear(void);
/* A stopped app is not started by the bootscreen countdown or btnOnlyStart until the user boots it
   (BOOT AGAIN, HOME), erases or replaces it. Kept in NVS for the image that was stopped. */
void l5_slots_mark_stopped(int index);
int l5_slots_stopped(void); /* slot index, -1 = none */
void l5_slots_clear_stopped(void);

/* ---------- app images in files (SD) ---------- */

typedef enum {
    L5_BIN_APP,        /* ESP32-P4 app image that this chip revision can run */
    L5_BIN_CHIP_REV,   /* ESP32-P4 app built for another chip revision */
    L5_BIN_OTHER_CHIP, /* app image for another ESP32 family member */
    L5_BIN_NO_APP,     /* 0xE9 image without an app description: bootloader or merged image */
    L5_BIN_UNKNOWN,    /* not an ESP image */
} l5_bin_kind_t;

typedef struct {
    l5_bin_kind_t kind;
    uint32_t size;          /* file size */
    uint16_t chip_id;       /* esp_image_header_t.chip_id */
    uint16_t rev_min, rev_max; /* supported chip revisions, major * 100 + minor */
    uint16_t rev_chip;      /* revision of this chip */
    esp_app_desc_t desc;    /* valid for L5_BIN_APP / L5_BIN_CHIP_REV / L5_BIN_OTHER_CHIP */
} l5_bin_info_t;

/* Reads the image header and esp_app_desc_t at the start of a file. */
esp_err_t l5_bin_probe(const char *path, l5_bin_info_t *out);
const char *l5_chip_name(uint16_t chip_id);
/* Smallest data.js scheme whose app slot holds `bytes` (L5.bestScheme), NULL when none does. */
const char *l5_slots_best_scheme(uint32_t bytes);
