/* Tab5 Launcher boot-loop guard: the bootloader's choice between the launcher and the app in otadata
   (bootloader_start.c) and the state it keeps for the launcher (main/l5_slots.c). No ESP-IDF types, so
   tools/host_test/test_guard.c runs the same decisions on the PC.

   State: LP_SYSTEM_REG_LP_STORE13..15 (IDF uses STORE1..10 on the P4). They keep their value through every
   reset of the HP system, the LP timer keeps counting through them, and power-on clears both.

   Two rows are counted for the app that otadata selects:
   - boots:   app boots that follow the previous app boot within 15 s without the launcher in between
              (software restarts, panics, an LP watchdog reset right after starting). The 4th boot in such a
              row, the 3rd quick restart, starts the launcher with `stopped` instead.
   - returns: resets that brought the device back to the launcher within 15 s of the app's start, other than
              RESET, power-on, deep-sleep wake and host resets (brown-out, CPU watchdog, an LP watchdog the app
              did not survive, ...). The launcher's bootscreen boots the app again and does not clear this row;
              the 3rd early return sets `stopped`.
   Both rows end when the app runs 15 s, with RESET and power-on, and when the user boots an app by hand
   (the launcher clears the registers). With `stopped` set the launcher shows "... was stopped" and keeps the
   app from starting by itself (NVS mark) until the user boots it again. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define L5_GUARD_MAGIC 0x5B200000u /* bits 31..20; the first layout (0x15B7 in bits 31..16) reads as empty */
#define L5_GUARD_MAGIC_MASK 0xFFF00000u
#define L5_GUARD_LAUNCHER (1u << 19) /* the bootloader started the launcher last */
#define L5_GUARD_RETURNS_SHIFT 16    /* bits 18..16: early returns to the launcher in a row */
#define L5_GUARD_RETURNS_MASK (7u << L5_GUARD_RETURNS_SHIFT)
#define L5_GUARD_STOPPED (1u << 15)
#define L5_GUARD_SLOT_SHIFT 8 /* bits 11..8: ota_N */
#define L5_GUARD_BOOTS_MASK 0xFFu
#define L5_GUARD_SALT 0xA5A5A5A5u
#define L5_GUARD_QUICK_MS 15000u
#define L5_GUARD_QUICK_BOOTS 4u   /* the first boot + 3 quick restarts */
#define L5_GUARD_RETURNS_STOP 3u  /* early returns to the launcher */

typedef enum {
    L5_RST_POWER,      /* power-on */
    L5_RST_KEY,        /* the RESET key (MWDT core reset on this Tab5) and resets from the host (USB, JTAG) */
    L5_RST_SW,         /* esp_restart(), panic: the launcher's hand-over or the app's own restart */
    L5_RST_DEEP_SLEEP, /* wake from deep sleep */
    L5_RST_LP_WDT,     /* RTC (LP) watchdog */
    L5_RST_OTHER,      /* brown-out, CPU watchdog, super watchdog, lockup, ... */
} l5_rst_kind_t;

/* soc_reset_reason_t of the ESP32-P4 (components/soc/esp32p4/include/soc/reset_reasons.h, checked by
   bootloader_start.c) */
static inline l5_rst_kind_t l5_guard_kind(uint32_t reason)
{
    switch (reason) {
    case 0x01: return L5_RST_POWER;      /* CHIP_POWER_ON */
    case 0x07:                           /* CORE_MWDT: rst:0x7 HP_SYS_HP_WDT_RESET, the RESET key */
    case 0x16:                           /* CORE_USB_JTAG */
    case 0x17:                           /* CORE_USB_UART: esptool, serial monitor */
    case 0x18: return L5_RST_KEY;        /* CPU_JTAG */
    case 0x03:                           /* CORE_SW */
    case 0x0C: return L5_RST_SW;         /* CPU_SW */
    case 0x05: return L5_RST_DEEP_SLEEP; /* CORE_DEEP_SLEEP */
    case 0x09:                           /* CORE_RWDT: rst:0x9 HP_SYS_LP_WDT_RESET */
    case 0x0D:                           /* CPU_RWDT */
    case 0x10: return L5_RST_LP_WDT;     /* SYS_RWDT */
    default: return L5_RST_OTHER;
    }
}

typedef struct {
    uint32_t state, time, check; /* time: LP timer ms of the last app boot */
} l5_guard_regs_t;

typedef struct {
    bool app;             /* boot the app in otadata, else the launcher */
    bool stop;            /* this decision set `stopped` */
    uint32_t boots, returns, age; /* the rows after the decision; age: ms the app ran (0 when it did not run last) */
    l5_guard_regs_t regs; /* to write back */
} l5_guard_t;

static inline bool l5_guard_valid(const l5_guard_regs_t *r)
{
    return (r->state & L5_GUARD_MAGIC_MASK) == L5_GUARD_MAGIC && r->check == (r->state ^ r->time ^ L5_GUARD_SALT);
}

static inline l5_guard_regs_t l5_guard_pack(uint32_t flags, int slot, uint32_t returns, uint32_t boots, uint32_t time)
{
    l5_guard_regs_t r;
    r.state = L5_GUARD_MAGIC | flags | (returns << L5_GUARD_RETURNS_SHIFT & L5_GUARD_RETURNS_MASK) |
              ((uint32_t)slot & 0xFu) << L5_GUARD_SLOT_SHIFT | (boots > 0xFFu ? 0xFFu : boots);
    r.time = time;
    r.check = r.state ^ time ^ L5_GUARD_SALT;
    return r;
}

/* one more app boot in the row of quick direct restarts */
static inline l5_guard_t l5_guard_boot(l5_guard_t d, int app, uint32_t now)
{
    d.boots++;
    d.stop = d.boots >= L5_GUARD_QUICK_BOOTS;
    d.app = !d.stop;
    d.regs = l5_guard_pack(d.stop ? L5_GUARD_LAUNCHER | L5_GUARD_STOPPED : 0, app, d.returns, d.boots, now);
    return d;
}

/* app: ota_N in otadata; now: LP timer ms; ds_launcher: a deep-sleep wake starts the launcher (cfg.deepSleepStart) */
static inline l5_guard_t l5_guard_decide(l5_guard_regs_t in, int app, l5_rst_kind_t kind, uint32_t now, bool ds_launcher)
{
    l5_guard_t d = {0};
    const bool valid = l5_guard_valid(&in);
    const uint32_t st = valid ? in.state : 0;
    const bool stopped = (st & L5_GUARD_STOPPED) != 0, launcher_last = (st & L5_GUARD_LAUNCHER) != 0;
    const bool same = valid && !stopped && (int)((st >> L5_GUARD_SLOT_SHIFT) & 0xFu) == app;
    const bool app_ran = same && !launcher_last; /* this reset ended a run of the app in otadata */
    d.age = app_ran ? now - in.time : 0;
    const bool early = app_ran && d.age < L5_GUARD_QUICK_MS;
    if (same) {
        d.boots = st & L5_GUARD_BOOTS_MASK;
        d.returns = (st & L5_GUARD_RETURNS_MASK) >> L5_GUARD_RETURNS_SHIFT;
    }
    if (app_ran && !early) { /* it ran 15 s: both rows end */
        d.boots = 0;
        d.returns = 0;
    }
    switch (kind) {
    case L5_RST_POWER:
    case L5_RST_KEY: /* the user's way back: the launcher, the rows start over; a stop not shown yet stays */
        if (valid && stopped) d.regs = in;
        return d;
    case L5_RST_DEEP_SLEEP: /* a fresh start */
        d.boots = d.returns = 0;
        if (ds_launcher) return d;
        return l5_guard_boot(d, app, now);
    case L5_RST_SW:
        if (valid && stopped) { /* the launcher restarted before it showed the stop */
            d.regs = in;
            return d;
        }
        if (!early) d.boots = 0; /* the launcher's hand-over or a restart after 15 s: a new row */
        return l5_guard_boot(d, app, now);
    case L5_RST_LP_WDT:
        if (early) return l5_guard_boot(d, app, now); /* e.g. a clean restart for USB right after starting */
        /* later, or not right after the app's start: the launcher, like the resets below */
        /* fall through */
    default:
        if (!valid) return d;
        if (launcher_last || stopped) { /* the launcher was running or has a stop to show: nothing to count */
            d.regs = in;
            return d;
        }
        if (!same) { /* another app ran than the one otadata selects now: a new start for that one */
            d.regs = l5_guard_pack(L5_GUARD_LAUNCHER, app, 0, 0, in.time);
            return d;
        }
        if (early) d.returns++;
        d.stop = d.returns >= L5_GUARD_RETURNS_STOP;
        d.regs = l5_guard_pack(L5_GUARD_LAUNCHER | (d.stop ? L5_GUARD_STOPPED : 0), app, d.returns, d.boots, in.time);
        return d;
    }
}
