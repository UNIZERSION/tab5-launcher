/* The second IO expander of the Tab5, PI4IOE5V6408 at 0x44: its registers and the pins the launcher drives.
   l5_board.c changes them one register at a time (read-modify-write), never through the BSP's expander driver,
   which resets the whole chip when it is first opened (all pins back to inputs with pull-downs: WLAN_PWR_EN,
   USB 5 V and CHG_EN drop). Host test of the charger steps: tools/host_test/test_charger.c (run.py --charger). */
#pragma once
#include <stdint.h>

#define L5_PI4IO_DIR 0x03     /* 1 = output */
#define L5_PI4IO_OUT 0x05     /* output level */
#define L5_PI4IO_HIGHZ 0x07   /* 1 = output off (high impedance) */
#define L5_PI4IO_PULL_EN 0x0B /* 1 = pull resistor on */
#define L5_PI4IO_IN 0x0F      /* pin levels */

#define L5_PI4IO_WLAN_PWR_EN 0x01 /* pin 0: ESP32-C6 supply */
#define L5_PI4IO_CHG_STAT 0x40    /* pin 6: the charger's status output */
#define L5_PI4IO_CHG_EN 0x80      /* pin 7: charger enable, high = charging allowed */

typedef struct {
    uint8_t reg, mask, value;
} l5_pi4io_step_t;

/* Charger on, as Aurora's tab5_charger_init() ("Same CHG_EN=HIGH as M5Unified"). CHG_EN: the level first, then
   push-pull, then output, so it is never driven low on the way (as WLAN_PWR_EN in l5_board_c6_power()). CHG_STAT:
   an input first (never driven), then no pull (the charger drives it), then the input buffer as Aurora sets it. */
static const l5_pi4io_step_t L5_PI4IO_CHARGER_ON[] = {
    {L5_PI4IO_OUT, L5_PI4IO_CHG_EN, L5_PI4IO_CHG_EN},
    {L5_PI4IO_HIGHZ, L5_PI4IO_CHG_EN, 0},
    {L5_PI4IO_DIR, L5_PI4IO_CHG_EN, L5_PI4IO_CHG_EN},
    {L5_PI4IO_DIR, L5_PI4IO_CHG_STAT, 0},
    {L5_PI4IO_PULL_EN, L5_PI4IO_CHG_STAT, 0},
    {L5_PI4IO_HIGHZ, L5_PI4IO_CHG_STAT, 0},
};
