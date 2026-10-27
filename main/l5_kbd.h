/* M5Stack Tab5 Keyboard (SKU A164) on the Tab5's side port Ext.Port1: an STM32 on its own I2C bus (GPIO 0 SDA, GPIO 1
   SCL, port 0, 100 kHz), address 0x6D, INT on GPIO 50, switched to HID mode (one event per key: HID modifier byte +
   usage ID). A worker looks for it every 2 s, so it can be plugged in and pulled out at any time, and repeats a held
   key. The LVGL task takes the keys (l5_keys.c) and sends each where a tap would go. Register map: M5Stack's keyboard
   firmware (M5Tab5-Keyboard-Internal-FW, user_i2c_reg.h) and its drivers (M5Unit-KEYBOARD, M5Tab5-Keyboard-UserDemo). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    L5_KEY_CHAR = 1,  /* ch: a printable ASCII character (US layout, shift applied), space included */
    L5_KEY_ENTER,     /* also Enter of a numeric keypad */
    L5_KEY_ESC,
    L5_KEY_BACKSPACE,
    L5_KEY_DELETE,
    L5_KEY_TAB,
    L5_KEY_LEFT,
    L5_KEY_RIGHT,
    L5_KEY_UP,
    L5_KEY_DOWN,
    L5_KEY_HOME,
    L5_KEY_END,
    L5_KEY_PGUP,
    L5_KEY_PGDN,
    L5_KEY_F1,        /* F1..F12 follow: L5_KEY_F(n) */
} l5_key_kind_t;
#define L5_KEY_F(n) (L5_KEY_F1 + (n) - 1)

typedef struct {
    uint8_t kind; /* l5_key_kind_t */
    char ch;      /* L5_KEY_CHAR: the ASCII character, 0 for a Polish letter */
    uint16_t uc;  /* L5_KEY_CHAR: the character as Unicode, 0 = ch (keys made before 27.10.2026, the PC tests) */
    bool ctrl, shift, alt;
    bool repeat;  /* sent again by the auto-repeat while the key is held */
} l5_key_t;

/* the character of an L5_KEY_CHAR key as Unicode (Alt + a c e l n o s x z: the Polish letters), 0 for other keys */
static inline uint32_t l5_key_char(const l5_key_t *k)
{
    return k->kind != L5_KEY_CHAR ? 0 : k->uc ? k->uc : (uint8_t)k->ch;
}

/* Own I2C bus and worker task; the BSP's bus (system I2C, port CONFIG_BSP_I2C_NUM) is not touched. At a restart the
   bus is deleted and GPIO 0, 1 and 50 are reset, so the next app finds them as after power-on. */
void l5_kbd_init(void);
bool l5_kbd_present(void);          /* a keyboard answered and is in HID mode */
bool l5_kbd_get(l5_key_t *key);     /* next key press, false when none is waiting (any task) */
/* l5_kbd_map.c: one HID event (modifier byte: bits 0/4 Ctrl, 1/5 Shift, 2/6 Alt; usage ID) as a launcher key, false
   for a key the launcher does not use; whether a held key repeats */
bool l5_kbd_map(uint8_t mod, uint8_t usage, l5_key_t *out);
bool l5_kbd_repeats(const l5_key_t *k);
