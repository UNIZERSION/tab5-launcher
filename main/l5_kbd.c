#include "l5_kbd.h"
#include <string.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "l5_kbd";

#define KBD_SDA 0 /* Ext.Port1 (J9) pin 7 */
#define KBD_SCL 1 /* Ext.Port1 (J9) pin 8 */
#define KBD_ADDR 0x6D
#define KBD_HZ 400000

/* registers (M5Tab5-Keyboard-Internal-FW user_i2c_reg.h) */
#define REG_INT_STA 0x01   /* write 0: clear the flags, release INT */
#define REG_EVENT_NUM 0x02 /* events queued in the current mode; write 0: flush */
#define REG_MODE 0x10      /* 0 key matrix, 1 HID, 2 characters */
#define REG_HID_EVENT 0x30 /* 2 bytes per read: HID modifier, usage ID (0 = release); FF FF = empty */
#define REG_FIRMWARE 0xFE
#define MODE_HID 1

#define POLL_MS 25           /* events while it is attached: a key shows up within 25 ms */
#define LOOK_MS 1000         /* looking for a keyboard that is not there */
#define REFUSED_MS 10000     /* it answered but would not switch to HID: do not log that every second */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_lock; /* the worker and the shutdown handler share the device */
static QueueHandle_t s_keys;
static volatile bool s_present, s_stop;
static uint8_t s_old_mode = 0xFF; /* the mode it had before the launcher switched it, given back at restart */

static esp_err_t reg_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, 50);
}

static esp_err_t reg_write(uint8_t reg, uint8_t value)
{
    const uint8_t w[] = {reg, value};
    return i2c_master_transmit(s_dev, w, sizeof(w), 50);
}

static void detach(void)
{
    if (s_dev) i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    s_present = false;
    s_old_mode = 0xFF;
}

/* a keyboard answers at 0x6D: HID mode, empty queues (as the drivers' begin()) */
static bool attach(uint32_t *wait_ms)
{
    if (i2c_master_probe(s_bus, KBD_ADDR, 50) != ESP_OK) return false;
    const i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = KBD_ADDR, .scl_speed_hz = KBD_HZ};
    if (i2c_master_bus_add_device(s_bus, &cfg, &s_dev) != ESP_OK) {
        s_dev = NULL;
        return false;
    }
    uint8_t fw = 0, mode = 0xFF, now = 0xFF;
    bool ok = reg_read(REG_FIRMWARE, &fw, 1) == ESP_OK && reg_read(REG_MODE, &mode, 1) == ESP_OK;
    if (ok && mode != MODE_HID) ok = reg_write(REG_MODE, MODE_HID) == ESP_OK;
    if (ok) {
        vTaskDelay(pdMS_TO_TICKS(5));
        ok = reg_read(REG_MODE, &now, 1) == ESP_OK && now == MODE_HID;
    }
    if (!ok) {
        ESP_LOGW(TAG, "keyboard at 0x%02X (firmware %u) did not take the HID setup", KBD_ADDR, fw);
        detach();
        *wait_ms = REFUSED_MS;
        return false;
    }
    reg_write(REG_INT_STA, 0);
    reg_write(REG_EVENT_NUM, 0);
    s_old_mode = mode;
    ESP_LOGI(TAG, "M5Stack Tab5 Keyboard at 0x%02X, firmware %u, mode %u -> HID", KBD_ADDR, fw, mode);
    xQueueReset(s_keys); /* nothing typed before it was there */
    s_present = true;
    return true;
}

/* HID usage ID -> ASCII, US layout (the keyboard's legends): unshifted and shifted, 0 = no character */
static char to_ascii(uint8_t usage, bool shift)
{
    static const char NUM[] = "1234567890", NUM_S[] = "!@#$%^&*()";
    static const char SYM[] = "-=[]\\\0;'`,./", SYM_S[] = "_+{}|\0:\"~<>?"; /* 0x2D..0x38 (0x32: non-US #) */
    if (usage >= 0x04 && usage <= 0x1D) return (char)((shift ? 'A' : 'a') + usage - 0x04);
    if (usage >= 0x1E && usage <= 0x27) return (shift ? NUM_S : NUM)[usage - 0x1E];
    if (usage == 0x2C) return ' ';
    if (usage >= 0x2D && usage <= 0x38) return (shift ? SYM_S : SYM)[usage - 0x2D];
    return 0;
}

static void event(uint8_t mod, uint8_t usage)
{
    l5_kbd_key_t k = {.kind = L5_KBD_CHAR};
    switch (usage) {
    case 0x00: return; /* release */
    case 0x28: k.kind = L5_KBD_ENTER; break;
    case 0x29: k.kind = L5_KBD_ESC; break;
    case 0x2A: k.kind = L5_KBD_BACKSPACE; break;
    default:
        if (mod & 0x55) return; /* left / right Ctrl or Alt: a shortcut, not a character */
        k.ch = to_ascii(usage, (mod & 0x22) != 0);
        if (!k.ch) return; /* Tab, arrows, Delete: the field has no cursor */
        break;
    }
    xQueueSend(s_keys, &k, 0); /* full: nobody reads, the key is dropped */
}

/* one pass over the queue on the keyboard; false when it does not answer */
static bool poll(void)
{
    uint8_t n = 0;
    if (reg_read(REG_EVENT_NUM, &n, 1) != ESP_OK) return false;
    if (n > 32) n = 32; /* FIFO_DEPTH */
    for (unsigned i = 0; i < n; i++) {
        uint8_t e[2];
        if (reg_read(REG_HID_EVENT, e, sizeof(e)) != ESP_OK) return false;
        if (e[0] == 0xFF && e[1] == 0xFF) break; /* empty */
        event(e[0], e[1]);
    }
    if (n) reg_write(REG_INT_STA, 0);
    return true;
}

static void worker(void *arg)
{
    (void)arg;
    unsigned misses = 0;
    for (;;) {
        uint32_t wait = s_present ? POLL_MS : LOOK_MS;
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_stop) { /* the restart took the bus away */
                xSemaphoreGive(s_lock);
                vTaskDelete(NULL);
            }
            if (!s_dev) {
                misses = 0;
                if (attach(&wait)) wait = POLL_MS;
            } else if (poll()) {
                misses = 0;
            } else if (++misses >= 3) { /* pulled out */
                ESP_LOGI(TAG, "keyboard gone");
                detach();
                wait = LOOK_MS;
            }
            xSemaphoreGive(s_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

/* esp_restart(): the keyboard gets its old mode back and GPIO 0 / 1 are released (i2c_del_master_bus) */
static void on_restart(void)
{
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(60)) != pdTRUE) return;
    s_stop = true;
    if (s_dev && s_old_mode <= 2 && s_old_mode != MODE_HID) reg_write(REG_MODE, s_old_mode);
    detach();
    if (s_bus) i2c_del_master_bus(s_bus);
    s_bus = NULL;
    xSemaphoreGive(s_lock);
}

void l5_kbd_init(void)
{
    const i2c_master_bus_config_t bus = {
        .i2c_port = -1, /* any free port: the BSP's system bus has CONFIG_BSP_I2C_NUM */
        .sda_io_num = KBD_SDA,
        .scl_io_num = KBD_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no keyboard support: %s", esp_err_to_name(err));
        return;
    }
    /* looking for a keyboard that is not plugged in fails every second (timeouts, failed transactions): keep that out
       of the log. The driver's own warnings for the BSP bus go too; the launcher logs its own I2C errors. */
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    s_lock = xSemaphoreCreateMutex();
    s_keys = xQueueCreate(16, sizeof(l5_kbd_key_t));
    if (!s_lock || !s_keys || xTaskCreatePinnedToCore(worker, "l5_kbd", 3072, NULL, 3, NULL, 1) != pdPASS) {
        ESP_LOGW(TAG, "no keyboard support: no memory for the worker");
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return;
    }
    if (esp_register_shutdown_handler(on_restart) != ESP_OK) {
        ESP_LOGW(TAG, "no shutdown handler: the keyboard bus stays set up across a restart");
    }
}

bool l5_kbd_present(void)
{
    return s_present;
}

bool l5_kbd_get(l5_kbd_key_t *key)
{
    return s_keys && xQueueReceive(s_keys, key, 0) == pdTRUE;
}

void l5_kbd_flush(void)
{
    if (s_keys) xQueueReset(s_keys);
}
