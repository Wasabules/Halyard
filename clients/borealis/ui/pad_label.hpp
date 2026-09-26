/* pad_label.hpp - turning the gamepad vocabulary into words on screen.
 *
 * WHY IT IS HERE AND NOT IN `core/input/pad_map`. The vocabulary itself is
 * data: which initialism each family uses for each target ("RB", "L1", "ZR"),
 * and which of them have no initialism and must be translated. That data
 * belongs to the core, which owns the mapping.
 *
 * Translating it does not. `pad_map.cpp` used to call `ui::tr` directly, and it
 * was the LAST thing in `core/` reaching up into the Borealis client -- the one
 * arrow still pointing the wrong way after the 2026-09-12 restructuring. The
 * core now answers "what is this called" with an initialism or with a catalogue
 * key, and this header is what turns either into a string.
 *
 * Header-only: it is two lookups and a `tr`, and it has exactly two call sites.
 */
#pragma once

#include <string>

#include "i18n.hpp"
#include "../../../core/input/pad_map.hpp"
#include "../device_caps.h"

namespace ui {

/* The family's name, for the section title. */
inline std::string padFamilyName(padmap::Family f)
{
    const char *raw = padmap::familyNameRaw(f);
    return (raw && raw[0]) ? std::string(raw) : tr("pad/family_generic");
}

/* === THE CONSOLE'S OWN BUTTONS, NAMED AS THIS CONSOLE MARKS THEM ========
 *
 * The mapping screen's left column is the SOURCE button - the one under the
 * player's thumb - and it took its name from the catalogue, which spells the
 * Switch: "A", "B", "ZL", "-". On a PS Vita every one of those is wrong, and
 * wrong in the way that matters most on a mapping screen: the reader has to
 * translate the label into the button before they can decide anything.
 * Reported from hardware 2026-09-13.
 *
 * The symbols are spelled out rather than drawn as glyphs, and paired with the
 * word: a lone U+25B3 relies on the theme's font covering it, and the one
 * screen whose job is to name buttons is the worst place to risk a tofu box.
 *
 * The INDICES follow Borealis, not physical position: its PSV input binds A to
 * CROSS, B to CIRCLE, X to SQUARE, Y to TRIANGLE, and the whole application
 * reads that. Naming them by position here would make this screen disagree
 * with the tester about the same finger. */
inline std::string padSourceLabel(padmap::Btn b)
{
#if SHADOW_PAD_ART == SHADOW_PAD_ART_PLAYSTATION
    switch (b) {
        /* U+00D7, not U+2715. The multiplication sign is a real cross and
         * Inter carries it; U+2715 MULTIPLICATION X is absent from the font
         * and rendered as a tofu box on the console - reported from the
         * hardware 2026-09-13. Checked against the font's own cmap: the
         * other three shapes are present, this one alone was not. */
        case padmap::Btn::A:      return "\u00D7  " + tr("pad/ps_cross");
        case padmap::Btn::B:      return "\u25CB  " + tr("pad/ps_circle");
        case padmap::Btn::X:      return "\u25A1  " + tr("pad/ps_square");
        case padmap::Btn::Y:      return "\u25B3  " + tr("pad/ps_triangle");
        case padmap::Btn::L:      return "L";
        case padmap::Btn::R:      return "R";
        case padmap::Btn::Minus:  return "SELECT";
        case padmap::Btn::Plus:   return "START";
        case padmap::Btn::LStick: return "L3";
        case padmap::Btn::RStick: return "R3";
        default: break;          /* the d-pad keeps the catalogue's wording */
    }
#endif
    return tr(padmap::key(b));
}

/* Does this console HAVE that button? ZL/ZR are a Switch pair: a handheld Vita
 * has one shoulder pair and `core/input/pad_sce.h` reads none, so offering to
 * map them would be a row that can never fire - the same defect the pad tester
 * was corrected for this morning, where four controls sat permanently unlit.
 * A machine that gains them gains the rows by reading them. */
inline bool padSourceExists(padmap::Btn b)
{
#if SHADOW_PAD_ART == SHADOW_PAD_ART_PLAYSTATION
    if (b == padmap::Btn::ZL || b == padmap::Btn::ZR) return false;
#else
    (void)b;
#endif
    return true;
}

/* A target's displayable name in a given family's vocabulary. */
inline std::string padTargetLabel(int target, padmap::Family f)
{
    /* The PlayStation face buttons have names, and names are translated -
     * targets 0..3 are left, top, bottom, right in the protocol's order. */
    static const char *const PS_FACE[4] = {
        "pad/ps_square", "pad/ps_triangle", "pad/ps_cross", "pad/ps_circle" };
    if (f == padmap::Family::Playstation && target >= 0 && target < 4)
        return tr(PS_FACE[target]);
    const char *sigle = padmap::targetSigle(target, f);
    if (sigle && sigle[0]) return sigle;
    return tr(padmap::targetGenericKey(target));
}

/* Short version: uses the family currently announced to the VM. */
inline std::string padTargetLabel(int target)
{
    return padTargetLabel(target, padmap::currentFamily());
}

}  // namespace ui
