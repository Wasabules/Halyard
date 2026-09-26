/* test_kbd_scancode.c - evdev -> "set 1" scancodes.
 *
 * The check that matters is not that the table is right: it is that the keys
 * THAT ALREADY WORK are not touched. A regression there would break the whole
 * keyboard in order to fix four arrow keys.
 */
#include "../core/protocol/kbd_scancode.h"
#include <stdio.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                              \
    checks++;                                                                 \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

static void inchangees(void)
{
    /* COUNTER-CASE - THE COINCIDENCE THAT HID THE BUG.
     * These keys have the SAME number in evdev and in set 1. That is why the
     * guided capture (which contained only letters) validated an evdev encoding
     * while being compatible with set 1. They must go through the table
     * UNCHANGED. */
    static const struct { uint16_t code; const char *name; } MEMES[] = {
        {  1, "Echap" },     { 14, "Retour arriere" }, { 15, "Tab" },
        { 16, "Q/A" },       { 25, "P" },              { 28, "Enter" },
        { 29, "Ctrl gauche"},{ 30, "A/Q" },            { 42, "Maj gauche" },
        { 56, "Alt gauche" },{ 57, "Espace" },
        { 59, "F1" },        { 68, "F10" },
        /* F11 and F12 are not contiguous (87/88) but coincide as well. */
        { 87, "F11" },       { 88, "F12" },
    };
    for (size_t i = 0; i < sizeof MEMES / sizeof MEMES[0]; i++)
        CHECK(kbd_evdev_to_set1(MEMES[i].code) == MEMES[i].code,
                MEMES[i].name);
}

static void etendues(void)
{
    /* The four arrow keys: the reported symptom. */
    CHECK(kbd_evdev_to_set1(103) == 0xE048, "Haut   -> E0 48");
    CHECK(kbd_evdev_to_set1(105) == 0xE04B, "Gauche -> E0 4B");
    CHECK(kbd_evdev_to_set1(106) == 0xE04D, "Droite -> E0 4D");
    CHECK(kbd_evdev_to_set1(108) == 0xE050, "Bas    -> E0 50");

    /* Same family, same cause: they did not work either, without anyone having
     * noticed yet. */
    CHECK(kbd_evdev_to_set1(111) == 0xE053, "Suppr   -> E0 53");
    CHECK(kbd_evdev_to_set1(125) == 0xE05B, "Windows -> E0 5B");

    /* The high byte carries the prefix, the low one the scancode: exactly what
     * the @142/@143 field can transport. */
    CHECK((kbd_evdev_to_set1(103) >> 8) == 0xE0, "prefix in the high byte");
    CHECK((kbd_evdev_to_set1(103) & 0xFF) == 0x48, "scancode in the low byte");
}

static void inconnues(void)
{
    /* A key outside the table must pass through as is rather than be lost: we do
     * not know the whole keyboard, and returning 0 would make the key mute
     * instead of letting it take its chance. */
    CHECK(kbd_evdev_to_set1(200) == 200, "an unknown code is returned unchanged");
    CHECK(kbd_evdev_to_set1(0)   == 0,   "zero is returned unchanged");
    CHECK(kbd_evdev_to_set1(65535) == 65535, "the upper bound is returned unchanged");
}

int main(void)
{
    printf("== keyboard: evdev -> set 1 scancodes ==\n");
    inchangees();
    etendues();
    inconnues();
    /* === K7 2026-08-27 - the guided capture, frozen here ===
     *
     * 22 (scancode, extended) pairs taken from the official client, checked
     * against the PC's set 1: 22 matches out of 22. This block freezes them. If
     * a single line changes, the campaign that made the arrow keys work has just
     * been undone.
     *
     * The central COUNTER-CASE: `0x1C` and `0x47` appear in BOTH forms. A table
     * that did not distinguish the extension would make the keypad Enter
     * indistinguishable from the main Enter, and Home indistinguishable from
     * KP 7 - those two pairs are what prove the information lives somewhere
     * other than in the scancode. */
    {
        struct { uint16_t evdev; uint16_t base; int etendue; const char *name; } K7[] = {
            /* mesurees en forme ETENDUE */
            { 103, 0x48, 1, "Fleche haut"   }, { 108, 0x50, 1, "Fleche bas"    },
            { 105, 0x4B, 1, "Fleche gauche" }, { 106, 0x4D, 1, "Fleche droite" },
            { 102, 0x47, 1, "Debut"         }, { 107, 0x4F, 1, "Fin"           },
            { 104, 0x49, 1, "Page haut"     }, { 109, 0x51, 1, "Page bas"      },
            { 110, 0x52, 1, "Inser"         }, { 111, 0x53, 1, "Suppr"         },
            {  96, 0x1C, 1, "KEYPAD Enter"   }, {  98, 0x35, 1, "PAVE /"        },
            /* mesurees en forme ORDINAIRE */
            {  28, 0x1C, 0, "main Enter" }, {  71, 0x47, 0, "PAVE 7"    },
            {  55, 0x37, 0, "PAVE *"        }, {  74, 0x4A, 0, "PAVE -"        },
            {  78, 0x4E, 0, "PAVE +"        }, {  14, 0x0E, 0, "Retour arriere"},
            {  54, 0x36, 0, "Maj droite"    },
        };
        for (unsigned i = 0; i < sizeof K7 / sizeof K7[0]; i++) {
            const uint16_t v = kbd_evdev_to_set1(K7[i].evdev);
            const int etendue = (v & 0xFF00u) == KBD_SET1_EXTENDED;
            const uint16_t base = v & 0x00FFu;
            char what[160];
            snprintf(what, sizeof what,
                     "K7 %s: base byte 0x%02X expected, extended %d",
                     K7[i].name, K7[i].base, K7[i].etendue);
            CHECK(base == K7[i].base && etendue == K7[i].etendue, what);
        }
    }

    /* The high byte serves ONLY as an internal marker: no key may produce a zero
     * base byte, otherwise the wire would carry scancode 0. */
    for (uint16_t e = 1; e < 128; e++) {
        const uint16_t v = kbd_evdev_to_set1(e);
        CHECK((v & 0x00FFu) != 0,
              "no conversion returns a zero base byte - a scancode 0 "
              "on the wire would be a phantom key");
        CHECK((v & 0xFF00u) == 0 || (v & 0xFF00u) == KBD_SET1_EXTENDED,
              "the high byte is either empty or the marker, never anything else");
    }

    /* === K7b - THE BYTE AND THE FLAG MUST NOT BE ABLE TO DIVERGE ===
     *
     * The K7 block above checks the TABLE. It went green while the arrow keys
     * typed "8", "2", "4", "6" on the VM: the table was right, it was the PATH
     * that was wrong. The template was chosen on the RAW evdev code, before
     * conversion, where the 0xE0 marker never appears - so the extended template
     * was never chosen and the byte went out alone. 0x48 with no flag is KP8,
     * that is, "8".
     *
     * `kbd_wire_byte` returns BOTH values at once; these checks bear on it, hence
     * on what really goes out. */
    {
        struct { uint16_t evdev; uint8_t octet; int etendue; const char *symptome; } K7b[] = {
            { 103, 0x48, 1, "Up arrow without the flag = KP8 = types 8" },
            { 108, 0x50, 1, "Down arrow without the flag = KP2 = types 2" },
            { 105, 0x4B, 1, "Left arrow without the flag = KP4 = types 4" },
            { 106, 0x4D, 1, "Right arrow without the flag = KP6 = types 6" },
            { 111, 0x53, 1, "Delete without the flag = KEYPAD dot" },
            { 110, 0x52, 1, "Insert without the flag = KEYPAD 0" },
            { 102, 0x47, 1, "Home without the flag = KEYPAD 7" },
            { 107, 0x4F, 1, "End without the flag = KEYPAD 1" },
            { 104, 0x49, 1, "Page Up without the flag = KEYPAD 9" },
            { 109, 0x51, 1, "Page Down without the flag = KEYPAD 3" },
            {  96, 0x1C, 1, "keypad Enter without the flag = the main Enter" },
            {  98, 0x35, 1, "keypad slash without the flag = the / key" },
            {  69, 0x45, 1, "Num Lock without the flag = Pause; the lock does not "
                            "toggle and the keypad types arrows" },
            {  71, 0x47, 0, "KEYPAD 7 WITH the flag would become Home" },
            {  28, 0x1C, 0, "the main Enter WITH the flag would become the keypad one" },
            {  16, 0x10, 0, "an ordinary letter is never extended" },
        };
        for (unsigned i = 0; i < sizeof K7b / sizeof K7b[0]; i++) {
            int etendue = -1;
            const uint8_t o = kbd_wire_byte(K7b[i].evdev, &etendue);
            char what[200];
            snprintf(what, sizeof what, "K7b evdev %u -> 0x%02X etendue=%d — %s",
                     K7b[i].evdev, K7b[i].octet, K7b[i].etendue, K7b[i].symptome);
            CHECK(o == K7b[i].octet && etendue == K7b[i].etendue, what);
        }
    }

    /* The wire byte NEVER carries the marker: the marker only serves to carry
     * the information as far as the template choice. */
    for (uint16_t e = 1; e < 128; e++) {
        int etendue = -1;
        const uint8_t o = kbd_wire_byte(e, &etendue);
        CHECK(o != 0, "no evdev code produces a zero wire byte");
        CHECK(etendue == 0 || etendue == 1, "the flag is a boolean");
        /* The check with substance: the two functions must describe the SAME
         * code. If `kbd_evdev_to_set1` gained an entry without `kbd_wire_byte`
         * seeing it, the byte and the flag would again be derivable separately -
         * exactly the K7b defect. */
        const uint16_t v = kbd_evdev_to_set1(e);
        CHECK(o == (uint8_t)(v & 0x00FFu),
                "the wire byte is the low half of the conversion");
        CHECK(etendue == (((v & 0xFF00u) == KBD_SET1_EXTENDED) ? 1 : 0),
                "the flag is the high half of the SAME conversion");
    }

    /* `extended` may be null: a caller that only writes the byte must not have
     * to invent a variable it has no use for. */
    CHECK(kbd_wire_byte(103, NULL) == 0x48,
            "kbd_wire_byte accepts a null pointer for the flag");

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
