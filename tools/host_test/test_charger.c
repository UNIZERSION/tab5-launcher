/* The charger steps on the PC: main/l5_pi4io.h L5_PI4IO_CHARGER_ON, applied as l5_board_charger_on() does it (one
   read-modify-write per step) to a model of the PI4IOE5V6408 at 0x44. Every state of the two charger pins (direction,
   level, high-Z and pull of pins 6 and 7) is tried with the other pins in several patterns: the chip's power-on
   defaults, the BSP driver's reset, a Tab5 in use (Wi-Fi and USB 5 V on), all 0, all 1 and mixed. After every
   write: no other pin moves, CHG_EN is never driven low, CHG_STAT is never driven unless it already was. At the end
   CHG_EN is an output, push-pull, high, and CHG_STAT an input without pull with its buffer on; a second run changes
   nothing. Only these four registers are written (never the reset register 0x01).

   run.py builds it (also --charger). */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "l5_pi4io.h"

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

#define N_STEPS (sizeof(L5_PI4IO_CHARGER_ON) / sizeof(L5_PI4IO_CHARGER_ON[0]))
#define CHARGER_PINS (L5_PI4IO_CHG_EN | L5_PI4IO_CHG_STAT)

typedef struct {
    uint8_t dir, out, highz, pull;
} expander_t;

static uint8_t *reg_of(expander_t *e, uint8_t reg)
{
    switch (reg) {
    case L5_PI4IO_DIR: return &e->dir;
    case L5_PI4IO_OUT: return &e->out;
    case L5_PI4IO_HIGHZ: return &e->highz;
    case L5_PI4IO_PULL_EN: return &e->pull;
    default: return NULL;
    }
}

static bool driven(const expander_t *e, uint8_t pin)
{
    return (e->dir & pin) && !(e->highz & pin);
}

static uint8_t other_pins_moved(const expander_t *a, const expander_t *b)
{
    return (uint8_t)(((a->dir ^ b->dir) | (a->out ^ b->out) | (a->highz ^ b->highz) | (a->pull ^ b->pull)) & ~CHARGER_PINS);
}

/* the steps from start; false if a step could not be applied */
static bool run(expander_t *e, const expander_t *start, int id)
{
    for (unsigned i = 0; i < N_STEPS; i++) {
        const l5_pi4io_step_t *s = &L5_PI4IO_CHARGER_ON[i];
        uint8_t *r = reg_of(e, s->reg);
        EXPECT(r, "state %d: step %u writes register 0x%02x", id, i, s->reg);
        if (!r) return false;
        EXPECT(!(s->mask & ~CHARGER_PINS), "step %u: mask 0x%02x reaches other pins", i, s->mask);
        *r = (uint8_t)((*r & ~s->mask) | (s->value & s->mask)); /* pi4io_update() */
        EXPECT(!other_pins_moved(e, start), "state %d: step %u moved pins 0x%02x", id, i, other_pins_moved(e, start));
        EXPECT(!(driven(e, L5_PI4IO_CHG_EN) && !(e->out & L5_PI4IO_CHG_EN)), "state %d: step %u drives CHG_EN low", id, i);
        EXPECT(!driven(e, L5_PI4IO_CHG_STAT) || driven(start, L5_PI4IO_CHG_STAT),
               "state %d: step %u starts driving CHG_STAT", id, i);
    }
    return true;
}

int main(void)
{
    /* the other pins: power-on defaults, the BSP driver's reset, a Tab5 in use (pins 0 WLAN_PWR_EN and 3 USB 5 V driven
       high, pins 4 and 5 driven low), then patterns */
    static const expander_t OTHERS[] = {
        {0x00, 0x00, 0xff, 0xff}, {0xff, 0x00, 0xff, 0xff}, {0xb9, 0x89, 0x06, 0xf9},
        {0x00, 0x00, 0x00, 0x00}, {0xff, 0xff, 0xff, 0xff}, {0x5a, 0xa5, 0x3c, 0xc3}, {0xa5, 0x5a, 0xc3, 0x3c},
    };
    int states = 0, already_on = 0;
    for (unsigned o = 0; o < sizeof(OTHERS) / sizeof(OTHERS[0]); o++) {
        for (unsigned bits = 0; bits < 256; bits++) { /* dir, out, high-Z, pull of pins 6 and 7 */
            const uint8_t pin_bits[4] = {(uint8_t)((bits & 3) << 6), (uint8_t)(((bits >> 2) & 3) << 6),
                                         (uint8_t)(((bits >> 4) & 3) << 6), (uint8_t)(((bits >> 6) & 3) << 6)};
            const expander_t start = {
                (uint8_t)((OTHERS[o].dir & ~CHARGER_PINS) | pin_bits[0]),
                (uint8_t)((OTHERS[o].out & ~CHARGER_PINS) | pin_bits[1]),
                (uint8_t)((OTHERS[o].highz & ~CHARGER_PINS) | pin_bits[2]),
                (uint8_t)((OTHERS[o].pull & ~CHARGER_PINS) | pin_bits[3]),
            };
            const int id = (int)(o * 256 + bits);
            expander_t e = start;
            if (!run(&e, &start, id)) continue;
            states++;
            EXPECT(driven(&e, L5_PI4IO_CHG_EN) && (e.out & L5_PI4IO_CHG_EN), "state %d: CHG_EN not driven high", id);
            EXPECT(!(e.dir & L5_PI4IO_CHG_STAT), "state %d: CHG_STAT not an input", id);
            EXPECT(!(e.pull & L5_PI4IO_CHG_STAT), "state %d: CHG_STAT still pulled", id);
            EXPECT(!(e.highz & L5_PI4IO_CHG_STAT), "state %d: CHG_STAT input buffer off", id);
            expander_t again = e;
            run(&again, &e, id);
            EXPECT(!memcmp(&again, &e, sizeof(e)), "state %d: a second run changed the expander", id);
            if (driven(&start, L5_PI4IO_CHG_EN) && (start.out & L5_PI4IO_CHG_EN) && !(start.dir & L5_PI4IO_CHG_STAT) &&
                !(start.pull & L5_PI4IO_CHG_STAT) && !(start.highz & L5_PI4IO_CHG_STAT)) {
                already_on++;
                EXPECT(!memcmp(&start, &e, sizeof(e)), "state %d: already on, but the steps changed it", id);
            }
        }
    }
    printf("%d start states (%d already on): other pins kept, CHG_EN never driven low, CHG_STAT never driven\n", states,
           already_on);
    printf(s_fails ? "charger: FAILED %d\n" : "charger: PASSED\n", s_fails);
    return s_fails ? 1 : 0;
}
