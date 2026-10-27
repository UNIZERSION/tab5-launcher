/* HID key events of the M5Stack Tab5 Keyboard as launcher keys (l5_kbd.c reads them; the PC tests call this too,
   tools/host_test/ui/shots.c): US layout, F1..F12, Alt + a c e l n o s x z = the Polish letters as on a computer's
   Polish (programmer's) layout, with Shift their capitals. */
#include <stddef.h>
#include "l5_kbd.h"

static const char NUM[] = "1234567890", NUM_S[] = "!@#$%^&*()";
static const char SYM[] = "-=[]\\#;'`,./", SYM_S[] = "_+{}|~:\"~<>?"; /* usage IDs 0x2D..0x38 */
/* Alt (right Alt = AltGr, or the left one) with these letters types the Polish ones */
static const struct {
    char key;
    uint16_t lower, upper;
} POLISH[] = {
    {'a', 0x105, 0x104}, {'c', 0x107, 0x106}, {'e', 0x119, 0x118}, {'l', 0x142, 0x141}, {'n', 0x144, 0x143},
    {'o', 0x0F3, 0x0D3}, {'s', 0x15B, 0x15A}, {'x', 0x17A, 0x179}, {'z', 0x17C, 0x17B},
};

bool l5_kbd_map(uint8_t mod, uint8_t usage, l5_key_t *out)
{
    l5_key_t k = {.ctrl = (mod & 0x11) != 0, .shift = (mod & 0x22) != 0, .alt = (mod & 0x44) != 0};
    if (usage >= 0x04 && usage <= 0x1D) {
        k.kind = L5_KEY_CHAR;
        k.ch = (char)((k.shift ? 'A' : 'a') + usage - 0x04);
        for (size_t i = 0; k.alt && i < sizeof(POLISH) / sizeof(POLISH[0]); i++) {
            if (POLISH[i].key != 'a' + usage - 0x04) continue;
            k.uc = k.shift ? POLISH[i].upper : POLISH[i].lower;
            k.ch = 0;
            k.alt = k.ctrl = false; /* a character like any other: it types and repeats */
        }
    } else if (usage >= 0x1E && usage <= 0x27) {
        k.kind = L5_KEY_CHAR;
        k.ch = (k.shift ? NUM_S : NUM)[usage - 0x1E];
    } else if (usage == 0x2C) {
        k.kind = L5_KEY_CHAR;
        k.ch = ' ';
    } else if (usage >= 0x2D && usage <= 0x38) {
        k.kind = L5_KEY_CHAR;
        k.ch = (k.shift ? SYM_S : SYM)[usage - 0x2D];
    } else if (usage >= 0x3A && usage <= 0x45) {
        k.kind = (uint8_t)(L5_KEY_F1 + usage - 0x3A);
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
        default: return false; /* Caps Lock, Insert...: nothing in the launcher */
        }
    }
    *out = k;
    return true;
}

bool l5_kbd_repeats(const l5_key_t *k)
{
    /* characters, Backspace, Delete, arrows, Home/End, PgUp/PgDn; not Enter, Esc, Tab, the F-keys, shortcuts */
    return !k->ctrl && !k->alt &&
           (k->kind == L5_KEY_CHAR || k->kind == L5_KEY_BACKSPACE || k->kind == L5_KEY_DELETE ||
            (k->kind > L5_KEY_TAB && k->kind <= L5_KEY_PGDN));
}
