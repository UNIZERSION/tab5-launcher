/* Files from the PC to the SD card over the USB console port (l5_link.h, tools/sd_put.py).
   The USB Serial/JTAG driver takes every packet in its interrupt into a ring buffer larger than a block; the console
   (the logs) goes through the driver too, which drops output instead of waiting when no PC reads it. The host sends
   8 KB and waits for their ACK; the launcher takes the whole block into RAM and only then writes it to the card.
   (Reading the RX FIFO from a task without the driver lost 16-97 bytes in a few MB on the Tab5.) */
#include "l5_link.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "l5_board.h"
#include "ui/l5_ui.h"

static const char *TAG = "l5_link";

#define ROOT "/sdcard"
#define BLOCK 8192             /* acknowledged in pieces of this size (the host waits for each ACK) */
#define RX_RING (BLOCK + 2048) /* the driver's RX buffer: a whole block and the next command fit */
#define MAX_SIZE (64l << 20)   /* larger than any app or picture the launcher reads */
#define IDLE_US (5 * 1000000LL) /* a transfer that stops for this long is dropped */

/* up to len bytes from the USB port, 0 when none came within 20 ms */
static int rx(uint8_t *buf, int len)
{
    const int n = usb_serial_jtag_read_bytes(buf, len, pdMS_TO_TICKS(20));
    return n > 0 ? n : 0;
}

static uint32_t fnv1a(uint32_t h, const uint8_t *p, int n)
{
    while (n--) h = (h ^ *p++) * 0x01000193u;
    return h;
}

/* "/a/b.png" -> "/sdcard/a/b.png"; no "..", no "\", no control characters, not the card root itself */
static bool card_path(const char *path, char *out, size_t len)
{
    if (path[0] != '/' || !path[1] || strstr(path, "..") || strchr(path, '\\') || path[strlen(path) - 1] == '/') {
        return false;
    }
    for (const char *c = path; *c; c++) {
        if ((unsigned char)*c < 0x20) return false;
    }
    return snprintf(out, len, ROOT "%s", path) < (int)len;
}

/* every folder on the way to the file */
static void make_dirs(char *full)
{
    for (char *s = strchr(full + strlen(ROOT) + 1, '/'); s; s = strchr(s + 1, '/')) {
        *s = 0;
        mkdir(full, 0777);
        *s = '/';
    }
}

static void put(const char *path, long size, uint32_t want)
{
    char full[256], part[264];
    if (!card_path(path, full, sizeof(full)) || size < 0 || size > MAX_SIZE) {
        printf("L5PUT FAIL bad path or size\n");
        return;
    }
    if (!l5_board_sd_ready()) {
        printf("L5PUT FAIL no SD card\n");
        return;
    }
    make_dirs(full);
    snprintf(part, sizeof(part), "%s.part", full);
    FILE *f = fopen(part, "wb");
    uint8_t *buf = heap_caps_malloc(BLOCK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); /* the card's DMA likes it */
    if (!buf) buf = malloc(BLOCK);
    if (!f || !buf) {
        printf("L5PUT FAIL cannot write %s (%d)\n", path, errno);
        if (f) fclose(f);
        free(buf);
        return;
    }
    printf("L5PUT READY\n");
    fflush(stdout);
    const int64_t t0 = esp_timer_get_time();
    int64_t last = t0;
    long got = 0;
    uint32_t h = 0x811c9dc5u;
    bool ok = true;
    /* a whole block from USB first, then to the card: USB is read without pauses (the host sends the block at once
       and waits for its ACK while the card writes) */
    while (ok && got < size) {
        const long fill = size - got < BLOCK ? size - got : BLOCK;
        long have = 0;
        while (have < fill) {
            const int n = rx(buf + have, (int)(fill - have));
            const int64_t now = esp_timer_get_time();
            if (n > 0) {
                have += n;
                last = now;
            } else if (now - last > IDLE_US) {
                printf("L5PUT FAIL timeout at %ld\n", got + have);
                ok = false;
                break;
            }
        }
        if (!ok) break;
        if (fwrite(buf, 1, fill, f) != (size_t)fill) {
            printf("L5PUT FAIL write at %ld (%d)\n", got, errno);
            ok = false;
            break;
        }
        h = fnv1a(h, buf, (int)fill);
        got += fill;
        printf("L5PUT ACK %ld\n", got);
        fflush(stdout);
    }
    free(buf);
    ok = fclose(f) == 0 && ok;
    if (ok && h != want) {
        printf("L5PUT FAIL checksum %08lx\n", (unsigned long)h);
        ok = false;
    }
    if (ok) {
        unlink(full); /* FAT does not rename over a file */
        ok = rename(part, full) == 0;
        if (!ok) printf("L5PUT FAIL rename (%d)\n", errno);
    }
    if (!ok) {
        unlink(part);
        return;
    }
    const int64_t ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "%s: %ld bytes in %lld ms (%lld KB/s)", path, size, ms, ms ? (long long)size / ms : 0);
    printf("L5PUT OK %s %ld\n", path, size);
    if (l5_board_lock(1000)) {
        l5_appicon_refresh(); /* a new wallpaper shows up now; icons when HOME is drawn again */
        l5_board_unlock();
    }
}

static void command(char *line)
{
    char path[200];
    long size;
    unsigned long want;
    if (strncmp(line, "l5", 2)) return;
    /* the PC is working with the launcher: the bootscreen must not hand over to the boot app in the middle */
    if (l5_board_lock(1000)) {
        l5_boot_screen_hold();
        l5_board_unlock();
    }
    if (sscanf(line, "l5put %199s %ld %lx", path, &size, &want) == 3) {
        put(path, size, (uint32_t)want);
    } else if (sscanf(line, "l5rm %199s", path) == 1) {
        char full[256];
        const bool ok = card_path(path, full, sizeof(full)) && unlink(full) == 0;
        printf(ok ? "L5RM OK %s\n" : "L5RM FAIL %s\n", path);
        if (ok && l5_board_lock(1000)) {
            l5_appicon_refresh();
            l5_board_unlock();
        }
    }
    fflush(stdout);
}

static void task(void *arg)
{
    char line[256];
    int len = 0;
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) <= 0) continue; /* sleeps until the PC sends */
        if (c == '\n' || c == '\r') {
            line[len] = 0;
            if (len) command(line);
            len = 0;
        } else if (len < (int)sizeof(line) - 1) {
            line[len++] = (char)c;
        }
    }
}

void l5_link_start(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = RX_RING;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "no USB Serial/JTAG driver: no file transfer over USB");
        return;
    }
    usb_serial_jtag_vfs_use_driver(); /* the logs through the driver as well (both would write the TX FIFO) */
    if (xTaskCreate(task, "l5_link", 4096, NULL, 1, NULL) != pdPASS) ESP_LOGW(TAG, "no memory for the USB link");
}
