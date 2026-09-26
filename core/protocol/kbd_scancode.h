/* kbd_scancode.h - evdev -> PC keyboard scancode (set 1), as a PURE function.
 *
 * === K4 2026-08-27 - WHY THE ARROW KEYS DID NOTHING ===
 *
 * Our virtual keyboard was sending **Linux evdev** codes. The guided capture
 * that validated the encoding (K1, 52 events, KB §3.20) contained only
 * LETTERS - and for letters, as for almost the whole main keyboard block, the
 * evdev code and the PC "set 1" scancode are the SAME number:
 *
 *     Esc 1, Backspace 14, Tab 15, Q (A on AZERTY) 16, Enter 28, Ctrl 29,
 *     Shift 42, Alt 56, Space 57, F1..F10 59..68, F11 87, F12 88
 *
 * That overlap is not a coincidence - evdev reused the AT keyboard numbering -
 * but it made the capture INCAPABLE of separating the two hypotheses: "the
 * server wants evdev" and "the server wants set 1" predict exactly the same
 * bytes for a letter.
 *
 * The two numberings DIVERGE on the so-called EXTENDED keys, the ones an
 * original AT keyboard did not have. Those are exactly the keys that did not
 * work:
 *
 *     key         evdev     set 1
 *     Up           103      E0 48
 *     Left         105      E0 4B
 *     Right        106      E0 4D
 *     Down         108      E0 50
 *     Delete       111      E0 53
 *     Windows      125      E0 5B
 *
 * The correspondence is exact: everything that works is where the two
 * numberings agree, everything that fails is where they differ. That is what
 * makes this table an explanation rather than a guess.
 *
 * SETTLED 2026-08-27 (K7) - the guided capture happened, and it REFUTED the
 * literal reading that used to stand here. There is NO 0xE0 prefix on the
 * wire: the high byte of the scancode field is 0 in 100% of the messages, both
 * in the live capture and across the 22,000+ messages of the repo's 29
 * captures. The official client sends the BASE scancode in one byte, plus a
 * separate BOOLEAN in the message (an optional FlatBuffers field, omitted when
 * false).
 *
 * The proof fits in two lines: the same byte shows up in two forms. 0x1C plain
 * = main Enter, 0x1C extended = KEYPAD Enter; 0x47 plain = keypad 7, 0x47
 * extended = Home. Without that boolean those keys would be indistinguishable.
 *
 * `KBD_SET1_EXTENDED` therefore stays useful but changes STATUS: it is no longer
 * a wire prefix, it is an INTERNAL MARKER that carries the extended bit inside
 * a single u16 through the event queue. It is stripped by
 * `ctrl_input_tcp_send_scancode`, which picks the extended message template
 * and sends only the base byte. See the K7 block above KBD_TEMPLATE in
 * ctrl_input_tcp.c for the exact FlatBuffers layout.
 *
 * Pure - no state, no I/O - hence checkable by tests/test_kbd_scancode.c.
 */
#ifndef KBD_SCANCODE_H
#define KBD_SCANCODE_H

#include <stdint.h>

/* INTERNAL MARKER for extended keys - NOT a wire byte.
 * It rides in the high byte of the u16 as far as ctrl_input_tcp.c, which
 * strips it and instead picks the message template carrying the "extended"
 * field. Writing it to the wire as-is sent a scancode that does not exist
 * (0xE048 instead of 0x48): that was the cause of the dead arrow keys. See K7
 * above. */
#define KBD_SET1_EXTENDED 0xE000u

/* Returns the "set 1" scancode matching the evdev code `evdev`.
 *
 * For anything that is not an extended key, returns `evdev` UNCHANGED: the two
 * numberings agree there, and remapping would only add risk on keys that
 * already work. */
static inline uint16_t kbd_evdev_to_set1(uint16_t evdev)
{
    switch (evdev) {
        /* Arrow keys */
        case 103: return KBD_SET1_EXTENDED | 0x48u;  /* Haut   */
        case 105: return KBD_SET1_EXTENDED | 0x4Bu;  /* Gauche */
        case 106: return KBD_SET1_EXTENDED | 0x4Du;  /* Droite */
        case 108: return KBD_SET1_EXTENDED | 0x50u;  /* Bas    */

        /* Editing block */
        case 110: return KBD_SET1_EXTENDED | 0x52u;  /* Inser  */
        case 111: return KBD_SET1_EXTENDED | 0x53u;  /* Suppr  */
        case 102: return KBD_SET1_EXTENDED | 0x47u;  /* Debut  */
        case 107: return KBD_SET1_EXTENDED | 0x4Fu;  /* Fin    */
        case 104: return KBD_SET1_EXTENDED | 0x49u;  /* Page haut */
        case 109: return KBD_SET1_EXTENDED | 0x51u;  /* Page bas  */

        /* Right-hand modifiers and Windows keys */
        case  97: return KBD_SET1_EXTENDED | 0x1Du;  /* Ctrl droit */
        case 100: return KBD_SET1_EXTENDED | 0x38u;  /* Alt droit (AltGr) */
        case 125: return KBD_SET1_EXTENDED | 0x5Bu;  /* Windows gauche */
        case 126: return KBD_SET1_EXTENDED | 0x5Cu;  /* Windows droit  */
        case 127: return KBD_SET1_EXTENDED | 0x5Du;  /* Menu contextuel */

        /* Keypad: Enter and divide are extended, the rest are not. */
        /* Num Lock - MEASURED AS EXTENDED by the K7 capture, even though its
         * scancode 0x45 carries no E0 prefix at all. That is a Windows oddity
         * (bit 24 of lParam), and it is exactly what proves the protocol field
         * means "the extended flag" and not "an E0 was on the wire".
         * Without the flag, 0x45 is the Pause form: the lock never toggles, and
         * the nine keypad digits then type Home, the arrows, PgUp and Delete. */
        case  69: return KBD_SET1_EXTENDED | 0x45u;  /* Verr. num      */
        case  96: return KBD_SET1_EXTENDED | 0x1Cu;  /* Entree du pave */
        case  98: return KBD_SET1_EXTENDED | 0x35u;  /* / du pave      */

        default:  return evdev;
    }
}


/* === K7b 2026-08-27 - THE BYTE AND THE FLAG COME FROM HERE, TOGETHER ===
 *
 * Returns what ACTUALLY goes on the wire: the scancode byte, and via
 * `*etendue` the extended flag.
 *
 * This is the ONLY function callers should reach for, and the reason is a bug
 * we paid for: picking the template and writing the byte happened at TWO
 * different places in the same file, one before the evdev -> set 1 conversion
 * and one after. The first therefore tested for `0xE0` on an evdev code, where
 * that marker never appears: the extended template was never chosen. Observed
 * on the VM: the arrow keys typed "8", "2", "4", "6" - that is KP8/KP2/KP4/KP6,
 * the right byte without its flag.
 *
 * Deriving both values from a single call makes that divergence IMPOSSIBLE.
 */
static inline uint8_t kbd_wire_byte(uint16_t evdev, int *etendue)
{
    const uint16_t v = kbd_evdev_to_set1(evdev);
    if (etendue) *etendue = ((v & 0xFF00u) == KBD_SET1_EXTENDED) ? 1 : 0;
    return (uint8_t)(v & 0x00FFu);
}

#endif /* KBD_SCANCODE_H */
