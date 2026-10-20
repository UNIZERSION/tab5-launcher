/* SD manager file actions: COPY / PASTE, RENAME, NEW FOLDER, DELETE and VIEW, port of sdAction() in
   views.js (Launcher 2.10's SD menu has the same set) with the file system work in l5_files.c.
   Copy, delete, counting and reading a text file run in a worker task: a transparent blocker keeps taps
   away at once, and the progress dialog with CANCEL appears when the work takes longer than 300 ms, so
   small files do not flash a dialog. Rename and new folder are single FatFs calls on the LVGL task. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "l5_board.h"
#include "l5_config.h"
#include "l5_files.h"
#include "l5_sd.h"
#include "l5_ui.h"

static const char *TAG = "l5_sd";

#define SHOW_AFTER_MS 300  /* the progress dialog only for work that takes longer */
#define PAINT_EVERY_MS 150 /* progress updates from the worker */
#define NAME_MAX_CHARS 128 /* keyboard limit for names (FAT allows 255) */

/* R.sd.clip: COPY keeps the path, PASTE copies it into the folder that is open then */
static struct {
    char path[L5_FILES_PATH];
} s_clip;

typedef enum { JOB_PASTE, JOB_COUNT_DELETE, JOB_DELETE, JOB_VIEW } job_op_t;

static struct {
    job_op_t op;
    bool busy;
    volatile bool cancel;
    volatile bool counting;
    char src[L5_FILES_PATH]; /* paste: the clipboard; delete, view: the target */
    char dst[L5_FILES_PATH]; /* paste: the new copy */
    char name[256];          /* what the dialogs name */
    bool dir, is_cwd, config, no_space, copying;
    l5_files_count_t count;
    l5_files_result_t res;
    l5_text_t text;
    esp_err_t err;
    char why[220];
    uint64_t free_bytes;
    /* LVGL side (lock held) */
    lv_obj_t *blocker;
    lv_timer_t *show;
    l5_progress_t *progress;
    char title[300];
    int64_t painted;
    uint64_t done, total;
} J;

/* RENAME / NEW FOLDER: what the keyboard edits, kept for EDIT NAME after a refused name */
static struct {
    bool mkdir;
    bool is_cwd, dir;
    char path[L5_FILES_PATH]; /* rename: the entry; new folder: the folder it goes into */
    char text[NAME_MAX_CHARS * 4 + 1];
} s_edit;

static char s_body[640]; /* dialog text, LVGL task only */

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static bool is_bin(const char *n)
{
    size_t l = strlen(n);
    return l >= 4 && strcasecmp(n + l - 4, ".bin") == 0;
}

/* a file the list does not show while ONLY BINS is on */
static const char *hidden_note(const char *name, bool dir)
{
    return g_cfg.onlyBins && !dir && !is_bin(name) ? " \xC2\xB7 ALL FILES shows it" : "";
}

const char *l5_sd_clip_name(void)
{
    return s_clip.path[0] ? base_name(s_clip.path) : NULL;
}

/* the clipboard follows a rename of itself or of a folder it is in, and goes with a delete */
static void clip_renamed(const char *from, const char *to)
{
    if (s_clip.path[0] && l5_files_inside(s_clip.path, from)) {
        char p[L5_FILES_PATH];
        if (snprintf(p, sizeof(p), "%s%s", to, s_clip.path + strlen(from)) < (int)sizeof(p)) {
            strlcpy(s_clip.path, p, sizeof(s_clip.path));
        } else {
            s_clip.path[0] = 0;
        }
    }
}

static void clip_deleted(const char *path)
{
    if (s_clip.path[0] && l5_files_inside(s_clip.path, path)) s_clip.path[0] = 0;
}

/* ---------- the worker: progress, blocker, dialog ---------- */

static void paint(void)
{
    if (!J.progress) return;
    char t[96], a[16], b[16];
    int pct = 0;
    if (J.counting) {
        snprintf(t, sizeof(t), "Counting\xE2\x80\xA6 %llu items", (unsigned long long)J.done);
    } else if (J.op == JOB_PASTE) {
        l5_fmt_bytes(a, sizeof(a), J.done);
        l5_fmt_bytes(b, sizeof(b), J.total);
        snprintf(t, sizeof(t), "%s of %s", a, b);
        pct = J.total ? (int)(J.done * 100 / J.total) : 0;
    } else if (J.op == JOB_DELETE) {
        snprintf(t, sizeof(t), "%llu of %llu items removed", (unsigned long long)J.done, (unsigned long long)J.total);
        pct = J.total ? (int)(J.done * 100 / J.total) : 0;
    } else {
        snprintf(t, sizeof(t), "Reading\xE2\x80\xA6");
    }
    l5_progress_set(J.progress, pct, t);
}

static void show_cb(lv_timer_t *t)
{
    (void)t;
    J.show = NULL; /* one run: LVGL deletes the timer */
    J.progress = l5_progress_open(J.op == JOB_PASTE ? "PASTE" : J.op == JOB_VIEW ? "VIEW" : "DELETE", J.title);
    if (J.op != JOB_VIEW) l5_progress_cancelable(J.progress, &J.cancel);
    paint();
}

static void job_progress(uint64_t done, uint64_t total, void *user)
{
    (void)user;
    int64_t now = esp_timer_get_time() / 1000;
    if (now - J.painted < PAINT_EVERY_MS) return;
    J.painted = now;
    if (l5_board_lock(0)) {
        J.done = done;
        J.total = total;
        paint();
        l5_board_unlock();
    }
}

static void blocker_deleted_cb(lv_event_t *e)
{
    (void)e;
    J.blocker = NULL; /* l5_dialogs_close_all */
}

static void job_end_ui(void)
{
    if (J.show) lv_timer_delete(J.show);
    J.show = NULL;
    l5_progress_close(J.progress);
    J.progress = NULL;
    if (J.blocker) lv_obj_delete(J.blocker);
    J.busy = false;
}

static void job_done(void);

static void job_task(void *arg)
{
    (void)arg;
    l5_files_job_t job = {.cancel = &J.cancel, .progress = job_progress};
    uint64_t total;
    uint32_t cluster = 32768;
    switch (J.op) {
    case JOB_PASTE:
        if (l5_files_space(&total, &J.free_bytes, &cluster) != ESP_OK) J.free_bytes = UINT64_MAX;
        job.cluster = cluster;
        J.counting = true;
        J.err = l5_files_count(J.src, &job, &J.count);
        J.counting = false;
        J.no_space = J.err == ESP_OK && J.count.alloc + cluster > J.free_bytes; /* + room for the folder entry */
        if (J.err == ESP_OK && !J.no_space) {
            J.copying = true;
            J.err = l5_files_copy(J.src, J.dst, &job, &J.count, &J.res);
            uint32_t ms = J.res.ms ? J.res.ms : 1;
            ESP_LOGI(TAG, "copy %llu bytes in %u ms (%llu KB/s), %u files, %u folders: %s -> %s%s",
                     (unsigned long long)J.res.bytes, (unsigned)ms, (unsigned long long)(J.res.bytes * 1000 / ms / 1024),
                     (unsigned)J.res.files, (unsigned)J.res.dirs, J.src, J.dst,
                     J.err == ESP_OK ? "" : J.res.cancelled ? " (cancelled, copy removed)" : " (failed, copy removed)");
        }
        break;
    case JOB_COUNT_DELETE:
        J.counting = true;
        J.err = l5_files_count(J.src, &job, &J.count);
        J.counting = false;
        break;
    case JOB_DELETE:
        if (J.config) { /* the settings file: not while the saver writes it, and no pending write after */
            l5_cfg_discard_pending();
            J.err = l5_config_remove();
            J.res.files = J.err == ESP_OK;
            if (J.err != ESP_OK) snprintf(J.res.why, sizeof(J.res.why), "Could not remove config.conf.");
        } else {
            J.err = l5_files_delete(J.src, &job, J.count.files + J.count.dirs, &J.res);
        }
        ESP_LOGI(TAG, "delete %s: %u files, %u folders removed in %u ms%s", J.src, (unsigned)J.res.files,
                 (unsigned)J.res.dirs, (unsigned)J.res.ms, J.err == ESP_OK ? "" : J.res.cancelled ? " (cancelled)" : " (failed)");
        break;
    case JOB_VIEW:
        J.err = l5_files_read_text(J.src, &J.text, J.why, sizeof(J.why));
        break;
    }
    l5_board_lock(0);
    job_end_ui();
    job_done();
    l5_board_unlock();
    vTaskDelete(NULL);
}

static void job_start(job_op_t op, const char *title)
{
    J.op = op;
    J.cancel = J.counting = J.no_space = J.copying = false;
    J.done = J.total = 0;
    J.painted = 0;
    J.err = ESP_OK;
    J.why[0] = 0;
    memset(&J.res, 0, sizeof(J.res));
    snprintf(J.title, sizeof(J.title), "%s", title);
    J.blocker = l5_box(l5_ui_layer()); /* transparent: taps go nowhere while the work runs */
    lv_obj_set_size(J.blocker, L5_SCREEN_W, L5_SCREEN_H);
    lv_obj_add_flag(J.blocker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(J.blocker, blocker_deleted_cb, LV_EVENT_DELETE, NULL);
    J.show = lv_timer_create(show_cb, SHOW_AFTER_MS, NULL);
    lv_timer_set_repeat_count(J.show, 1);
    J.busy = true;
    /* copy and delete recurse into folders (at most 48 deep), with snprintf and logging on top */
    if (xTaskCreatePinnedToCore(job_task, "l5_sd_job", 12288, NULL, 3, NULL, 1) != pdPASS) {
        job_end_ui();
        l5_info("SD", "Not enough memory", "The file action could not start.");
    }
}

/* ---------- results (LVGL task, after the worker) ---------- */

static void delete_ok_cb(bool ok, void *user)
{
    (void)user;
    if (!ok) return;
    char t[300];
    snprintf(t, sizeof(t), "Deleting %s", J.name);
    job_start(JOB_DELETE, t);
}

static void confirm_delete(void)
{
    char title[300], sz[16];
    snprintf(title, sizeof(title), "Delete %s?", J.name);
    if (J.config) {
        snprintf(s_body, sizeof(s_body), "config.conf holds the launcher settings and the OTA favorites. Deleting it "
                 "resets the settings to the defaults now (brightness, dimming, colour, rotation, countdown, ...). "
                 "The launcher writes a new config.conf at the next change.");
    } else if (J.dir) {
        uint32_t items = J.count.files + J.count.dirs - 1; /* without the folder itself */
        l5_fmt_bytes(sz, sizeof(sz), J.count.bytes);
        int n = items ? snprintf(s_body, sizeof(s_body), "The folder and its %u item%s (%u file%s, %u folder%s, %s) are "
                                 "removed from the SD card.", (unsigned)items, items == 1 ? "" : "s", (unsigned)J.count.files,
                                 J.count.files == 1 ? "" : "s", (unsigned)(J.count.dirs - 1), J.count.dirs == 2 ? "" : "s", sz)
                      : snprintf(s_body, sizeof(s_body), "The empty folder is removed from the SD card.");
        if (J.is_cwd && n > 0 && (size_t)n < sizeof(s_body)) {
            snprintf(s_body + n, sizeof(s_body) - n, " This is the open folder: the list goes up one level.");
        }
    } else {
        snprintf(s_body, sizeof(s_body), "The file is removed from the SD card. A copy already in flash keeps working.");
    }
    l5_confirm("SD", title, s_body, "DELETE", true, delete_ok_cb, NULL);
}

/* ---------- an old config.conf put back (RENAME or PASTE onto the name /config.conf) ----------
   The launcher writes /config.conf at every change of a setting, also after the file was deleted, so the name is
   taken by the time a saved copy comes back: it asks, swaps the files (l5_config_replace) and applies the settings. */

static struct {
    char path[L5_FILES_PATH]; /* the file that becomes /config.conf */
    char name[256];           /* its name now */
    bool copy;                /* PASTE: a copy made next to config.conf */
} s_repl;

static void replace_now(void)
{
    l5_cfg_discard_pending(); /* a settings save still waiting would write the old settings into the new file */
    if (l5_config_replace(s_repl.path) != ESP_OK) {
        snprintf(s_body, sizeof(s_body),
                 s_repl.copy ? "The copy %s could not take the name config.conf: it stays next to it."
                             : "%s could not take the name config.conf. Both files are as they were.",
                 s_repl.name);
        l5_info("SD", "Not replaced", s_body);
        l5_sd_after_op(NULL, s_repl.name);
        return;
    }
    clip_renamed(s_repl.path, L5_CONFIG_PATH);
    if (l5_cfg_reload_now() != ESP_OK) {
        l5_info("SD", "config.conf is not valid",
                "The new config.conf is not a JSON object, so the settings stay as they are. Fix the file on a computer: "
                "the next change of a setting writes a new config.conf over it.");
        l5_sd_after_op(NULL, base_name(L5_CONFIG_PATH));
        return;
    }
    l5_toast("config.conf put back: its settings apply now"); /* l5_cfg_reload_now() rendered every view */
}

static void replace_cb(bool ok, void *user)
{
    (void)user;
    if (ok) {
        replace_now();
        return;
    }
    l5_toast("Not replaced");
    l5_sd_after_op(NULL, s_repl.copy ? s_repl.name : NULL);
}

/* shown: the name in the question (PASTE: the file that was copied, not the copy) */
static void ask_replace(const char *path, const char *name, bool copy, const char *shown)
{
    strlcpy(s_repl.path, path, sizeof(s_repl.path));
    strlcpy(s_repl.name, name, sizeof(s_repl.name));
    s_repl.copy = copy;
    snprintf(s_body, sizeof(s_body),
             "The card already has a config.conf: the launcher writes one at every change of a setting, also after the "
             "old one was deleted. REPLACE puts %s%s in its place, and its settings apply now. CANCEL leaves the files as "
             "they are.", copy ? "a copy of " : "", shown);
    l5_ask("SD", "Replace config.conf?", s_body, "CANCEL", "REPLACE", L5_ICON_RESTART, replace_cb, NULL);
}

/* a file at `path` would be named /config.conf (any letter case) while the card has one */
static bool onto_config(const char *path)
{
    struct stat st;
    return l5_files_same_name(path, L5_CONFIG_PATH) && stat(L5_CONFIG_PATH, &st) == 0 && !S_ISDIR(st.st_mode);
}

static void paste_done(void)
{
    char a[16], b[16];
    if (J.no_space) {
        l5_fmt_bytes(a, sizeof(a), J.count.alloc);
        l5_fmt_bytes(b, sizeof(b), J.free_bytes);
        snprintf(s_body, sizeof(s_body), "%s needs %s on the card, %s is free. Nothing was copied.", base_name(J.src), a, b);
        l5_info("PASTE", "Not enough space", s_body);
    } else if (!J.copying && J.err != L5_FILES_CANCELLED) { /* counting failed */
        snprintf(s_body, sizeof(s_body), "%s could not be read completely, so nothing was copied.", base_name(J.src));
        l5_info("PASTE", "Paste failed", s_body);
    } else if (J.err == ESP_OK) {
        /* a config.conf pasted into the card root: it became config-copy.conf next to the one the launcher wrote */
        char dir[L5_FILES_PATH], want[L5_FILES_PATH];
        bool config_copy = !J.dir && l5_files_parent(J.dst, dir, sizeof(dir)) &&
                           l5_files_join(want, sizeof(want), dir, base_name(J.src)) && onto_config(want) &&
                           !l5_files_same_name(J.dst, L5_CONFIG_PATH);
        if (config_copy) {
            ask_replace(J.dst, J.name, true, base_name(J.src));
            return;
        }
        if (!J.dir && l5_files_same_name(J.dst, L5_CONFIG_PATH) && l5_cfg_reload_now() == ESP_OK) {
            l5_toast("config.conf put back: its settings apply now"); /* there was none: nothing to replace */
            return;
        }
        l5_toast("Pasted %s%s", J.name, hidden_note(J.name, J.dir));
        l5_sd_after_op(NULL, J.name);
    } else if (J.err == L5_FILES_CANCELLED && !J.res.left) {
        l5_toast("%s", J.res.bytes ? "Paste cancelled: the partial copy was removed" : "Paste cancelled");
        l5_sd_after_op(NULL, NULL);
    } else {
        snprintf(s_body, sizeof(s_body), "%s %s", J.res.why[0] ? J.res.why : "The card did not answer.",
                 J.res.left ? "Part of the copy could not be removed: delete it before you try again."
                            : "The partial copy was removed.");
        l5_info("PASTE", J.err == L5_FILES_CANCELLED ? "Paste cancelled" : "Paste failed", s_body);
        l5_sd_after_op(NULL, NULL);
    }
}

static void delete_done(void)
{
    char parent[L5_FILES_PATH];
    bool up = J.is_cwd && l5_files_parent(J.src, parent, sizeof(parent));
    if (J.err == ESP_OK) {
        clip_deleted(J.src);
        if (J.config) {
            l5_cfg_defaults_now(); /* renders every view, SD without the file */
            l5_toast("config.conf deleted: settings back to the defaults");
            return;
        }
        l5_toast("Deleted %s", J.name);
        l5_sd_after_op(up ? parent : NULL, NULL);
        return;
    }
    uint32_t done = J.res.files + J.res.dirs, total = J.count.files + J.count.dirs;
    if (done) clip_deleted(J.src); /* may be gone in part */
    if (J.err == L5_FILES_CANCELLED) {
        l5_toast("Delete stopped: %u of %u items removed", (unsigned)done, (unsigned)total);
    } else {
        snprintf(s_body, sizeof(s_body), "%s %u of %u items were removed.", J.res.why[0] ? J.res.why : "The card did not answer.",
                 (unsigned)done, (unsigned)total);
        l5_info("DELETE", "Delete stopped", s_body);
    }
    l5_sd_after_op(NULL, done ? NULL : J.name);
}

static void view_done(void)
{
    char note[64], sz[16];
    if (J.err != ESP_OK) {
        l5_info("VIEW", "Cannot show the file", J.why);
        return;
    }
    if (J.text.binary) {
        snprintf(s_body, sizeof(s_body), "%s contains NUL bytes (binary data), so it is not shown.", J.name);
        l5_info("VIEW", "Not a text file", s_body);
        return;
    }
    l5_fmt_bytes(sz, sizeof(sz), J.text.file_size);
    if (J.text.cut) {
        snprintf(note, sizeof(note), "FIRST 64 KB OF %s", sz);
    } else if (!J.text.file_size) {
        snprintf(note, sizeof(note), "EMPTY FILE");
    } else {
        snprintf(note, sizeof(note), "%s \xC2\xB7 %u LINE%s", sz, (unsigned)J.text.lines, J.text.lines == 1 ? "" : "S");
    }
    l5_text_dialog(J.name, note, J.text.text, J.text.pieces, free); /* the dialog owns the text now */
    J.text.text = NULL;
}

static void job_done(void)
{
    switch (J.op) {
    case JOB_PASTE:
        paste_done();
        break;
    case JOB_COUNT_DELETE:
        if (J.err == L5_FILES_CANCELLED) {
            l5_toast("Delete cancelled");
        } else if (J.err != ESP_OK) {
            snprintf(s_body, sizeof(s_body), "%s could not be read completely, so it is not deleted.", J.name);
            l5_info("DELETE", "Cannot delete", s_body);
        } else {
            confirm_delete();
        }
        break;
    case JOB_DELETE:
        delete_done();
        break;
    case JOB_VIEW:
        view_done();
        break;
    }
}

/* ---------- RENAME and NEW FOLDER: keyboard, rules, refusals ---------- */

static void open_keyboard(const char *value);

static void edit_again_cb(bool ok, void *user)
{
    (void)user;
    if (ok) open_keyboard(s_edit.text);
}

/* the name is not taken: a dialog that says why, EDIT NAME opens the keyboard with it again */
static void refuse(const char *title, const char *why)
{
    l5_ask("SD", title, why, "CANCEL", "EDIT NAME", L5_ICON_EDIT, edit_again_cb, NULL);
}

static void keyboard_cb(const char *text, void *user)
{
    (void)user;
    if (!text) return; /* CANCEL */
    strlcpy(s_edit.text, text, sizeof(s_edit.text));
    char name[sizeof(s_edit.text)], why[sizeof(s_edit.text) + 64], dir[L5_FILES_PATH], to[L5_FILES_PATH];
    strlcpy(name, text, sizeof(name));
    if (!l5_files_name_ok(name, why, sizeof(why))) {
        refuse("Name not allowed", why);
        return;
    }
    const char *old = s_edit.mkdir ? "" : base_name(s_edit.path);
    if (!s_edit.mkdir && !strcmp(name, old)) return; /* unchanged */
    if (s_edit.mkdir) {
        strlcpy(dir, s_edit.path, sizeof(dir));
    } else if (!l5_files_parent(s_edit.path, dir, sizeof(dir))) {
        return;
    }
    if (!l5_files_join(to, sizeof(to), dir, name)) {
        snprintf(why, sizeof(why), "The path would be longer than the card allows here (%d characters).", L5_FILES_PATH - 1);
        refuse("Name too long", why);
        return;
    }
    struct stat st;
    if (!s_edit.mkdir && !s_edit.dir && !l5_files_same_name(name, old) && onto_config(to)) {
        ask_replace(s_edit.path, old, false, old); /* a saved config.conf renamed back */
        return;
    }
    if ((s_edit.mkdir || !l5_files_same_name(name, old)) && stat(to, &st) == 0) { /* letter case only: same entry */
        snprintf(why, sizeof(why), "A %s named %s is already in this folder.", S_ISDIR(st.st_mode) ? "folder" : "file", name);
        refuse("Name already used", why);
        return;
    }
    if (s_edit.mkdir) {
        if (l5_files_mkdir(to, why, sizeof(why)) != ESP_OK) {
            l5_info("SD", "Folder not created", why);
            return;
        }
        l5_toast("Folder created: %s", name);
        l5_sd_after_op(NULL, name);
        return;
    }
    if (l5_files_rename(s_edit.path, to, why, sizeof(why)) != ESP_OK) {
        l5_info("SD", "Not renamed", why);
        return;
    }
    clip_renamed(s_edit.path, to);
    if (l5_files_same_name(s_edit.path, L5_CONFIG_PATH)) {
        l5_toast("Renamed: the launcher starts with default settings next time");
    } else {
        l5_toast("Renamed to %s%s", name, hidden_note(name, s_edit.dir));
    }
    if (s_edit.is_cwd) {
        l5_sd_after_op(to, NULL);
    } else {
        l5_sd_after_op(NULL, name);
    }
}

static void open_keyboard(const char *value)
{
    const l5_keyboard_opts_t o = {
        .title = s_edit.mkdir ? "NEW FOLDER NAME" : s_edit.dir ? "RENAME FOLDER" : "RENAME FILE",
        .value = value,
        .max_len = NAME_MAX_CHARS,
    };
    l5_keyboard(&o, keyboard_cb, NULL);
}

/* ---------- the actions ---------- */

static void copy_action(void)
{
    l5_sd_target_t t;
    if (!l5_sd_target(&t) || t.is_cwd) return; /* COPY takes the selected entry */
    strlcpy(s_clip.path, t.path, sizeof(s_clip.path));
    l5_sd_paint_clip();
    l5_toast("Copied %s \xC2\xB7 open a folder and tap PASTE", t.name);
}

static void paste_action(void)
{
    if (!s_clip.path[0]) return;
    char cwd[L5_FILES_PATH], name[256], title[300];
    const char *src = base_name(s_clip.path);
    l5_sd_cwd_path(cwd, sizeof(cwd));
    struct stat st;
    if (stat(s_clip.path, &st) != 0) {
        snprintf(s_body, sizeof(s_body), "%s is no longer on the card. Copy it again.", src);
        s_clip.path[0] = 0;
        l5_sd_paint_clip();
        l5_info("PASTE", "Nothing to paste", s_body);
        return;
    }
    if (S_ISDIR(st.st_mode) && l5_files_inside(cwd, s_clip.path)) {
        snprintf(s_body, sizeof(s_body), "This folder is %s or lies inside it: a folder cannot be pasted into itself.", src);
        l5_info("PASTE", "Cannot paste here", s_body);
        return;
    }
    if (!l5_files_unique(cwd, src, name, sizeof(name)) || !l5_files_join(J.dst, sizeof(J.dst), cwd, name)) {
        snprintf(s_body, sizeof(s_body), "No free name for %s fits into this folder.", src);
        l5_info("PASTE", "Cannot paste here", s_body);
        return;
    }
    strlcpy(J.src, s_clip.path, sizeof(J.src));
    strlcpy(J.name, name, sizeof(J.name));
    J.dir = S_ISDIR(st.st_mode);
    snprintf(title, sizeof(title), "Pasting %s", name);
    job_start(JOB_PASTE, title);
}

static void rename_action(void)
{
    l5_sd_target_t t;
    if (!l5_sd_target(&t)) return;
    s_edit.mkdir = false;
    s_edit.is_cwd = t.is_cwd;
    s_edit.dir = t.dir;
    strlcpy(s_edit.path, t.path, sizeof(s_edit.path));
    open_keyboard(t.name);
}

static void mkdir_action(void)
{
    char name[64];
    s_edit.mkdir = true;
    s_edit.is_cwd = false;
    s_edit.dir = true;
    l5_sd_cwd_path(s_edit.path, sizeof(s_edit.path));
    if (!l5_files_free_name(s_edit.path, "new_folder", name, sizeof(name))) strlcpy(name, "new_folder", sizeof(name));
    open_keyboard(name);
}

static void delete_action(void)
{
    l5_sd_target_t t;
    if (!l5_sd_target(&t)) return;
    strlcpy(J.src, t.path, sizeof(J.src));
    strlcpy(J.name, t.name, sizeof(J.name));
    J.dir = t.dir;
    J.is_cwd = t.is_cwd;
    J.config = !t.dir && l5_files_same_name(t.path, L5_CONFIG_PATH);
    if (t.dir) {
        char title[300];
        snprintf(title, sizeof(title), "Counting %s", t.name);
        job_start(JOB_COUNT_DELETE, title); /* the confirmation names how much goes */
        return;
    }
    memset(&J.count, 0, sizeof(J.count));
    J.count.files = 1;
    confirm_delete();
}

static void view_action(void)
{
    l5_sd_target_t t;
    if (!l5_sd_target(&t) || t.dir) return;
    char title[300];
    strlcpy(J.src, t.path, sizeof(J.src));
    strlcpy(J.name, t.name, sizeof(J.name));
    snprintf(title, sizeof(title), "Opening %s", t.name);
    job_start(JOB_VIEW, title);
}

void l5_sd_action(const char *id)
{
    if (J.busy || l5_install_busy()) return;
    if (!strcmp(id, "copy")) {
        copy_action();
    } else if (!strcmp(id, "paste")) {
        paste_action();
    } else if (!strcmp(id, "rename")) {
        rename_action();
    } else if (!strcmp(id, "mkdir")) {
        mkdir_action();
    } else if (!strcmp(id, "delete")) {
        delete_action();
    } else if (!strcmp(id, "view")) {
        view_action();
    }
}
