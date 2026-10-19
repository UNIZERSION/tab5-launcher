/* Partition table at 0x8000: read, build a new layout for the scheme-managed area, validate,
   write it safely, and the data-partition tools of PMAN (format / backup / restore).
   Port of L5.table / L5.applyScheme / pmAction in tab5-launcher (app.js, views2.js).

   Never touched: bootloader (0x2000), the table location, nvs, otadata, the launcher
   (factory @0x10000), Aurora Player's PIN store aurora_cfg (@0xF10000) and coredump (@0xFF0000).

   Scheme-managed area (16 MB flash):
     0x210000 .. 0xF10000  13312 KB  app slots app0..appN-1 (ota_N), then FAT "vfs"
     0xF10000 .. 0xF16000     24 KB  aurora_cfg (data, nvs): fixed in every scheme
     0xF16000 .. 0xFF0000    872 KB  SPIFFS (whole gap) or free */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define L5_PT_OFFSET   0x8000
#define L5_PT_LEN      0xC00   /* gen_esp32part.py MAX_PARTITION_LENGTH */
#define L5_PT_MAX      95      /* 96 records of 32 bytes, one is the MD5 record */
#define L5_MIN_DATA_KB 448     /* 0x70000: smallest data partition (data.js minData) */
#define L5_MIN_APP_KB  1024    /* smallest app slot offered (views2.js resize) */
#define L5_BACKUP_DIR  "/sdcard/backups"

#define L5_POOL_START   0x210000 /* end of the 2 MB launcher */
#define L5_POOL_END     0xFF0000 /* coredump */
#define L5_AURORA_LABEL "aurora_cfg"
#define L5_AURORA_ADDR  0xF10000 /* Aurora's LOCK_ADDRESS / LOCK_SIZE (screen_lock_storage.cpp) */
#define L5_AURORA_SIZE  0x6000
#define L5_GAP_ADDR     (L5_AURORA_ADDR + L5_AURORA_SIZE)
#define L5_FRONT_KB     ((L5_AURORA_ADDR - L5_POOL_START) / 1024) /* 13312: app slots + FAT */
#define L5_GAP_KB       ((L5_POOL_END - L5_GAP_ADDR) / 1024)      /* 872: SPIFFS or nothing */

typedef struct {
    char label[17];
    uint8_t type, subtype;
    uint32_t offset, size, flags;
} l5_part_t;

typedef struct {
    int n;
    l5_part_t p[L5_PT_MAX];
} l5_ptable_t;

/* data.js schemes: app area split into n equal slots, SPIFFS and FAT sizes in KB.
   SPIFFS is 0 or L5_GAP_KB; n x slot + FAT fill the 13312 KB before aurora_cfg. */
typedef struct {
    const char *id, *label;
    uint32_t app_kb, n, spiffs_kb, fat_kb;
} l5_scheme_def_t;
extern const l5_scheme_def_t l5_schemes[];
extern const int l5_scheme_count;

#define L5_MAX_EXTRA 4
#define L5_SUBTYPE_LITTLEFS 0x83  /* ESP_PARTITION_SUBTYPE_DATA_LITTLEFS */
#define L5_SUBTYPE_UNDEFINED 0x06 /* ESP_PARTITION_SUBTYPE_DATA_UNDEFINED */

/* a data partition a firmware's own table asks for (AUTO PARTITION from the install check) */
typedef struct {
    char label[17];
    uint8_t subtype; /* data subtype: FAT, SPIFFS, LittleFS, NVS */
    uint32_t kb;
} l5_extra_t;

/* The scheme-managed area: n equal app slots, then the extra data partitions, then FAT (all before
   aurora_cfg); after aurora_cfg one partition of spiffs_kb (at most the 872 KB gap). Schemes use no
   extras, labels "vfs" / "spiffs" and the whole gap; AUTO PARTITION for a firmware uses its labels and
   exact sizes, so the firmware's data images can be written. Empty labels mean "vfs" / "spiffs". */
typedef struct {
    uint32_t n, slot_kb, spiffs_kb, fat_kb;
    uint32_t nextra;
    l5_extra_t extra[L5_MAX_EXTRA];
    char fat_label[17], gap_label[17];
    uint8_t gap_subtype; /* 0 = SPIFFS */
} l5_layout_t;

typedef struct {
    l5_ptable_t table;              /* as read from flash */
    uint32_t flash_size;
    uint32_t pool_start, pool_end;  /* scheme-managed area in bytes: launcher end .. coredump */
    bool managed;                   /* the area holds only app slots, SPIFFS, FAT, aurora_cfg: `layout` is valid */
    bool aurora;                    /* aurora_cfg is in the table at 0xF10000 / 0x6000 */
    l5_layout_t layout;             /* as found; may predate the aurora_cfg rule (e.g. SPIFFS 448 KB) */
    char why[96];                   /* why the table is not managed */
} l5_flash_map_t;

typedef enum { L5_DATA_NONE, L5_DATA_KEPT, L5_DATA_ERASED, L5_DATA_REMOVED } l5_data_change_t;
typedef struct {
    bool apps;                  /* app slot positions change: every installed app is erased */
    bool aurora_new;            /* aurora_cfg is created now: its 24 KB (old FAT bytes) are erased */
    l5_data_change_t spiffs, fat; /* the gap partition and the main FAT (the last FAT before aurora_cfg) */
    int nerase;                 /* data partitions of `next` that are new, moved or resized: erased */
    const l5_part_t *erase[L5_MAX_EXTRA + 2];
    char erased[96], removed[96]; /* labels for the confirmation ("vfs, sys") */
} l5_pt_diff_t;

typedef void (*l5_pt_progress_t)(int pct, const char *label, void *user);

esp_err_t l5_pt_read_map(l5_flash_map_t *map);
/* the current layout brought under the aurora_cfg rule (SPIFFS -> gap, FAT clipped before 0xF10000),
   the starting point for RESIZE / DELETE / CREATE / OPTIMIZE / AUTO on tables made before the rule */
l5_layout_t l5_pt_base_layout(const l5_flash_map_t *map);
uint32_t l5_layout_front_kb(const l5_layout_t *l);           /* n x slot + extras + FAT, at most L5_FRONT_KB */
uint32_t l5_layout_extras_kb(const l5_layout_t *l);
const char *l5_layout_fat_label(const l5_layout_t *l);       /* "vfs" unless AUTO PARTITION named it */
const char *l5_layout_gap_label(const l5_layout_t *l);       /* "spiffs" unless AUTO PARTITION named it */
l5_layout_t l5_layout_of_scheme(const l5_scheme_def_t *s);   /* L5.slotKB: floor(app / n / 64) * 64 */
const l5_scheme_def_t *l5_scheme_find(const char *id_or_label);
const l5_scheme_def_t *l5_layout_match(const l5_layout_t *l); /* NULL = Custom */
bool l5_layout_equal(const l5_layout_t *a, const l5_layout_t *b);

/* New table for `l`, fully validated (layout rules, fixed region unchanged, bootloader check
   esp_partition_table_verify incl. MD5). why gets a sentence on failure. */
esp_err_t l5_pt_build(const l5_flash_map_t *map, const l5_layout_t *l, l5_ptable_t *out, char *why, size_t len);
void l5_pt_diff(const l5_flash_map_t *map, const l5_ptable_t *next, l5_pt_diff_t *d);

/* The dangerous part, call from a worker task: back up the old table (NVS + SD), clear otadata,
   erase + write 0x8000, read back and compare (old table written back on mismatch), then erase
   the app slots / data partitions that changed. The caller restarts afterwards.
   *table_written tells whether flash now holds the new table. */
esp_err_t l5_pt_apply(const l5_flash_map_t *map, const l5_ptable_t *next, l5_pt_progress_t cb, void *user,
                      bool *table_written, char *why, size_t len);

/* Boot check for tables written over USB: when aurora_cfg sits on bytes that are neither erased
   nor NVS (old FAT data), Aurora cannot open its PIN store. Erases the 24 KB only in that case;
   anything with a valid NVS page header is never touched. Returns true when it erased. */
bool l5_pt_aurora_sanitize(void);

/* data partitions (spiffs / fat / other non-system data), by label, from the running table */
bool l5_part_editable(const char *label);
esp_err_t l5_part_erase(const char *label, l5_pt_progress_t cb, void *user);
esp_err_t l5_part_backup(const char *label, char *out_path, size_t len, l5_pt_progress_t cb, void *user);
esp_err_t l5_part_restore(const char *label, const char *path, l5_pt_progress_t cb, void *user);
int l5_part_blank(const char *label); /* 1 = first 4 KB erased, 0 = holds data, -1 = unknown */

/* SANITY CHECK of an app image in a file, starting at `offset` (0, or the app inside a merged image): header,
   segments, checksum, appended SHA-256 */
typedef struct {
    bool ok;
    uint16_t chip_id;
    uint8_t segments;
    bool hash_appended, hash_ok, checksum_ok;
    uint32_t image_len, file_size;
    char error[80];
} l5_image_check_t;
esp_err_t l5_image_check_file(const char *path, uint32_t offset, l5_image_check_t *out);
