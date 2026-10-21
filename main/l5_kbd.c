#include "l5_kbd.h"
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "l5_kbd";

#define KBD_PORT I2C_NUM_0 /* the BSP's system bus has CONFIG_BSP_I2C_NUM (1) */
#define KBD_SDA 0          /* Ext.Port1 (J9) pin 7 */
#define KBD_SCL 1          /* Ext.Port1 (J9) pin 8 */
#define KBD_INT 50         /* Ext.Port1 (J9) pin 10, low while events wait */
#define KBD_ADDR 0x6D
#define KBD_HZ 100000

/* registers (M5Tab5-Keyboard-Internal-FW user_i2c_reg.h) */
#define REG_INT_CFG 0x00   /* bit 0 key matrix, bit 1 HID, bit 2 characters */
#define REG_INT_STA 0x01   /* write 0: clear the flags, release INT */
#define REG_EVENT_NUM 0x02 /* events queued in the current mode; write 0: flush */
#define REG_MODE 0x10      /* 0 key matrix, 1 HID, 2 characters */
#define REG_HID_EVENT 0x30 /* 2 bytes per read: HID modifier, usage ID (0 = release); FF FF = empty */
#define REG_FIRMWARE 0xFE
#define REG_ADDR 0xFF      /* its own I2C address */
#define MODE_HID 1
#define INT_HID 0x02
#define FIFO_DEPTH 32

#define LOOK_MS 2000       /* looking for a keyboard that is not there */
#define POLL_MS 100        /* events without an INT edge (a missed one, or no INT line) */
#define CHECK_MS 1000      /* still in HID mode? (it may have been reset or plugged again) */
#define REPEAT_DELAY_MS 450
#define REPEAT_EVERY_MS 60
#define REPEAT_MAX_MS 10000 /* a key that never reports its release stops repeating */
#define QUEUE_LEN 64

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_lock; /* the worker and the shutdown handler share the bus */
static QueueHandle_t s_keys;
static volatile bool s_present, s_stop;

/* the held key, repeated by the worker */
static struct {
    l5_key_t key;
    bool active;
    int64_t since, next; /* ms */
} s_rep;

static esp_err_t reg_write(uint8_t reg, uint8_t value)
{
    const uint8_t w[] = {reg, value};
    return i2c_master_transmit(s_dev, w, sizeof(w), 50);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, 50);
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void gone(void)
{
    s_present = false;
    xQueueReset(s_keys);
    s_rep.active = false;
    ESP_LOGI(TAG, "keyboard gone");
}

static void push(const l5_key_t *k)
{
    if (xQueueSend(s_keys, k, 0) != pdTRUE) { /* nobody takes them: start over */
        xQueueReset(s_keys);
        s_rep.active = false;
    }
}

/* HID mode, interrupt for HID events only, both queues empty (as the drivers' begin()) */
static esp_err_t setup_hid(void)
{
    esp_err_t err = reg_write(REG_MODE, MODE_HID);
    if (err == ESP_OK) err = reg_write(REG_INT_CFG, INT_HID);
    if (err == ESP_OK) err = reg_write(REG_EVENT_NUM, 0);
    if (err == ESP_OK) err = reg_write(REG_INT_STA, 0);
    return err;
}

/* a keyboard answers at 0x6D with its own address in register 0xFF */
static bool probe(uint8_t *fw, uint8_t *mode)
{
    /* an empty port fails every 2 s (failed transactions, timeouts): out of the log for the probe only */
    esp_log_level_t level = esp_log_level_get("i2c.master");
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    uint8_t addr = 0;
    bool ok = reg_read(REG_FIRMWARE, fw, 1) == ESP_OK && reg_read(REG_ADDR, &addr, 1) == ESP_OK && addr == KBD_ADDR &&
              reg_read(REG_MODE, mode, 1) == ESP_OK;
    esp_log_level_set("i2c.master", level);
    return ok;
}

static const char NUM[] = "1234567890", NUM_S[] = "!@#$%^&*()";
static const char SYM[] = "-=[]\\#;'`,./", SYM_S[] = "_+{}|~:\"~<>?"; /* usage IDs 0x2D..0x38 */

/* one HID event: modifier byte (bits 0/4 Ctrl, 1/5 Shift, 2/6 Alt), usage ID (0 = all keys released) */
static void event(uint8_t mod, uint8_t usage, int64_t now)
{
    if (!usage) {
        s_rep.active = false;
        return;
    }
    l5_key_t k = {.ctrl = (mod & 0x11) != 0, .shift = (mod & 0x22) != 0, .alt = (mod & 0x44) != 0};
    if (usage >= 0x04 && usage <= 0x1D) {
        k.kind = L5_KEY_CHAR;
        k.ch = (char)((k.shift ? 'A' : 'a') + usage - 0x04);
    } else if (usage >= 0x1E && usage <= 0x27) {
        k.kind = L5_KEY_CHAR;
        k.ch = (k.shift ? NUM_S : NUM)[usage - 0x1E];
    } else if (usage == 0x2C) {
        k.kind = L5_KEY_CHAR;
        k.ch = ' ';
    } else if (usage >= 0x2D && usage <= 0x38) {
        k.kind = L5_KEY_CHAR;
        k.ch = (k.shift ? SYM_S : SYM)[usage - 0x2D];
    } else {
        switch (usage) {
        case 0x28: case 0x58: k.kind = L5_KEY_ENTER; break;
        case 0x29: k.kind = L5_KEY_ESC; break;
        case 0x2A: k.kind = L5_KEY_BACKSPACE; break;
        case 0x2B: k.kind = L5_KEY_TAB; break;
        case 0x4A: k.kind = L5_KEY_HOME; break;
        case 0x4B: k.kind = L5_KEY_PGUP; break;
        case 0x4C: k.kind = L5_KEY_DELETE; break;
        case 0x4D: k.kind = L5_KEY_END; break;
        case 0x4E: k.kind = L5_KEY_PGDN; break;
        case 0x4F: k.kind = L5_KEY_RIGHT; break;
        case 0x50: k.kind = L5_KEY_LEFT; break;
        case 0x51: k.kind = L5_KEY_DOWN; break;
        case 0x52: k.kind = L5_KEY_UP; break;
        default: s_rep.active = false; return; /* F-keys, Caps Lock...: nothing in the launcher */
        }
    }
    push(&k);
    /* held keys repeat: characters, Backspace, Delete, arrows, Home/End, PgUp/PgDn; not Enter, Esc, Tab, shortcuts */
    bool repeats = !k.ctrl && !k.alt &&
                   (k.kind == L5_KEY_CHAR || k.kind == L5_KEY_BACKSPACE || k.kind == L5_KEY_DELETE || k.kind > L5_KEY_TAB);
    s_rep.key = k;
    s_rep.key.repeat = true;
    s_rep.active = repeats;
    s_rep.since = now;
    s_rep.next = now + REPEAT_DELAY_MS;
}

/* the events the keyboard queued; false when it does not answer */
static bool read_events(int64_t now)
{
    uint8_t n = 0;
    if (reg_read(REG_EVENT_NUM, &n, 1) != ESP_OK) return false;
    if (n > FIFO_DEPTH) { /* not a count: start the queue over */
        return reg_write(REG_EVENT_NUM, 0) == ESP_OK && reg_write(REG_INT_STA, 0) == ESP_OK;
    }
    for (unsigned i = 0; i < n; i++) {
        uint8_t e[2];
        if (reg_read(REG_HID_EVENT, e, sizeof(e)) != ESP_OK) return false;
        if ((e[0] & e[1]) == 0xFF) break; /* empty */
        event(e[0], e[1], now);
    }
    return reg_write(REG_INT_STA, 0) == ESP_OK; /* releases INT */
}

static void worker(void *arg)
{
    (void)arg;
    bool first = true;
    int64_t looked = 0, polled = 0, checked = 0;
    for (;;) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_stop) { /* the restart took the bus away; the lock stays with it */
                vTaskDelete(NULL);
            }
            int64_t now = now_ms();
            if (!s_present) {
                if (first || now - looked >= LOOK_MS) {
                    first = false;
                    looked = polled = checked = now;
                    uint8_t fw = 0, mode = 0;
                    if (probe(&fw, &mode)) {
                        if (setup_hid() != ESP_OK) {
                            ESP_LOGW(TAG, "keyboard at 0x%02X (firmware %u) did not take the HID setup", KBD_ADDR, fw);
                        } else {
                            s_rep.active = false;
                            s_present = true;
                            ESP_LOGI(TAG, "M5Stack Tab5 Keyboard at 0x%02X, firmware %u, mode %u -> HID", KBD_ADDR, fw, mode);
                        }
                    }
                }
            } else {
                if (gpio_get_level(KBD_INT) == 0 || now - polled >= POLL_MS) {
                    if (read_events(now)) {
                        polled = now;
                    } else {
                        gone();
                    }
                }
                if (s_present && now - checked >= CHECK_MS) {
                    uint8_t mode = MODE_HID;
                    if (reg_read(REG_MODE, &mode, 1) != ESP_OK || (mode != MODE_HID && setup_hid() != ESP_OK)) {
                        gone();
                    }
                    checked = now;
                }
                if (s_present && s_rep.active) {
                    if (now - s_rep.since >= REPEAT_MAX_MS) {
                        s_rep.active = false;
                    } else if (now >= s_rep.next) {
                        s_rep.next = now + REPEAT_EVERY_MS;
                        push(&s_rep.key);
                    }
                }
            }
            xSemaphoreGive(s_lock);
        }
        if (s_stop) vTaskDelete(NULL);
        vTaskDelay(pdMS_TO_TICKS(s_present ? 10 : 100));
    }
}

/* esp_restart(): the bus goes and GPIO 0, 1 and 50 are back as after power-on for the next app */
static void on_restart(void)
{
    s_stop = true;
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) return;
    if (s_dev) i2c_master_bus_rm_device(s_dev);
    if (s_bus) i2c_del_master_bus(s_bus);
    s_dev = NULL;
    s_bus = NULL;
    gpio_reset_pin(KBD_SDA);
    gpio_reset_pin(KBD_SCL);
    gpio_reset_pin(KBD_INT);
}

void l5_kbd_init(void)
{
    if (s_keys) return;
    const gpio_config_t intr = {.pin_bit_mask = 1ULL << KBD_INT, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    esp_err_t err = gpio_config(&intr);
    if (err == ESP_OK) {
        const i2c_master_bus_config_t bus = {
            .i2c_port = KBD_PORT,
            .sda_io_num = KBD_SDA,
            .scl_io_num = KBD_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        err = i2c_new_master_bus(&bus, &s_bus);
    }
    if (err == ESP_OK) {
        const i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = KBD_ADDR, .scl_speed_hz = KBD_HZ};
        err = i2c_master_bus_add_device(s_bus, &dev, &s_dev);
    }
    s_keys = xQueueCreate(QUEUE_LEN, sizeof(l5_key_t));
    s_lock = xSemaphoreCreateMutex();
    if (err != ESP_OK || !s_keys || !s_lock) {
        ESP_LOGW(TAG, "no keyboard support: %s", esp_err_to_name(err != ESP_OK ? err : ESP_ERR_NO_MEM));
        return;
    }
    if (esp_register_shutdown_handler(on_restart) != ESP_OK) {
        ESP_LOGW(TAG, "no shutdown handler: the keyboard bus stays set up across a restart");
    }
    if (xTaskCreatePinnedToCore(worker, "l5_kbd", 3072, NULL, 2, NULL, 1) != pdPASS) {
        ESP_LOGW(TAG, "no keyboard support: no memory for the worker");
    }
}

bool l5_kbd_present(void)
{
    return s_present;
}

bool l5_kbd_get(l5_key_t *key)
{
    return s_keys && xQueueReceive(s_keys, key, 0) == pdTRUE;
}
