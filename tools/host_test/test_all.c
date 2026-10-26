/* Host test for l5_ptable.c, l5_fw.c and l5_inst.c against a simulated 16 MB flash and a simulated
   HTTPS server. Run it with tools/host_test/run.py (it compiles this file with the stubs in stub/). */
#include <math.h>
#include "l5_ptable.h"
#include "l5_fw.h"
#include "l5_inst.h"
#include "l5_net.h"

#define FLASH_SIZE (16u * 1024 * 1024)
static uint8_t *flash;
static bool protect = true;
static int fail_table_write, fail_restore, forget_calls, nvs_fail, dangerous_hits;
static esp_partition_t running[96];
static int nrunning;
esp_flash_t *esp_flash_default_chip = (esp_flash_t *)1;
static int failures;

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

/* ---- MD5 (RFC 1321) ---- */
static uint32_t K[64];
static const uint8_t S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9,
                              14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
static void md5_block(md5_context_t *c, const uint8_t *p)
{
    uint32_t M[16];
    for (int i = 0; i < 16; i++) {
        M[i] = p[i * 4] | p[i * 4 + 1] << 8 | p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
    }
    uint32_t A = c->a, B = c->b, C = c->c, D = c->d;
    for (int i = 0; i < 64; i++) {
        uint32_t F;
        int g;
        if (i < 16) { F = (B & C) | (~B & D); g = i; }
        else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
        else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) % 16; }
        else { F = C ^ (B | ~D); g = (7 * i) % 16; }
        F = F + A + K[i] + M[g];
        A = D; D = C; C = B;
        B = B + ((F << S[i]) | (F >> (32 - S[i])));
    }
    c->a += A; c->b += B; c->c += C; c->d += D;
}
void esp_rom_md5_init(md5_context_t *c)
{
    for (int i = 0; i < 64; i++) {
        K[i] = (uint32_t)(fabs(sin(i + 1.0)) * 4294967296.0);
    }
    c->a = 0x67452301; c->b = 0xefcdab89; c->c = 0x98badcfe; c->d = 0x10325476; c->len = 0;
}
void esp_rom_md5_update(md5_context_t *c, const void *data, uint32_t len)
{
    const uint8_t *p = data;
    for (uint32_t i = 0; i < len; i++) {
        c->buf[c->len % 64] = p[i];
        c->len++;
        if (c->len % 64 == 0) {
            md5_block(c, c->buf);
        }
    }
}
void esp_rom_md5_final(uint8_t *digest, md5_context_t *c)
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80, zero = 0;
    esp_rom_md5_update(c, &pad, 1);
    while (c->len % 64 != 56) {
        esp_rom_md5_update(c, &zero, 1);
    }
    for (int i = 0; i < 8; i++) {
        uint8_t b = (uint8_t)(bits >> (8 * i));
        esp_rom_md5_update(c, &b, 1);
    }
    uint32_t v[4] = {c->a, c->b, c->c, c->d};
    for (int i = 0; i < 16; i++) {
        digest[i] = (uint8_t)(v[i / 4] >> (8 * (i % 4)));
    }
}

/* ---- bootloader_support flash_partitions.c esp_partition_table_verify, same logic ---- */
esp_err_t esp_partition_table_verify(const esp_partition_info_t *t, bool log_errors, int *num)
{
    int md5_found = 0;
    *num = 0;
    for (int n = 0; n < ESP_PARTITION_TABLE_MAX_ENTRIES; n++) {
        const esp_partition_info_t *part = &t[n];
        if (part->magic == ESP_PARTITION_MAGIC) {
            if (part->pos.offset > FLASH_SIZE || part->pos.offset + part->pos.size > FLASH_SIZE) return ESP_ERR_INVALID_SIZE;
        } else if (part->magic == ESP_PARTITION_MAGIC_MD5) {
            if (md5_found) return ESP_ERR_INVALID_STATE;
            md5_context_t ctx;
            uint8_t d[16];
            esp_rom_md5_init(&ctx);
            esp_rom_md5_update(&ctx, t, n * sizeof(esp_partition_info_t));
            esp_rom_md5_final(d, &ctx);
            if (memcmp((const uint8_t *)part + ESP_PARTITION_MD5_OFFSET, d, 16) != 0) {
                if (log_errors) printf("  verify: bad MD5\n");
                return ESP_ERR_INVALID_STATE;
            }
            md5_found = 1;
        } else if (n != 0 && part->magic == 0xFFFF && part->type == PART_TYPE_END && part->subtype == PART_SUBTYPE_END) {
            *num = n - md5_found;
            return ESP_OK;
        } else {
            if (log_errors) printf("  verify: bad magic 0x%x at %d\n", part->magic, n);
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_ERR_INVALID_STATE;
}

/* ---- flash ---- */
static bool dangerous(uint32_t addr, uint32_t len) /* esp_partition_main_flash_region_safe */
{
    if (addr <= 0x8000 + 0xC00) return true;
    if (addr >= 0x10000 && addr < 0x210000) return true;
    if (addr < 0x10000 && addr + len > 0x10000) return true;
    return false;
}
esp_err_t esp_flash_read(esp_flash_t *c, void *dst, uint32_t addr, uint32_t len)
{
    (void)c;
    if (addr + len > FLASH_SIZE) return ESP_ERR_INVALID_ARG;
    memcpy(dst, flash + addr, len);
    return ESP_OK;
}
esp_err_t esp_flash_write(esp_flash_t *c, const void *src, uint32_t addr, uint32_t len)
{
    (void)c;
    if (protect && dangerous(addr, len)) {
        dangerous_hits++;
        printf("  ABORT: protected write 0x%x+0x%x\n", addr, len);
        return ESP_ERR_INVALID_ARG;
    }
    if (addr == 0x8000 && len == 0xC00 && fail_table_write) {
        fail_table_write--;
        memset(flash + addr, 0x00, 64); /* half-written garbage */
        return ESP_FAIL;
    }
    if (addr == 0x8000 && len == 0x1000 && fail_restore) {
        fail_restore--;
        return ESP_FAIL;
    }
    const uint8_t *s = src;
    for (uint32_t i = 0; i < len; i++) flash[addr + i] &= s[i]; /* NOR: 1 -> 0 only */
    return ESP_OK;
}
esp_err_t esp_flash_erase_region(esp_flash_t *c, uint32_t addr, uint32_t len)
{
    (void)c;
    if (addr % 0x1000 || len % 0x1000 || addr + len > FLASH_SIZE) return ESP_ERR_INVALID_ARG;
    if (protect && dangerous(addr, len)) {
        dangerous_hits++;
        printf("  ABORT: protected erase 0x%x+0x%x\n", addr, len);
        return ESP_ERR_INVALID_ARG;
    }
    memset(flash + addr, 0xff, len);
    return ESP_OK;
}
esp_err_t esp_flash_get_size(esp_flash_t *c, uint32_t *size) { (void)c; *size = FLASH_SIZE; return ESP_OK; }
esp_err_t esp_flash_set_dangerous_write_protection(esp_flash_t *c, bool p) { (void)c; protect = p; return ESP_OK; }
bool esp_flash_encryption_enabled(void) { return false; }

/* ---- partition API over the table that was loaded at "boot" ---- */
static void load_running(void)
{
    nrunning = 0;
    const esp_partition_info_t *t = (const esp_partition_info_t *)(flash + 0x8000);
    for (int i = 0; i < 96 && t[i].magic == ESP_PARTITION_MAGIC; i++) {
        esp_partition_t *p = &running[nrunning++];
        memset(p, 0, sizeof(*p));
        p->address = t[i].pos.offset;
        p->size = t[i].pos.size;
        p->erase_size = 0x1000;
        p->type = t[i].type;
        p->subtype = t[i].subtype;
        memcpy(p->label, t[i].label, 16);
    }
}
const esp_partition_t *esp_partition_find_first(esp_partition_type_t type, int subtype, const char *label)
{
    for (int i = 0; i < nrunning; i++) {
        esp_partition_t *p = &running[i];
        if ((type == ESP_PARTITION_TYPE_ANY || p->type == type) && (subtype == 0xff || p->subtype == subtype) &&
            (!label || strcmp(label, p->label) == 0)) {
            return p;
        }
    }
    return NULL;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t len)
{
    if (off + len > p->size) return ESP_ERR_INVALID_SIZE;
    return esp_flash_erase_region(NULL, p->address + off, len);
}
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t len)
{
    return esp_flash_read(NULL, dst, p->address + off, len);
}
esp_err_t esp_partition_write(const esp_partition_t *p, size_t off, const void *src, size_t len)
{
    return esp_flash_write(NULL, src, p->address + off, len);
}
const esp_partition_t *esp_ota_get_running_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
}

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) { (void)ns; (void)mode; *h = 1; return nvs_fail ? ESP_FAIL : ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len) { (void)h; (void)k; (void)v; (void)len; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
bool l5_board_sd_ready(void) { return false; }
void l5_slots_forget_all(void) { forget_calls++; }

/* ---- CRC32 as the ESP ROM crc32_le ---- */
uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len)
{
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* ---- test scenarios (appended to the flash simulation of pt-test/test.c by make_test.py) ---- */
static uint8_t tables[3][0x1000];
static const char *outdir;
static uint8_t *snapshot;

static bool all(uint32_t addr, uint32_t len, uint8_t v)
{
    for (uint32_t i = 0; i < len; i++) if (flash[addr + i] != v) return false;
    return true;
}

static void load(const uint8_t *table)
{
    memset(flash, 0xff, FLASH_SIZE);
    memcpy(flash + 0x8000, table, 0x1000);
    memset(flash + 0x2000, 0x11, 0x6000);      /* bootloader */
    memset(flash + 0x9000, 0x22, 0x5000);      /* nvs */
    memset(flash + 0xE000, 0x44, 32);          /* otadata entry */
    memset(flash + 0x10000, 0x33, 0x200000);   /* launcher */
    memset(flash + 0x210000, 0x66, 0xFF0000 - 0x210000); /* old app / FAT / SPIFFS bytes */
    memset(flash + 0xFF0000, 0x77, 0x10000);   /* coredump */
    load_running();
    for (int i = 0; i < nrunning; i++) {
        const esp_partition_t *p = &running[i];
        if (p->type == 0 && p->subtype >= 0x10) memset(flash + p->address, 0xE9, 64);
        if (strcmp(p->label, "aurora_cfg") == 0) memset(flash + p->address, 0x5A, p->size); /* "the PIN" */
    }
    protect = true;
    forget_calls = dangerous_hits = 0;
}

static void check_fixed(void)
{
    CHECK(all(0x2000, 0x6000, 0x11), "bootloader changed");
    CHECK(all(0x9000, 0x5000, 0x22), "nvs changed");
    CHECK(all(0x10000, 0x200000, 0x33), "launcher changed");
    CHECK(all(0xFF0000, 0x10000, 0x77), "coredump changed");
    CHECK(protect, "write protection left off");
    CHECK(dangerous_hits == 0, "a protected region was hit %d times", dangerous_hits);
}

typedef struct {
    uint32_t a, n;
    bool erased;
} range_t;
static int cmp_range(const void *x, const void *y)
{
    return ((const range_t *)x)->a < ((const range_t *)y)->a ? -1 : 1;
}

/* every byte outside the allowed ranges equals the snapshot; erased ranges are 0xFF */
static void check_only(range_t *r, int n)
{
    qsort(r, n, sizeof(*r), cmp_range);
    uint32_t pos = 0;
    for (int i = 0; i <= n; i++) {
        uint32_t end = i < n ? r[i].a : FLASH_SIZE;
        if (end > pos && memcmp(flash + pos, snapshot + pos, end - pos) != 0) {
            uint32_t k = pos;
            while (flash[k] == snapshot[k]) k++;
            CHECK(0, "unexpected change at 0x%06x", k);
        }
        if (i < n) {
            if (r[i].erased) CHECK(all(r[i].a, r[i].n, 0xff), "range 0x%06x+0x%x not erased", r[i].a, r[i].n);
            if (r[i].a + r[i].n > pos) pos = r[i].a + r[i].n;
        }
    }
}

static const l5_part_t *by_offset(const l5_ptable_t *t, uint32_t off)
{
    for (int i = 0; i < t->n; i++) if (t->p[i].offset == off) return &t->p[i];
    return NULL;
}

static const l5_part_t *by_label(const l5_ptable_t *t, const char *label)
{
    for (int i = 0; i < t->n; i++) if (!strcmp(t->p[i].label, label)) return &t->p[i];
    return NULL;
}

static void print_layout(const char *what, const l5_layout_t *l)
{
    char t[300];
    l5_layout_text(l, t, sizeof(t));
    printf("   %s: %s\n", what, t);
}

static l5_flash_map_t s_map, s_again;
static l5_ptable_t s_next;

static bool run(const char *name, int base, l5_layout_t l, bool expect_ok)
{
    static const char *const BASE[] = {"current Default", "old Default", "old UiFlow2"};
    printf("== %s from %s\n", name, BASE[base]);
    print_layout("layout", &l);
    load(tables[base]);
    char why[200] = "";
    CHECK(l5_pt_read_map(&s_map) == ESP_OK && s_map.managed, "map: %s", s_map.why);
    CHECK(s_map.aurora == (base == 0), "aurora detection");
    esp_err_t err = l5_pt_build(&s_map, &l, &s_next, why, sizeof(why));
    if (!expect_ok) {
        CHECK(err != ESP_OK, "build should fail");
        printf("   refused as expected: %s\n", why);
        return false;
    }
    CHECK(err == ESP_OK, "build: %s", why);
    if (err != ESP_OK) return false;
    l5_pt_diff_t d;
    l5_pt_diff(&s_map, &s_next, &d);
    memcpy(snapshot, flash, FLASH_SIZE);
    bool written = false;
    err = l5_pt_apply(&s_map, &s_next, NULL, NULL, &written, why, sizeof(why));
    CHECK(err == ESP_OK && written, "apply: %s", why);
    CHECK(l5_pt_read_map(&s_again) == ESP_OK && s_again.managed && s_again.aurora, "re-read: %s", s_again.why);
    CHECK(l5_layout_equal(&s_again.layout, &l), "layout on flash differs");
    if (!l5_layout_equal(&s_again.layout, &l)) print_layout("read back", &s_again.layout);
    range_t r[32];
    int n = 0;
    r[n++] = (range_t){0x8000, 0x1000, false};
    r[n++] = (range_t){0xE000, 0x2000, true};
    for (int i = 0; d.apps && i < s_next.n; i++) {
        if (s_next.p[i].type == 0 && s_next.p[i].subtype >= 0x10) r[n++] = (range_t){s_next.p[i].offset, 0x10000, true};
    }
    for (int a = 0; a < d.nerase; a++) r[n++] = (range_t){d.erase[a]->offset, d.erase[a]->size, true};
    if (d.aurora_new) r[n++] = (range_t){0xF10000, 0x6000, true};
    check_only(r, n);
    CHECK(d.aurora_new == (base != 0), "aurora_new");
    if (!d.aurora_new) CHECK(all(0xF10000, 0x6000, 0x5A), "the PIN store changed");
    for (int i = 0; i < s_next.n; i++) { /* data before aurora_cfg ends at 0xF10000 */
        const l5_part_t *p = &s_next.p[i];
        if (p->type == 1 && p->offset >= 0x210000 && p->offset < 0xF10000) CHECK(p->offset + p->size <= 0xF10000, "%s crosses 0xF10000", p->label);
    }
    const l5_part_t *gap = by_offset(&s_next, 0xF16000);
    CHECK(gap ? gap->size == l.spiffs_kb * 1024 : l.spiffs_kb == 0, "gap partition size");
    CHECK((forget_calls > 0) == d.apps, "registry forget %d, apps changed %d", forget_calls, d.apps);
    printf("   apps %s, erased: %s, removed: %s, aurora %s\n", d.apps ? "erased" : "kept", d.erased[0] ? d.erased : "-",
           d.removed[0] ? d.removed : "-", d.aurora_new ? "created (24 KB erased)" : "kept");
    check_fixed();
    char path[512];
    snprintf(path, sizeof(path), "%s/pt_%s_from_%d.bin", outdir, name, base);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(flash + 0x8000, 1, 0xC00, f);
        fclose(f);
    }
    return true;
}

static void refuse_next(const char *what, void (*tamper)(l5_ptable_t *))
{
    printf("== %s: refused\n", what);
    load(tables[0]);
    char why[200] = "";
    l5_layout_t l = {.n = 1, .slot_kb = 7168, .spiffs_kb = L5_GAP_KB, .fat_kb = 6144};
    l5_pt_read_map(&s_map);
    CHECK(l5_pt_build(&s_map, &l, &s_next, why, sizeof(why)) == ESP_OK, "build: %s", why);
    tamper(&s_next);
    memcpy(snapshot, flash, FLASH_SIZE);
    bool written = true;
    esp_err_t err = l5_pt_apply(&s_map, &s_next, NULL, NULL, &written, why, sizeof(why));
    CHECK(err != ESP_OK && !written, "apply should refuse");
    CHECK(memcmp(flash, snapshot, FLASH_SIZE) == 0, "flash touched");
    printf("   %s\n", why);
}

static void tamper_aurora_size(l5_ptable_t *t)
{
    for (int i = 0; i < t->n; i++) if (!strcmp(t->p[i].label, "aurora_cfg")) t->p[i].size = 0x7000;
}
static void tamper_no_aurora(l5_ptable_t *t)
{
    int k = 0;
    for (int i = 0; i < t->n; i++) if (strcmp(t->p[i].label, "aurora_cfg")) t->p[k++] = t->p[i];
    t->n = k;
}
static void tamper_fat_over_aurora(l5_ptable_t *t)
{
    for (int i = 0; i < t->n; i++) if (!strcmp(t->p[i].label, "vfs")) t->p[i].size += 0x1000;
}
static void tamper_nvs(l5_ptable_t *t)
{
    t->p[0].size += 0x1000;
}

static l5_layout_t scheme(const char *id)
{
    return l5_layout_of_scheme(l5_scheme_find(id));
}

/* ======================= OTA ======================= */

static uint8_t *app_img, *boot_img, *srv;
static size_t app_len, boot_len, srv_len;
static uint8_t *expect_app; /* what the slot must hold after an install (esp_image_verify stub) */
static size_t expect_len;
static int net_opens, net_drops_left, verify_calls, release_calls, register_calls;
static uint32_t net_drop_at, net_cancel_at;
static volatile bool *net_cancel_flag;
static bool rev_ok_answer = true;
static char reg_name[64], reg_src[16], reg_fid[40], reg_ver[32];
static uint32_t reg_len;

struct l5_net_stream {
    uint32_t pos, end;
    volatile bool *cancel;
};

esp_err_t l5_net_open(const char *url, uint32_t offset, uint32_t len, volatile bool *cancel, l5_net_stream_t **out,
                      int64_t *length, int64_t *total, l5_net_err_t *e)
{
    (void)url;
    net_opens++;
    *out = NULL;
    if (cancel && *cancel) {
        e->fail = L5_NET_CANCEL;
        strcpy(e->text, "Cancelled.");
        return L5_NET_CANCELLED;
    }
    if (offset >= srv_len) {
        e->fail = L5_NET_HTTP;
        e->status = 416;
        strcpy(e->text, "The server answered HTTP 416.");
        return ESP_FAIL;
    }
    struct l5_net_stream *s = calloc(1, sizeof(*s));
    s->pos = offset;
    s->cancel = cancel;
    s->end = len ? offset + len : (uint32_t)srv_len;
    if (s->end > srv_len) s->end = (uint32_t)srv_len;
    if (length) *length = s->end - s->pos;
    if (total) *total = (int64_t)srv_len;
    *out = s;
    return ESP_OK;
}

int l5_net_read(l5_net_stream_t *s, uint8_t *buf, size_t n, l5_net_err_t *e)
{
    if (s->cancel && *s->cancel) { /* as l5_net.c: checked before every read */
        e->fail = L5_NET_CANCEL;
        strcpy(e->text, "Cancelled.");
        return -1;
    }
    if (s->pos >= s->end) return 0;
    if (net_drops_left > 0 && s->pos >= net_drop_at) { /* the Wi-Fi link drops at this byte */
        net_drops_left--;
        net_drop_at += 300000;
        e->fail = L5_NET_DROPPED;
        snprintf(e->text, sizeof(e->text), "The connection broke off after some bytes.");
        return -1;
    }
    size_t k = s->end - s->pos;
    if (k > n) k = n;
    if (net_drops_left > 0 && s->pos < net_drop_at && s->pos + k > net_drop_at) k = net_drop_at - s->pos;
    memcpy(buf, srv + s->pos, k);
    s->pos += (uint32_t)k;
    if (net_cancel_flag && s->pos >= net_cancel_at) *net_cancel_flag = true;
    return (int)k;
}

void l5_net_close(l5_net_stream_t *s)
{
    free(s);
}

esp_err_t esp_image_verify(esp_image_load_mode_t mode, const esp_partition_pos_t *part, esp_image_metadata_t *data)
{
    (void)mode;
    l5_img_parse_t p;
    l5_img_begin(&p);
    if (!l5_img_feed(&p, flash + part->offset, part->size) || !p.len) return ESP_ERR_IMAGE_INVALID;
    data->image_len = p.len;
    verify_calls++;
    /* stands for the SHA-256 check: the slot holds exactly the source image */
    return p.len == expect_len && memcmp(flash + part->offset, expect_app, expect_len) == 0 ? ESP_OK : ESP_ERR_IMAGE_INVALID;
}

int64_t esp_timer_get_time(void)
{
    static int64_t t;
    return t += 1000;
}

esp_err_t l5_slots_register(int index, const uint8_t elf_sha[32], uint32_t image_len, const l5_slot_reg_t *r)
{
    (void)index;
    (void)elf_sha;
    register_calls++;
    reg_len = image_len;
    strlcpy(reg_name, r->name ? r->name : "", sizeof(reg_name));
    strlcpy(reg_src, r->src ? r->src : "", sizeof(reg_src));
    strlcpy(reg_fid, r->fid ? r->fid : "", sizeof(reg_fid));
    strlcpy(reg_ver, r->ver ? r->ver : "", sizeof(reg_ver));
    return ESP_OK;
}

void l5_slots_release(int index)
{
    (void)index;
    release_calls++;
}

bool l5_chip_rev_ok(uint16_t a, uint16_t b)
{
    (void)a;
    (void)b;
    return rev_ok_answer;
}

const char *l5_chip_name(uint16_t chip)
{
    return chip == 9 ? "ESP32-S3" : chip == 0x12 ? "ESP32-P4" : "another chip";
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n + 1);
    *len = fread(b, 1, n, f);
    b[*len] = 0;
    fclose(f);
    return b;
}

static void write_file(const char *path, const uint8_t *d, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(d, 1, n, f);
        fclose(f);
    }
}

/* "C:/x/y" -> "/x/y": l5_inst treats a leading '/' as an SD path */
static const char *drive_less(const char *p)
{
    return p[1] == ':' ? p + 2 : p;
}

static void add_part(uint8_t *table, int *n, const char *label, uint8_t type, uint8_t sub, uint32_t off, uint32_t size)
{
    esp_partition_info_t e = {.magic = ESP_PARTITION_MAGIC, .type = type, .subtype = sub, .pos = {off, size}};
    memset(e.label, 0, sizeof(e.label));
    memcpy(e.label, label, strlen(label));
    memcpy(table + (*n)++ * sizeof(e), &e, sizeof(e));
}

/* a merged image as esptool merge_bin writes it by default (and M5Burner ships it): flash from 0x0, so 0xFF up
   to the bootloader @0x2000, table @0x8000 (with its MD5 record), app @0x10000, sys FAT image @0x400000, vfs FAT
   image @0x500000; storage SPIFFS @0x9C0000 has no image */
static void make_merged(void)
{
    srv_len = 0x9C0000;
    srv = malloc(srv_len);
    memset(srv, 0xff, srv_len);
    memcpy(srv + 0x2000, boot_img, boot_len);
    uint8_t *t = srv + 0x8000;
    int n = 0;
    add_part(t, &n, "nvs", 1, 0x02, 0x9000, 0x6000);
    add_part(t, &n, "phy_init", 1, 0x01, 0xF000, 0x1000);
    add_part(t, &n, "factory", 0, 0x00, 0x10000, 0x3F0000);
    add_part(t, &n, "sys", 1, 0x81, 0x400000, 0x100000);
    add_part(t, &n, "vfs", 1, 0x81, 0x500000, 0x4C0000);
    add_part(t, &n, "storage", 1, 0x82, 0x9C0000, 0xA0000);
    uint8_t *md5 = t + n * 32;
    md5[0] = md5[1] = 0xeb;
    md5_context_t ctx;
    esp_rom_md5_init(&ctx);
    esp_rom_md5_update(&ctx, t, n * 32);
    esp_rom_md5_final(md5 + 16, &ctx);
    memcpy(srv + 0x10000, app_img, app_len);
    for (uint32_t i = 0; i < 0x100000; i++) srv[0x400000 + i] = (uint8_t)(i * 7 + 1);
    for (uint32_t i = 0; i < 0x4C0000; i++) srv[0x500000 + i] = (uint8_t)(i * 13 + 5);
}

static void test_image_parser(const char *name, const uint8_t *img, size_t len)
{
    static const size_t chunks[] = {1, 7, 24, 4096, 1 << 20};
    for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        l5_img_parse_t p;
        l5_img_begin(&p);
        for (size_t o = 0; o < len && !p.len && !p.failed; o += chunks[c]) {
            l5_img_feed(&p, img + o, o + chunks[c] <= len ? chunks[c] : len - o);
        }
        CHECK(!p.failed && p.len == len, "%s: chunks of %u: len %u, file %u (%s)", name, (unsigned)chunks[c], p.len,
              (unsigned)len, p.error);
    }
    /* walking the headers only, as l5_inst_app_len_file does on the SD card */
    l5_img_parse_t p;
    l5_img_begin(&p);
    size_t pos = 0, reads = 0;
    while (!p.len && !p.failed && pos < len) {
        uint32_t skip = l5_img_skippable(&p);
        if (skip) {
            l5_img_skip(&p, skip);
            pos += skip;
            continue;
        }
        size_t need = (p.nseg ? 8u : 24u) - p.have;
        l5_img_feed(&p, img + pos, need);
        pos += need;
        reads++;
    }
    CHECK(p.len == len, "%s: header walk len %u", name, p.len);
    printf("   %s: %u bytes, %u segments, chip 0x%04x, SHA-256 %s, header walk %u reads\n", name, (unsigned)len, p.nseg, p.chip,
           p.hash ? "appended" : "none", (unsigned)reads);
}

static l5_fw_t fw;
static l5_fw_report_t rep;

static void show_report(const l5_fw_t *f, const l5_ptable_t *t, uint32_t slot)
{
    char text[1200];
    l5_fw_check(f, t, slot, &rep);
    l5_fw_report_text(&rep, text, sizeof(text));
    printf("   check: %d problem(s), %d image(s) not writable, %d writable\n", rep.problems, rep.lost, rep.images);
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) printf("     %s\n", line);
}

static bool plan(const l5_fw_t *f, l5_layout_t *out)
{
    char notes[400];
    esp_err_t err = l5_fw_plan(f, out, notes, sizeof(notes));
    if (err == ESP_OK) print_layout("AUTO PARTITION", out);
    if (notes[0]) printf("   notes: %s\n", notes);
    return err == ESP_OK;
}

static l5_inst_result_t res;
static int prog_calls, prog_last;

static void prog_cb(int pct, const char *text, void *user)
{
    (void)user;
    (void)text;
    CHECK(pct >= prog_last || pct == 0, "progress went back %d -> %d (%s)", prog_last, pct, text);
    prog_last = pct;
    prog_calls++;
}

static int hw_calls;
static bool hw_answer = true;

static bool hw_cb(void *user)
{
    (void)user;
    hw_calls++;
    return hw_answer;
}

static esp_err_t install(const l5_fw_t *f, const l5_ptable_t *table, bool data, volatile bool *cancel)
{
    l5_inst_job_t job = {.fw = f, .table = table, .slot = 0, .data = data, .name = "Merged Test", .author = "tester",
                         .src = "OTA", .cancel = cancel, .progress = prog_cb, .hw_warn = hw_cb};
    hw_calls = 0;
    net_opens = verify_calls = release_calls = register_calls = prog_calls = prog_last = 0;
    esp_err_t err = l5_inst_run(&job, &res);
    printf("   install: %s, stage %d, image %u, data %d, opens %d%s%s\n", err == ESP_OK ? "OK" : "FAILED", res.stage,
           res.image_len, res.data_written, net_opens, res.text[0] ? " - " : "", res.text);
    return err;
}

/* the names l5_hw_scan_feed finds in `d`, fed in chunks of `chunk` bytes */
static uint8_t hw_scan(const uint8_t *d, size_t n, size_t chunk)
{
    l5_hw_scan_t s = {0};
    for (size_t o = 0; o < n; o += chunk) l5_hw_scan_feed(&s, d + o, o + chunk <= n ? chunk : n - o);
    return s.found;
}

static void hw_names(char *out, size_t len, uint8_t f)
{
    snprintf(out, len, "%s%s%s%s%s%s", f & L5_HW_GT911 ? " gt911" : "", f & L5_HW_ILI9881 ? " ili9881" : "",
             f & L5_HW_ST7123 ? " st7123" : "", f & L5_HW_ST7121 ? " st7121" : "", f & L5_HW_ST712X ? " st712x" : "",
             f ? "" : " none");
}

/* every byte-chunking of a small text finds the same names */
static void hw_scan_tests(void)
{
    printf("== driver names: chunk borders, letter case, short chunks\n");
    static const char text[] = "....Touch GT911 init.... ILI9881C panel ... St7123 ... ST712X ... st7121";
    const size_t n = sizeof(text) - 1;
    for (size_t c = 1; c <= n; c++) {
        uint8_t f = hw_scan((const uint8_t *)text, n, c);
        CHECK(f == (L5_HW_OLD | L5_HW_NEW), "chunks of %u: found 0x%x", (unsigned)c, f);
    }
    static const char old[] = "esp_lcd_touch_new_i2c_gt911 / esp_lcd_new_panel_ili9881c";
    l5_hw_scan_t s = {0};
    l5_hw_scan_feed(&s, (const uint8_t *)old, sizeof(old) - 1);
    CHECK(l5_hw_old_only(&s), "old-only text not flagged");
    static const char none[] = "gt91 ili988 st712 st7 g i s";
    CHECK(hw_scan((const uint8_t *)none, sizeof(none) - 1, 3) == 0, "partial names matched");
}

/* the rule on a real image: old-only -> warn, both sets or none -> no warning */
static void hw_scan_file(const char *label, const uint8_t *d, size_t n)
{
    char names[80];
    uint8_t f = hw_scan(d, n, 32 * 1024);
    l5_hw_scan_t s = {.found = f};
    hw_names(names, sizeof(names), f);
    printf("   %s:%s -> %s\n", label, names, l5_hw_old_only(&s) ? "WARNING (older Tab5 only)" : "no warning");
}

static void ota_tests(const char *build_dir)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/tab5_launcher.bin", build_dir);
    app_img = read_file(path, &app_len);
    snprintf(path, sizeof(path), "%s/bootloader/bootloader.bin", build_dir);
    boot_img = read_file(path, &boot_len);
    CHECK(app_img && boot_img, "build outputs missing");
    if (!app_img || !boot_img) return;

    printf("== image length parser on the real build outputs\n");
    test_image_parser("tab5_launcher.bin", app_img, app_len);
    test_image_parser("bootloader.bin", boot_img, boot_len);

    hw_scan_tests();
    printf("== driver names in the real build outputs\n");
    hw_scan_file("tab5_launcher.bin", app_img, app_len);
    hw_scan_file("bootloader.bin", boot_img, boot_len);
    CHECK((hw_scan(app_img, app_len, 4096) & (L5_HW_OLD | L5_HW_NEW)) == (L5_HW_OLD | L5_HW_NEW), "launcher: both sets expected");
    CHECK(hw_scan(boot_img, boot_len, 4096) == 0, "bootloader: no names expected");

    printf("== UIFlow2.0 v2.5.3 install manifest (launcherhub answer)\n");
    size_t jl;
    snprintf(path, sizeof(path), "%s/uiflow2_manifest.json", outdir);
    char *json = (char *)read_file("uiflow2_manifest.json", &jl);
    CHECK(json, "manifest sample missing");
    char why[200] = "";
    l5_fw_init(&fw, L5_FW_MERGED);
    CHECK(json && l5_fw_from_manifest(&fw, json, why, sizeof(why)) == ESP_OK, "manifest: %s", why);
    CHECK(fw.format == L5_FW_MERGED && fw.app_offset == 0x10000 && fw.app_size == 7709696 && fw.app_part_size == 10027008,
          "app fields %d %x %u %u", fw.format, fw.app_offset, fw.app_size, fw.app_part_size);
    CHECK(fw.nparts == 3 && !strcmp(fw.part[0].label, "sys") && fw.part[0].copy_size == 1048576 && !strcmp(fw.part[1].label, "vfs") &&
              fw.part[1].size == 4980736 && !strcmp(fw.part[2].label, "storage") && fw.part[2].copy_size == 0,
          "partitions");
    CHECK(!strcmp(fw.version, "v2.5.3") && !strcmp(fw.date, "2026-09-11") && !strcmp(fw.name, "UIFlow2.0") &&
              !strcmp(l5_fw_source_url(&fw, "firmware"), "8ef7ffee567c8a8baa20bca24392d67f.bin"),
          "names %s %s %s", fw.version, fw.date, fw.name);
    load(tables[0]);
    l5_pt_read_map(&s_map);
    show_report(&fw, &s_map.table, 0);
    CHECK(rep.app_fits && rep.problems == 2 && rep.lost == 2 && rep.images == 0, "UIFlow2 vs Default"); /* sys has nowhere to go, vfs is larger */
    l5_layout_t l;
    CHECK(plan(&fw, &l), "UIFlow2 plan");
    CHECK(l.n == 1 && l.slot_kb == 7552 && l.nextra == 1 && !strcmp(l.extra[0].label, "sys") && l.extra[0].kb == 1024 &&
              l.fat_kb == 4736 && !l.fat_label[0] && l.spiffs_kb == 640 && !strcmp(l.gap_label, "storage") && !l.gap_subtype,
          "UIFlow2 layout");
    if (run("uiflow2_auto", 0, l, true)) {
        show_report(&fw, &s_again.table, 0);
        CHECK(rep.problems == 1 && rep.images == 1 && rep.item[0].writable, "UIFlow2 after AUTO PARTITION: sys written, vfs smaller");
    }
    char *bad = malloc(jl + 16);
    strcpy(bad, json);
    char *esp = strstr(bad, "\"p4\"");
    if (esp) memcpy(esp, "\"s3\"", 4);
    l5_fw_init(&fw, L5_FW_MERGED);
    CHECK(l5_fw_from_manifest(&fw, bad, why, sizeof(why)) == ESP_ERR_NOT_SUPPORTED, "S3 manifest accepted");
    printf("   other chip: %s\n", why);
    free(bad);
    free(json);

    printf("== merged image: probe, table, exact app length, check, AUTO PARTITION\n");
    make_merged();
    l5_fw_init(&fw, L5_FW_APP_ONLY);
    CHECK(l5_fw_from_probe(&fw, srv, L5_FW_PROBE_LEN, (uint32_t)srv_len, why, sizeof(why)) == ESP_OK, "probe: %s", why);
    CHECK(fw.format == L5_FW_MERGED && fw.app_offset == 0x10000 && fw.app_part_size == 0x3F0000 && fw.nparts == 4, "merged fields");
    CHECK(fw.base == 0 && !strcmp(fw.app_label, "factory") && fw.table_end == 0xA60000, "base %x label %s end %x", fw.base,
          fw.app_label, fw.table_end);
    CHECK(l5_fw_app_bound(&fw) == 0x3F0000, "bound %x", l5_fw_app_bound(&fw));
    snprintf(path, sizeof(path), "%s/merged.bin", outdir);
    write_file(path, srv, srv_len);
    uint32_t alen = 0;
    CHECK(l5_inst_app_len_file(drive_less(path), fw.app_offset, l5_fw_app_bound(&fw), &alen) == ESP_OK && alen == app_len,
          "app length from the file %u", alen);
    fw.app_size = alen;
    l5_fw_set_source(&fw, "firmware", "https://example.invalid/merged.bin");
    strcpy(fw.fid, "abc123");
    strcpy(fw.version, "v1.0");
    load(tables[0]);
    l5_pt_read_map(&s_map);
    show_report(&fw, &s_map.table, 0);
    CHECK(rep.app_fits && rep.problems == 2 && rep.lost == 2, "merged vs Default");
    CHECK(plan(&fw, &l), "merged plan");
    CHECK(l.slot_kb == 13312 - 1024 - 4864 && l.fat_kb == 4864 && l.nextra == 1 && l.spiffs_kb == 640, "merged layout");
    static l5_ptable_t merged_table;
    static uint8_t *after_apply;
    if (!run("merged_auto", 0, l, true)) return;
    show_report(&fw, &s_again.table, 0);
    CHECK(!rep.problems && !rep.lost && rep.images == 2, "merged after AUTO PARTITION");
    merged_table = s_again.table;
    after_apply = malloc(FLASH_SIZE);
    memcpy(after_apply, flash, FLASH_SIZE);
    const l5_part_t *app0 = by_label(&merged_table, "app0"), *sys = by_label(&merged_table, "sys"),
                    *vfs = by_label(&merged_table, "vfs"), *sto = by_label(&merged_table, "storage");
    expect_app = app_img;
    expect_len = app_len;

    printf("== install over HTTPS (simulated): app + data images into the new table\n");
    memcpy(snapshot, flash, FLASH_SIZE);
    CHECK(install(&fw, &merged_table, true, NULL) == ESP_OK, "install");
    CHECK(res.image_len == app_len && res.data_written == 2 && verify_calls == 1 && register_calls == 1 && release_calls == 1,
          "result fields");
    CHECK(!strcmp(reg_name, "Merged Test") && !strcmp(reg_src, "OTA") && !strcmp(reg_fid, "abc123") && !strcmp(reg_ver, "v1.0") &&
              reg_len == app_len,
          "registry entry");
    CHECK(memcmp(flash + sys->offset, srv + 0x400000, 0x100000) == 0, "sys image");
    CHECK(memcmp(flash + vfs->offset, srv + 0x500000, 0x4C0000) == 0, "vfs image");
    CHECK(all(sto->offset, sto->size, 0xff), "storage not empty");
    {
        range_t r[3] = {{app0->offset, (uint32_t)((app_len + 0xFFF) & ~0xFFFu), false}, {sys->offset, sys->size, false},
                        {vfs->offset, vfs->size, false}};
        check_only(r, 3);
        CHECK(all(app0->offset + (uint32_t)app_len, (uint32_t)(((app_len + 0xFFF) & ~0xFFFu) - app_len), 0xff), "slot tail");
    }
    check_fixed();
    CHECK(prog_last == 100 && prog_calls > 10, "progress %d / %d calls", prog_last, prog_calls);

    printf("== APP ONLY: the data partitions stay as they are\n");
    memcpy(flash, after_apply, FLASH_SIZE);
    memset(flash + sys->offset, 0x3C, 0x1000); /* "the user's files" */
    memcpy(snapshot, flash, FLASH_SIZE);
    CHECK(install(&fw, &merged_table, false, NULL) == ESP_OK && res.data_written == 0, "app only");
    {
        range_t r[1] = {{app0->offset, (uint32_t)((app_len + 0xFFF) & ~0xFFFu), false}};
        check_only(r, 1);
    }

    printf("== Wi-Fi drops twice during the app: resumed with Range\n");
    memcpy(flash, after_apply, FLASH_SIZE);
    net_drops_left = 2;
    net_drop_at = 0x10000 + 600000;
    CHECK(install(&fw, &merged_table, true, NULL) == ESP_OK, "install with drops");
    CHECK(net_opens == 2 + 2 + 2, "opens %d (head, app, 2 resumes, 2 data)", net_opens);
    CHECK(memcmp(flash + vfs->offset, srv + 0x500000, 0x4C0000) == 0, "vfs after drops");

    printf("== Wi-Fi drops four times: stops, the slot is left without an image\n");
    memcpy(flash, after_apply, FLASH_SIZE);
    memset(flash + app0->offset, 0xE9, 64); /* an older app in the slot */
    net_drops_left = 4;
    net_drop_at = 0x10000 + 300000;
    CHECK(install(&fw, &merged_table, true, NULL) != ESP_OK, "install should fail");
    CHECK(res.net && res.erased && !res.cancelled && res.stage == L5_INST_APP, "result flags");
    CHECK(all(app0->offset, 0x1000, 0xff), "slot header left behind");
    CHECK(register_calls == 0, "registry written after a failure");
    net_drops_left = 0;

    printf("== CANCEL during the download\n");
    memcpy(flash, after_apply, FLASH_SIZE);
    volatile bool cancel = false;
    net_cancel_flag = &cancel;
    net_cancel_at = 0x10000 + 1000000;
    CHECK(install(&fw, &merged_table, true, &cancel) != ESP_OK, "cancel should stop");
    CHECK(res.cancelled && !res.net && res.erased && !strcmp(res.text, "Cancelled.") && res.stage == L5_INST_APP, "cancel flags");
    CHECK(all(app0->offset, 0x1000, 0xff), "slot header after cancel");
    net_cancel_flag = NULL;

    printf("== an app for the first Tab5 only (GT911 / ILI9881C, no ST712x): asked before the data and the header\n");
    {
        uint8_t *keep_app = expect_app;
        uint8_t *old_app = malloc(app_len);
        memcpy(old_app, app_img, app_len);
        int hidden = 0; /* the launcher names both sets: hide the new ones, the image stays the same length */
        for (size_t i = 0; i + 5 < app_len; i++) {
            if ((old_app[i] | 0x20) == 's' && (old_app[i + 1] | 0x20) == 't' && old_app[i + 2] == '7' && old_app[i + 3] == '1' &&
                old_app[i + 4] == '2') {
                old_app[i] = 'z';
                hidden++;
            }
        }
        l5_hw_scan_t sc = {.found = hw_scan(old_app, app_len, 32 * 1024)};
        CHECK(l5_hw_old_only(&sc), "the edited image should name the old display only (%d names hidden)", hidden);
        memcpy(srv + 0x10000, old_app, app_len);
        expect_app = old_app;
        memcpy(flash, after_apply, FLASH_SIZE);
        memset(flash + sys->offset, 0x3C, 0x1000); /* "the user's files" in a data partition */
        memcpy(snapshot, flash, FLASH_SIZE);
        hw_answer = false;
        CHECK(install(&fw, &merged_table, true, NULL) != ESP_OK && hw_calls == 1 && res.cancelled && res.erased &&
                  res.data_written == 0 && register_calls == 0,
              "CANCEL at the warning");
        CHECK(all(app0->offset, 0x1000, 0xff), "no image header after CANCEL");
        CHECK(memcmp(flash + sys->offset, snapshot + sys->offset, sys->size) == 0, "CANCEL touched a data partition");
        {
            range_t r[1] = {{app0->offset, (uint32_t)((app_len + 0xFFF) & ~0xFFFu), false}};
            check_only(r, 1);
        }
        hw_answer = true;
        memcpy(flash, after_apply, FLASH_SIZE);
        CHECK(install(&fw, &merged_table, true, NULL) == ESP_OK && hw_calls == 1 && res.data_written == 2, "INSTALL ANYWAY");
        memcpy(srv + 0x10000, app_img, app_len); /* back to the real launcher image */
        expect_app = keep_app;
        memcpy(flash, after_apply, FLASH_SIZE);
        CHECK(install(&fw, &merged_table, true, NULL) == ESP_OK && hw_calls == 0, "both sets: no question");
        free(old_app);
    }

    printf("== an ESP32-S3 app: refused before anything is erased\n");
    memcpy(flash, after_apply, FLASH_SIZE);
    memcpy(snapshot, flash, FLASH_SIZE);
    srv[0x10000 + 12] = 9;
    CHECK(install(&fw, &merged_table, true, NULL) != ESP_OK && !res.erased && res.stage == L5_INST_CHECK, "S3 refused");
    CHECK(memcmp(flash, snapshot, FLASH_SIZE) == 0, "flash touched by a refused image");
    srv[0x10000 + 12] = 0x12;
    rev_ok_answer = false;
    CHECK(install(&fw, &merged_table, true, NULL) != ESP_OK && !res.erased, "chip revision refused");
    rev_ok_answer = true;

    printf("== the catalog size is larger than the slot: refused at once\n");
    {
        l5_fw_t big = fw;
        big.app_size = app0->size + 1;
        CHECK(install(&big, &merged_table, true, NULL) != ESP_OK && !res.erased && net_opens == 0, "too big refused");
    }

    printf("== the catalog size is too small: the installer erases on as the header says\n");
    {
        l5_fw_t small = fw;
        small.app_size = 512 * 1024;
        memcpy(flash, after_apply, FLASH_SIZE);
        memset(flash + app0->offset, 0x00, app0->size); /* not erased: a missed erase would corrupt the image */
        CHECK(install(&small, &merged_table, false, NULL) != ESP_OK, "a too small bound must fail (range ends)");
        printf("   (the source range ends at the catalog size: %s)\n", res.text);
    }

    printf("== the same merged image from the SD card\n");
    memcpy(flash, after_apply, FLASH_SIZE);
    {
        l5_fw_t sd = fw;
        l5_fw_set_source(&sd, "firmware", drive_less(path));
        uint8_t *keep = srv;
        srv = NULL; /* no network */
        CHECK(install(&sd, &merged_table, true, NULL) == ESP_OK && net_opens == 0 && res.data_written == 2, "SD install");
        srv = keep;
    }

    printf("== merged image that starts with the bootloader (flash 0x2000) from the SD card\n");
    {
        const uint8_t *img2 = srv + 0x2000; /* the same image without the 8 KB before the bootloader */
        size_t len2 = srv_len - 0x2000;
        l5_fw_t b2;
        l5_fw_init(&b2, L5_FW_APP_ONLY);
        CHECK(l5_fw_from_probe(&b2, img2, L5_FW_PROBE_LEN, (uint32_t)len2, why, sizeof(why)) == ESP_OK && b2.format == L5_FW_MERGED &&
                  b2.base == 0x2000 && b2.app_offset == 0xE000 && b2.nparts == 4,
              "base 0x2000 probe: %s (base %x, app at %x)", why, b2.base, b2.app_offset);
        const l5_fw_part_t *ps = NULL;
        for (int i = 0; i < b2.nparts; i++) {
            if (!strcmp(b2.part[i].label, "sys")) ps = &b2.part[i];
        }
        CHECK(ps && ps->offset == 0x400000 && ps->src_offset == 0x3FE000 && ps->copy_size == 0x100000, "sys image offsets");
        char p2[400];
        snprintf(p2, sizeof(p2), "%s/merged2000.bin", outdir);
        write_file(p2, img2, len2);
        uint32_t a2 = 0;
        CHECK(l5_inst_app_len_file(drive_less(p2), b2.app_offset, l5_fw_app_bound(&b2), &a2) == ESP_OK && a2 == app_len,
              "app length from the 0x2000 file %u", a2);
        b2.app_size = a2;
        l5_fw_set_source(&b2, "firmware", drive_less(p2));
        memcpy(flash, after_apply, FLASH_SIZE);
        uint8_t *keep = srv;
        srv = NULL; /* no network */
        CHECK(install(&b2, &merged_table, true, NULL) == ESP_OK && net_opens == 0 && res.data_written == 2, "0x2000 SD install");
        srv = keep;
        CHECK(memcmp(flash + app0->offset, app_img, app_len) == 0, "app0 from the 0x2000 file");
        CHECK(memcmp(flash + sys->offset, srv + 0x400000, 0x100000) == 0, "sys from the 0x2000 file");
        CHECK(memcmp(flash + vfs->offset, srv + 0x500000, 0x4C0000) == 0, "vfs from the 0x2000 file");

        printf("== SANITY CHECK: the app inside both merged images\n");
        l5_image_check_t c;
        l5_image_check_file(drive_less(path), 0x10000, &c);
        CHECK(c.chip_id == 0x0012 && c.segments && c.checksum_ok && c.image_len == app_len, "app in the 0x0 file: %s", c.error);
        l5_image_check_file(drive_less(p2), 0xE000, &c);
        CHECK(c.chip_id == 0x0012 && c.segments && c.checksum_ok && c.image_len == app_len, "app in the 0x2000 file: %s", c.error);
        CHECK(l5_image_check_file(drive_less(path), 0, &c) != ESP_OK && !c.segments, "offset 0 of the 0x0 file is padding");
    }

    printf("== refused: a copy of a whole flash, an image of the launcher, bootloader at 0 with the table at 0x8000\n");
    {
        uint8_t *dump = malloc(16u << 20);
        memset(dump, 0xff, 16u << 20);
        memcpy(dump, srv, srv_len);
        l5_fw_t d;
        l5_fw_init(&d, L5_FW_APP_ONLY);
        CHECK(l5_fw_from_probe(&d, dump, L5_FW_PROBE_LEN, 16u << 20, why, sizeof(why)) == ESP_ERR_INVALID_SIZE &&
                  strstr(why, "copy of a whole 16 MB flash") && d.format == L5_FW_MERGED && d.app_offset == 0x10000,
              "16 MB dump: %s", why);
        printf("   %s\n", why);
        l5_fw_init(&d, L5_FW_APP_ONLY);
        CHECK(l5_fw_from_probe(&d, dump, L5_FW_PROBE_LEN, 12u << 20, why, sizeof(why)) == ESP_OK, "12 MB merged image: %s", why);
        memcpy(dump + 0x8000 + 2 * 32 + 12, "launcher\0\0\0\0\0\0\0\0", 16); /* the factory entry's label */
        l5_fw_init(&d, L5_FW_APP_ONLY);
        CHECK(l5_fw_from_probe(&d, dump, L5_FW_PROBE_LEN, (uint32_t)srv_len, why, sizeof(why)) == ESP_ERR_INVALID_STATE &&
                  strstr(why, "Tab5 Launcher"),
              "launcher image: %s", why);
        printf("   %s\n", why);
        memset(dump, 0xff, 0x8000); /* the layout of the old test image: bootloader at 0, table at 0x8000 */
        memcpy(dump, boot_img, boot_len);
        l5_fw_init(&d, L5_FW_APP_ONLY);
        CHECK(l5_fw_from_probe(&d, dump, L5_FW_PROBE_LEN, (uint32_t)srv_len, why, sizeof(why)) == ESP_ERR_NOT_SUPPORTED,
              "bootloader at 0 with the table at 0x8000 accepted");
        free(dump);
    }

    printf("== an app image from the SD card into the current Default table\n");
    load(tables[0]);
    l5_pt_read_map(&s_map);
    snprintf(path, sizeof(path), "%s/app.bin", outdir);
    write_file(path, app_img, app_len);
    {
        l5_fw_t a;
        l5_fw_init(&a, L5_FW_APP_ONLY);
        CHECK(l5_fw_from_probe(&a, app_img, L5_FW_PROBE_LEN, (uint32_t)app_len, why, sizeof(why)) == ESP_OK &&
                  a.format == L5_FW_APP_ONLY && a.app_size == app_len,
              "app probe");
        l5_fw_set_source(&a, "firmware", drive_less(path));
        memcpy(snapshot, flash, FLASH_SIZE);
        CHECK(install(&a, &s_map.table, true, NULL) == ESP_OK, "SD app install");
        range_t r[1] = {{0x210000, (uint32_t)((app_len + 0xFFF) & ~0xFFFu), false}};
        check_only(r, 1);
        check_fixed();
    }

    printf("== plans for other firmware tables\n");
    {
        l5_fw_t f;
        l5_fw_init(&f, L5_FW_MERGED);
        f.app_size = 12800 * 1024;
        f.nparts = 1;
        f.part[0] = (l5_fw_part_t){.label = "sys", .type = 1, .subtype = 0x81, .size = 0x100000, .copy_size = 0x100000};
        CHECK(plan(&f, &l) && l.nextra == 0 && l.fat_kb == 512 && !strcmp(l.fat_label, "sys"), "12.5 MB app + 1 MB FAT image");
        f.app_size = 13 * 1024 * 1024 + 1;
        CHECK(!plan(&f, &l), "an app larger than the front must fail");
        l5_fw_init(&f, L5_FW_MERGED);
        f.app_size = 3 * 1024 * 1024;
        f.nparts = 3;
        f.part[0] = (l5_fw_part_t){.label = "spiffs", .type = 1, .subtype = 0x82, .size = 0x40000, .copy_size = 0x40000};
        f.part[1] = (l5_fw_part_t){.label = "littlefs", .type = 1, .subtype = 0x83, .size = 0x200000};
        f.part[2] = (l5_fw_part_t){.label = "nvs_keys", .type = 1, .subtype = 0x04, .size = 0x1000};
        CHECK(plan(&f, &l) && l.spiffs_kb == 256 && !l.gap_label[0] && l.nextra == 1 && l.extra[0].subtype == 0x83, "spiffs 256 KB");
        run("spiffs256_auto", 0, l, true);
        f.part[0].size = f.part[0].copy_size = 0; /* drop spiffs: the LittleFS goes to the gap */
        f.part[0] = f.part[1];
        f.part[1].size = 0xD0000;
        strcpy(f.part[1].label, "lfs2");
        f.nparts = 2;
        CHECK(plan(&f, &l) && l.gap_subtype == 0x83 && !strcmp(l.gap_label, "lfs2"), "LittleFS in the gap");
        run("littlefs_gap_auto", 0, l, true);
        l5_fw_init(&f, L5_FW_MERGED); /* six partitions: two are left out */
        f.app_size = 1024 * 1024;
        static const char *const names[] = {"a", "b", "c", "d", "e", "f", "g"};
        for (int i = 0; i < 7; i++) {
            f.part[i] = (l5_fw_part_t){.type = 1, .subtype = 0x82, .size = 0x10000 * (i + 1)};
            strcpy(f.part[i].label, names[i]);
        }
        f.nparts = 7;
        CHECK(plan(&f, &l) && l.nextra == 4, "seven partitions");
        run("seven_parts_auto", 0, l, true);
    }
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "--scan")) { /* run.py --scan file...: the old-hardware rule on any image */
        printf("driver names (warning on a later Tab5 when only gt911/ili9881 are named):\n");
        for (int i = 2; i < argc; i++) {
            size_t n;
            uint8_t *d = read_file(argv[i], &n);
            if (!d) {
                printf("   %s: cannot be read\n", argv[i]);
                failures++;
                continue;
            }
            hw_scan_file(argv[i], d, n);
            free(d);
        }
        return failures ? 1 : 0;
    }
    if (argc < 6) {
        printf("usage: test cur_default.bin old_default.bin old_uiflow2.bin outdir build_dir\n");
        return 2;
    }
    outdir = argv[4];
    flash = malloc(FLASH_SIZE);
    snapshot = malloc(FLASH_SIZE);
    for (int t = 0; t < 3; t++) {
        memset(tables[t], 0xff, 0x1000);
        FILE *f = fopen(argv[1 + t], "rb");
        if (!f || fread(tables[t], 1, 0xC00, f) != 0xC00) return 3;
        fclose(f);
    }
    printf("front %u KB, gap %u KB\n", (unsigned)L5_FRONT_KB, (unsigned)L5_GAP_KB);
    for (int i = 0; i < l5_scheme_count; i++) {
        l5_layout_t l = l5_layout_of_scheme(&l5_schemes[i]);
        CHECK(l5_layout_front_kb(&l) == L5_FRONT_KB, "%s does not fill the front", l5_schemes[i].id);
        printf("scheme %-10s %lu x %5lu KB%s, vfs %5lu KB, spiffs %lu KB\n", l5_schemes[i].id, (unsigned long)l.n,
               (unsigned long)l.slot_kb, l.app0_kb ? " (app0 bigger)" : "", (unsigned long)l.fat_kb, (unsigned long)l.spiffs_kb);
        if (strcmp(l5_schemes[i].id, "default") == 0) continue; /* that is the current Default table itself */
        run(l5_schemes[i].id, 0, l, true);
    }
    run("custom3", 0, (l5_layout_t){.n = 3, .slot_kb = 3072, .fat_kb = 4096}, true);
    run("no_spiffs", 0, (l5_layout_t){.n = 2, .slot_kb = 4096, .fat_kb = 5120}, true);
    run("no_fat", 0, (l5_layout_t){.n = 2, .slot_kb = 4096, .spiffs_kb = L5_GAP_KB}, true);
    run("fat_smaller", 0, (l5_layout_t){.n = 2, .slot_kb = 4096, .spiffs_kb = L5_GAP_KB, .fat_kb = 2048}, true);
    run("aurora_app", 0, (l5_layout_t){.n = 1, .slot_kb = 6720, .spiffs_kb = L5_GAP_KB, .fat_kb = 6592}, true);
    run("spiffs448", 0, (l5_layout_t){.n = 1, .slot_kb = 7168, .spiffs_kb = 448, .fat_kb = 6144}, true);
    run("default", 1, scheme("default"), true);
    run("uiflow2", 1, scheme("uiflow2"), true);
    run("default", 2, scheme("default"), true);
    run("uiflow2", 2, scheme("uiflow2"), true);
    run("spiffs_odd", 0, (l5_layout_t){.n = 1, .slot_kb = 7168, .spiffs_kb = 450, .fat_kb = 6144}, false);
    run("spiffs_big", 0, (l5_layout_t){.n = 1, .slot_kb = 7168, .spiffs_kb = 876, .fat_kb = 6144}, false);
    run("front_overflow", 0, (l5_layout_t){.n = 2, .slot_kb = 4096, .spiffs_kb = L5_GAP_KB, .fat_kb = 5184}, false);
    run("small_slot", 0, (l5_layout_t){.n = 1, .slot_kb = 512, .spiffs_kb = L5_GAP_KB, .fat_kb = 448}, false);
    run("small_fat", 0, (l5_layout_t){.n = 2, .slot_kb = 4096, .fat_kb = 100}, false);
    run("nine_slots", 0, (l5_layout_t){.n = 9, .slot_kb = 1024}, false);
    /* app0 of its own size (26.10.2026): smaller than the others is fine too; it needs a second slot, 64 KB steps, 1 MB */
    run("app0_smaller", 0, (l5_layout_t){.n = 3, .app0_kb = 2048, .slot_kb = 4096, .spiffs_kb = L5_GAP_KB, .fat_kb = 3072}, true);
    run("bigsmall_from_uiflow2", 1, scheme("bigsmall"), true);
    run("app0_one_slot", 0, (l5_layout_t){.n = 1, .app0_kb = 4096, .slot_kb = 4096, .fat_kb = 4096}, false);
    run("app0_odd", 0, (l5_layout_t){.n = 2, .app0_kb = 4000, .slot_kb = 4096, .fat_kb = 4096}, false);
    run("app0_tiny", 0, (l5_layout_t){.n = 2, .app0_kb = 512, .slot_kb = 4096, .fat_kb = 4096}, false);
    {
        l5_layout_t x = scheme("default");
        x.nextra = 1;
        strcpy(x.extra[0].label, "nvs");
        x.extra[0].subtype = 0x02;
        x.extra[0].kb = 64;
        x.fat_kb -= 64;
        run("extra_named_nvs", 0, x, false);
        strcpy(x.extra[0].label, "app9");
        run("extra_named_app", 0, x, false);
    }
    refuse_next("aurora_cfg resized in the new table", tamper_aurora_size);
    refuse_next("aurora_cfg missing in the new table", tamper_no_aurora);
    refuse_next("vfs over aurora_cfg", tamper_fat_over_aurora);
    refuse_next("nvs changed", tamper_nvs);

    printf("== base layout of the old UiFlow2 table\n");
    {
        load(tables[2]);
        l5_pt_read_map(&s_map);
        l5_layout_t b = l5_pt_base_layout(&s_map);
        print_layout("raw", &s_map.layout);
        print_layout("base", &b);
        CHECK(b.spiffs_kb == L5_GAP_KB && l5_layout_front_kb(&b) <= L5_FRONT_KB, "base layout breaks the rule");
    }
    printf("== table write fails (old Default -> UiFlow2): old table back, nothing else touched\n");
    {
        load(tables[1]);
        char why[200] = "";
        l5_layout_t l = scheme("uiflow2");
        l5_pt_read_map(&s_map);
        CHECK(l5_pt_build(&s_map, &l, &s_next, why, sizeof(why)) == ESP_OK, "build: %s", why);
        memcpy(snapshot, flash, FLASH_SIZE);
        bool written = true;
        fail_table_write = 1;
        esp_err_t err = l5_pt_apply(&s_map, &s_next, NULL, NULL, &written, why, sizeof(why));
        CHECK(err != ESP_OK && !written, "apply should fail");
        CHECK(memcmp(flash + 0x8000, tables[1], 0x1000) == 0, "old table not restored");
        range_t r[2] = {{0xE000, 0x2000, true}, {0x8000, 0x1000, false}};
        check_only(r, 2);
        printf("   %s\n", why);
        check_fixed();
    }
    ota_tests(argv[5]);
    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
