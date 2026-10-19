/* The installer shared by SD and OTA: streams an app image, and the data images that fit their
   partition exactly, from a file on the SD card or over HTTPS straight into flash (no full-size
   RAM buffer). Order of work, so a stop at any point leaves no half app behind:
     1. the first 4 KB of the app are read and checked (ESP32-P4, chip revision, app description);
        nothing is erased before the source answered with a valid image
     2. the app slot (as far as the image reaches) is erased
     3. the app from byte 4096 on is written, its driver names are searched on the way
        (l5_hw_scan_feed); a dropped connection is resumed with a Range request (3 tries)
     4. names of the older Tab5 display/touch only: hw_warn() asks INSTALL ANYWAY / CANCEL
     5. each data image: its partition is erased, then written
     6. the first sector goes last, then esp_image_verify (segments, SHA-256) on flash
     7. the registry entry (name, author, source, catalog id and version) is written to NVS
   Blocking: call from a worker task. Throughput is logged per piece. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "l5_fw.h"
#include "l5_ptable.h"

typedef enum {
    L5_INST_CHECK,  /* reading and checking the image header */
    L5_INST_ERASE,
    L5_INST_APP,
    L5_INST_DATA,
    L5_INST_VERIFY,
    L5_INST_DONE,
} l5_inst_stage_t;

/* pct 0..100 over the whole install, text for the progress dialog */
typedef void (*l5_inst_progress_t)(int pct, const char *text, void *user);

typedef struct {
    const l5_fw_t *fw;          /* sources: SD paths ("/sdcard/...") or https:// URLs */
    const l5_ptable_t *table;   /* the slot and data partitions come from it (the running table, or the
                                   one AUTO PARTITION just wrote) */
    int slot;                   /* ota index */
    bool data;                  /* also write the data images that match exactly (APP + DATA) */
    const char *name, *author, *src; /* registry: display name, author, "SD" / "OTA" / "LINK" */
    volatile bool *cancel;
    l5_inst_progress_t progress;
    /* the image names only the older display/touch (ILI9881C + GT911): called from the install task
       before anything but the app slot was touched; true installs anyway. NULL: no question. */
    bool (*hw_warn)(void *user);
    void *user;
} l5_inst_job_t;

typedef struct {
    l5_inst_stage_t stage;      /* where it stopped (L5_INST_DONE on success) */
    bool erased;                /* flash was touched: the slot is empty now */
    bool net;                   /* a network failure: RETRY can work */
    bool cancelled;
    uint32_t image_len;         /* app image length */
    int data_written;           /* data images written */
    uint8_t hw;                 /* L5_HW_* driver names found in the app image */
    char text[200];             /* one sentence for the result dialog */
} l5_inst_result_t;

esp_err_t l5_inst_run(const l5_inst_job_t *job, l5_inst_result_t *res);

/* The first L5_FW_PROBE_LEN bytes of a source (fewer when it is shorter) and its size, to tell an
   app image from a merged image (l5_fw_from_probe). head must hold L5_FW_PROBE_LEN bytes. */
esp_err_t l5_inst_probe(const char *url, volatile bool *cancel, uint8_t *head, size_t *len, uint32_t *size,
                        char *why, size_t wlen, bool *net);

/* Exact app image length of a merged image on the SD card (walks the segment headers), so AUTO
   PARTITION sizes the slot for the image and not for the firmware's whole app partition. */
esp_err_t l5_inst_app_len_file(const char *path, uint32_t offset, uint32_t max, uint32_t *len);
