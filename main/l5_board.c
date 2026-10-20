#include "l5_board.h"
#include <sys/time.h>
#include "l5_display.h"
#include "l5_pi4io.h"
#include <time.h>
#include "bsp/m5stack_tab5.h"
#include "diskio_sdmmc.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static const char *TAG = "l5_board";

static lv_display_t *s_disp;
static bool s_sd;
static l5_sd_state_t s_sd_state = L5_SD_UNCHECKED;
static esp_err_t s_sd_err = ESP_OK;
static i2c_master_dev_handle_t s_rtc, s_ina;

/* ---------- display / touch ---------- */

/* Without a picture nothing the launcher shows can be seen: say once in the log what failed and blink the backlight
   (1 s on, 1 s off) so the Tab5 does not look dead. Nothing starts by itself from here: RESET runs the launcher again. */
static void no_display(const char *step)
{
    ESP_LOGE(TAG, "NO DISPLAY: %s. The launcher stops here (backlight blinks); RESET tries again. Please report the "
                  "Tab5 board revision at github.com/UNIZERSION/tab5-launcher/issues", step ? step : "the panel did not start");
    bsp_display_brightness_init(); /* the panel start may have failed before the backlight PWM existed */
    for (bool on = true;; on = !on) {
        bsp_display_brightness_set(on ? 60 : 0);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

lv_display_t *l5_board_display_start(uint16_t rot)
{
    /* l5_display.c: hidden frame buffer per LVGL refresh, published at VSYNC (no tearing) */
    const char *step = NULL;
    esp_err_t err = l5_display_start(rot, &s_disp, &step);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display start failed: %s", esp_err_to_name(err));
        if (!s_disp) no_display(step); /* a later step (LCD on, touch) leaves a working display behind */
    }
    return s_disp;
}

void l5_board_reveal(uint8_t percent)
{
    if (l5_display_reveal(percent) != ESP_OK) {
        l5_board_backlight(percent); /* show whatever is there rather than stay dark */
    }
}

void l5_board_set_rotation(uint16_t rot)
{
    if (!s_disp || !lvgl_port_lock(1000)) {
        return;
    }
    /* Same direction as esp_lvgl_port's PPA CCW90; 180 = landscape inverted (cfg.rot) */
    lv_display_set_rotation(s_disp, rot == 180 ? LV_DISPLAY_ROTATION_270 : LV_DISPLAY_ROTATION_90);
    lvgl_port_unlock();
}

void l5_board_backlight(uint8_t percent)
{
    bsp_display_brightness_set(percent > 100 ? 100 : percent);
}

bool l5_board_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void l5_board_unlock(void)
{
    lvgl_port_unlock();
}

const char *l5_board_new_panel(void)
{
    return l5_display_new_panel();
}

bool l5_board_touch_pressed(void)
{
    lv_indev_t *in = l5_display_touch();
    if (!in) {
        return false;
    }
    lv_indev_read(in);
    return lv_indev_get_state(in) == LV_INDEV_STATE_PRESSED;
}

/* ---------- SD card ---------- */

/* The BSP mounts with format_if_mount_failed = false (CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL off):
   the launcher never formats a card. */
esp_err_t l5_board_sd_mount(void)
{
    if (s_sd) {
        return ESP_OK;
    }
    esp_err_t err = bsp_sdcard_mount();
    s_sd = err == ESP_OK;
    s_sd_err = err;
    if (s_sd) {
        s_sd_state = L5_SD_MOUNTED;
        return ESP_OK;
    }
    /* vfs_fat_sdmmc: card init errors pass through; a FatFs f_mount failure (13 =
       FR_NO_FILESYSTEM: unformatted, NTFS, ...) after a card answered becomes ESP_FAIL */
    switch (err) {
    case ESP_FAIL:
        s_sd_state = L5_SD_UNREADABLE;
        break;
    case ESP_ERR_TIMEOUT:
    case ESP_ERR_INVALID_RESPONSE:
    case ESP_ERR_INVALID_CRC:
    case ESP_ERR_NOT_FOUND:
        s_sd_state = L5_SD_NO_CARD;
        break;
    default:
        s_sd_state = L5_SD_ERROR;
        break;
    }
    ESP_LOGW(TAG, "SD card not mounted: %s (%s)", esp_err_to_name(err), l5_board_sd_status(false));
    /* release the SD LDO channel the BSP acquired, so a later retry can acquire it again */
    bsp_sdcard_unmount();
    return err;
}

bool l5_board_sd_ready(void)
{
    return s_sd;
}

l5_sd_state_t l5_board_sd_state(void)
{
    return s_sd_state;
}

esp_err_t l5_board_sd_error(void)
{
    return s_sd_err;
}

const char *l5_board_sd_status(bool detail)
{
    switch (s_sd_state) {
    case L5_SD_MOUNTED:
        return detail ? "SD card mounted." : "MOUNTED";
    case L5_SD_NO_CARD:
        return detail ? "No card answered. Insert a microSD card formatted as FAT32 or exFAT, then tap TRY AGAIN."
                      : "NO CARD";
    case L5_SD_UNREADABLE:
        return detail ? "A card answered, but it has no FAT32 or exFAT volume (FatFs error 13, for example NTFS or "
                        "unformatted). Format it as FAT32 or exFAT on a computer. The launcher never formats cards."
                      : "NOT READABLE";
    case L5_SD_ERROR:
        return detail ? "The SD driver reported an error. Reinsert the card and tap TRY AGAIN." : "SD ERROR";
    default:
        return detail ? "The SD card was not checked yet." : "NOT CHECKED";
    }
}

uint64_t l5_board_sd_total_bytes(void)
{
    sdmmc_card_t *card = s_sd ? bsp_sdcard_get_handle() : NULL;
    return card ? (uint64_t)card->csd.capacity * card->csd.sector_size : 0;
}

int l5_board_sd_drive(void)
{
    sdmmc_card_t *card = s_sd ? bsp_sdcard_get_handle() : NULL;
    BYTE pdrv = card ? ff_diskio_get_pdrv_card(card) : 0xFF;
    return pdrv == 0xFF ? -1 : pdrv;
}

/* ---------- RTC (RX8130 @0x32) and battery (INA226 @0x41) ---------- */

static esp_err_t add_device(uint16_t addr, i2c_master_dev_handle_t *dev)
{
    const i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 400000};
    return i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, dev);
}

static esp_err_t reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *data, size_t len)
{
    return dev ? i2c_master_transmit_receive(dev, &reg, 1, data, len, 100) : ESP_ERR_INVALID_STATE;
}

static int bcd(uint8_t v)
{
    return ((v >> 4) & 0x0f) * 10 + (v & 0x0f);
}

static void rtc_to_system_time(void)
{
    uint8_t flags, d[7];
    if (reg_read(s_rtc, 0x1d, &flags, 1) != ESP_OK || (flags & 0x02) || reg_read(s_rtc, 0x10, d, sizeof(d)) != ESP_OK) {
        ESP_LOGW(TAG, "RTC time not valid");
        return;
    }
    struct tm t = {
        .tm_sec = bcd(d[0] & 0x7f), .tm_min = bcd(d[1] & 0x7f), .tm_hour = bcd(d[2] & 0x3f),
        .tm_mday = bcd(d[4] & 0x3f), .tm_mon = bcd(d[5] & 0x1f) - 1, .tm_year = bcd(d[6]) + 100, .tm_isdst = -1,
    };
    time_t now = mktime(&t);
    if (now > 0) {
        struct timeval tv = {.tv_sec = now};
        settimeofday(&tv, NULL);
    }
}

esp_err_t l5_board_sensors_init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "I2C");
    if (add_device(0x32, &s_rtc) == ESP_OK) {
        rtc_to_system_time();
    }
    /* 0x40 is the ES7210 microphone ADC; the battery INA226 is 0x41 */
    ESP_RETURN_ON_ERROR(add_device(0x41, &s_ina), TAG, "INA226");
    const uint8_t config[] = {0x00, 0x05, 0x27}; /* 16 samples, 1.1 ms, continuous */
    return i2c_master_transmit(s_ina, config, sizeof(config), 100);
}

int l5_board_battery_percent(void)
{
    uint8_t d[2];
    if (reg_read(s_ina, 0x02, d, sizeof(d)) != ESP_OK) {
        return -1;
    }
    int mv = (((int)d[0] << 8) | d[1]) * 5 / 4; /* bus voltage LSB 1.25 mV */
    if (mv < 5000 || mv > 9000) {
        return -1; /* no pack (USB only) */
    }
    /* 2S Li-ion: 6.4 V empty .. 8.3 V full, linear placeholder for a proper curve */
    int pct = (mv - 6400) * 100 / (8300 - 6400);
    return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

/* The current through the pack's 5 mOhm shunt: INA226 register 0x01 (16 averaged samples), 2.5 uV per LSB = 0.5 mA,
   negative while current flows into the pack: the sign Aurora Player's tab5_hal measured on this Tab5 (Aurora: +485 mA
   charging; the launcher on USB with a full pack: -1 mA). More than 20 mA into the pack: charging; more than 20 mA out
   of it: on battery; in between: external power without charging (a full pack, or a charger that stopped). Near a
   threshold a new state needs two readings in a row that agree; more than 100 mA either way switches at once (USB
   plugged in or pulled out): l5_charge_next(). Every change is logged with the current, and so is the first reading
   each time Charge Mode opens (fresh: the state from an earlier visit may be hours old). CHG_STAT on the IO expander
   is not used: Aurora saw it stay high at -202 mA after unplugging. */
l5_charge_t l5_board_battery_state(bool fresh)
{
    static const char *const NAMES[] = {"on battery", "charging", "external power, not charging"};
    static l5_charge_t state = L5_CHARGE_UNKNOWN, pending = L5_CHARGE_UNKNOWN;
    if (fresh) state = pending = L5_CHARGE_UNKNOWN;
    uint8_t d[2];
    if (l5_board_battery_percent() < 0 || reg_read(s_ina, 0x01, d, sizeof(d)) != ESP_OK) {
        if (state != L5_CHARGE_UNKNOWN || fresh) ESP_LOGI(TAG, "battery: no pack or no current reading");
        state = pending = L5_CHARGE_UNKNOWN;
        return state;
    }
    int ma = -(int)(int16_t)(((uint16_t)d[0] << 8) | d[1]) / 2;
    l5_charge_t next = l5_charge_next(state, &pending, ma);
    if (next != state) ESP_LOGI(TAG, "battery: %d mA into the pack: %s", ma, NAMES[next]);
    state = next;
    return state;
}

/* ---------- power-off ----------
   Ported from ../aurora-player/components/tab5_hal/tab5_power.c, which follows
   bsp_generate_poweroff_signal in M5Stack's Tab5 UserDemo: release the RX8130 /IRQ
   (it shares the PMS150G trigger line) and pulse PI4IOE 0x44 pin 4 three times. */

#define POWER_OFF_PIN IO_EXPANDER_PIN_NUM_4

static esp_err_t rtc_release_irq(void)
{
    uint8_t ctrl0;
    ESP_RETURN_ON_ERROR(reg_read(s_rtc, 0x1e, &ctrl0, 1), TAG, "RTC ctrl0");
    const uint8_t disable[] = {0x1e, (uint8_t)(ctrl0 & ~0x38U)}; /* UIE/TIE/AIE off */
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_rtc, disable, sizeof(disable), 100), TAG, "RTC irq off");
    const uint8_t clear[] = {0x1d, 0x87}; /* clear UF/TF/AF only */
    return i2c_master_transmit(s_rtc, clear, sizeof(clear), 100);
}

esp_err_t l5_board_power_off(void)
{
    esp_io_expander_handle_t io = bsp_io_expander1_init();
    if (!io) {
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(rtc_release_irq(), TAG, "RTC IRQ must be released before power pulses");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_output_mode(io, POWER_OFF_PIN, IO_EXPANDER_OUTPUT_MODE_OPEN_DRAIN), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_pullupdown(io, POWER_OFF_PIN, IO_EXPANDER_PULL_NONE), TAG, "pull");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(io, POWER_OFF_PIN, IO_EXPANDER_OUTPUT), TAG, "dir");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(io, POWER_OFF_PIN, 0), TAG, "low");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_output_mode(io, POWER_OFF_PIN, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL), TAG, "push-pull");
    ESP_LOGI(TAG, "power-off: PI4IO 0x44 pin4, 3 x 100/100 ms");
    esp_err_t err = ESP_OK;
    for (int step = 0; step < 6 && err == ESP_OK; step++) {
        err = esp_io_expander_set_level(io, POWER_OFF_PIN, !(step & 1));
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_io_expander_set_level(io, POWER_OFF_PIN, 0);
    ESP_LOGE(TAG, "power did not go off");
    return err != ESP_OK ? err : ESP_ERR_TIMEOUT;
}

/* ---------- ESP32-C6 supply ----------
   PI4IOE5V6408 0x44 pin 0 (WLAN_PWR_EN), written register by register (l5_pi4io.h). The BSP's expander driver
   resets the whole chip when it is first opened (bsp_feature_enable(BSP_FEATURE_WIFI)), which would
   also drop USB 5 V and the charger pins of the same expander. */

#define PI4IO_ADDR 0x44
#define WLAN_PWR_EN L5_PI4IO_WLAN_PWR_EN

static i2c_master_dev_handle_t s_pi4io;

static esp_err_t pi4io_update(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t v;
    ESP_RETURN_ON_ERROR(reg_read(s_pi4io, reg, &v, 1), TAG, "PI4IOE 0x44 reg 0x%02x", reg);
    const uint8_t w[] = {reg, (uint8_t)((v & ~mask) | (value & mask))};
    return i2c_master_transmit(s_pi4io, w, sizeof(w), 100);
}

esp_err_t l5_board_c6_power(bool on)
{
    if (!s_pi4io) {
        ESP_RETURN_ON_ERROR(add_device(PI4IO_ADDR, &s_pi4io), TAG, "PI4IOE 0x44");
    }
    /* level first, then drive it: no glitch on a pin that was an input */
    ESP_RETURN_ON_ERROR(pi4io_update(L5_PI4IO_OUT, WLAN_PWR_EN, on ? WLAN_PWR_EN : 0), TAG, "WLAN_PWR_EN level");
    ESP_RETURN_ON_ERROR(pi4io_update(L5_PI4IO_HIGHZ, WLAN_PWR_EN, 0), TAG, "WLAN_PWR_EN push-pull");
    return pi4io_update(L5_PI4IO_DIR, WLAN_PWR_EN, WLAN_PWR_EN);
}

/* ---------- battery charger ----------
   CHG_EN high and CHG_STAT an input on PI4IOE 0x44 (L5_PI4IO_CHARGER_ON), as Aurora's tab5_charger_init(). Without
   it the charger was off in the launcher: 19.10.2026 Aurora charged at +411..+485 mA, and 14 s after a RESET into the
   launcher the pack current was -1 mA. main.c calls this before the panel starts. Nothing turns it off again: the
   app hand-over and restarts leave the expander alone and l5_board_c6_power() moves only pin 0. The power-off pulses
   open the BSP's expander driver, which resets the chip, so the power-off task calls this again when the power
   stays on (USB). The log line says how CHG_EN was found: after a RESET from Aurora "was high" means the expander
   kept it, "was off" that it was lost. */
esp_err_t l5_board_charger_on(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "I2C");
    if (!s_pi4io) {
        ESP_RETURN_ON_ERROR(add_device(PI4IO_ADDR, &s_pi4io), TAG, "PI4IOE 0x44");
    }
    uint8_t dir = 0, out = 0, highz = 0, in = 0;
    ESP_RETURN_ON_ERROR(reg_read(s_pi4io, L5_PI4IO_DIR, &dir, 1), TAG, "PI4IOE 0x44 direction");
    ESP_RETURN_ON_ERROR(reg_read(s_pi4io, L5_PI4IO_OUT, &out, 1), TAG, "PI4IOE 0x44 levels");
    ESP_RETURN_ON_ERROR(reg_read(s_pi4io, L5_PI4IO_HIGHZ, &highz, 1), TAG, "PI4IOE 0x44 high-Z");
    for (size_t i = 0; i < sizeof(L5_PI4IO_CHARGER_ON) / sizeof(L5_PI4IO_CHARGER_ON[0]); i++) {
        const l5_pi4io_step_t *s = &L5_PI4IO_CHARGER_ON[i];
        ESP_RETURN_ON_ERROR(pi4io_update(s->reg, s->mask, s->value), TAG, "charger step %u", (unsigned)i);
    }
    const int stat = reg_read(s_pi4io, L5_PI4IO_IN, &in, 1) == ESP_OK ? (in & L5_PI4IO_CHG_STAT) != 0 : -1;
    const uint8_t pin = L5_PI4IO_CHG_EN;
    ESP_LOGI(TAG, "charger: CHG_EN high (CHG_STAT %d); it was %s: %s, high-Z %d, level %d", stat,
             (dir & pin) && !(highz & pin) && (out & pin) ? "high" : "off", (dir & pin) ? "output" : "input",
             (highz & pin) != 0, (out & pin) != 0);
    return ESP_OK;
}
