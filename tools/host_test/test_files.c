/* Host test for l5_files.c (SD manager: names, copy, delete, text preview) on a real folder that stands
   in for the card. Run it with tools/host_test/run.py: test_files <work folder>. */
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include "l5_files.h"
#include "stubs.h"

extern uint64_t l5_files_host_total, l5_files_host_free;
extern uint32_t l5_files_host_cluster;
extern long l5_files_host_write_budget;

static int failures;
static char root[260];

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            failures++;                                     \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

const char *esp_err_to_name(esp_err_t e)
{
    static char b[16];
    snprintf(b, sizeof(b), "0x%x", e);
    return b;
}

int64_t esp_timer_get_time(void)
{
    return (int64_t)clock() * 1000000 / CLOCKS_PER_SEC;
}

/* ---------- helpers ---------- */

static const char *P(const char *rel) /* root + "/" + rel, four rotating buffers */
{
    static char b[4][400];
    static int k;
    k = (k + 1) % 4;
    snprintf(b[k], sizeof(b[k]), "%s/%s", root, rel);
    return b[k];
}

static void put_file(const char *rel, const void *data, size_t n)
{
    FILE *f = fopen(P(rel), "wb");
    CHECK(f, "create %s", rel);
    if (f) {
        fwrite(data, 1, n, f);
        fclose(f);
    }
}

static void put_pattern(const char *rel, size_t n, unsigned seed)
{
    uint8_t *b = malloc(n ? n : 1);
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(seed * 131 + i * 7 + (i >> 9));
    put_file(rel, b, n);
    free(b);
}

static bool there(const char *rel)
{
    struct stat st;
    return stat(P(rel), &st) == 0;
}

static bool same_file(const char *a, const char *b)
{
    FILE *x = fopen(P(a), "rb"), *y = fopen(P(b), "rb");
    bool same = x && y;
    while (same) {
        int c = fgetc(x), d = fgetc(y);
        if (c != d) same = false;
        if (c == EOF || d == EOF) break;
    }
    if (x) fclose(x);
    if (y) fclose(y);
    return same;
}

static void mk(const char *rel)
{
    CHECK(mkdir(P(rel), 0777) == 0, "mkdir %s", rel);
}

static const char *outside_path(void) /* next to the card folder, not inside it */
{
    static char b[400];
    snprintf(b, sizeof(b), "%s-outside", root);
    return b;
}

/* removes a test tree with the module itself (checked separately) */
static void wipe(const char *rel)
{
    l5_files_result_t r;
    if (there(rel)) l5_files_delete(P(rel), NULL, 0, &r);
}

typedef struct {
    int calls;
    uint64_t last, total;
    bool monotonic;
    uint64_t cancel_at;
    volatile bool *cancel;
} prog_t;

static void prog_cb(uint64_t done, uint64_t total, void *user)
{
    prog_t *p = user;
    if (p->calls && done < p->last) p->monotonic = false;
    p->calls++;
    p->last = done;
    p->total = total;
    if (p->cancel_at && done >= p->cancel_at && p->cancel) *p->cancel = true;
}

/* src tree used by count / copy / delete:
   src/a.txt 1000 B, src/b.bin 70000 B, src/sub/c 0 B, src/sub/deep/d 5000 B, src/sub/zdjęcia.txt 300 B */
static void make_src(void)
{
    mk("src");
    mk("src/sub");
    mk("src/sub/deep");
    put_pattern("src/a.txt", 1000, 1);
    put_pattern("src/b.bin", 70000, 2);
    put_pattern("src/sub/c", 0, 3);
    put_pattern("src/sub/deep/d", 5000, 4);
    put_pattern("src/sub/zdj\xC4\x99" "cia.txt", 300, 5);
}

static bool same_tree(void)
{
    return same_file("src/a.txt", "src-copy/a.txt") && same_file("src/b.bin", "src-copy/b.bin") &&
           same_file("src/sub/c", "src-copy/sub/c") && same_file("src/sub/deep/d", "src-copy/sub/deep/d") &&
           same_file("src/sub/zdj\xC4\x99" "cia.txt", "src-copy/sub/zdj\xC4\x99" "cia.txt");
}

/* ---------- names ---------- */

static void test_names(void)
{
    printf("names\n");
    char why[200], n[600];
    strcpy(n, "  new_folder  ");
    CHECK(l5_files_name_ok(n, why, sizeof(why)) && !strcmp(n, "new_folder"), "trim: '%s'", n);
    strcpy(n, "   ");
    CHECK(!l5_files_name_ok(n, why, sizeof(why)) && strstr(why, "empty"), "empty: %s", why);
    for (const char *c = "/\\:*?\"<>|"; *c; c++) {
        snprintf(n, sizeof(n), "a%cb", *c);
        bool ok = l5_files_name_ok(n, why, sizeof(why));
        CHECK(!ok && !strncmp(why, "Names cannot contain ", 21) && why[21] == *c && strstr(why, "FAT and exFAT refuse"),
              "char %c: %s", *c, why);
    }
    strcpy(n, "a\x01" "b");
    CHECK(!l5_files_name_ok(n, why, sizeof(why)) && strstr(why, "Control"), "control: %s", why);
    strcpy(n, "a\x7F");
    CHECK(!l5_files_name_ok(n, why, sizeof(why)), "DEL accepted");
    strcpy(n, ".hidden");
    CHECK(!l5_files_name_ok(n, why, sizeof(why)) && strstr(why, "dot"), "dot start: %s", why);
    strcpy(n, "name.");
    CHECK(!l5_files_name_ok(n, why, sizeof(why)) && strstr(why, "end with a dot"), "dot end: %s", why);
    strcpy(n, "system volume INFORMATION");
    CHECK(!l5_files_name_ok(n, why, sizeof(why)) && strstr(why, "Windows"), "SVI: %s", why);
    strcpy(n, "zdj\xC4\x99" "cia \xC5\x81\xC3\xB3" "d\xC5\xBA 2024");
    CHECK(l5_files_name_ok(n, why, sizeof(why)), "Polish name refused: %s", why);
    memset(n, 'a', 255);
    n[255] = 0;
    CHECK(l5_files_name_ok(n, why, sizeof(why)), "255 chars refused: %s", why);
    memset(n, 'a', 256);
    n[256] = 0;
    CHECK(!l5_files_name_ok(n, why, sizeof(why)) && strstr(why, "255"), "256 chars: %s", why);
    for (int i = 0; i < 255; i++) memcpy(n + 2 * i, "\xC4\x85", 2); /* 255 x U+0105: 510 bytes, 255 units */
    n[510] = 0;
    CHECK(l5_files_name_ok(n, why, sizeof(why)), "255 Polish chars refused: %s", why);
    strcpy(n, "ab\xC4");
    CHECK(!l5_files_name_ok(n, why, sizeof(why)) && strstr(why, "UTF-8"), "broken UTF-8: %s", why);

    CHECK(l5_files_same_name("Plik.TXT", "plik.txt"), "case");
    CHECK(l5_files_same_name("\xC5\x81\xC3\x93" "D\xC5\xB9", "\xC5\x82\xC3\xB3" "d\xC5\xBA"), "Polish case");
    CHECK(!l5_files_same_name("a", "ab") && !l5_files_same_name("ab", "a") && !l5_files_same_name("a", ""), "prefix");
    CHECK(l5_files_hidden(".x") && l5_files_hidden("._a.bin") && l5_files_hidden("System Volume Information") &&
          !l5_files_hidden("a"), "hidden");
    CHECK(l5_files_is_text("A.TXT") && l5_files_is_text("x.json") && l5_files_is_text("x.log") && l5_files_is_text("x.py") &&
          l5_files_is_text("config.conf") && l5_files_is_text("r.md") && l5_files_is_text("t.csv") && l5_files_is_text("s.ini") &&
          l5_files_is_text("c.cfg") && !l5_files_is_text("x.bin") && !l5_files_is_text("noext") && !l5_files_is_text("x.txt.bin"),
          "text extensions");

    CHECK(l5_files_inside(P("a/b"), P("a")) && l5_files_inside(P("a"), P("a")) && !l5_files_inside(P("ab"), P("a")) &&
          l5_files_inside(P("A/b"), P("a")) && !l5_files_inside(P("a"), P("a/b")), "inside");
    CHECK(l5_files_parent(P("a/b"), n, sizeof(n)) && !strcmp(n, P("a")), "parent a/b: %s", n);
    CHECK(l5_files_parent(P("a"), n, sizeof(n)) && !strcmp(n, root), "parent a: %s", n);
    CHECK(!l5_files_parent(root, n, sizeof(n)), "parent of the root");
}

static void test_unique(void)
{
    printf("unique names\n");
    char out[300];
    mk("u");
    CHECK(l5_files_unique(P("u"), "x.txt", out, sizeof(out)) && !strcmp(out, "x.txt"), "free: %s", out);
    put_file("u/x.txt", "1", 1);
    CHECK(l5_files_unique(P("u"), "x.txt", out, sizeof(out)) && !strcmp(out, "x-copy.txt"), "copy: %s", out);
    put_file("u/x-copy.txt", "1", 1);
    CHECK(l5_files_unique(P("u"), "x.txt", out, sizeof(out)) && !strcmp(out, "x-copy2.txt"), "copy2: %s", out);
    put_file("u/x-copy2.txt", "1", 1);
    CHECK(l5_files_unique(P("u"), "x.txt", out, sizeof(out)) && !strcmp(out, "x-copy3.txt"), "copy3: %s", out);
    put_file("u/Y.TXT", "1", 1); /* NTFS, like FAT, ignores letter case */
    CHECK(l5_files_unique(P("u"), "y.txt", out, sizeof(out)) && !strcmp(out, "y-copy.txt"), "case: %s", out);
    mk("u/dir");
    CHECK(l5_files_unique(P("u"), "dir", out, sizeof(out)) && !strcmp(out, "dir-copy"), "folder: %s", out);
    put_file("u/a.b.c", "1", 1);
    CHECK(l5_files_unique(P("u"), "a.b.c", out, sizeof(out)) && !strcmp(out, "a.b-copy.c"), "last dot: %s", out);
    CHECK(l5_files_free_name(P("u"), "new_folder", out, sizeof(out)) && !strcmp(out, "new_folder"), "nf: %s", out);
    mk("u/new_folder");
    CHECK(l5_files_free_name(P("u"), "new_folder", out, sizeof(out)) && !strcmp(out, "new_folder2"), "nf2: %s", out);
    mk("u/new_folder2");
    CHECK(l5_files_free_name(P("u"), "new_folder", out, sizeof(out)) && !strcmp(out, "new_folder3"), "nf3: %s", out);
    CHECK(!l5_files_unique(P("u"), "x.txt", out, 6), "a name that does not fit");
    wipe("u");
}

static bool list_cb(const l5_files_entry_t *e, void *user)
{
    int *k = user;
    if (!strcmp(e->name, "b.bin")) CHECK(!e->dir && e->size == 70000 && e->mtime > 0, "b.bin entry");
    if (!strcmp(e->name, "sub")) CHECK(e->dir, "sub entry");
    (*k)++;
    return true;
}

static void test_list_count(void)
{
    printf("listing and counting\n");
    int k = 0;
    put_file("src/.hidden", "h", 1);
    CHECK(l5_files_list(P("src"), list_cb, &k) == ESP_OK && k == 4, "entries %d", k);
    CHECK(l5_files_items(P("src"), 999) == 3, "items %d", l5_files_items(P("src"), 999));
    CHECK(l5_files_items(P("src"), 2) == 2, "items max");
    CHECK(l5_files_items(P("nothere"), 99) == -1, "missing folder");
    remove(P("src/.hidden"));

    l5_files_count_t c;
    l5_files_job_t job = {.cluster = 4096};
    CHECK(l5_files_count(P("src"), &job, &c) == ESP_OK, "count");
    CHECK(c.files == 5 && c.dirs == 3 && c.bytes == 76300, "files %u dirs %u bytes %llu", c.files, c.dirs,
          (unsigned long long)c.bytes);
    /* folders 3 x 4096, files: 1000 -> 4096, 70000 -> 73728, 0 -> 0, 5000 -> 8192, 300 -> 4096 */
    CHECK(c.alloc == 3 * 4096 + 4096 + 73728 + 0 + 8192 + 4096, "alloc %llu", (unsigned long long)c.alloc);
    CHECK(l5_files_count(P("src/b.bin"), &job, &c) == ESP_OK && c.files == 1 && c.dirs == 0 && c.alloc == 73728, "file count");
    CHECK(l5_files_count(P("nothere"), &job, &c) == ESP_ERR_NOT_FOUND, "count missing");
}

static void test_copy(void)
{
    printf("copy\n");
    l5_files_result_t r;
    l5_files_count_t c;
    volatile bool cancel = false;
    prog_t pg = {.monotonic = true, .cancel = &cancel};
    l5_files_job_t job = {.cancel = &cancel, .progress = prog_cb, .user = &pg, .cluster = 4096};

    /* a file */
    mk("dst");
    CHECK(l5_files_copy(P("src/b.bin"), P("dst/b.bin"), &job, NULL, &r) == ESP_OK, "file copy: %s", r.why);
    CHECK(same_file("src/b.bin", "dst/b.bin") && r.files == 1 && r.bytes == 70000, "file copy content");

    /* a folder, with progress */
    l5_files_count(P("src"), &job, &c);
    pg = (prog_t){.monotonic = true, .cancel = &cancel};
    CHECK(l5_files_copy(P("src"), P("src-copy"), &job, &c, &r) == ESP_OK, "tree copy: %s", r.why);
    CHECK(same_tree(), "tree content");
    CHECK(r.files == 5 && r.dirs == 3 && r.bytes == 76300, "tree result %u %u %llu", r.files, r.dirs,
          (unsigned long long)r.bytes);
    CHECK(pg.monotonic && pg.last == 76300 && pg.total == 76300, "progress %llu/%llu", (unsigned long long)pg.last,
          (unsigned long long)pg.total);

    /* refusals: into itself, onto something that exists, outside the card, a missing source */
    CHECK(l5_files_copy(P("src"), P("src/sub/src"), &job, &c, &r) == ESP_ERR_INVALID_ARG && strstr(r.why, "itself") &&
          !there("src/sub/src"), "into itself: %s", r.why);
    CHECK(l5_files_copy(P("src"), P("SRC/sub/x"), &job, &c, &r) == ESP_ERR_INVALID_ARG, "into itself, other case");
    CHECK(l5_files_copy(P("src/a.txt"), P("dst/b.bin"), &job, NULL, &r) != ESP_OK && same_file("src/b.bin", "dst/b.bin") &&
          strstr(r.why, "already there"), "existing target: %s", r.why);
    CHECK(l5_files_copy(P("src/a.txt"), root, &job, NULL, &r) == ESP_ERR_INVALID_ARG, "onto the root");
    CHECK(l5_files_copy(P("src/a.txt"), outside_path(), &job, NULL, &r) == ESP_ERR_INVALID_ARG && access(outside_path(), 0),
          "outside the card");
    CHECK(l5_files_copy(P("nothere"), P("dst/n"), &job, NULL, &r) == ESP_ERR_NOT_FOUND && strstr(r.why, "no longer"),
          "missing source: %s", r.why);

    /* CANCEL in the middle of b.bin: nothing of the copy stays */
    cancel = false;
    pg = (prog_t){.monotonic = true, .cancel = &cancel, .cancel_at = 30000};
    esp_err_t err = l5_files_copy(P("src"), P("src-cancel"), &job, &c, &r);
    CHECK(err == L5_FILES_CANCELLED && r.cancelled && !r.left && !there("src-cancel"), "cancel: err %x left %d", err, r.left);
    cancel = false;

    /* the card fills up after 40 KB: error sentence, partial copy removed, source untouched */
    l5_files_host_write_budget = 40000;
    pg = (prog_t){.monotonic = true};
    err = l5_files_copy(P("src"), P("src-full"), &job, &c, &r);
    l5_files_host_write_budget = -1;
    CHECK(err != ESP_OK && !r.cancelled && strstr(r.why, "card is full") && !there("src-full"), "card full: %s", r.why);
    CHECK(there("src/b.bin") && there("src/sub/deep/d"), "source after a failed copy");

    /* a file copy that fails removes the half file */
    l5_files_host_write_budget = 10000;
    err = l5_files_copy(P("src/b.bin"), P("dst/half.bin"), &job, NULL, &r);
    l5_files_host_write_budget = -1;
    CHECK(err != ESP_OK && !there("dst/half.bin"), "half file left");
    wipe("src-copy");
    wipe("dst");
}

static void test_delete(void)
{
    printf("delete\n");
    l5_files_result_t r;
    l5_files_count_t c;
    volatile bool cancel = false;
    prog_t pg = {.monotonic = true, .cancel = &cancel};
    l5_files_job_t job = {.cancel = &cancel, .progress = prog_cb, .user = &pg};
    char why[200];

    CHECK(l5_files_copy(P("src"), P("del"), NULL, NULL, &r) == ESP_OK, "setup copy: %s", r.why);
    CHECK(chmod(P("del/sub/deep/d"), 0444) == 0, "read-only file");
    l5_files_count(P("del"), NULL, &c);
    CHECK(l5_files_delete(P("del"), &job, c.files + c.dirs, &r) == ESP_OK && !there("del"), "tree delete: %s", r.why);
    CHECK(r.files == 5 && r.dirs == 3 && pg.last == 8 && pg.total == 8 && pg.monotonic, "delete result %u %u %llu",
          r.files, r.dirs, (unsigned long long)pg.last);

    put_file("one.txt", "x", 1);
    CHECK(l5_files_delete(P("one.txt"), NULL, 0, &r) == ESP_OK && !there("one.txt") && r.files == 1, "file delete");
    CHECK(l5_files_delete(root, NULL, 0, &r) == ESP_ERR_INVALID_ARG && strstr(r.why, "not the card itself"), "root: %s", r.why);
    char path[400];
    snprintf(path, sizeof(path), "%s/", root);
    CHECK(l5_files_delete(path, NULL, 0, &r) == ESP_ERR_INVALID_ARG, "root with a slash");
    snprintf(path, sizeof(path), "%s/src/..", root);
    CHECK(l5_files_delete(path, NULL, 0, &r) == ESP_ERR_INVALID_ARG && there("src"), "dot dot");
    snprintf(path, sizeof(path), "%s/../x", root);
    CHECK(l5_files_delete(path, NULL, 0, &r) == ESP_ERR_INVALID_ARG, "outside through dot dot");
    FILE *o = fopen(outside_path(), "wb");
    CHECK(o, "create the outside file");
    if (o) fclose(o);
    CHECK(l5_files_delete(outside_path(), NULL, 0, &r) == ESP_ERR_INVALID_ARG && !access(outside_path(), 0),
          "outside the card");
    remove(outside_path());
    CHECK(l5_files_delete(P("nothere"), NULL, 0, &r) == ESP_ERR_NOT_FOUND && strstr(r.why, "no longer"), "missing: %s", r.why);

    /* CANCEL after a few entries: what is removed is gone, the rest stays */
    CHECK(l5_files_copy(P("src"), P("del2"), NULL, NULL, &r) == ESP_OK, "setup copy 2");
    cancel = false;
    pg = (prog_t){.monotonic = true, .cancel = &cancel, .cancel_at = 2};
    CHECK(l5_files_delete(P("del2"), &job, 8, &r) == L5_FILES_CANCELLED && r.cancelled && there("del2") &&
          r.files + r.dirs >= 2 && r.files + r.dirs < 8, "delete cancel %u+%u", r.files, r.dirs);
    cancel = false;
    wipe("del2");

    /* rename and new folder */
    put_file("r.txt", "r", 1);
    CHECK(l5_files_rename(P("r.txt"), P("s.txt"), why, sizeof(why)) == ESP_OK && there("s.txt") && !there("r.txt"), "rename");
    put_file("t.txt", "t", 1);
    CHECK(l5_files_rename(P("t.txt"), P("s.txt"), why, sizeof(why)) != ESP_OK && there("t.txt"), "rename onto a file");
    CHECK(l5_files_rename(P("s.txt"), P("S.TXT"), why, sizeof(why)) == ESP_OK, "rename letter case: %s", why);
    CHECK(l5_files_rename(P("S.TXT"), outside_path(), why, sizeof(why)) == ESP_ERR_INVALID_ARG, "rename outside");
    CHECK(l5_files_rename(root, P("x"), why, sizeof(why)) == ESP_ERR_INVALID_ARG, "rename the root");
    CHECK(l5_files_mkdir(P("nf"), why, sizeof(why)) == ESP_OK && there("nf"), "mkdir");
    CHECK(l5_files_mkdir(P("nf"), why, sizeof(why)) != ESP_OK && strstr(why, "already there"), "mkdir twice: %s", why);
    CHECK(l5_files_mkdir(root, why, sizeof(why)) == ESP_ERR_INVALID_ARG, "mkdir root");
    wipe("nf");
    remove(P("S.TXT"));
    remove(P("t.txt"));
}

/* ---------- text ---------- */

static l5_text_t prep(const char *s, size_t n)
{
    l5_text_t t;
    memset(&t, 0, sizeof(t));
    CHECK(l5_text_prepare((const uint8_t *)s, n, &t) || t.binary, "prepare");
    return t;
}

/* pieces joined with '\n' where a piece ended */
static size_t joined(const l5_text_t *t, char *out, size_t cap)
{
    size_t w = 0;
    const char *p = t->text;
    for (uint32_t i = 0; i < t->pieces; i++) {
        size_t l = strlen(p);
        if (w + l + 1 >= cap) break;
        memcpy(out + w, p, l);
        w += l;
        if (i + 1 < t->pieces) out[w++] = '\n';
        p += l + 1;
    }
    out[w] = 0;
    return w;
}

static bool pieces_ok(const l5_text_t *t)
{
    const char *p = t->text;
    size_t used = 0;
    for (uint32_t i = 0; i < t->pieces; i++) {
        size_t l = strlen(p);
        if (l + 1 > L5_TEXT_PIECE) return false;
        const unsigned char *u = (const unsigned char *)p;
        if (l && (u[0] & 0xC0) == 0x80) return false; /* starts inside a character */
        p += l + 1;
        used += l + 1;
    }
    return used == t->len;
}

static void test_text(void)
{
    printf("text preview\n");
    static char big[300000], out[300000];
    l5_text_t t = prep("hello\nworld\n", 12);
    CHECK(t.pieces == 1 && !strcmp(t.text, "hello\nworld") && t.lines == 2, "simple: '%s' %u", t.text, t.lines);
    l5_text_free(&t);
    t = prep("a\r\nb\rc\fd", 8);
    CHECK(!strcmp(t.text, "a\nb\nc\nd") && t.lines == 4, "line ends: '%s'", t.text);
    l5_text_free(&t);
    t = prep("a\tb\n\tx\nabcd\te", 13);
    CHECK(!strcmp(t.text, "a   b\n    x\nabcd    e"), "tabs: '%s'", t.text);
    l5_text_free(&t);
    const char *ansi = "\x1b[0;32mI (123) tag: ok\x1b[0m\n\x1b" "Mx";
    t = prep(ansi, strlen(ansi));
    CHECK(!strcmp(t.text, "I (123) tag: ok\nx"), "colour codes: '%s'", t.text);
    l5_text_free(&t);
    t = prep("a\x01" "b\x7F" "c\xFF" "d\xC4", 8);
    CHECK(!strcmp(t.text, "a?b?c?d?"), "controls and broken bytes: '%s'", t.text);
    l5_text_free(&t);
    const char *pl = "\xEF\xBB\xBFza\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87 g\xC4\x99\xC5\x9Bl\xC4\x85 ja\xC5\xBA\xC5\x84";
    t = prep(pl, strlen(pl));
    CHECK(!strcmp(t.text, pl + 3), "Polish text and BOM: '%s'", t.text);
    l5_text_free(&t);
    t = prep("ab\0cd", 5);
    CHECK(t.binary && !t.text, "NUL byte: binary");
    t = prep("", 0);
    CHECK(t.pieces == 1 && t.len == 1 && t.lines == 0, "empty");
    l5_text_free(&t);

    /* 4000 lines of 40 characters: pieces end at newlines, nothing lost */
    size_t n = 0;
    for (int i = 0; i < 4000; i++) n += (size_t)sprintf(big + n, "line %05d abcdefghijklmnopqrstuvwxyz012\n", i);
    t = prep(big, n);
    CHECK(pieces_ok(&t) && t.pieces > 70 && !t.cut && t.lines == 4000, "lines: pieces %u lines %u", t.pieces, t.lines);
    joined(&t, out, sizeof(out));
    big[n - 1] = 0;
    CHECK(!strcmp(out, big), "lines joined differ");
    l5_text_free(&t);

    /* one line of 'x' without spaces: hard breaks; with two-byte letters: never inside one */
    memset(big, 'x', 20000);
    t = prep(big, 20000);
    CHECK(pieces_ok(&t) && t.pieces == 10 && t.len == 20000 + 10, "hard breaks: %u pieces, %zu", t.pieces, t.len);
    l5_text_free(&t);
    for (int i = 0; i < 10000; i++) memcpy(big + 2 * i, "\xC4\x85", 2);
    t = prep(big, 20000);
    CHECK(pieces_ok(&t) && t.len == 20000 + t.pieces, "two-byte letters: %u pieces", t.pieces);
    l5_text_free(&t);
    /* a long line with spaces breaks at a space, which goes */
    n = 0;
    for (int i = 0; i < 2000; i++) n += (size_t)sprintf(big + n, "word%04d ", i);
    t = prep(big, n);
    CHECK(pieces_ok(&t) && t.text[strlen(t.text) - 1] != ' ', "space breaks");
    size_t first = strlen(t.text);
    CHECK(first > L5_TEXT_PIECE / 2 && first < L5_TEXT_PIECE && big[first] == ' ', "break at a space: %zu", first);
    l5_text_free(&t);
    /* tabs that would make it longer than the viewer keeps */
    memset(big, '\t', 65536);
    t = prep(big, 65536);
    CHECK(t.cut && t.len <= 192 * 1024 && pieces_ok(&t), "tab flood: cut %d len %zu", t.cut, t.len);
    l5_text_free(&t);

    /* files: 100 KB with a two-byte letter across the 64 KB end, an empty file, a binary file */
    memset(big, 'a', 102400);
    for (int i = 1; i < 100; i++) big[i * 1000] = '\n';
    big[65535] = '\xC4';
    big[65536] = '\x85';
    put_file("long.log", big, 102400);
    char why[200];
    l5_text_t f;
    CHECK(l5_files_read_text(P("long.log"), &f, why, sizeof(why)) == ESP_OK && f.cut && f.file_size == 102400 && !f.binary,
          "long file: %s", why);
    joined(&f, out, sizeof(out));
    CHECK(strlen(out) == 65535 && !memcmp(out, big, 65535) && pieces_ok(&f), "long file text %zu", strlen(out));
    l5_text_free(&f);
    put_file("empty.txt", "", 0);
    CHECK(l5_files_read_text(P("empty.txt"), &f, why, sizeof(why)) == ESP_OK && f.pieces == 1 && f.text[0] == 0 &&
          !f.cut && f.file_size == 0, "empty file");
    l5_text_free(&f);
    put_file("bin.txt", "PK\x03\x04\0\0zip", 9);
    CHECK(l5_files_read_text(P("bin.txt"), &f, why, sizeof(why)) == ESP_OK && f.binary && !f.text, "binary file");
    CHECK(l5_files_read_text(P("gone.txt"), &f, why, sizeof(why)) == ESP_ERR_NOT_FOUND && strstr(why, "no longer"),
          "missing file: %s", why);
    remove(P("long.log"));
    remove(P("empty.txt"));
    remove(P("bin.txt"));
}

static void test_space(void)
{
    printf("space\n");
    uint64_t total, free_bytes;
    uint32_t cluster;
    l5_files_host_total = 32ull << 30;
    l5_files_host_free = 100000;
    l5_files_host_cluster = 16384;
    CHECK(l5_files_space(&total, &free_bytes, &cluster) == ESP_OK && total == 32ull << 30 && free_bytes == 100000 &&
          cluster == 16384, "space");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: test_files <work folder>\n");
        return 2;
    }
    snprintf(root, sizeof(root), "%s/sdcard", argv[1]);
    for (char *s = root; *s; s++) {
        if (*s == '\\') *s = '/';
    }
    mkdir(root, 0777);
    l5_files_root = root;
    test_names();
    test_unique();
    make_src();
    test_list_count();
    test_copy();
    test_delete();
    test_text();
    test_space();
    wipe("src");
    printf(failures ? "FAILED: %d\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
