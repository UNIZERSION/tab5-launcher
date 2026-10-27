/* U.keyboard (ui.js): on-screen keyboard over the whole screen, panel at the bottom (.kb-wrap,
   .kb). l5_keyboard() returns at once and the text arrives in the callback, NULL for CANCEL, like
   the prototype's promise. Two layouts (abc, ?123), one-shot SHIFT, masked input with SHOW / HIDE.
   Text is typed at the caret; the field shows the text around it ("..." where it is cut) when it is longer than
   the field.
   With the M5Stack Tab5 Keyboard plugged in (l5_kbd.c) the panel sits in the middle of the screen with only the
   field and a bar (hint, CANCEL, OK). Its keys (l5_keys.c sends them here as l5_key_event()) edit as on a
   computer: characters type at the caret (Alt + a c e l n o s x z: Polish letters), Left / Right and Home / End
   move it, Ctrl+Left / Right by a word, Backspace / Delete erase before / after it, Ctrl+Backspace / Delete a word,
   Ctrl+U = CLEAR, Ctrl+C / Ctrl+X copy / cut the whole text (not a password), Ctrl+V pastes, Enter = OK,
   Esc = CANCEL, F1 the shortcuts. A tap on the field brings the on-screen keys; plugging the keyboard in or pulling
   it out switches the layout. */
#include <stdio.h>
#include <string.h>
#include "l5_files.h"
#include "l5_kbd.h"
#include "l5_ui.h"

#define DOT "\xC2\xB7"
#define HW_HINT "KEYBOARD " DOT " ENTER = OK " DOT " ESC = CANCEL " DOT " F1 = KEYS " DOT " TAP THE FIELD FOR ON-SCREEN KEYS"

#define ROWS 5
#define COLS 11
#define KEY_H 64

/* both layouts have the same shape, so the keys are built once and only relabelled */
static const char *const LAYOUT[2][ROWS][COLS + 1] = {
    {
        {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "BKSP", NULL},
        {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "-", NULL},
        {"a", "s", "d", "f", "g", "h", "j", "k", "l", "_", ".", NULL},
        {"SHIFT", "z", "x", "c", "v", "b", "n", "m", "/", ":", "@", NULL},
        {"SYM", "SPACE", "CLEAR", "CANCEL", "OK", NULL},
    },
    {
        {"!", "#", "$", "%", "&", "*", "(", ")", "=", "+", "BKSP", NULL},
        {"[", "]", "{", "}", "<", ">", "^", "~", "`", "|", "\\", NULL},
        /* the prototype has a euro sign where the dot is; none of the launcher's fonts has U+20AC */
        {";", ",", "'", "\"", "?", ".", "\xC2\xA3", "\xC2\xB0", "\xC2\xA7", "1", "2", NULL},
        {"SHIFT", "3", "4", "5", "6", "7", "8", "9", "0", "/", ":", NULL},
        {"ABC", "SPACE", "CLEAR", "CANCEL", "OK", NULL},
    },
};

/* KEYLBL / KEYCLS: label and flex share (.key 1, .w15 1.5, .w2 2, .w5 5, doubled) */
static const struct {
    const char *key, *label;
    uint8_t grow;
} FN[] = {
    {"BKSP", LV_SYMBOL_BACKSPACE, 3}, {"SHIFT", LV_SYMBOL_UP " SHIFT", 3}, {"SYM", "?123", 3},
    {"ABC", "ABC", 3},                {"SPACE", "SPACE", 10},              {"CLEAR", "CLEAR", 3},
    {"CANCEL", "CANCEL", 4},          {"OK", "OK", 4},
};
#define NFN (sizeof(FN) / sizeof(FN[0]))
#define EYE_ID 0xff

typedef struct {
    l5_keyboard_cb_t cb;
    void *user;
    char *text;  /* UTF-8 */
    char *dots;  /* mask: one bullet per character */
    size_t len, cap;
    size_t pos;  /* the caret: a byte index of text, on a character boundary */
    uint16_t chars, max_chars;
    bool shift, sym, mask, show;
    lv_obj_t *wrap, *field, *tx, *caret, *tx2, *eye; /* tx before the caret, tx2 after it */
    lv_obj_t *key[ROWS][COLS];
    lv_obj_t *kb;         /* the panel */
    lv_obj_t *row[ROWS];
    lv_obj_t *bar;        /* hint, CANCEL, OK: shown instead of the rows while the keyboard is plugged in */
    bool compact;         /* keyboard plugged in: the on-screen keys are hidden */
    bool keys_on;         /* the field was tapped: on-screen keys even with the keyboard plugged in */
    lv_timer_t *blink;
} kb_t;

static lv_style_t s_key, s_key_fn, s_key_pr, s_key_ok;
static char s_clip[64 * 4 + 1]; /* Ctrl+C / Ctrl+X of a text field, Ctrl+V in any of them */
static lv_font_t s_font_fn, s_font_field;

static int fn_index(const char *key)
{
    for (size_t i = 0; i < NFN; i++) {
        if (!strcmp(FN[i].key, key)) return (int)i;
    }
    return -1;
}

static void styles_init(void)
{
    static bool done;
    if (!done) {
        /* the backspace / arrow glyphs come from LVGL's Montserrat, pound / section from Chakra Petch */
        s_font_fn = l5_mono_16;
        s_font_fn.fallback = &lv_font_montserrat_14;
        s_font_field = l5_mono_22;
        s_font_field.fallback = &l5_chakra_b24;

        lv_style_init(&s_key); /* .key */
        lv_style_set_radius(&s_key, 4);
        lv_style_set_bg_opa(&s_key, LV_OPA_COVER);
        lv_style_set_bg_grad_dir(&s_key, LV_GRAD_DIR_VER);
        /* 0 3px 0 #000: the colour below with the theme's */
        lv_style_set_shadow_width(&s_key, 2);
        lv_style_set_shadow_offset_y(&s_key, 3);
        lv_style_set_shadow_opa(&s_key, LV_OPA_COVER);
        lv_style_set_border_side(&s_key, LV_BORDER_SIDE_TOP); /* inset 0 1px 0 rgba(255,255,255,.08) */
        lv_style_set_border_width(&s_key, 1);
        lv_style_set_border_color(&s_key, L5_WHITE);
        lv_style_set_border_opa(&s_key, 20);
        lv_style_set_text_font(&s_key, &l5_chakra_b24);

        lv_style_init(&s_key_fn); /* .key.fn */
        lv_style_set_text_font(&s_key_fn, &s_font_fn);
        lv_style_set_text_letter_space(&s_key_fn, 1);

        lv_style_init(&s_key_ok); /* .key.ok */
        lv_style_init(&s_key_pr); /* .key:active */
        lv_style_set_translate_y(&s_key_pr, 2);
        lv_style_set_shadow_offset_y(&s_key_pr, 1);
        done = true;
    }
    /* the theme and the accent can change between two keyboards (CFG > Theme, UI color) */
    lv_style_set_bg_color(&s_key, L5C.key_top);
    lv_style_set_bg_grad_color(&s_key, L5C.key_bot);
    lv_style_set_shadow_color(&s_key, L5C.btn_shadow);
    lv_style_set_text_color(&s_key, L5_ON_PANEL);
    lv_style_set_bg_color(&s_key_fn, L5C.keyfn_top);
    lv_style_set_bg_grad_color(&s_key_fn, L5C.keyfn_bot);
    lv_style_set_bg_color(&s_key_ok, L5P.accent);
    lv_style_set_bg_grad_color(&s_key_ok, L5P.accent_lo);
    lv_style_set_bg_color(&s_key_pr, L5P.accent);
    lv_style_set_bg_grad_color(&s_key_pr, L5P.accent);
}

/* ---------- text ---------- */

static const char *utf8_next(const char *s)
{
    if (*s) {
        s++;
        while ((*s & 0xC0) == 0x80) s++;
    }
    return s;
}

static uint16_t utf8_count(const char *s)
{
    uint16_t n = 0;
    for (; *s; s = utf8_next(s)) n++;
    return n;
}

static void caret_on(kb_t *k)
{
    lv_obj_set_style_opa(k->caret, LV_OPA_COVER, 0);
    lv_timer_reset(k->blink);
}

static int32_t width(const char *s, size_t n)
{
    return lv_text_get_width(s, (uint32_t)n, &s_font_field, 0);
}

/* the text before the caret in tx, after it in tx2; too long: the part next to the caret, cut with "..." */
static void paint(kb_t *k)
{
    const char *all = k->text;
    size_t split = k->pos;
    if (k->mask && !k->show) {
        size_t n = 0;
        for (uint16_t i = 0; i < k->chars; i++, n += 3) memcpy(&k->dots[n], "\xE2\x80\xA2", 3);
        k->dots[n] = '\0';
        split = 0;
        for (const char *s = k->text; s < k->text + k->pos; s = utf8_next(s)) split += 3;
        all = k->dots;
    }
    const size_t n = strlen(all);
    const int32_t room = lv_obj_get_content_width(k->field) - lv_obj_get_width(k->caret) - 4;
    const int32_t wb = width(all, split), wa = width(all + split, n - split);
    size_t from = 0, after = n - split; /* shown: all[from..split) and all[split..split + after) */
    bool cut_b = false, cut_a = false;
    if (wb + wa > room) {
        const int32_t ell = width("\xE2\x80\xA6", 3);
        int32_t ra = room - wb > room / 3 ? room - wb : room / 3; /* the text after the caret gets a third at least */
        if (ra > wa) ra = wa;
        const int32_t rb = room - ra;
        while (wa > ra && after && width(all + split, after) > ra - ell) { /* from the right */
            after--;
            while (after && ((uint8_t)all[split + after] & 0xC0) == 0x80) after--;
            cut_a = true;
        }
        while (wb > rb && from < split && width(all + from, split - from) > rb - ell) { /* from the left */
            from = (size_t)(utf8_next(all + from) - all);
            cut_b = true;
        }
    }
    lv_label_set_text_fmt(k->tx, "%s%.*s", cut_b ? "\xE2\x80\xA6" : "", (int)(split - from), all + from);
    lv_label_set_text_fmt(k->tx2, "%.*s%s", (int)after, all + split, cut_a ? "\xE2\x80\xA6" : "");
    if (k->eye) lv_label_set_text(lv_obj_get_child(k->eye, 0), k->show ? "HIDE" : "SHOW");
    caret_on(k);
}

static void relabel(kb_t *k)
{
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS && LAYOUT[k->sym][r][c]; c++) {
            const char *key = LAYOUT[k->sym][r][c];
            lv_obj_t *l = lv_obj_get_child(k->key[r][c], 0);
            int f = fn_index(key);
            if (f >= 0) {
                lv_label_set_text(l, FN[f].label);
                if (!strcmp(key, "SHIFT")) { /* .key.on */
                    lv_obj_set_style_text_color(l, k->shift ? L5P.accent : L5_ON_PANEL, 0);
                }
            } else if (k->shift && key[1] == '\0' && key[0] >= 'a' && key[0] <= 'z') {
                char up[2] = {(char)(key[0] - 'a' + 'A'), '\0'};
                lv_label_set_text(l, up);
            } else {
                lv_label_set_text(l, key);
            }
        }
    }
}

/* one character (UTF-8) at the caret */
static void append(kb_t *k, const char *s)
{
    size_t n = strlen(s);
    if (k->chars >= k->max_chars || k->len + n >= k->cap) return; /* maxLen */
    memmove(&k->text[k->pos + n], &k->text[k->pos], k->len - k->pos + 1);
    memcpy(&k->text[k->pos], s, n);
    k->len += n;
    k->pos += n;
    k->chars++;
}

/* text[from..to) goes */
static void erase(kb_t *k, size_t from, size_t to)
{
    if (to <= from) return;
    for (const char *s = k->text + from; s < k->text + to; s = utf8_next(s)) k->chars--;
    memmove(&k->text[from], &k->text[to], k->len - to + 1);
    k->len -= to - from;
    if (k->pos > to) k->pos -= to - from;
    else if (k->pos > from) k->pos = from;
}

static size_t prev_char(const kb_t *k, size_t i)
{
    while (i > 0 && ((uint8_t)k->text[--i] & 0xC0) == 0x80) {
    }
    return i;
}

/* Ctrl+Left / Right: to the start of the word before, or past the word after (words end at spaces) */
static size_t word_left(const kb_t *k, size_t i)
{
    while (i > 0 && k->text[prev_char(k, i)] == ' ') i = prev_char(k, i);
    while (i > 0 && k->text[prev_char(k, i)] != ' ') i = prev_char(k, i);
    return i;
}

static size_t word_right(const kb_t *k, size_t i)
{
    while (i < k->len && k->text[i] != ' ') i = (size_t)(utf8_next(k->text + i) - k->text);
    while (i < k->len && k->text[i] == ' ') i++;
    return i;
}


static void close_kb(kb_t *k, bool ok)
{
    l5_keyboard_cb_t cb = k->cb;
    void *user = k->user;
    char *out = ok ? lv_strdup(k->text) : NULL;
    lv_obj_delete(k->wrap); /* frees k */
    if (cb) cb(out, user);
    if (out) {
        memset(out, 0, strlen(out)); /* may be a password */
        lv_free(out);
    }
}

static void press(kb_t *k, const char *key)
{
    if (!strcmp(key, "OK")) {
        close_kb(k, true);
        return;
    }
    if (!strcmp(key, "CANCEL")) {
        close_kb(k, false);
        return;
    }
    if (!strcmp(key, "BKSP")) {
        erase(k, prev_char(k, k->pos), k->pos);
    } else if (!strcmp(key, "CLEAR")) {
        k->text[0] = '\0';
        k->len = k->chars = 0;
        k->pos = 0;
    } else if (!strcmp(key, "SHIFT")) {
        k->shift = !k->shift;
        relabel(k);
    } else if (!strcmp(key, "SYM") || !strcmp(key, "ABC")) {
        k->sym = !strcmp(key, "SYM");
        relabel(k);
    } else if (!strcmp(key, "EYE")) {
        k->show = !k->show;
    } else if (!strcmp(key, "SPACE")) {
        append(k, " ");
    } else {
        char up[2] = {key[0], '\0'};
        if (k->shift && key[1] == '\0' && key[0] >= 'a' && key[0] <= 'z') {
            up[0] = (char)(key[0] - 'a' + 'A');
            key = up;
        }
        append(k, key);
        if (k->shift && !k->sym) { /* one-shot on the letters */
            k->shift = false;
            relabel(k);
        }
    }
    paint(k);
}

/* ---------- events ---------- */

/* the on-screen keys, or (keyboard plugged in) only the field and the bar, the panel in the middle */
static void layout(kb_t *k, bool compact)
{
    k->compact = compact;
    for (int r = 0; r < ROWS; r++) lv_obj_set_flag(k->row[r], LV_OBJ_FLAG_HIDDEN, compact);
    lv_obj_set_flag(k->bar, LV_OBJ_FLAG_HIDDEN, !compact);
    lv_obj_align(k->kb, compact ? LV_ALIGN_CENTER : LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_update_layout(k->wrap); /* paint() measures the field */
    paint(k);
}

static void key_cb(lv_event_t *e)
{
    kb_t *k = lv_event_get_user_data(e);
    int id = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    if (id == EYE_ID) {
        press(k, "EYE");
        return;
    }
    const char *key = LAYOUT[k->sym][id >> 4][id & 0xf];
    lv_event_code_t code = lv_event_get_code(e);
    bool bksp = !strcmp(key, "BKSP");
    /* backspace repeats while held; every other key types on release */
    if (bksp ? code != LV_EVENT_CLICKED : code == LV_EVENT_CLICKED) press(k, key);
}

/* keys of the Tab5 Keyboard */
static void kb_key_cb(lv_event_t *e)
{
    kb_t *k = lv_event_get_user_data(e);
    const l5_key_t *key = lv_event_get_param(e);
    switch (key->kind) {
    case L5_KEY_ENTER: press(k, "OK"); return;      /* k is gone */
    case L5_KEY_ESC: press(k, "CANCEL"); return;    /* k is gone */
    case L5_KEY_F1: l5_keys_help(); return;
    case L5_KEY_BACKSPACE: erase(k, key->ctrl ? word_left(k, k->pos) : prev_char(k, k->pos), k->pos); break;
    case L5_KEY_DELETE:
        erase(k, k->pos, key->ctrl ? word_right(k, k->pos) : (size_t)(utf8_next(k->text + k->pos) - k->text));
        break;
    case L5_KEY_LEFT: k->pos = key->ctrl ? word_left(k, k->pos) : prev_char(k, k->pos); break;
    case L5_KEY_RIGHT:
        k->pos = key->ctrl ? word_right(k, k->pos) : (size_t)(utf8_next(k->text + k->pos) - k->text);
        break;
    case L5_KEY_HOME: k->pos = 0; break;
    case L5_KEY_END: k->pos = k->len; break;
    case L5_KEY_CHAR:
        if (key->ctrl) {
            switch (key->ch | 0x20) {
            case 'u': press(k, "CLEAR"); return; /* Ctrl+U, as in a shell */
            case 'c':
            case 'x': /* the whole text: the field has no selection */
                if (k->mask) return; /* a password stays where it is */
                snprintf(s_clip, sizeof(s_clip), "%s", k->text);
                if ((key->ch | 0x20) == 'x') press(k, "CLEAR");
                l5_toast((key->ch | 0x20) == 'x' ? "Text cut" : "Text copied");
                return;
            case 'v':
                for (const char *s = s_clip; *s; s = utf8_next(s)) { /* as typed: up to the field's limit */
                    char c[5] = {0};
                    memcpy(c, s, (size_t)(utf8_next(s) - s) < 4 ? (size_t)(utf8_next(s) - s) : 4);
                    append(k, c);
                }
                break;
            default: return;
            }
        } else if (!key->alt) {
            char s[5] = {0};
            l5_files_utf8(s, l5_key_char(key));
            append(k, s);
        } else {
            return;
        }
        break;
    default: return;
    }
    paint(k);
}

/* the caret; plugging the keyboard in or pulling it out switches the layout */
static void blink_cb(lv_timer_t *t)
{
    kb_t *k = lv_timer_get_user_data(t);
    bool on = lv_obj_get_style_opa(k->caret, 0) == LV_OPA_COVER;
    lv_obj_set_style_opa(k->caret, on ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    bool compact = l5_kbd_present() && !k->keys_on;
    if (k->compact != compact) layout(k, compact);
}

static void field_cb(lv_event_t *e)
{
    kb_t *k = lv_event_get_user_data(e);
    if (k->compact) {
        k->keys_on = true;
        layout(k, false);
    }
}

/* CANCEL (user data 0) and OK (1) on the bar */
static void bar_cb(lv_event_t *e)
{
    kb_t *k = lv_event_get_user_data(e);
    press(k, lv_obj_get_user_data(lv_event_get_current_target(e)) ? "OK" : "CANCEL");
}

static void delete_cb(lv_event_t *e)
{
    kb_t *k = lv_event_get_user_data(e);
    lv_timer_delete(k->blink);
    memset(k->text, 0, k->cap);
    lv_free(k->text);
    lv_free(k->dots);
    lv_free(k);
}

/* ---------- building ---------- */

static lv_obj_t *key_obj(kb_t *k, lv_obj_t *row, bool fn, int id)
{
    lv_obj_t *b = lv_button_create(row);
    lv_obj_remove_style_all(b);
    lv_obj_add_style(b, &s_key, 0);
    if (fn) lv_obj_add_style(b, &s_key_fn, 0);
    lv_obj_add_style(b, &s_key_pr, LV_STATE_PRESSED);
    l5_dither_hook(b);
    lv_obj_set_height(b, KEY_H);
    lv_obj_set_user_data(b, (void *)(intptr_t)id);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, key_cb, LV_EVENT_CLICKED, k);
    return b;
}

void l5_keyboard(const l5_keyboard_opts_t *opts, l5_keyboard_cb_t cb, void *user)
{
    styles_init();
    kb_t *k = lv_zalloc(sizeof(kb_t));
    const char *value = opts && opts->value ? opts->value : "";
    k->cb = cb;
    k->user = user;
    k->mask = opts && opts->mask;
    k->show = !k->mask;
    k->max_chars = opts && opts->max_len ? opts->max_len : 64;
    k->cap = strlen(value) + k->max_chars * 4u + 1;
    k->text = lv_zalloc(k->cap);
    strcpy(k->text, value);
    k->len = k->pos = strlen(value); /* the caret after the text */
    k->chars = utf8_count(value);
    if (k->mask) k->dots = lv_zalloc((k->chars > k->max_chars ? k->chars : k->max_chars) * 3u + 1);

    /* .kb-wrap: dims the screen and swallows taps, the panel sits at the bottom */
    k->wrap = l5_box(l5_ui_layer());
    lv_obj_set_size(k->wrap, L5_SCREEN_W, L5_SCREEN_H);
    lv_obj_set_style_bg_opa(k->wrap, 140, 0);
    lv_obj_set_style_bg_color(k->wrap, L5C.scrim, 0);
    lv_obj_add_flag(k->wrap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(k->wrap, delete_cb, LV_EVENT_DELETE, k);
    lv_obj_add_event_cb(k->wrap, kb_key_cb, l5_key_event(), k);

    lv_obj_t *kb = l5_box(k->wrap); /* .kb.dark */
    k->kb = kb;
    l5_panel(kb, true);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(kb, L5_SCREEN_W, LV_SIZE_CONTENT);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_top(kb, 14, 0);
    lv_obj_set_style_pad_hor(kb, 16, 0);
    lv_obj_set_style_pad_bottom(kb, 16, 0);
    lv_obj_set_style_pad_row(kb, 8, 0);
    lv_obj_set_flex_flow(kb, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *top = l5_box(kb); /* .kb-top */
    lv_obj_set_size(top, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_bottom(top, 4, 0);
    lv_obj_set_style_pad_column(top, 14, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *q = l5_label(top, &l5_mono_14, L5_ON_PANEL_MUTED, opts && opts->title ? opts->title : "INPUT");
    lv_obj_set_style_text_letter_space(q, 2, 0);
    k->field = l5_box(top); /* .kb-field */
    lv_obj_set_height(k->field, 56);
    lv_obj_set_flex_grow(k->field, 1);
    lv_obj_set_style_pad_hor(k->field, 16, 0);
    lv_obj_set_style_bg_opa(k->field, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(k->field, L5C.well, 0);
    lv_obj_set_style_border_width(k->field, 1, 0);
    lv_obj_set_style_border_color(k->field, L5P.accent, 0);
    lv_obj_set_style_radius(k->field, 3, 0);
    lv_obj_set_flex_flow(k->field, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(k->field, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(k->field, LV_OBJ_FLAG_CLICKABLE); /* with the keyboard plugged in: the on-screen keys */
    lv_obj_add_event_cb(k->field, field_cb, LV_EVENT_CLICKED, k);
    k->tx = l5_label(k->field, &s_font_field, L5_ON_PANEL, "");
    k->caret = l5_box(k->field);
    lv_obj_set_size(k->caret, 2, 30);
    lv_obj_set_style_margin_left(k->caret, 1, 0);
    lv_obj_set_style_margin_right(k->caret, 1, 0);
    lv_obj_set_style_bg_opa(k->caret, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(k->caret, L5P.accent, 0);
    k->tx2 = l5_label(k->field, &s_font_field, L5_ON_PANEL, "");
    if (k->mask) { /* SHOW / HIDE */
        k->eye = key_obj(k, top, true, EYE_ID);
        lv_obj_set_size(k->eye, 110, 56);
    }

    for (int r = 0; r < ROWS; r++) { /* .kb-row */
        lv_obj_t *row = l5_box(kb);
        k->row[r] = row;
        lv_obj_set_size(row, lv_pct(100), KEY_H);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE); /* key shadow and press offset */
        for (int c = 0; c < COLS && LAYOUT[0][r][c]; c++) {
            int f = fn_index(LAYOUT[0][r][c]);
            lv_obj_t *b = key_obj(k, row, f >= 0, r << 4 | c);
            lv_obj_set_flex_grow(b, f >= 0 ? FN[f].grow : 2);
            if (f >= 0 && !strcmp(FN[f].key, "OK")) lv_obj_add_style(b, &s_key_ok, 0);
            if (f >= 0 && !strcmp(FN[f].key, "BKSP")) {
                lv_obj_add_event_cb(b, key_cb, LV_EVENT_SHORT_CLICKED, k);
                lv_obj_add_event_cb(b, key_cb, LV_EVENT_LONG_PRESSED, k);
                lv_obj_add_event_cb(b, key_cb, LV_EVENT_LONG_PRESSED_REPEAT, k);
            }
            k->key[r][c] = b;
        }
    }

    k->bar = l5_box(kb); /* keyboard plugged in: what the keys do, and the two actions for a finger */
    lv_obj_set_size(k->bar, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(k->bar, 10, 0);
    lv_obj_set_style_pad_bottom(k->bar, 3, 0); /* room for the 3px button shadow */
    lv_obj_set_style_pad_column(k->bar, 12, 0);
    lv_obj_set_flex_flow(k->bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(k->bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *hint = l5_label(k->bar, &l5_mono_14, L5_ON_PANEL_MUTED, HW_HINT);
    lv_obj_set_style_text_letter_space(hint, 1, 0);
    lv_obj_set_flex_grow(hint, 1);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = l5_act_button(k->bar, i ? L5_ICON_CHECK : L5_ICON_X, i ? "OK" : "CANCEL", i == 1);
        lv_obj_set_style_min_width(b, 170, 0);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, bar_cb, LV_EVENT_CLICKED, k);
    }

    k->blink = lv_timer_create(blink_cb, 500, k); /* @keyframes blink, steps(1) */
    relabel(k);
    layout(k, l5_kbd_present());
}
