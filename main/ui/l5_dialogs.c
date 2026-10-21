/* ui.js modal / menu / progress / toast. Overlays live in #layer so they follow rotation.
   Keys of the Tab5 Keyboard (l5_keys.c sends them to the topmost overlay as l5_key_event()):
   - menu: Up / Down, Tab / Shift+Tab, Home / End move the focus ring, Enter picks, Esc closes;
   - modal: Left / Right, Tab / Shift+Tab move between the actions, Enter takes the focused one (none focused: the
     primary, the last), Esc the cancel action, Up / Down / PgUp / PgDn / Home / End scroll a long text;
   - text viewer: Enter / Esc close, the rest scrolls; progress: Esc is CANCEL. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "l5_ui.h"

#define MAX_ITEMS 16 /* the list scrolls inside the 620 px modal */
#define MAX_BTNS 4

typedef struct {
    l5_menu_cb_t menu_cb;
    l5_confirm_cb_t confirm_cb; /* modal_actions: cb(index == ok_index) */
    l5_choose_cb_t choose_cb;
    void *user;
    char *ids[MAX_ITEMS];
    int dismiss_index;    /* action taken when the scrim is tapped */
    int ok_index;
    bool dismissable;
    lv_obj_t *list;          /* menu: the items */
    lv_obj_t *body;          /* modal: the text, scrolls when long */
    lv_obj_t *btn[MAX_BTNS]; /* modal: the actions in order, the last one primary */
    int nbtn;
    int focus;               /* keyboard: focused item or action, -1 = none yet */
    int cancel_index;        /* Esc: the dismiss action, else the one labelled CANCEL; -1 = none */
} dlg_t;

/* LV_STATE_FOCUS_KEY on objs[focus] only (L5S.focus draws the ring), scrolled into view */
static void focus_paint(lv_obj_t *const *objs, int n, int focus)
{
    for (int i = 0; i < n; i++) lv_obj_set_state(objs[i], LV_STATE_FOCUS_KEY, i == focus);
    if (focus >= 0 && focus < n) lv_obj_scroll_to_view(objs[focus], LV_ANIM_OFF);
}

/* a text that scrolls: 48 px a line, a page is its height less 40 px */
static void scroll_key(lv_obj_t *obj, const l5_key_t *k)
{
    int32_t h = lv_obj_get_height(obj);
    switch (k->kind) {
    case L5_KEY_UP: lv_obj_scroll_by_bounded(obj, 0, 48, LV_ANIM_OFF); break;
    case L5_KEY_DOWN: lv_obj_scroll_by_bounded(obj, 0, -48, LV_ANIM_OFF); break;
    case L5_KEY_PGUP: lv_obj_scroll_by_bounded(obj, 0, h - 40, LV_ANIM_OFF); break;
    case L5_KEY_PGDN: lv_obj_scroll_by_bounded(obj, 0, 40 - h, LV_ANIM_OFF); break;
    case L5_KEY_HOME: lv_obj_scroll_to_y(obj, 0, LV_ANIM_OFF); break;
    case L5_KEY_END: lv_obj_scroll_to_y(obj, LV_COORD_MAX, LV_ANIM_OFF); break;
    default: break;
    }
}

struct l5_progress {
    lv_obj_t *scrim, *bar, *label, *pct, *cancel_btn;
    volatile bool *cancel;
};

static void dlg_delete_cb(lv_event_t *e)
{
    dlg_t *d = lv_event_get_user_data(e);
    for (int i = 0; i < MAX_ITEMS; i++) {
        lv_free(d->ids[i]);
    }
    lv_free(d);
}

static lv_obj_t *scrim_create(dlg_t *d)
{
    l5_ui_wake(); /* a dialog that opens on its own (end of an install) is seen on a dimmed or dark screen too */
    lv_obj_t *s = l5_box(l5_ui_layer());
    lv_obj_add_style(s, &L5S.scrim, 0);
    lv_obj_set_size(s, L5_SCREEN_W, L5_SCREEN_H);
    lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
    if (d) {
        lv_obj_add_event_cb(s, dlg_delete_cb, LV_EVENT_DELETE, d);
    }
    return s;
}

/* .modal.dark.chamfer with the .mh kind row and the .mt title */
static lv_obj_t *modal_create(lv_obj_t *scrim, int32_t width, const char *kind, const char *title)
{
    lv_obj_t *m = l5_box(scrim);
    l5_panel(m, true);
    lv_obj_add_flag(m, LV_OBJ_FLAG_CLICKABLE); /* taps inside do not reach the scrim */
    lv_obj_set_width(m, width);
    lv_obj_set_height(m, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(m, 620, 0);
    lv_obj_center(m);
    lv_obj_set_flex_flow(m, LV_FLEX_FLOW_COLUMN);
    l5_chamfer(m, L5_CHAMFER, lv_color_hex(0x5a5b5e)); /* plate under the .62 scrim */

    lv_obj_t *mh = l5_box(m);
    lv_obj_set_size(mh, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(mh, 24, 0);
    lv_obj_set_style_pad_hor(mh, 28, 0);
    lv_obj_set_style_pad_bottom(mh, 8, 0);
    lv_obj_set_style_pad_column(mh, 12, 0);
    lv_obj_set_flex_flow(mh, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mh, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    l5_dot(mh);
    lv_obj_t *k = l5_label(mh, &l5_mono_15, L5_ON_PANEL, kind);
    lv_obj_set_style_text_letter_space(k, 2, 0);

    if (title && *title) {
        lv_obj_t *t = l5_label(m, &l5_chakra_b28, L5_ON_PANEL, title);
        lv_obj_set_width(t, lv_pct(100));
        lv_obj_set_style_pad_top(t, 4, 0);
        lv_obj_set_style_pad_hor(t, 28, 0);
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    }
    return m;
}

void l5_dialogs_close_all(void)
{
    lv_obj_clean(l5_ui_layer());
}

/* ---------- menu (Launcher option list) ---------- */

static void menu_close(lv_obj_t *scrim, const char *id)
{
    dlg_t *d = lv_obj_get_user_data(scrim);
    l5_menu_cb_t cb = d->menu_cb;
    void *user = d->user;
    char picked[24] = "";
    if (id) {
        strlcpy(picked, id, sizeof(picked));
    }
    lv_obj_delete(scrim); /* frees d */
    if (cb) {
        cb(id ? picked : NULL, user);
    }
}

static void menu_item_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_current_target(e);
    lv_obj_t *scrim = lv_event_get_user_data(e);
    dlg_t *d = lv_obj_get_user_data(scrim);
    int i = (int)(intptr_t)lv_obj_get_user_data(btn);
    menu_close(scrim, d->ids[i]);
}

static void menu_scrim_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) {
        menu_close(lv_event_get_current_target(e), NULL);
    }
}

static void menu_key_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_current_target(e);
    dlg_t *d = lv_obj_get_user_data(s);
    const l5_key_t *k = lv_event_get_param(e);
    lv_obj_t *items[MAX_ITEMS];
    int n = (int)lv_obj_get_child_count(d->list);
    if (n > MAX_ITEMS) n = MAX_ITEMS;
    for (int i = 0; i < n; i++) items[i] = lv_obj_get_child(d->list, i);
    int f = d->focus;
    bool up = k->kind == L5_KEY_UP || (k->kind == L5_KEY_TAB && k->shift);
    switch (k->kind) {
    case L5_KEY_TAB:
        if (k->ctrl) return;
        /* fall through */
    case L5_KEY_UP:
    case L5_KEY_DOWN:
        if (up) {
            f = f < 0 ? n - 1 : f > 0 ? f - 1 : 0;
        } else {
            f = f < 0 ? 0 : f + 1 < n ? f + 1 : n - 1;
        }
        break;
    case L5_KEY_HOME: f = 0; break;
    case L5_KEY_END: f = n - 1; break;
    case L5_KEY_ENTER:
        if (f >= 0 && f < n) menu_close(s, d->ids[(int)(intptr_t)lv_obj_get_user_data(items[f])]);
        return;
    case L5_KEY_ESC: menu_close(s, NULL); return;
    default: return;
    }
    d->focus = f;
    focus_paint(items, n, f);
}

void l5_menu(const char *kind, const char *title, const l5_menu_item_t *items, int n, l5_menu_cb_t cb, void *user)
{
    dlg_t *d = lv_zalloc(sizeof(dlg_t));
    d->menu_cb = cb;
    d->user = user;
    d->focus = -1;
    lv_obj_t *s = scrim_create(d);
    lv_obj_set_user_data(s, d);
    lv_obj_add_event_cb(s, menu_scrim_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s, menu_key_cb, l5_key_event(), NULL);
    lv_obj_t *m = modal_create(s, 520, kind, title);
    lv_obj_set_style_pad_bottom(lv_obj_get_child(m, -1), 12, 0);

    lv_obj_t *list = l5_box(m);
    lv_obj_set_size(list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(list, 620 - 140, 0); /* .menu-list { overflow-y: auto } */
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE); /* clickable: a drag between rows scrolls too */
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_style_pad_top(list, 4, 0);
    lv_obj_set_style_pad_bottom(list, 14, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    d->list = list;
    int slot = 0;
    for (int i = 0; i < n && slot < MAX_ITEMS; i++) {
        if (!items[i].id) {
            continue;
        }
        d->ids[slot] = lv_strdup(items[i].id);
        lv_obj_t *b = lv_button_create(list);
        lv_obj_remove_style_all(b);
        lv_obj_add_style(b, &L5S.menu_btn, 0);
        lv_obj_add_style(b, &L5S.menu_btn_pr, LV_STATE_PRESSED);
        lv_obj_add_style(b, &L5S.focus, LV_STATE_FOCUS_KEY);
        lv_color_t c = items[i].danger ? L5_DANGER : L5_ON_PANEL;
        lv_obj_set_style_text_color(b, c, 0);
        l5_icon(b, &l5_icon_22, items[i].danger ? L5_DANGER : L5_ON_PANEL_MUTED, items[i].icon ? items[i].icon : L5_ICON_CHEV);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, items[i].label);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(l, 1);
        lv_obj_set_user_data(b, (void *)(intptr_t)slot);
        lv_obj_add_event_cb(b, menu_item_cb, LV_EVENT_CLICKED, s);
        slot++;
    }
}

/* ---------- modal with actions (U.modal / U.confirm / U.info) ---------- */

static void choose_close(lv_obj_t *scrim, int index)
{
    dlg_t *d = lv_obj_get_user_data(scrim);
    l5_choose_cb_t cb = d->choose_cb;
    l5_confirm_cb_t ccb = d->confirm_cb;
    bool ok = index == d->ok_index;
    void *user = d->user;
    lv_obj_delete(scrim); /* frees d */
    if (ccb) {
        ccb(ok, user);
    } else if (cb) {
        cb(index, user);
    }
}

static void choose_btn_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_current_target(e);
    choose_close(lv_event_get_user_data(e), (int)(intptr_t)lv_obj_get_user_data(btn));
}

static void choose_scrim_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_current_target(e);
    dlg_t *d = lv_obj_get_user_data(s);
    if (lv_event_get_target(e) == s && d->dismissable) {
        choose_close(s, d->dismiss_index);
    }
}

static void modal_key_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_current_target(e);
    dlg_t *d = lv_obj_get_user_data(s);
    const l5_key_t *k = lv_event_get_param(e);
    int dir = 0;
    if (k->kind == L5_KEY_LEFT || k->kind == L5_KEY_RIGHT) {
        dir = k->kind == L5_KEY_LEFT ? -1 : 1;
    } else if (k->kind == L5_KEY_TAB && !k->ctrl) {
        dir = k->shift ? -1 : 1;
    } else if (k->kind == L5_KEY_ENTER) {
        int i = d->focus >= 0 ? d->focus : d->nbtn - 1; /* nothing focused yet: the primary action */
        if (!lv_obj_has_state(d->btn[i], LV_STATE_DISABLED)) choose_close(s, i);
        return;
    } else if (k->kind == L5_KEY_ESC && d->cancel_index >= 0) {
        choose_close(s, d->cancel_index);
        return;
    }
    if (!dir) {
        if (d->body) scroll_key(d->body, k);
        return;
    }
    int f = (d->focus >= 0 ? d->focus : d->nbtn - 1) + dir; /* the ring starts from the primary action */
    f = f < 0 ? 0 : f >= d->nbtn ? d->nbtn - 1 : f;
    d->focus = f;
    focus_paint(d->btn, d->nbtn, f);
}

static dlg_t *modal_n(const char *kind, const char *title, const char *body, const char *const *labels, int n,
                      const char *icon, int dismiss, l5_choose_cb_t cb, void *user)
{
    dlg_t *d = lv_zalloc(sizeof(dlg_t));
    d->choose_cb = cb;
    d->user = user;
    d->dismissable = dismiss >= 0;
    d->dismiss_index = dismiss;
    d->focus = -1;
    d->cancel_index = dismiss; /* Esc does what a tap beside the dialog does, else CANCEL (below) */
    lv_obj_t *s = scrim_create(d);
    lv_obj_set_user_data(s, d);
    lv_obj_add_event_cb(s, choose_scrim_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s, modal_key_cb, l5_key_event(), NULL);
    lv_obj_t *m = modal_create(s, n > 2 ? 720 : 600, kind, title);

    lv_obj_t *wrap = l5_box(m); /* long bodies (install checks) scroll, the actions stay */
    lv_obj_set_size(wrap, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(wrap, 380, 0);
    lv_obj_add_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(wrap, LV_DIR_VER);
    d->body = wrap;
    lv_obj_t *mb = l5_label(wrap, &l5_chakra_r17, L5_ON_PANEL_MUTED, body);
    lv_obj_set_width(mb, lv_pct(100));
    lv_label_set_long_mode(mb, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_top(mb, 12, 0);
    lv_obj_set_style_pad_hor(mb, 28, 0);
    lv_obj_set_style_pad_bottom(mb, 6, 0);
    lv_obj_set_style_text_line_space(mb, 6, 0);

    lv_obj_t *mf = l5_box(m);
    lv_obj_set_size(mf, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(mf, 18, 0);
    lv_obj_set_style_pad_hor(mf, 28, 0);
    lv_obj_set_style_pad_column(mf, 12, 0);
    lv_obj_set_flex_flow(mf, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mf, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_bottom(mf, 27, 0); /* room for the 3px button shadow */
    for (int i = 0; i < n; i++) { /* the last one is the primary action */
        lv_obj_t *b = l5_act_button(mf, i == n - 1 ? icon : NULL, labels[i], i == n - 1);
        lv_obj_set_style_min_width(b, n > 2 ? 150 : 170, 0);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, choose_btn_cb, LV_EVENT_CLICKED, s);
        lv_obj_add_style(b, &L5S.focus, LV_STATE_FOCUS_KEY);
        if (i < MAX_BTNS) d->btn[d->nbtn++] = b;
        if (d->cancel_index < 0 && n != 1 && strcmp(labels[i], "CANCEL") == 0) d->cancel_index = i;
    }
    return d;
}

void l5_choose(const char *kind, const char *title, const char *body, const char *const *labels, int n,
               const char *icon, l5_choose_cb_t cb, void *user)
{
    modal_n(kind, title, body, labels, n, icon, -1, cb, user);
}

static void modal_actions(const char *kind, const char *title, const char *body, const char *cancel, const char *ok,
                          const char *ok_icon, bool dismissable, l5_confirm_cb_t cb, void *user)
{
    const char *labels[2] = {cancel, ok};
    int n = cancel ? 2 : 1; /* no CANCEL (U.info): the one action and a tap on the scrim both count as ok */
    dlg_t *d = modal_n(kind, title, body, cancel ? labels : labels + 1, n, ok_icon, dismissable ? (cancel ? 0 : n - 1) : -1,
                       NULL, user);
    d->confirm_cb = cb;
    d->ok_index = n - 1;
    if (cancel) d->cancel_index = 0; /* Esc: the cancel action, whatever it is called */
}

void l5_confirm(const char *kind, const char *title, const char *body, const char *ok, bool danger,
                l5_confirm_cb_t cb, void *user)
{
    modal_actions(kind, title, body, "CANCEL", ok, danger ? L5_ICON_TRASH : L5_ICON_CHECK, false, cb, user);
}

void l5_info(const char *kind, const char *title, const char *body)
{
    modal_actions(kind, title, body, NULL, "OK", NULL, true, NULL, NULL);
}

void l5_ask(const char *kind, const char *title, const char *body, const char *cancel, const char *ok,
            const char *ok_icon, l5_confirm_cb_t cb, void *user)
{
    modal_actions(kind, title, body, cancel, ok, ok_icon, false, cb, user);
}

/* ---------- text viewer (VIEW in the SD manager) ---------- */

typedef struct {
    char *text;
    void (*free_fn)(void *);
    lv_obj_t *body; /* the keys scroll it */
} text_dlg_t;

static void text_deleted_cb(lv_event_t *e)
{
    text_dlg_t *t = lv_event_get_user_data(e);
    if (t->free_fn) t->free_fn(t->text); /* the labels are deleted right after, without drawing */
    lv_free(t);
}

static void text_close_cb(lv_event_t *e)
{
    lv_obj_t *scrim = lv_event_get_user_data(e);
    if (lv_event_get_current_target(e) != scrim || lv_event_get_target(e) == scrim) {
        lv_obj_delete(scrim); /* CLOSE, or a tap beside the dialog */
    }
}

static void text_key_cb(lv_event_t *e)
{
    text_dlg_t *t = lv_event_get_user_data(e);
    const l5_key_t *k = lv_event_get_param(e);
    if (k->kind == L5_KEY_ENTER || k->kind == L5_KEY_ESC) {
        lv_obj_delete(lv_event_get_current_target(e)); /* CLOSE */
    } else if (t->body) {
        scroll_key(t->body, k);
    }
}

void l5_text_dialog(const char *title, const char *note, char *text, uint32_t pieces, void (*free_fn)(void *))
{
    text_dlg_t *t = lv_zalloc(sizeof(text_dlg_t));
    t->text = text;
    t->free_fn = free_fn;
    lv_obj_t *s = scrim_create(NULL);
    lv_obj_add_event_cb(s, text_deleted_cb, LV_EVENT_DELETE, t);
    lv_obj_add_event_cb(s, text_close_cb, LV_EVENT_CLICKED, s);
    lv_obj_add_event_cb(s, text_key_cb, l5_key_event(), t);
    lv_obj_t *m = modal_create(s, 1040, "VIEW", title);
    lv_obj_set_style_max_height(m, 680, 0);
    lv_obj_t *mh = lv_obj_get_child(m, 0); /* the note at the end of the kind row */
    lv_obj_t *sp = l5_box(mh);
    lv_obj_set_size(sp, 1, 1);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_t *nt = l5_label(mh, &l5_mono_14, L5_ON_PANEL_MUTED, note);
    lv_obj_set_style_text_letter_space(nt, 1, 0);

    lv_obj_t *body = l5_box(m); /* <pre>: scrolls, the CLOSE row stays */
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(body, 680 - 47 - 42 - 96, 0);
    lv_obj_set_style_margin_top(body, 10, 0);
    lv_obj_set_style_pad_ver(body, 12, 0);
    lv_obj_set_style_pad_hor(body, 28, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x0e0f11), 0);
    lv_obj_set_style_border_side(body, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_border_color(body, L5_PANEL_LINE, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 3, 0); /* the line space between two pieces too */
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE); /* clickable: drags on the text scroll */
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    t->body = body;
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(body, L5P.accent, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(body, 5, LV_PART_SCROLLBAR);
    const char *p = text;
    for (uint32_t i = 0; i < pieces; i++) { /* one label per piece: only the visible ones are drawn */
        lv_obj_t *l = lv_label_create(body);
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_text_font(l, &l5_mono15_tall, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xd7d9dd), 0);
        lv_obj_set_style_text_line_space(l, 3, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_label_set_text_static(l, p);
        p += strlen(p) + 1;
    }

    lv_obj_t *mf = l5_box(m);
    lv_obj_set_size(mf, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(mf, 14, 0);
    lv_obj_set_style_pad_hor(mf, 28, 0);
    lv_obj_set_style_pad_bottom(mf, 22, 0);
    lv_obj_set_flex_flow(mf, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mf, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *b = l5_act_button(mf, L5_ICON_X, "CLOSE", true);
    lv_obj_set_style_min_width(b, 170, 0);
    lv_obj_add_event_cb(b, text_close_cb, LV_EVENT_CLICKED, s);
}

/* ---------- progress ---------- */

static void progress_deleted_cb(lv_event_t *e)
{
    l5_progress_t *p = lv_event_get_user_data(e);
    p->scrim = NULL; /* gone with the layer (l5_dialogs_close_all): the owner still calls close */
}

static void progress_cancel(l5_progress_t *p)
{
    if (p->cancel) *p->cancel = true;
    lv_obj_add_state(p->cancel_btn, LV_STATE_DISABLED);
    lv_label_set_text(lv_obj_get_child(p->cancel_btn, -1), "CANCELLING\xE2\x80\xA6");
}

static void progress_key_cb(lv_event_t *e)
{
    l5_progress_t *p = lv_event_get_user_data(e);
    const l5_key_t *k = lv_event_get_param(e);
    if (k->kind == L5_KEY_ESC && p->cancel_btn && !lv_obj_has_state(p->cancel_btn, LV_STATE_DISABLED)) {
        progress_cancel(p);
    }
}

static void progress_cancel_cb(lv_event_t *e)
{
    progress_cancel(lv_event_get_user_data(e));
}

l5_progress_t *l5_progress_open(const char *kind, const char *title)
{
    l5_progress_t *p = lv_zalloc(sizeof(l5_progress_t));
    p->scrim = scrim_create(NULL);
    lv_obj_add_event_cb(p->scrim, progress_deleted_cb, LV_EVENT_DELETE, p);
    lv_obj_add_event_cb(p->scrim, progress_key_cb, l5_key_event(), p);
    lv_obj_t *m = modal_create(p->scrim, 600, kind, title);

    lv_obj_t *wrap = l5_box(m); /* .prog margin 18px 28px 6px */
    lv_obj_set_size(wrap, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(wrap, 18, 0);
    lv_obj_set_style_pad_hor(wrap, 28, 0);
    p->bar = lv_bar_create(wrap);
    lv_obj_remove_flag(p->bar, LV_OBJ_FLAG_CLICKABLE); /* lv_bar is clickable by default */
    lv_obj_remove_style_all(p->bar);
    lv_obj_set_size(p->bar, lv_pct(100), 14);
    lv_obj_set_style_bg_opa(p->bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(p->bar, lv_color_hex(0x2b2d32), LV_PART_MAIN);
    lv_obj_set_style_border_width(p->bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(p->bar, L5_BLACK, LV_PART_MAIN);
    lv_obj_set_style_radius(p->bar, 2, LV_PART_MAIN);
    lv_obj_add_style(p->bar, &L5S.meter_ind, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_width(p->bar, 10, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_color(p->bar, L5P.accent, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_opa(p->bar, 153, LV_PART_INDICATOR);
    lv_bar_set_range(p->bar, 0, 100);

    lv_obj_t *row = l5_box(m);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(row, 6, 0);
    lv_obj_set_style_pad_hor(row, 28, 0);
    lv_obj_set_style_pad_bottom(row, 22, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    p->label = l5_label(row, &l5_mono_15, L5_ON_PANEL_MUTED, "Starting\xE2\x80\xA6");
    p->pct = l5_label(row, &l5_orb_b16, L5_ON_PANEL, "0%");
    return p;
}

void l5_progress_set(l5_progress_t *p, int pct, const char *label)
{
    if (!p || !p->scrim) return;
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    lv_bar_set_value(p->bar, pct, LV_ANIM_OFF);
    lv_label_set_text_fmt(p->pct, "%d%%", pct);
    if (label) {
        lv_label_set_text(p->label, label);
    }
}

void l5_progress_cancelable(l5_progress_t *p, volatile bool *flag)
{
    if (!p || !p->scrim || p->cancel_btn) return;
    p->cancel = flag;
    lv_obj_t *m = lv_obj_get_child(p->scrim, 0);
    lv_obj_t *row = lv_obj_get_child(m, -1); /* label + percent: CANCEL goes under it, right aligned */
    lv_obj_set_style_pad_bottom(row, 8, 0);
    lv_obj_t *mf = l5_box(m);
    lv_obj_set_size(mf, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(mf, 28, 0);
    lv_obj_set_style_pad_bottom(mf, 27, 0);
    lv_obj_set_flex_flow(mf, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mf, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    p->cancel_btn = l5_act_button(mf, L5_ICON_X, "CANCEL", false);
    lv_obj_set_style_min_width(p->cancel_btn, 170, 0);
    lv_obj_add_event_cb(p->cancel_btn, progress_cancel_cb, LV_EVENT_CLICKED, p);
}

void l5_progress_close(l5_progress_t *p)
{
    if (p) {
        if (p->scrim) lv_obj_delete(p->scrim);
        lv_free(p);
    }
}

/* ---------- toast ---------- */

static void toast_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

void l5_toast(const char *fmt, ...)
{
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    lv_obj_t *box = l5_ui_toasts();
    lv_obj_clean(box);
    lv_obj_t *t = lv_label_create(box);
    lv_obj_add_style(t, &L5S.toast, 0);
    lv_label_set_text(t, msg);
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -26);
    lv_obj_fade_in(t, 200, 0);
    /* not lv_obj_fade_out(): it applies its start value at once and removes the fade-in running on the
       same object (LVGL 9.3 lv_anim_start), which left every toast at opacity 0 */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, t);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_exec_cb(&a, toast_opa_cb);
    lv_anim_set_duration(&a, 400);
    lv_anim_set_delay(&a, 2200);
    lv_anim_set_early_apply(&a, false);
    lv_anim_start(&a);
    lv_obj_delete_delayed(t, 2700);
}
