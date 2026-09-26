/* env_note - marks a setting the session does not obey.
 *
 * `env.txt` has priority over every saved setting (see shadow/env_override.h).
 * That priority is right and it stays; what was missing is that the screen said
 * nothing about it, so a row could show "off" while the session ran "on".
 *
 * Usage: wrap the DESCRIPTION, not the label - the label is what you look for in
 * a list, and appending to it would make the same setting hard to find depending
 * on a file's contents:
 *
 *     v.push_back(toggle(SET_HW_VIDEO, ui::tr("settings/hw_video"),
 *                        ui::envNote("SHADOW_HWACCEL", ui::tr("settings/hw_video_desc")),
 *                        s.hw_decode));
 *
 * A setting with no variable behind it stays as it is: passing an unknown key
 * changes nothing, so the wrap is safe to apply and cheap to leave in place. */
#pragma once

#include <string>

#include "i18n.hpp"
#include "../../../core/services/env_override.h"

namespace ui {

/* Returns `desc` unchanged, or `desc` followed by the warning line. */
inline std::string envNote(const char *key, std::string desc)
{
    if (!env_override_active(key)) return desc;
    if (!desc.empty()) desc += "  ";
    /* The variable's NAME is part of the message: knowing that a value is forced
     * is not enough to act on it - you have to know which line of the file to
     * delete, and that file is edited from a computer, away from this screen. */
    return desc + tr("settings/forced_env", key);
}

}  // namespace ui
