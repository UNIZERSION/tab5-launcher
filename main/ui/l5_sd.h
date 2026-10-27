/* SD manager internals shared by the browser (l5_sd.c) and the file actions (l5_sd_ops.c).
   LVGL lock held for all of them. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "l5_files.h"

typedef struct {
    char path[L5_FILES_PATH]; /* VFS path */
    const char *name;         /* last part of path */
    bool dir;
    bool is_cwd;              /* nothing selected: the current folder */
    int items;                /* folders: listed entries inside, -1 when not counted */
} l5_sd_target_t;

/* RENAME / DELETE / COPY act on the selected entry, else on the current folder; false at the card root
   with nothing selected */
bool l5_sd_target(l5_sd_target_t *t);
void l5_sd_cwd_path(char *out, size_t len); /* VFS path of the current folder */
/* after a file action: re-read the folder (now `cwd`, a VFS path, when not NULL) with `select` selected
   (NULL: nothing); the view is rebuilt after the finger is lifted */
void l5_sd_after_op(const char *cwd, const char *select);
void l5_sd_paint_clip(void); /* clipboard line in the foot and the PASTE chip, in place */

/* l5_sd_ops.c */
void l5_sd_action(const char *id); /* "copy" "cut" "paste" "rename" "mkdir" "delete" "view" */
const char *l5_sd_clip_name(void); /* NULL while the clipboard is empty */
bool l5_sd_clip_cut(void);         /* the clipboard came from CUT: PASTE moves it */
