/* rear_label.hpp - ONE table naming the rear panel's actions, zones and
 * gestures, read by everything that shows them.
 *
 * === WHY A SHARED TABLE AND NOT TWO PRIVATE ONES ========================
 *
 * Three screens name the same values: the settings rows that choose them, the
 * tester that shows what each zone does, and the tester's live legend. B1 and
 * B3 are this repository's own record of what happens otherwise - three private
 * bitrate ladders that disagreed, and each screen then misread the choice the
 * others had recorded. A user who reads "ZL" in the settings and "L2" in the
 * tester has no way to know they are the same row.
 *
 * The order of `ACTIONS` and `GESTURES` is the enum's order in
 * `core/input/rear_touch.h`, so the index IS the stored value - which is also
 * what `settings.txt` holds. Adding an action means appending, never inserting.
 */
#pragma once

#include "i18n.hpp"

#include <string>
#include <vector>

extern "C" {
#include "../../../core/input/rear_touch.h"
}

namespace ui {

/* Indexed by `rear_action`. `REAR_ACT_COUNT` entries, in enum order. */
inline std::vector<std::string> rearActionLabels()
{
    return {
        tr("pad/rear_act_none"),
        tr("pad/rear_act_zl"),
        tr("pad/rear_act_zr"),
        tr("pad/rear_act_l3"),
        tr("pad/rear_act_r3"),
        tr("pad/rear_act_zl_analog"),
        tr("pad/rear_act_zr_analog"),
    };
}

/* Indexed by `rear_gesture`. */
inline std::vector<std::string> rearGestureLabels()
{
    return {
        tr("pad/rear_gest_tap"),
        tr("pad/rear_gest_hold"),
        tr("pad/rear_gest_slide"),
    };
}

/* A short form for the tester, where a zone is a rectangle and not a row: it
 * must fit inside the drawn quarter. */
inline std::string rearActionShort(uint32_t a)
{
    switch (a) {
    case REAR_ACT_ZL:        return "ZL";
    case REAR_ACT_ZR:        return "ZR";
    case REAR_ACT_L3:        return "L3";
    case REAR_ACT_R3:        return "R3";
    case REAR_ACT_ZL_ANALOG: return "L2";
    case REAR_ACT_ZR_ANALOG: return "R2";
    default:                 return tr("pad/rear_act_none");
    }
}

inline std::string rearZoneLabel(int z)
{
    switch (z) {
    case REAR_ZONE_TL: return tr("pad/rear_zone_tl");
    case REAR_ZONE_TR: return tr("pad/rear_zone_tr");
    case REAR_ZONE_BL: return tr("pad/rear_zone_bl");
    default:           return tr("pad/rear_zone_br");
    }
}

}  // namespace ui
