/* SD card file operations behind the SD manager's COPY / PASTE, RENAME, NEW FOLDER, DELETE and VIEW
   (ui/l5_sd_ops.c): name rules, free names, folder listing, counting, recursive copy and delete, the
   text preview. No LVGL here: tools/host_test runs it on the PC against a folder standing in for the
   card. Paths are VFS paths ("/sdcard/a/b"). Blocking calls: run them from a worker task. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#define L5_FILES_PATH 320 /* path buffer: /sdcard + folders + name */

/* The card root ("/sdcard"). Copy, rename and delete never touch anything outside it, and delete never
   the root itself. */
extern const char *l5_files_root;

/* ---------- names ---------- */

/* NEW FOLDER / RENAME: trims spaces at both ends of `name` (in place), then false with a sentence for
   the dialog when FAT / exFAT would refuse or change the name, or the SD list would hide it. */
bool l5_files_name_ok(char *name, char *why, size_t len);
/* one entry for FAT: names that differ only in letter case (FatFs' own case folding on the Tab5) */
bool l5_files_same_name(const char *a, const char *b);
/* uniq() of the prototype: `name` when nothing in `dir` has it, else "<base>-copy<ext>",
   "<base>-copy2<ext>", ... (extension from the last dot). False when no free name fits. */
bool l5_files_unique(const char *dir, const char *name, char *out, size_t len);
/* first free of "<base>", "<base>2", "<base>3", ... (NEW FOLDER suggests "new_folder") */
bool l5_files_free_name(const char *dir, const char *base, char *out, size_t len);
/* `path` is `dir` or lies below it (letter case ignored, as FAT does) */
bool l5_files_inside(const char *path, const char *dir);
/* the files VIEW opens: .txt .conf .json .cfg .ini .md .csv .log .py */
bool l5_files_is_text(const char *name);
/* `src` in capitals as FatFs folds names (on the PC: ASCII, Latin-1 and Latin Extended-A, so "zolc" with Polish
   letters becomes their capitals), cut on a character boundary to fit `size` */
void l5_files_upper(char *dst, size_t size, const char *src);
/* not listed: "." and "..", names starting with a dot (macOS "._x.bin" forks), Windows' own folder */
bool l5_files_hidden(const char *name);
/* "/sdcard/a/b" -> "/sdcard/a"; false for the root */
bool l5_files_parent(const char *path, char *out, size_t len);
/* dir + "/" + name; false when it does not fit */
bool l5_files_join(char *out, size_t len, const char *dir, const char *name);

/* ---------- listing ---------- */

typedef struct {
    const char *name; /* valid during the callback */
    bool dir;
    uint64_t size;
    time_t mtime;     /* 0 when unknown */
} l5_files_entry_t;
typedef bool (*l5_files_entry_cb_t)(const l5_files_entry_t *e, void *user); /* false: stop */

/* Every entry of `dir`, hidden ones too. On the Tab5 FatFs is read directly, so size and date come with
   the name (a stat() per entry searches the folder again: slow for big folders). */
esp_err_t l5_files_list(const char *dir, l5_files_entry_cb_t cb, void *user);
/* entries of `dir` that are listed (not hidden), at most `max`; -1 when it cannot be read */
int l5_files_items(const char *dir, int max);

/* ---------- counting, copy, delete ---------- */

#ifndef ESP_ERR_NOT_FINISHED
#define ESP_ERR_NOT_FINISHED 0x10C
#endif
#define L5_FILES_CANCELLED ESP_ERR_NOT_FINISHED /* what count / copy / delete return after CANCEL */

typedef struct {
    uint32_t files, dirs; /* the top included: a file is 1 file, a folder 1 dir + its contents */
    uint64_t bytes;       /* file contents */
    uint64_t alloc;       /* what a copy takes on the card: whole clusters per file and per folder */
} l5_files_count_t;

typedef struct {
    volatile bool *cancel; /* CANCEL: checked between 64 KB blocks and between entries */
    /* copy: bytes copied / total bytes; delete: entries removed / total; count: entries seen, 0 */
    void (*progress)(uint64_t done, uint64_t total, void *user);
    void *user;
    uint32_t cluster;      /* bytes per cluster for alloc, 0 = 32 KB */
} l5_files_job_t;

typedef struct {
    uint64_t bytes;       /* copied */
    uint32_t files, dirs; /* copied or removed */
    uint32_t ms;
    bool cancelled;
    bool left;            /* copy: the partial copy could not be removed completely */
    char why[200];        /* what went wrong, one sentence */
} l5_files_result_t;

esp_err_t l5_files_count(const char *path, const l5_files_job_t *job, l5_files_count_t *out);
/* Copies the file or folder src to dst, which must not exist. After an error or CANCEL whatever was
   copied to dst is removed again. total: from l5_files_count (progress), may be NULL. */
esp_err_t l5_files_copy(const char *src, const char *dst, const l5_files_job_t *job, const l5_files_count_t *total,
                        l5_files_result_t *res);
/* Removes the file or folder (with everything inside) below the card root. total: entries for the
   progress (files + dirs from l5_files_count), 0 = unknown. Read-only entries are made writable first. */
esp_err_t l5_files_delete(const char *path, const l5_files_job_t *job, uint32_t total, l5_files_result_t *res);
esp_err_t l5_files_rename(const char *from, const char *to, char *why, size_t len);
esp_err_t l5_files_mkdir(const char *path, char *why, size_t len);
/* card size, free space and cluster size (the host test sets them) */
esp_err_t l5_files_space(uint64_t *total, uint64_t *free_bytes, uint32_t *cluster);

/* ---------- text preview ---------- */

#define L5_TEXT_MAX (64 * 1024) /* VIEW shows the first 64 KB */
#define L5_TEXT_PIECE 2048      /* bytes per label in the viewer */

typedef struct {
    /* Sanitized UTF-8 in pieces of at most L5_TEXT_PIECE bytes, each NUL-terminated, one after the
       other (a piece that ended at a newline lost it to the NUL). PSRAM on the Tab5. */
    char *text;
    size_t len;         /* bytes used, NULs included */
    uint32_t pieces;
    uint32_t lines;
    uint64_t file_size;
    bool binary;        /* a NUL byte in the first 64 KB: not a text file, nothing kept */
    bool cut;           /* only the first L5_TEXT_MAX bytes */
} l5_text_t;

/* Reads the first 64 KB of `path` into `out`: CR LF -> LF, tabs to 4 columns, terminal colour codes
   dropped, other control characters and broken UTF-8 bytes shown as '?', a cut character at the 64 KB
   end dropped. */
esp_err_t l5_files_read_text(const char *path, l5_text_t *out, char *why, size_t len);
/* the same on bytes in memory (host test) */
bool l5_text_prepare(const uint8_t *in, size_t n, l5_text_t *out);
void l5_text_free(l5_text_t *t);
