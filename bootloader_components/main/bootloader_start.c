/*
 * SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileContributor: 2026 UNIZERSION (Tab5 Launcher changes)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * Tab5 Launcher second-stage bootloader (override of ESP-IDF 5.5.2
 * components/bootloader/subproject/main/bootloader_start.c).
 *
 * Stock behaviour: boot the ota_N app selected in otadata, or the factory app when
 * otadata is erased. Added here, M5Launcher style: the launcher (factory) gets the
 * device back after every reset that the user or the hardware caused.
 *
 *   power-on, RESET key, USB reset, watchdog, brown-out  -> launcher (factory)
 *   software restart (esp_restart from launcher or app)  -> app selected in otadata
 *   LP watchdog within 15 s of the app's start           -> that app again (counted as a
 *        quick restart; some apps restart themselves cleanly this way after a software reset)
 *   wake from deep sleep                                 -> launcher, unless the
 *        launcher wrote "L5:DS=0" into otadata.seq_label (cfg.deepSleepStart = false)
 *   the app restarted 3 times right after starting, or   -> launcher with the app stopped
 *        came back to the launcher 3 times within 15 s       (boot-loop guard, l5_guard.h)
 *        of starting
 *
 * The launcher hands over to an app by writing otadata and calling esp_restart().
 */
#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bootloader_common.h"
#include "esp32p4/rom/rtc.h"
#include "hal/lp_timer_ll.h"
#include "soc/clk_tree_defs.h"
#include "soc/lp_system_reg.h"
#include "soc/reset_reasons.h"
#include "l5_guard.h"

static const char *TAG = "l5boot";

_Static_assert(RESET_REASON_CHIP_POWER_ON == 0x01 && RESET_REASON_CORE_SW == 0x03 && RESET_REASON_CORE_DEEP_SLEEP == 0x05 &&
                   RESET_REASON_CORE_MWDT == 0x07 && RESET_REASON_CORE_RWDT == 0x09 && RESET_REASON_CPU_SW == 0x0C &&
                   RESET_REASON_CPU_RWDT == 0x0D && RESET_REASON_SYS_RWDT == 0x10 && RESET_REASON_CORE_USB_JTAG == 0x16 &&
                   RESET_REASON_CORE_USB_UART == 0x17 && RESET_REASON_CPU_JTAG == 0x18,
               "l5_guard_kind() uses the ESP32-P4 reset reason codes");

/* ---------- boot-loop guard (l5_guard.h) ----------
   A panic or esp_restart() is a software reset (rst:0xc), and every software reset boots the app in
   otadata again: an app that dies right after starting (e.g. a display driver for other hardware)
   would restart forever, and one that dies by brown-out or watchdog would come back through the
   launcher's bootscreen forever. l5_guard_decide() counts both rows in LP_SYSTEM_REG_LP_STORE13..15;
   the launcher reads them (l5_slots_guard_take() in main/l5_slots.c) and keeps them until the app runs
   15 s, RESET or the user's own BOOT. */
#define GUARD_STATE_REG LP_SYSTEM_REG_LP_STORE13_REG
#define GUARD_TIME_REG LP_SYSTEM_REG_LP_STORE14_REG
#define GUARD_CHECK_REG LP_SYSTEM_REG_LP_STORE15_REG

static uint32_t now_ms(void)
{
    lp_timer_ll_counter_snapshot(&LP_TIMER);
    uint64_t ticks = ((uint64_t)lp_timer_ll_get_counter_value_high(&LP_TIMER, 0) << 32) |
                     lp_timer_ll_get_counter_value_low(&LP_TIMER, 0);
    /* RTC_SLOW_CLK period in us << 19, calibrated by the app that ran last; not set yet: ~136 kHz */
    uint32_t cal = REG_READ(RTC_SLOW_CLK_CAL_REG);
    if (cal < (1u << 19) || cal > (100u << 19)) {
        cal = (uint32_t)((1000000ull << 19) / SOC_CLK_RC_SLOW_FREQ_APPROX);
    }
    return (uint32_t)(((ticks >> 10) * cal) / (1000u << 9)); /* ticks * cal / 2^19 / 1000, no overflow */
}

static bool deep_sleep_starts_launcher(const bootloader_state_t *bs)
{
    esp_ota_select_entry_t otadata[2];
    if (bs->ota_info.offset == 0 || bootloader_common_read_otadata(&bs->ota_info, otadata) != ESP_OK) {
        return true;
    }
    int active = bootloader_common_get_active_otadata(otadata);
    if (active < 0) {
        return true;
    }
    return memcmp(otadata[active].seq_label, "L5:DS=0", 7) != 0;
}

static int select_partition_number(bootloader_state_t *bs)
{
    if (!bootloader_utility_load_partition_table(bs)) {
        ESP_LOGE(TAG, "load partition table error!");
        return INVALID_INDEX;
    }
    int boot_index = bootloader_utility_get_selected_boot_partition(bs);
    if (boot_index == INVALID_INDEX || boot_index == FACTORY_INDEX || bs->factory.offset == 0) {
        return boot_index;
    }
    soc_reset_reason_t reason = esp_rom_get_reset_reason(0);
    l5_rst_kind_t kind = l5_guard_kind(reason);
    l5_guard_regs_t in = {REG_READ(GUARD_STATE_REG), REG_READ(GUARD_TIME_REG), REG_READ(GUARD_CHECK_REG)};
    l5_guard_t d = l5_guard_decide(in, boot_index, kind, now_ms(),
                                   kind == L5_RST_DEEP_SLEEP && deep_sleep_starts_launcher(bs));
    REG_WRITE(GUARD_STATE_REG, d.regs.state);
    REG_WRITE(GUARD_TIME_REG, d.regs.time);
    REG_WRITE(GUARD_CHECK_REG, d.regs.check);
    if (d.stop) {
        ESP_LOGW(TAG, "ota_%d stopped: %u quick restarts, %u early returns within 15 s (reset reason %d): starting launcher",
                 boot_index, (unsigned)(d.boots ? d.boots - 1 : 0), (unsigned)d.returns, (int)reason);
    } else if (d.app && d.boots > 1) {
        ESP_LOGW(TAG, "ota_%d: quick restart %u of %u (reset reason %d, %u ms after start)", boot_index,
                 (unsigned)(d.boots - 1), (unsigned)(L5_GUARD_QUICK_BOOTS - 1), (int)reason, (unsigned)d.age);
    } else if (!d.app && d.age) {
        ESP_LOGW(TAG, "reset reason %d %u ms after ota_%d started: starting launcher (early returns %u of %u)",
                 (int)reason, (unsigned)d.age, boot_index, (unsigned)d.returns, (unsigned)L5_GUARD_RETURNS_STOP);
    } else if (!d.app) {
        ESP_LOGW(TAG, "reset reason %d: starting launcher", (int)reason);
    }
    return d.app ? boot_index : FACTORY_INDEX;
}

void __attribute__((noreturn)) call_start_cpu0(void)
{
    if (bootloader_init() != ESP_OK) {
        bootloader_reset();
    }

    bootloader_state_t bs = {0};
    int boot_index = select_partition_number(&bs);
    if (boot_index == INVALID_INDEX) {
        bootloader_reset();
    }
    bootloader_utility_load_boot_image(&bs, boot_index);
}

#if CONFIG_LIBC_NEWLIB
// Return global reent struct if any newlib functions are linked to bootloader
struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}
#endif
