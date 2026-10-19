#include "l5_inst.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "l5_net.h"
#include "l5_slots.h"

static const char *TAG = "l5_inst";
#define SECTOR 0x1000
#define BLOCK 0x10000
#define CHUNK (32 * 1024)
#define RESUMES 3
#define APP_DESC_OFFSET (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))

static void say(char *out, size_t len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void say(char *out, size_t len, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, len, fmt, ap);
    va_end(ap);
}

static bool is_file(const char *url)
{
    return url[0] == '/';
}

/* ---------- sources: a byte range of an SD file or of an HTTPS resource ---------- */

typedef struct {
    FILE *f;
    l5_net_stream_t *ns;
    const char *url;
    uint32_t off, len, got; /* range (len 0 = to the end), bytes delivered */
    int resumes;
    volatile bool *cancel;
    l5_net_err_t ne;
} src_t;

static bool cancelled(volatile bool *c)
{
    return c && *c;
}

static void net_failed(const l5_net_err_t *e, l5_inst_result_t *res)
{
    res->cancelled = e->fail == L5_NET_CANCEL;
    /* a missing file (404) stays missing; the rest may work on the next try */
    res->net = !res->cancelled && !(e->fail == L5_NET_HTTP && e->status < 500);
    strlcpy(res->text, e->text, sizeof(res->text));
}

static esp_err_t src_open(src_t *s, const char *url, uint32_t off, uint32_t len, volatile bool *cancel, l5_inst_result_t *res)
{
    memset(s, 0, sizeof(*s));
    s->url = url;
    s->off = off;
    s->len = len;
    s->cancel = cancel;
    if (is_file(url)) {
        s->f = fopen(url, "rb");
        if (!s->f || fseek(s->f, off, SEEK_SET) != 0) {
            say(res->text, sizeof(res->text), "%.140s cannot be read from the SD card.", url);
            if (s->f) fclose(s->f);
            s->f = NULL;
            return ESP_ERR_NOT_FOUND;
        }
        return ESP_OK;
    }
    esp_err_t err = l5_net_open(url, off, len, cancel, &s->ns, NULL, NULL, &s->ne);
    if (err != ESP_OK) net_failed(&s->ne, res);
    return err;
}

static void src_close(src_t *s)
{
    if (s->f) fclose(s->f);
    if (s->ns) l5_net_close(s->ns);
    s->f = NULL;
    s->ns = NULL;
}

static bool resumable(const l5_net_err_t *e)
{
    return e->fail == L5_NET_DROPPED || e->fail == L5_NET_TIMEOUT || e->fail == L5_NET_CONNECT || e->fail == L5_NET_DNS ||
           e->fail == L5_NET_NO_WIFI;
}

/* a broken download goes on where it stopped (Range), up to RESUMES times; Wi-Fi may rejoin meanwhile */
static bool resume(src_t *s)
{
    while (resumable(&s->ne) && s->resumes < RESUMES && !cancelled(s->cancel)) {
        s->resumes++;
        ESP_LOGW(TAG, "%s Resuming at byte %lu (try %d of %d).", s->ne.text, (unsigned long)(s->off + s->got), s->resumes,
                 RESUMES);
        if (s->ns) l5_net_close(s->ns);
        s->ns = NULL;
        for (int t = 0; t < 20 * s->resumes && !cancelled(s->cancel); t++) vTaskDelay(pdMS_TO_TICKS(100));
        if (l5_net_open(s->url, s->off + s->got, s->len ? s->len - s->got : 0, s->cancel, &s->ns, NULL, NULL, &s->ne) == ESP_OK) {
            return true;
        }
    }
    return false;
}

/* > 0: bytes, 0: end of the range, < 0: error (res->text) */
static int src_read(src_t *s, uint8_t *buf, size_t n, l5_inst_result_t *res)
{
    if (s->len && s->got >= s->len) return 0;
    if (s->len && n > s->len - s->got) n = s->len - s->got;
    if (s->f) {
        if (cancelled(s->cancel)) {
            res->cancelled = true;
            say(res->text, sizeof(res->text), "Cancelled.");
            return -1;
        }
        size_t r = fread(buf, 1, n, s->f);
        if (r == 0 && s->len) {
            say(res->text, sizeof(res->text), "Reading the SD card stopped after %lu bytes.", (unsigned long)s->got);
            return -1;
        }
        s->got += r;
        return (int)r;
    }
    for (;;) {
        int r = s->ns ? l5_net_read(s->ns, buf, n, &s->ne) : -1;
        if (r >= 0) {
            s->got += r;
            return r;
        }
        if (!resume(s)) {
            net_failed(&s->ne, res);
            return -1;
        }
    }
}

/* ---------- flash: only the app area, never aurora_cfg ---------- */

static bool range_ok(uint32_t addr, uint32_t len)
{
    return addr >= L5_POOL_START && len <= L5_POOL_END - addr &&
           !(addr < L5_AURORA_ADDR + L5_AURORA_SIZE && addr + len > L5_AURORA_ADDR);
}

static esp_err_t flash_erase(uint32_t addr, uint32_t len)
{
    if (!range_ok(addr, len) || addr % SECTOR || len % SECTOR) {
        ESP_LOGE(TAG, "refusing to erase 0x%lx+0x%lx", (unsigned long)addr, (unsigned long)len);
        return ESP_ERR_INVALID_ARG;
    }
    return esp_flash_erase_region(NULL, addr, len);
}

static esp_err_t flash_write(uint32_t addr, const void *buf, uint32_t len)
{
    if (!range_ok(addr, len)) {
        ESP_LOGE(TAG, "refusing to write 0x%lx+0x%lx", (unsigned long)addr, (unsigned long)len);
        return ESP_ERR_INVALID_ARG;
    }
    return esp_flash_write(NULL, buf, addr, len);
}

/* ---------- progress ---------- */

typedef struct {
    const l5_inst_job_t *job;
    int pct;
    int64_t t;
} prog_t;

static void prog(prog_t *p, int pct, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void prog(prog_t *p, int pct, const char *fmt, ...)
{
    int64_t now = esp_timer_get_time();
    if (!p->job->progress || (pct == p->pct && now - p->t < 500000)) return;
    char text[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    p->pct = pct;
    p->t = now;
    p->job->progress(pct, text, p->job->user);
}

/* ---------- the install ---------- */

typedef struct {
    const char *url;
    uint32_t src_off, len;  /* bytes to copy; the app: an upper bound, its header tells the length */
    uint32_t addr, size;    /* target partition */
    uint32_t erase;         /* bytes erased up front */
    const char *label;      /* NULL = the app */
    uint8_t subtype;
} piece_t;

static void log_rate(const char *what, uint32_t bytes, int64_t t0, int64_t t_flash)
{
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "%s: %lu bytes in %lld ms (%llu KB/s), flash writes %lld ms", what, (unsigned long)bytes, ms,
             ms > 0 ? (unsigned long long)bytes * 1000 / 1024 / ms : 0ULL, t_flash / 1000);
}

esp_err_t l5_inst_run(const l5_inst_job_t *job, l5_inst_result_t *res)
{
    memset(res, 0, sizeof(*res));
    const l5_fw_t *fw = job->fw;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    uint8_t *buf = NULL, *head = NULL;
    src_t s = {0};
    prog_t pr = {.job = job, .pct = -1};
    piece_t piece[1 + L5_FW_MAX_PARTS];
    int np = 0;
    char a[16], b[16];
    const char *verb = "Writing";

    const l5_part_t *slot = NULL;
    for (int i = 0; i < job->table->n; i++) {
        const l5_part_t *q = &job->table->p[i];
        if (q->type == ESP_PARTITION_TYPE_APP && q->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_MIN + job->slot) slot = q;
    }
    const char *app_url = l5_fw_source_url(fw, fw->app_source);
    if (!slot || !range_ok(slot->offset, slot->size)) {
        say(res->text, sizeof(res->text), "app%d is not in the partition table.", job->slot);
        goto out;
    }
    if (!app_url) {
        say(res->text, sizeof(res->text), "The firmware entry names no file for the app.");
        goto out;
    }
    if (!is_file(app_url)) verb = "Downloading";
    if (fw->app_size > slot->size) {
        l5_fw_size(a, sizeof(a), fw->app_size);
        l5_fw_size(b, sizeof(b), slot->size);
        say(res->text, sizeof(res->text), "The app needs %s, app%d has %s.", a, job->slot, b);
        err = ESP_ERR_INVALID_SIZE;
        goto out;
    }
    uint32_t bound = l5_fw_app_bound(fw);
    if (!bound || bound > slot->size) bound = slot->size;
    piece[np++] = (piece_t){.url = app_url, .src_off = fw->app_offset, .len = bound, .addr = slot->offset, .size = slot->size};
    for (int i = 0; job->data && i < fw->nparts; i++) { /* data images that fit their partition exactly */
        const l5_fw_part_t *p = &fw->part[i];
        const l5_part_t *q = p->copy_size ? l5_fw_match(p, job->table) : NULL;
        const char *url = q ? l5_fw_source_url(fw, p->source) : NULL;
        if (!url) continue;
        if (q->type != ESP_PARTITION_TYPE_DATA || !range_ok(q->offset, q->size)) {
            ESP_LOGW(TAG, "%s: not written (outside the app area)", p->label);
            continue;
        }
        piece[np++] = (piece_t){.url = url, .src_off = p->src_offset, .len = p->copy_size, .addr = q->offset,
                                .size = q->size, .label = p->label, .subtype = p->subtype};
    }
    buf = heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    head = heap_caps_malloc(SECTOR, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf || !head) {
        say(res->text, sizeof(res->text), "Not enough memory for the install.");
        err = ESP_ERR_NO_MEM;
        goto out;
    }

    /* 1. the first 4 KB: the source answers and holds an app this chip runs */
    res->stage = L5_INST_CHECK;
    prog(&pr, 0, "%s the image header", is_file(app_url) ? "Reading" : "Downloading");
    if ((err = src_open(&s, app_url, fw->app_offset, bound, job->cancel, res)) != ESP_OK) goto out;
    uint32_t h = 0;
    for (int r = 1; h < SECTOR && r > 0; h += r > 0 ? r : 0) {
        if ((r = src_read(&s, head + h, SECTOR - h, res)) < 0) {
            err = ESP_FAIL;
            goto out;
        }
    }
    l5_img_parse_t ip;
    l5_img_begin(&ip);
    esp_app_desc_t desc;
    memcpy(&desc, head + APP_DESC_OFFSET, sizeof(desc));
    err = ESP_ERR_IMAGE_INVALID;
    l5_hw_scan_t hw = {0};
    l5_hw_scan_feed(&hw, head, h);
    if (h < APP_DESC_OFFSET + sizeof(desc) || !l5_img_feed(&ip, head, h)) {
        say(res->text, sizeof(res->text), "%s", ip.failed ? ip.error : "The file is too short for an app image.");
        goto out;
    }
    if (ip.chip != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        say(res->text, sizeof(res->text), "The app is built for %s, not ESP32-P4.", l5_chip_name(ip.chip));
        goto out;
    }
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        say(res->text, sizeof(res->text), "There is no app at offset 0x%lx of the file (a bootloader?).",
            (unsigned long)fw->app_offset);
        goto out;
    }
    if (!l5_chip_rev_ok(ip.rev_min, ip.rev_max)) {
        say(res->text, sizeof(res->text), "The app is built for chip revision v%u.%u to v%u.%u, not for this ESP32-P4.",
            ip.rev_min / 100, ip.rev_min % 100, ip.rev_max / 100, ip.rev_max % 100);
        goto out;
    }
    if (ip.len ? ip.len > h && h < SECTOR : h < SECTOR) {
        say(res->text, sizeof(res->text), "The file ends inside the app image.");
        goto out;
    }
    if (!is_file(app_url)) src_close(&s); /* not left idle while flash is erased; reopened at byte 4096 */
    ESP_LOGI(TAG, "%s %s into app%d (0x%lx), %d data image(s), from %.80s", desc.project_name, desc.version, job->slot,
             (unsigned long)slot->offset, np - 1, app_url);

    /* 2. erase the slot as far as the image can reach (data partitions: right before their image) */
    res->stage = L5_INST_ERASE;
    l5_slots_release(job->slot);
    res->erased = true;
    uint64_t total = 0, done = 0;
    for (int i = 0; i < np; i++) {
        piece[i].erase = i == 0 ? (piece[i].len + SECTOR - 1) / SECTOR * SECTOR : piece[i].size;
        total += piece[i].len;
    }
    for (uint32_t off = 0; off < piece[0].erase; off += BLOCK) {
        uint32_t n = piece[0].erase - off < BLOCK ? piece[0].erase - off : BLOCK;
        if (cancelled(job->cancel)) {
            res->cancelled = true;
            err = L5_NET_CANCELLED;
            goto out;
        }
        if ((err = flash_erase(slot->offset + off, n)) != ESP_OK) {
            say(res->text, sizeof(res->text), "Erasing flash at 0x%lx failed (%s).", (unsigned long)(slot->offset + off),
                esp_err_to_name(err));
            goto out;
        }
        prog(&pr, 2 + (int)((uint64_t)(off + n) * 12 / piece[0].erase), "Erasing the app slot");
    }

    /* 3. the app from byte h on: the header tells where it ends */
    res->stage = L5_INST_APP;
    int64_t t0 = esp_timer_get_time(), t_flash = 0;
    if (!s.f && (err = src_open(&s, app_url, fw->app_offset + h, bound - h, job->cancel, res)) != ESP_OK) goto out;
    uint32_t pos = h, app_erased = piece[0].erase;
    done = h;
    while (!ip.len || pos < ip.len) {
        uint32_t want = CHUNK;
        if (ip.len && ip.len - pos < want) want = ip.len - pos;
        int r = src_read(&s, buf, want, res);
        if (r <= 0) {
            if (r == 0) say(res->text, sizeof(res->text), "The file ends inside the app image (%lu bytes).", (unsigned long)pos);
            err = ESP_FAIL;
            goto out;
        }
        l5_hw_scan_feed(&hw, buf, r);
        if (!l5_img_feed(&ip, buf, r) || ip.len > slot->size) {
            l5_fw_size(a, sizeof(a), ip.len);
            say(res->text, sizeof(res->text), "%s", ip.failed ? ip.error : "The app image is larger than the slot.");
            if (!ip.failed) snprintf(res->text + strlen(res->text), sizeof(res->text) - strlen(res->text), " (%s)", a);
            err = ESP_ERR_IMAGE_INVALID;
            goto out;
        }
        if (pos + r > app_erased) { /* the catalog size was too small: erase on (inside the slot) */
            uint32_t to = (pos + r + BLOCK - 1) / BLOCK * BLOCK;
            to = to > slot->size ? slot->size : to;
            if ((err = flash_erase(slot->offset + app_erased, to - app_erased)) != ESP_OK) {
                say(res->text, sizeof(res->text), "Erasing the app slot failed (%s).", esp_err_to_name(err));
                goto out;
            }
            app_erased = to;
        }
        int64_t tw = esp_timer_get_time();
        if ((err = flash_write(slot->offset + pos, buf, r)) != ESP_OK) {
            say(res->text, sizeof(res->text), "Writing flash at 0x%lx failed (%s).", (unsigned long)(slot->offset + pos),
                esp_err_to_name(err));
            goto out;
        }
        t_flash += esp_timer_get_time() - tw;
        pos += r;
        done += r;
        l5_fw_size(a, sizeof(a), pos);
        l5_fw_size(b, sizeof(b), ip.len ? ip.len : bound);
        prog(&pr, 14 + (int)(done * 81 / total), "%s app%d \xC2\xB7 %s of %s", verb, job->slot, a, b);
        vTaskDelay(1);
    }
    res->image_len = ip.len;
    res->hw = hw.found;
    src_close(&s);
    log_rate("app", ip.len, t0, t_flash);
    ESP_LOGI(TAG, "display/touch names in the app:%s%s%s%s%s%s", hw.found & L5_HW_GT911 ? " gt911" : "",
             hw.found & L5_HW_ILI9881 ? " ili9881" : "", hw.found & L5_HW_ST7123 ? " st7123" : "",
             hw.found & L5_HW_ST7121 ? " st7121" : "", hw.found & L5_HW_ST712X ? " st712x" : "", hw.found ? "" : " none");

    /* 4. an app for the first Tab5 (ILI9881C + GT911) only: the user decides before it is installed */
    if (job->hw_warn && l5_hw_old_only(&hw) && !job->hw_warn(job->user)) {
        res->cancelled = true;
        say(res->text, sizeof(res->text), "Cancelled: the firmware is made for the older Tab5 display and touch.");
        err = L5_NET_CANCELLED;
        goto out;
    }

    /* 5. data images (APP + DATA): each partition is erased right before its image */
    res->stage = L5_INST_DATA;
    total = total - piece[0].len + ip.len;
    for (int i = 1; i < np; i++) {
        const piece_t *p = &piece[i];
        t0 = esp_timer_get_time();
        t_flash = 0;
        for (uint32_t off = 0; off < p->erase; off += BLOCK) {
            uint32_t n = p->erase - off < BLOCK ? p->erase - off : BLOCK;
            if (cancelled(job->cancel)) {
                res->cancelled = true;
                err = L5_NET_CANCELLED;
                goto out;
            }
            if ((err = flash_erase(p->addr + off, n)) != ESP_OK) {
                say(res->text, sizeof(res->text), "Erasing %s failed (%s).", p->label, esp_err_to_name(err));
                goto out;
            }
            prog(&pr, 14 + (int)(done * 81 / total), "Erasing %s", p->label);
        }
        if ((err = src_open(&s, p->url, p->src_off, p->len, job->cancel, res)) != ESP_OK) goto out;
        for (uint32_t off = 0; off < p->len;) {
            int r = src_read(&s, buf, CHUNK, res);
            if (r <= 0) {
                if (r == 0) say(res->text, sizeof(res->text), "The image of %s ends after %lu bytes.", p->label, (unsigned long)off);
                err = ESP_FAIL;
                goto out;
            }
            int64_t tw = esp_timer_get_time();
            if ((err = flash_write(p->addr + off, buf, r)) != ESP_OK) {
                say(res->text, sizeof(res->text), "Writing %s failed (%s).", p->label, esp_err_to_name(err));
                goto out;
            }
            t_flash += esp_timer_get_time() - tw;
            off += r;
            done += r;
            l5_fw_size(a, sizeof(a), off);
            l5_fw_size(b, sizeof(b), p->len);
            prog(&pr, 14 + (int)(done * 81 / total), "%s %s (%s) \xC2\xB7 %s of %s", verb, p->label,
                 l5_fw_subtype_name(p->subtype), a, b);
            vTaskDelay(1);
        }
        src_close(&s);
        log_rate(p->label, p->len, t0, t_flash);
        res->data_written++;
    }

    /* 6. the first sector last, then the bootloader's own check on flash */
    res->stage = L5_INST_VERIFY;
    prog(&pr, 96, "Verifying the image (SHA-256)");
    if ((err = flash_write(slot->offset, head, h < SECTOR ? h : SECTOR)) != ESP_OK) {
        say(res->text, sizeof(res->text), "Writing the image header failed (%s).", esp_err_to_name(err));
        goto out;
    }
    const esp_partition_pos_t ppos = {.offset = slot->offset, .size = slot->size};
    esp_image_metadata_t meta;
    if ((err = esp_image_verify(ESP_IMAGE_VERIFY, &ppos, &meta)) != ESP_OK || meta.image_len != ip.len) {
        say(res->text, sizeof(res->text), "The image in app%d did not pass the check (%s): the file is damaged.", job->slot,
            esp_err_to_name(err ? err : ESP_ERR_INVALID_SIZE));
        err = err ? err : ESP_ERR_INVALID_SIZE;
        goto out;
    }

    /* 7. registry: what HOME and UPDATE ALL show */
    const l5_slot_reg_t reg = {.name = job->name, .author = job->author, .src = job->src, .fid = fw->fid, .ver = fw->version};
    if (l5_slots_register(job->slot, desc.app_elf_sha256, meta.image_len, &reg) != ESP_OK) {
        ESP_LOGW(TAG, "app%d: registry entry not saved", job->slot); /* installed anyway */
    }
    res->stage = L5_INST_DONE;
    prog(&pr, 100, "Done");
    ESP_LOGI(TAG, "installed into app%d: %lu bytes, %d data image(s)", job->slot, (unsigned long)meta.image_len,
             res->data_written);

out:
    src_close(&s);
    if (err != ESP_OK) {
        if (cancelled(job->cancel)) {
            res->cancelled = true;
            res->net = false;
            say(res->text, sizeof(res->text), "Cancelled.");
        }
        if (res->erased && slot) flash_erase(slot->offset, SECTOR); /* no image header is left behind */
        ESP_LOGE(TAG, "install into app%d stopped at stage %d: %s (%s)", job->slot, (int)res->stage, res->text,
                 esp_err_to_name(err));
    }
    heap_caps_free(buf);
    heap_caps_free(head);
    return err;
}

/* ---------- probes ---------- */

esp_err_t l5_inst_probe(const char *url, volatile bool *cancel, uint8_t *head, size_t *len, uint32_t *size,
                        char *why, size_t wlen, bool *net)
{
    *len = 0;
    *size = 0;
    *net = false;
    if (is_file(url)) {
        struct stat st;
        FILE *f = fopen(url, "rb");
        if (!f || stat(url, &st) != 0) {
            if (f) fclose(f);
            say(why, wlen, "The file cannot be read from the SD card.");
            return ESP_ERR_NOT_FOUND;
        }
        *size = (uint32_t)st.st_size;
        *len = fread(head, 1, L5_FW_PROBE_LEN, f);
        fclose(f);
        return ESP_OK;
    }
    l5_net_stream_t *ns = NULL;
    int64_t total = -1;
    l5_net_err_t e = {0};
    esp_err_t err = l5_net_open(url, 0, L5_FW_PROBE_LEN, cancel, &ns, NULL, &total, &e);
    while (err == ESP_OK && *len < L5_FW_PROBE_LEN) {
        int r = l5_net_read(ns, head + *len, L5_FW_PROBE_LEN - *len, &e);
        if (r < 0) err = e.fail == L5_NET_CANCEL ? L5_NET_CANCELLED : ESP_FAIL;
        if (r <= 0) break;
        *len += r;
    }
    if (ns) l5_net_close(ns);
    if (err != ESP_OK) {
        strlcpy(why, e.text, wlen);
        *net = e.fail != L5_NET_CANCEL && !(e.fail == L5_NET_HTTP && e.status < 500);
        return err;
    }
    *size = total > 0 ? (uint32_t)total : 0;
    return ESP_OK;
}

esp_err_t l5_inst_app_len_file(const char *path, uint32_t offset, uint32_t max, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    l5_img_parse_t p;
    l5_img_begin(&p);
    uint8_t b[24];
    esp_err_t err = fseek(f, offset, SEEK_SET) == 0 ? ESP_OK : ESP_FAIL;
    while (err == ESP_OK && !p.len) {
        uint32_t skip = l5_img_skippable(&p);
        if (skip) {
            err = fseek(f, skip, SEEK_CUR) == 0 ? ESP_OK : ESP_FAIL;
            l5_img_skip(&p, skip);
            continue;
        }
        size_t need = (p.nseg ? 8u : 24u) - p.have, r = fread(b, 1, need, f);
        if (r == 0) err = ESP_ERR_INVALID_SIZE;
        else if (!l5_img_feed(&p, b, r)) err = ESP_ERR_IMAGE_INVALID;
        else if (p.pos > max) err = ESP_ERR_INVALID_SIZE;
    }
    fclose(f);
    if (err == ESP_OK && p.len > max) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) *len = p.len;
    return err;
}
