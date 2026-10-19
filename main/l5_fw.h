/* A firmware to install, whatever it comes as: an app image, a merged image (bootloader +
   partition table + app + data images, M5Burner "merged"), or an entry whose data images are
   separate files (launcherhub install manifest with "sources").
   It knows where the app image is, and which data partitions the firmware's own table expects
   (label, type, size, and whether an image for it is included).

   l5_fw_check() compares that with a partition table, so the install dialog can say what will not
   work, and l5_fw_plan() makes the AUTO PARTITION layout that provides it as far as the fixed rules
   allow (launcher, nvs, otadata, aurora_cfg at 0xF10000, coredump never move).
   Pure C (no flash, no network): tested on the host with real catalog answers and images. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "l5_ptable.h"

#define L5_FW_MAX_PARTS 12
#define L5_FW_MAX_SRC 4
#define L5_FW_URL_LEN 256
#define L5_FW_TABLE_OFFSET 0x8000 /* partition table in flash */
#define L5_FW_BOOT_OFFSET 0x2000  /* ESP32-P4 bootloader in flash */
#define L5_FW_PROBE_LEN (L5_FW_TABLE_OFFSET + L5_PT_LEN) /* bytes that tell app image / merged image */

typedef enum { L5_FW_APP_ONLY, L5_FW_MERGED, L5_FW_MULTI } l5_fw_format_t;

typedef struct {
    char label[17];
    uint8_t type, subtype;   /* esp_partition types */
    uint32_t offset;         /* in the firmware's own flash layout, 0 when unknown */
    uint32_t size;           /* partition size the firmware's table asks for */
    char source[16];         /* source key; "" = the main file */
    uint32_t src_offset;     /* where its image starts in the source */
    uint32_t copy_size;      /* image bytes included, 0 = none (the app formats the partition) */
} l5_fw_part_t;

typedef struct {
    l5_fw_format_t format;
    char name[64], author[48], version[32], fid[40], date[12];
    char app_source[16];     /* source key of the app, "" = the main file */
    uint32_t app_offset;     /* app image offset in its source (merged: the table's boot app) */
    uint32_t base;           /* merged image: flash address of its first byte (0x0 or 0x2000) */
    char app_label[17];      /* merged image: label of the app partition it boots */
    uint32_t table_end;      /* merged image: end of the last partition of its table (flash address) */
    uint32_t app_size;       /* image bytes as the catalog gives them, 0 = unknown (parsed while installing) */
    uint32_t app_part_size;  /* the firmware's own app partition, 0 = unknown */
    uint32_t file_size;      /* main file size, 0 = unknown */
    bool has_table;          /* the data partitions are known */
    int nparts;
    l5_fw_part_t part[L5_FW_MAX_PARTS];
    int nsrc;                /* src[0] is the main file */
    struct {
        char key[16];
        char url[L5_FW_URL_LEN]; /* catalog file name, URL or SD path */
    } src[L5_FW_MAX_SRC];
} l5_fw_t;

void l5_fw_init(l5_fw_t *fw, l5_fw_format_t format);
/* Flash address of the first byte of a merged image, from its first bytes: 0x0 when it pads up to the
   bootloader at 0x2000 and has the table at 0x8000 (esptool merge_bin's default, M5Burner files), 0x2000 when
   it starts with the bootloader and has the table at 0x6000; -1: not a merged image. */
int32_t l5_fw_merged_base(const uint8_t *head, size_t len);
/* esp_partition_info_t records of a table (flash 0x8000, len up to 0xC00) found in a merged image whose first
   byte is flash address `base`: images sit at their flash offset - base in the file, and file_size tells which
   data images the file includes. Fails when there is no app partition. */
esp_err_t l5_fw_from_table(l5_fw_t *fw, const uint8_t *table, size_t len, uint32_t file_size, uint32_t base);
/* launcherhub install manifest (GET /firmwares?fid=X&version=V). ESP_ERR_NOT_FOUND: the entry has
   no install data (the file itself is probed then); other errors say why in `why`. */
esp_err_t l5_fw_from_manifest(l5_fw_t *fw, const char *json, char *why, size_t len);
/* First L5_FW_PROBE_LEN bytes of a file (fewer when the file is shorter): app image or merged image (either
   base). Refused with a sentence: a copy of a whole flash (base 0x0, 4/8/16/32 MB, every partition of its table
   inside: esptool read_flash; ESP_ERR_INVALID_SIZE) and an image that boots the Tab5 Launcher itself (app
   partition "launcher"; ESP_ERR_INVALID_STATE); format, base and app_offset are filled in for those too. */
esp_err_t l5_fw_from_probe(l5_fw_t *fw, const uint8_t *head, size_t len, uint32_t file_size, char *why, size_t wlen);
/* url of a source key ("" / "firmware" = main file): NULL when the entry has none */
const char *l5_fw_source_url(const l5_fw_t *fw, const char *key);
bool l5_fw_set_source(l5_fw_t *fw, const char *key, const char *url);
/* bytes of the app image at most (catalog size, else the firmware's app partition / file end) */
uint32_t l5_fw_app_bound(const l5_fw_t *fw);

/* the partition of `t` that takes the image of `p`: same label, type, subtype and size, else NULL */
const l5_part_t *l5_fw_match(const l5_fw_part_t *p, const l5_ptable_t *t);
bool l5_fw_system_part(const l5_fw_part_t *p); /* nvs: the launcher's own, never written */

typedef enum {
    L5_FWI_OK,       /* present with the same type and size: an included image is written */
    L5_FWI_LARGER,   /* present and larger: the app can use it, an included image is not written */
    L5_FWI_SMALL,    /* present but smaller than the firmware expects */
    L5_FWI_TYPE,     /* a partition with this label has another type */
    L5_FWI_MISSING,  /* no partition with this label */
    L5_FWI_INFO,     /* system partition that differs (nvs size): told, not a problem */
} l5_fwi_status_t;

typedef struct {
    char label[17];
    uint8_t subtype;
    uint32_t want, have;     /* bytes: what the firmware's table asks for / what the table has */
    bool image;              /* the firmware brings an image for it */
    bool writable;           /* that image can be written (same label, type and size) */
    l5_fwi_status_t st;
} l5_fw_item_t;

typedef struct {
    bool app_known, app_fits;
    uint32_t app_need;       /* image bytes (upper bound when not known exactly) */
    uint32_t slot_size;      /* the slot it goes to */
    int n;
    int problems;            /* MISSING / SMALL / TYPE items */
    int lost;                /* included images that cannot be written */
    int images;              /* included images that are written (APP + DATA) */
    l5_fw_item_t item[L5_FW_MAX_PARTS];
} l5_fw_report_t;

/* `cur` is the table to install into, slot_size the app slot the image goes to (0 = largest slot) */
void l5_fw_check(const l5_fw_t *fw, const l5_ptable_t *cur, uint32_t slot_size, l5_fw_report_t *rep);
/* One line per item for the install dialog ("sys  FAT 1 MB  missing"), problems first. */
void l5_fw_report_text(const l5_fw_report_t *rep, char *out, size_t len);

/* AUTO PARTITION: one app slot for the image (plus what is left over, room for updates), the
   firmware's data partitions with their labels and exact sizes (so their images can be written),
   the 872 KB after aurora_cfg for one of them. When everything does not fit, partitions without an
   image shrink first, then images are dropped (largest first); notes says what changed.
   Fails (why in notes) when the app alone does not fit. */
esp_err_t l5_fw_plan(const l5_fw_t *fw, l5_layout_t *out, char *notes, size_t len);
/* "app0 7.4 MB · sys FAT 1 MB · vfs FAT 4.6 MB · storage SPIFFS 640 KB" */
void l5_layout_text(const l5_layout_t *l, char *out, size_t len);
void l5_fw_size(char *out, size_t len, uint64_t bytes); /* "7.4 MB", "640 KB", "512 B" */
const char *l5_fw_subtype_name(uint8_t subtype);        /* "FAT", "SPIFFS", "LittleFS", "NVS", ... */

/* ---------- driver names in an app image: which Tab5 display and touch it supports ----------
   The first Tab5 boards have an ILI9881C panel with GT911 touch, later ones ST7121 / ST7123 with
   ST712x touch. An app built only for the first kind panics when its touch init fails (rst:0xc
   loop). The names are searched case-insensitively while the image streams by. */
enum { L5_HW_GT911 = 1, L5_HW_ILI9881 = 2, L5_HW_ST7123 = 4, L5_HW_ST7121 = 8, L5_HW_ST712X = 16 };
#define L5_HW_OLD (L5_HW_GT911 | L5_HW_ILI9881)
#define L5_HW_NEW (L5_HW_ST7123 | L5_HW_ST7121 | L5_HW_ST712X)
typedef struct {
    uint8_t found;   /* L5_HW_* seen so far */
    uint8_t ntail, tail[6]; /* last bytes of the previous chunk: names across chunk borders */
} l5_hw_scan_t;
void l5_hw_scan_feed(l5_hw_scan_t *s, const uint8_t *d, size_t n);
/* names of the old display/touch only: on a newer Tab5 the app may not start */
bool l5_hw_old_only(const l5_hw_scan_t *s);

/* ---------- ESP app image length while streaming (esp_image_format.c layout) ---------- */
typedef struct {
    uint32_t pos;            /* bytes seen so far */
    uint32_t want;           /* position of the next header the parser must see */
    uint32_t len;            /* image length once known: header, segments, checksum, SHA-256 */
    uint16_t chip;           /* esp_image_header_t.chip_id */
    uint16_t rev_min, rev_max;
    uint8_t nseg, seg, hash;
    uint8_t have, hdr[24];
    bool failed;
    char error[64];
} l5_img_parse_t;

void l5_img_begin(l5_img_parse_t *p);
/* the next n bytes of the image; false on a format error (p->error) */
bool l5_img_feed(l5_img_parse_t *p, const uint8_t *d, size_t n);
/* bytes from p->pos on the parser does not need to see (a file reader may seek over them) */
uint32_t l5_img_skippable(const l5_img_parse_t *p);
void l5_img_skip(l5_img_parse_t *p, uint32_t n);
