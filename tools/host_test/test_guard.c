/* The boot-loop guard on the PC: bootloader_components/main/l5_guard.h decides between the launcher and the app
   in otadata for every reset reason and guard state, and a small device model (LP registers, LP clock, the
   launcher's start, its bootscreen and the user's BOOT, as main.c and l5_slots.c do them) runs the boot loops:
   OrcSDR's clean restart through the LP watchdog, crash loops, brown-out and watchdog loops through the
   launcher with short and long cycles, and what ends a row (15 s of running, RESET, the user's BOOT).

   run.py builds it (also --guard). */
#include <stdio.h>
#include <string.h>
#include "l5_guard.h"

static int s_fails;
#define EXPECT(cond, ...)                         \
    do {                                          \
        if (!(cond)) {                            \
            s_fails++;                            \
            printf("  FAIL line %d: ", __LINE__); \
            printf(__VA_ARGS__);                  \
            printf("\n");                         \
        }                                         \
    } while (0)

static const l5_guard_regs_t ZERO = {0, 0, 0};

static uint32_t st_returns(l5_guard_regs_t r)
{
    return (r.state & L5_GUARD_RETURNS_MASK) >> L5_GUARD_RETURNS_SHIFT;
}

static uint32_t st_boots(l5_guard_regs_t r)
{
    return r.state & L5_GUARD_BOOTS_MASK;
}

static bool same_regs(l5_guard_regs_t a, l5_guard_regs_t b)
{
    return a.state == b.state && a.time == b.time && a.check == b.check;
}

/* ---------- single decisions ---------- */

static void kinds(void)
{
    printf("reset reasons\n");
    static const struct {
        uint32_t code;
        l5_rst_kind_t kind;
    } k[] = {{0x01, L5_RST_POWER}, {0x03, L5_RST_SW},     {0x05, L5_RST_DEEP_SLEEP}, {0x07, L5_RST_KEY},
             {0x09, L5_RST_LP_WDT}, {0x0B, L5_RST_OTHER}, {0x0C, L5_RST_SW},         {0x0D, L5_RST_LP_WDT},
             {0x0F, L5_RST_OTHER}, {0x10, L5_RST_LP_WDT}, {0x12, L5_RST_OTHER},      {0x13, L5_RST_OTHER},
             {0x14, L5_RST_OTHER}, {0x16, L5_RST_KEY},    {0x17, L5_RST_KEY},        {0x18, L5_RST_KEY},
             {0x1A, L5_RST_OTHER}, {0x00, L5_RST_OTHER}};
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        EXPECT(l5_guard_kind(k[i].code) == k[i].kind, "reset reason 0x%02X: kind %d, expected %d", (unsigned)k[i].code,
               (int)l5_guard_kind(k[i].code), (int)k[i].kind);
    }
}

static void decisions(void)
{
    printf("single decisions\n");
    const uint32_t now = 500000;
    l5_guard_t d;

    /* empty registers (power-on, RESET, the user's BOOT) */
    d = l5_guard_decide(ZERO, 0, L5_RST_POWER, now, true);
    EXPECT(!d.app && same_regs(d.regs, ZERO), "power-on: launcher, nothing kept");
    d = l5_guard_decide(ZERO, 0, L5_RST_KEY, now, true);
    EXPECT(!d.app && same_regs(d.regs, ZERO), "RESET: launcher");
    d = l5_guard_decide(ZERO, 0, L5_RST_SW, now, true);
    EXPECT(d.app && d.boots == 1 && l5_guard_valid(&d.regs) && st_boots(d.regs) == 1 && d.regs.time == now &&
               !(d.regs.state & L5_GUARD_LAUNCHER),
           "hand-over: app, first boot of a row (boots %u)", (unsigned)d.boots);
    d = l5_guard_decide(ZERO, 2, L5_RST_SW, now, true);
    EXPECT(d.app && ((d.regs.state >> L5_GUARD_SLOT_SHIFT) & 0xF) == 2, "hand-over to ota_2: the slot is kept");
    d = l5_guard_decide(ZERO, 0, L5_RST_DEEP_SLEEP, now, true);
    EXPECT(!d.app && same_regs(d.regs, ZERO), "wake with deepSleepStart: launcher");
    d = l5_guard_decide(ZERO, 0, L5_RST_DEEP_SLEEP, now, false);
    EXPECT(d.app && d.boots == 1, "wake with L5:DS=0: app, counted from its start");
    d = l5_guard_decide(ZERO, 0, L5_RST_LP_WDT, now, true);
    EXPECT(!d.app && same_regs(d.regs, ZERO), "LP watchdog without an app boot on record: launcher");
    d = l5_guard_decide(ZERO, 0, L5_RST_OTHER, now, true);
    EXPECT(!d.app && same_regs(d.regs, ZERO), "brown-out without an app boot on record: launcher");

    /* the app in otadata started 2 s ago */
    l5_guard_regs_t ran2 = l5_guard_pack(0, 0, 1, 1, now - 2000);
    d = l5_guard_decide(ran2, 0, L5_RST_LP_WDT, now, true);
    EXPECT(d.app && d.boots == 2 && d.returns == 1 && d.age == 2000,
           "LP watchdog 2 s after start: the app again, quick restart (boots %u, returns %u)", (unsigned)d.boots,
           (unsigned)d.returns);
    d = l5_guard_decide(ran2, 0, L5_RST_SW, now, true);
    EXPECT(d.app && d.boots == 2 && d.returns == 1, "panic 2 s after start: quick restart, the early returns stay");
    d = l5_guard_decide(ran2, 0, L5_RST_OTHER, now, true);
    EXPECT(!d.app && !d.stop && d.returns == 2 && st_returns(d.regs) == 2 && (d.regs.state & L5_GUARD_LAUNCHER),
           "brown-out 2 s after start: launcher, early return 2 (returns %u)", (unsigned)d.returns);
    d = l5_guard_decide(ran2, 0, L5_RST_KEY, now, true);
    EXPECT(!d.app && same_regs(d.regs, ZERO), "RESET 2 s after start: launcher, both rows end");
    d = l5_guard_decide(ran2, 0, L5_RST_DEEP_SLEEP, now, false);
    EXPECT(d.app && d.boots == 1 && d.returns == 0, "wake: a fresh start");

    /* two early returns already: the third stops */
    l5_guard_regs_t ran2b = l5_guard_pack(0, 0, 2, 1, now - 2000);
    d = l5_guard_decide(ran2b, 0, L5_RST_OTHER, now, true);
    EXPECT(!d.app && d.stop && d.returns == 3 && (d.regs.state & L5_GUARD_STOPPED), "third early return: stopped");
    d = l5_guard_decide(ran2b, 0, L5_RST_LP_WDT, now, true);
    EXPECT(d.app && d.boots == 2, "LP watchdog right after start is a restart, not a return");

    /* three quick restarts already: the fourth boot stops */
    l5_guard_regs_t quick3 = l5_guard_pack(0, 0, 0, 3, now - 1000);
    d = l5_guard_decide(quick3, 0, L5_RST_SW, now, true);
    EXPECT(!d.app && d.stop && d.boots == 4 && (d.regs.state & L5_GUARD_STOPPED) && (d.regs.state & L5_GUARD_LAUNCHER),
           "4th quick boot (software): stopped");
    d = l5_guard_decide(quick3, 0, L5_RST_LP_WDT, now, true);
    EXPECT(!d.app && d.stop && d.boots == 4, "4th quick boot (LP watchdog): stopped");

    /* the app ran 20 s */
    l5_guard_regs_t ran20 = l5_guard_pack(0, 0, 2, 3, now - 20000);
    d = l5_guard_decide(ran20, 0, L5_RST_LP_WDT, now, true);
    EXPECT(!d.app && !d.stop && d.returns == 0 && st_returns(d.regs) == 0,
           "LP watchdog after 20 s: launcher as before, the rows end");
    d = l5_guard_decide(ran20, 0, L5_RST_SW, now, true);
    EXPECT(d.app && d.boots == 1 && d.returns == 0, "restart after 20 s: a new row");
    d = l5_guard_decide(ran20, 0, L5_RST_OTHER, now, true);
    EXPECT(!d.app && !d.stop && d.returns == 0, "brown-out after 20 s: launcher, no early return");
    d = l5_guard_decide(l5_guard_pack(0, 0, 0, 1, now - L5_GUARD_QUICK_MS), 0, L5_RST_LP_WDT, now, true);
    EXPECT(!d.app, "LP watchdog exactly 15 s after start: launcher");
    d = l5_guard_decide(l5_guard_pack(0, 0, 0, 1, now - L5_GUARD_QUICK_MS + 1), 0, L5_RST_LP_WDT, now, true);
    EXPECT(d.app, "LP watchdog 14.999 s after start: the app");

    /* the launcher ran last (it went there after an early return) */
    l5_guard_regs_t in_launcher = l5_guard_pack(L5_GUARD_LAUNCHER, 0, 2, 1, now - 9000);
    d = l5_guard_decide(in_launcher, 0, L5_RST_OTHER, now, true);
    EXPECT(!d.app && same_regs(d.regs, in_launcher), "brown-out in the launcher: nothing counted, the rows stay");
    d = l5_guard_decide(in_launcher, 0, L5_RST_LP_WDT, now, true);
    EXPECT(!d.app && same_regs(d.regs, in_launcher), "LP watchdog in the launcher: launcher, the rows stay");
    d = l5_guard_decide(in_launcher, 0, L5_RST_SW, now, true);
    EXPECT(d.app && d.boots == 1 && d.returns == 2 && !(d.regs.state & L5_GUARD_LAUNCHER),
           "the bootscreen's hand-over: app, new row of restarts, early returns kept (%u)", (unsigned)d.returns);
    d = l5_guard_decide(in_launcher, 0, L5_RST_KEY, now, true);
    EXPECT(!d.app && same_regs(d.regs, ZERO), "RESET in the launcher: the rows end");

    /* a stop the launcher has not taken yet */
    l5_guard_regs_t stop = l5_guard_pack(L5_GUARD_LAUNCHER | L5_GUARD_STOPPED, 0, 3, 1, now - 9000);
    for (int k = L5_RST_POWER; k <= L5_RST_OTHER; k++) {
        if (k == L5_RST_DEEP_SLEEP) continue;
        d = l5_guard_decide(stop, 0, (l5_rst_kind_t)k, now, true);
        EXPECT(!d.app && same_regs(d.regs, stop), "pending stop, reset kind %d: launcher, the stop stays", k);
    }

    /* the app that ran is not the one otadata selects now */
    l5_guard_regs_t other_slot = l5_guard_pack(0, 1, 2, 2, now - 2000);
    d = l5_guard_decide(other_slot, 0, L5_RST_LP_WDT, now, true);
    EXPECT(!d.app && st_returns(d.regs) == 0 && ((d.regs.state >> L5_GUARD_SLOT_SHIFT) & 0xF) == 0,
           "LP watchdog, otadata changed: launcher, a new start for ota_0");
    d = l5_guard_decide(other_slot, 0, L5_RST_SW, now, true);
    EXPECT(d.app && d.boots == 1 && d.returns == 0, "restart into another slot: a new row");

    /* registers that do not check out */
    l5_guard_regs_t bad = ran2b;
    bad.check ^= 1;
    d = l5_guard_decide(bad, 0, L5_RST_OTHER, now, true);
    EXPECT(!d.app && same_regs(d.regs, ZERO), "broken check word: empty");
    d = l5_guard_decide(bad, 0, L5_RST_LP_WDT, now, true);
    EXPECT(!d.app, "broken check word: the LP watchdog rule needs a recorded app boot");
    l5_guard_regs_t old = {0x15B70101u, now - 1000, 0x15B70101u ^ (now - 1000) ^ L5_GUARD_SALT};
    d = l5_guard_decide(old, 1, L5_RST_SW, now, true);
    EXPECT(d.app && d.boots == 1, "the first layout's state reads as empty");

    /* the LP clock wraps between the app boot and the reset */
    l5_guard_regs_t wrap = l5_guard_pack(0, 0, 0, 1, 0xFFFFF000u);
    d = l5_guard_decide(wrap, 0, L5_RST_LP_WDT, 0x00000500u, true);
    EXPECT(d.app && d.age == 0x1500u, "LP clock wrap: %u ms after start", (unsigned)d.age);
}

/* ---------- the device: LP registers, LP clock, bootloader, launcher ---------- */

typedef enum { RUN_LAUNCHER, RUN_APP } run_t;

static struct {
    l5_guard_regs_t regs;
    uint32_t t;       /* LP clock, ms */
    int otadata;      /* ota_N the launcher's hand-over selected (the bootloader's boot_index) */
    run_t running;
    int app_boots;    /* app starts in this scenario */
    bool stopped;     /* the launcher took a stop and shows "... was stopped" */
    int stop_restarts, stop_returns;
} D;

static void power_on(void)
{
    memset(&D, 0, sizeof(D));
    D.t = 1000;
}

/* the bootloader after a reset */
static void reset(l5_rst_kind_t kind, bool ds_launcher)
{
    l5_guard_t d = l5_guard_decide(D.regs, D.otadata, kind, D.t, ds_launcher);
    D.regs = d.regs;
    D.t += 300; /* ROM, bootloader, image check */
    if (d.app) {
        D.running = RUN_APP;
        D.app_boots++;
        return;
    }
    D.running = RUN_LAUNCHER;
    /* app_main: l5_slots_guard_take() */
    if (l5_guard_valid(&D.regs) && (D.regs.state & L5_GUARD_STOPPED)) {
        uint32_t boots = D.regs.state & L5_GUARD_BOOTS_MASK;
        D.stopped = true;
        D.stop_restarts = boots >= L5_GUARD_QUICK_BOOTS ? (int)boots - 1 : 0;
        D.stop_returns = (int)st_returns(D.regs);
        D.regs = ZERO;
    }
}

/* the launcher hands over: otadata, esp_restart() */
static void hand_over(bool by_user)
{
    if (by_user) D.regs = ZERO; /* l5_boot_app(): l5_slots_guard_clear() */
    D.t += 120;
    reset(L5_RST_SW, true);
}

/* the launcher's start, then the bootscreen's countdown (unless the app is stopped) */
static void launcher_runs(uint32_t ms)
{
    D.t += ms;
    if (!D.stopped) hand_over(false);
}

static void app_runs(uint32_t ms)
{
    D.t += ms;
}

static void loops(void)
{
    printf("boot loops\n");

    /* OrcSDR (log of 19.10.2026, build 0x1CE880): after a software reset it restarts itself through the LP
       watchdog 1.5 s after starting. Before: the launcher every time, 5 rounds until the user stepped in. */
    power_on();
    D.otadata = 0;
    launcher_runs(7500);
    EXPECT(D.running == RUN_APP && D.app_boots == 1, "OrcSDR: the bootscreen boots it");
    app_runs(1500);
    reset(L5_RST_LP_WDT, true);
    EXPECT(D.running == RUN_APP && D.app_boots == 2 && st_boots(D.regs) == 2,
           "OrcSDR: LP watchdog 1.5 s after start boots it again (running %d, boots %u)", (int)D.running,
           (unsigned)st_boots(D.regs));
    app_runs(60000);
    reset(L5_RST_KEY, true);
    EXPECT(D.running == RUN_LAUNCHER && same_regs(D.regs, ZERO) && !D.stopped, "OrcSDR: RESET after a minute: launcher");
    launcher_runs(7500);
    app_runs(1500);
    reset(L5_RST_LP_WDT, true);
    EXPECT(D.running == RUN_APP && D.app_boots == 4, "OrcSDR: the next bootscreen boot works the same way");
    app_runs(16000);
    reset(L5_RST_SW, true); /* its own esp_restart after a while, then its clean restart again */
    app_runs(1500);
    reset(L5_RST_LP_WDT, true);
    EXPECT(D.running == RUN_APP && st_boots(D.regs) == 2, "OrcSDR: own restart after 16 s, clean restart: still the app");

    /* crash right after starting: quick software restarts */
    power_on();
    launcher_runs(7500);
    for (int i = 0; i < 10 && D.running == RUN_APP; i++) {
        app_runs(800);
        reset(L5_RST_SW, true);
    }
    EXPECT(D.stopped && D.app_boots == 3 && D.stop_restarts == 3 && D.stop_returns == 0,
           "panic loop: stopped after the 3rd quick restart (app boots %d, restarts %d)", D.app_boots, D.stop_restarts);

    /* an app that only ever restarts through the LP watchdog */
    power_on();
    launcher_runs(7500);
    for (int i = 0; i < 10 && D.running == RUN_APP; i++) {
        app_runs(1000);
        reset(L5_RST_LP_WDT, true);
    }
    EXPECT(D.stopped && D.app_boots == 3 && D.stop_restarts == 3, "LP watchdog loop: stopped (app boots %d)", D.app_boots);

    /* brown-out / CPU watchdog loops through the launcher, short and long rounds (bootTime 1..5 s) */
    for (uint32_t boot_ms = 1000; boot_ms <= 5000; boot_ms += 4000) {
        for (uint32_t run_ms = 200; run_ms <= 14000; run_ms += 6900) {
            power_on();
            launcher_runs(boot_ms + 2500);
            int rounds = 0;
            while (!D.stopped && rounds < 50) {
                rounds++;
                app_runs(run_ms);
                reset(L5_RST_OTHER, true);
                launcher_runs(boot_ms + 2500);
            }
            EXPECT(D.stopped && rounds == 3 && D.stop_returns == 3 && D.stop_restarts == 0,
                   "loop through the launcher (bootTime %u ms, app runs %u ms): stopped after %d rounds",
                   (unsigned)boot_ms, (unsigned)run_ms, rounds);
        }
    }

    /* every loop ends: any mix of resets right after starting reaches a stop */
    static const l5_rst_kind_t mix[] = {L5_RST_LP_WDT, L5_RST_OTHER, L5_RST_SW, L5_RST_OTHER, L5_RST_LP_WDT,
                                        L5_RST_SW,     L5_RST_SW,    L5_RST_OTHER, L5_RST_LP_WDT};
    power_on();
    launcher_runs(4000);
    int n = 0;
    while (!D.stopped && n < 100) {
        app_runs(700);
        reset(mix[n % (int)(sizeof(mix) / sizeof(mix[0]))], true);
        n++;
        if (D.running == RUN_LAUNCHER) launcher_runs(4000);
    }
    EXPECT(D.stopped && n <= 12, "mixed early resets: stopped after %d resets", n);

    /* a run of 15 s ends the row of early returns */
    power_on();
    launcher_runs(7500);
    app_runs(2000);
    reset(L5_RST_OTHER, true);
    launcher_runs(7500);
    app_runs(2000);
    reset(L5_RST_OTHER, true);
    EXPECT(st_returns(D.regs) == 2, "two early returns counted (%u)", (unsigned)st_returns(D.regs));
    launcher_runs(7500);
    app_runs(20000);
    reset(L5_RST_OTHER, true);
    EXPECT(!D.stopped && st_returns(D.regs) == 0, "after 20 s of running the brown-out is no early return");
    launcher_runs(7500);
    app_runs(2000);
    reset(L5_RST_OTHER, true);
    EXPECT(!D.stopped && st_returns(D.regs) == 1, "a new row starts at 1 (%u)", (unsigned)st_returns(D.regs));

    /* RESET ends the row */
    power_on();
    for (int i = 0; i < 2; i++) {
        launcher_runs(7500);
        app_runs(2000);
        reset(L5_RST_OTHER, true);
    }
    D.t += 3000;
    reset(L5_RST_KEY, true); /* pressed during the bootscreen */
    launcher_runs(7500);
    app_runs(2000);
    reset(L5_RST_OTHER, true);
    EXPECT(!D.stopped && st_returns(D.regs) == 1, "RESET ended the row: 1 early return after it (%u)",
           (unsigned)st_returns(D.regs));

    /* the user's own BOOT ends the row */
    power_on();
    for (int i = 0; i < 2; i++) {
        launcher_runs(7500);
        app_runs(2000);
        reset(L5_RST_OTHER, true);
    }
    D.t += 4000; /* ENTER LAUNCHER, then BOOT on HOME */
    hand_over(true);
    app_runs(2000);
    reset(L5_RST_OTHER, true);
    EXPECT(!D.stopped && st_returns(D.regs) == 1, "the user's BOOT ended the row (%u)", (unsigned)st_returns(D.regs));

    /* BOOT AGAIN after a stop: the row starts over, the next loop is stopped again */
    power_on();
    for (int i = 0; i < 50 && !D.stopped; i++) {
        launcher_runs(7500);
        app_runs(2000);
        reset(L5_RST_OTHER, true);
    }
    EXPECT(D.stopped, "loop through the launcher before BOOT AGAIN: stopped");
    D.stopped = false; /* BOOT AGAIN */
    hand_over(true);
    int again = 1;
    while (!D.stopped && again < 50) {
        app_runs(2000);
        reset(L5_RST_OTHER, true);
        launcher_runs(7500);
        again++;
    }
    EXPECT(D.stopped && D.stop_returns == 3, "after BOOT AGAIN the loop is stopped again (%d app runs)", again - 1);

    /* the launcher itself browns out while it counts down: nothing counted */
    power_on();
    launcher_runs(7500);
    app_runs(2000);
    reset(L5_RST_OTHER, true);
    D.t += 3000;
    reset(L5_RST_OTHER, true); /* in the launcher */
    EXPECT(D.running == RUN_LAUNCHER && st_returns(D.regs) == 1, "brown-out in the launcher: still 1 early return (%u)",
           (unsigned)st_returns(D.regs));

    /* a stop survives a launcher restart before it was shown */
    power_on();
    D.regs = l5_guard_pack(L5_GUARD_LAUNCHER | L5_GUARD_STOPPED, 0, 3, 1, D.t - 9000);
    reset(L5_RST_SW, true); /* the launcher panicked before l5_slots_guard_take() */
    EXPECT(D.running == RUN_LAUNCHER && D.stopped && D.stop_returns == 3, "pending stop after a launcher panic: shown");

    /* deep sleep with L5:DS=0: the app is counted from its wake */
    power_on();
    D.otadata = 1;
    reset(L5_RST_DEEP_SLEEP, false);
    EXPECT(D.running == RUN_APP && st_boots(D.regs) == 1, "wake into the app");
    app_runs(900);
    reset(L5_RST_SW, true);
    EXPECT(D.running == RUN_APP && st_boots(D.regs) == 2, "a panic right after the wake: quick restart 1");
}

int main(void)
{
    kinds();
    decisions();
    loops();
    printf(s_fails ? "guard: FAILED %d\n" : "guard: PASSED\n", s_fails);
    return s_fails ? 1 : 0;
}
