/* M5Stack Tab5 Keyboard (SKU A164) on the Tab5's side port Ext.Port1: an STM32 on its own I2C bus (GPIO 0 SDA, GPIO 1
   SCL, 400 kHz), address 0x6D, switched to HID mode (one event per key: HID modifier byte + usage ID). A worker looks
   for it once a second, so it can be plugged in and pulled out at any time. Keys go to the on-screen keyboard's field
   (l5_keyboard.c): characters, Backspace, Enter = OK, Esc = CANCEL. Register map: M5Stack's keyboard firmware
   (M5Tab5-Keyboard-Internal-FW, user_i2c_reg.h) and its drivers (M5Unit-KEYBOARD, M5Tab5-Keyboard-UserDemo). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    L5_KBD_CHAR,      /* ch: a printable ASCII character (US layout, shift applied) */
    L5_KBD_ENTER,
    L5_KBD_ESC,
    L5_KBD_BACKSPACE,
} l5_kbd_kind_t;

typedef struct {
    uint8_t kind; /* l5_kbd_kind_t */
    char ch;
} l5_kbd_key_t;

/* Own I2C bus and worker task; the BSP's bus (system I2C, port CONFIG_BSP_I2C_NUM) is not touched. At a restart
   the keyboard gets back the mode it had and the bus is deleted, so the next app finds both as after power-on. */
void l5_kbd_init(void);
bool l5_kbd_present(void);           /* a keyboard answered and is in HID mode */
bool l5_kbd_get(l5_kbd_key_t *key);  /* next key press, false when none is waiting (any task) */
void l5_kbd_flush(void);             /* drop keys typed while nothing listened */
