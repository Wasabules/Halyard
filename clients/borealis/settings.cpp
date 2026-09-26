#include "settings.hpp"

#include "../../core/services/env_override.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <string>

extern "C" {
#include "../../core/services/config.h"   // SHADOW_DATA_DIR
#include "../../core/services/journal.h"  // S81 - log level
#include "../../core/services/win_compat.h"  // WIN1 - setenv/unsetenv, absent from the Windows CRT
#include "../../core/services/atomic_file.h" // AF4 - written beside, then moved over
}

#include "clients/borealis/ui/sfx.hpp"
#include "clients/borealis/ui/haptics.hpp"
#include "clients/borealis/ui/pointer.hpp"      // S88 — sons de l'interface
#include "device_mode.hpp" // S90 - docked or handheld
#include "../../core/protocol/bitrate.h"      // B1 - the one bitrate ladder and its rule
#include "../../core/protocol/ctrl_session.h"  // B1 - applying the bitrate live

extern "C" {
#include "../../core/media/audio.h"  // S90 — audio_eq_configure (tire streaming/eq.h)

/* Settings belong to the UI, but this one line reports on the DEVELOPMENT
 * CHANNEL, which is a system facility - hence the category. */
#define stlog(...) JOURNAL_INFO_(JOURNAL_CAT_SYSTEM, __VA_ARGS__)
}

static const char *kSettingsPath = SHADOW_DATA_DIR "settings.txt";

Settings& Settings::instance() {
    static Settings s;
    return s;
}

void Settings::load() {
    FILE *f = atomic_file_open_read(kSettingsPath, "r");   // AF4 - see save()
    if (!f) {
        /* No settings file at all is the FIRST launch, and it is one of the two
         * paths that used to leave `dev_channel_known` false - see the block at
         * the end of this function, which must run either way. */
        adoptDevChannelFromDisk();
        return;
    }
    char line[128];
    while (std::fgets(line, sizeof(line), f)) {
        // strip newline
        size_t n = std::strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';

        char *eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;

        if (std::strcmp(key, "vsync_flux") == 0) {
            vsync_stream = (val[0] == '1' || val[0] == 't');
        }
        if (std::strcmp(key, "etirer_image") == 0) {
            stretch_image = (val[0] == '1' || val[0] == 't');
        }
        if (std::strcmp(key, "verrou_au_reveil") == 0) {
            lock_on_wake = (val[0] == '1' || val[0] == 't');
        }
        if (std::strcmp(key, "show_perf_stats") == 0) {
            show_perf_stats = (val[0] == '1' || val[0] == 't');
        } else if (std::strcmp(key, "hud_refresh_ms") == 0) {
            int v = std::atoi(val); if (v >= 100 && v <= 2000) hud_refresh_ms = (uint32_t)v;
        } else if (std::strcmp(key, "ui_scale") == 0) {
            int v = std::atoi(val); if (v >= 50 && v <= 250) ui_scale = (uint32_t)v;
        } else if (std::strcmp(key, "hud_charts") == 0) {
            hud_charts = (uint32_t)std::strtoul(val, nullptr, 10);
        } else if (std::strcmp(key, "language") == 0) {
            language = val;
        } else if (std::strcmp(key, "rumble_intensite") == 0) {
            const int v = std::atoi(val);
            rumble_strength = (v >= 0 && v <= 100) ? (uint32_t)v : 100;
        } else if (std::strcmp(key, "gamepad_type") == 0) {
            const int v = std::atoi(val);
            gamepad_type = (v >= 0 && v <= 5) ? (uint32_t)v : 0;
        } else if (std::strcmp(key, "udp_register_input_v2") == 0) {
            /* === G55 2026-08-28 - THE KEY IS RENAMED TO PURGE THE SD CARDS ===
             * This setting sends a 25-byte datagram IN CLEAR on `:base+13`. The
             * official client NEVER emits one - it only puts encrypted 42-byte
             * packets there - and the server tears the gamepad channel down
             * (`CHANNEL_DOWN canal=CONTROLLER`) when we speak on it within the
             * ~500 ms following the handshake: 5 sessions out of 5. Our plug
             * announcement then went out on an already dead channel.
             *
             * The compiled default has always been `false`, but the SD card's
             * `settings.txt` carries `udp_register_input=1` and is re-read on
             * every startup: changing the default would have fixed nothing.
             * Renaming the key makes the old value inert without touching the
             * file, and the settings screen's toggle remains the way back on
             * console - where `getenv` always returns NULL. */
            udp_register_input = (val[0] == '1' || val[0] == 't');
        } else if (std::strcmp(key, "couleur_444") == 0) {
            color_444 = (std::atoi(val) != 0);
        } else if (std::strcmp(key, "audio_hifi") == 0) {
            audio_hifi = (std::atoi(val) != 0);
        } else if (std::strcmp(key, "codec") == 0) {
            /* === K13 2026-08-27 - A MANDATORY MIGRATION ===
             * The old numbering was a guess: 2 = H.264, 1 = H.265, 3 = AV1,
             * 0 = "automatic". The real wire enum is 0 = H.264, 1 = H.265,
             * 2 = AV1. So 2 changes meaning: a setting already saved as "H.264"
             * would silently become AV1, and the user would see their picture
             * disappear without having touched anything. We convert once, on
             * read. */
            const int v = std::atoi(val);
            switch (v) {
                case 0: codec = 0; break;   /* "auto" = H.264 on the wire */
                case 1: codec = 1; break;   /* H.265: already the right value */
                case 2: codec = 0; break;   /* ancien H.264 -> 0 */
                case 3: codec = 2; break;   /* ancien AV1   -> 2 */
                default: codec = 0; break;
            }
        } else if (std::strncmp(key, "geste_", 6) == 0) {
            /* `geste_<index>=<action>`. Both bounds are checked: a
             * hand-edited file must not be able to write outside the table nor
             * set a nonexistent action. */
            const int gi = std::atoi(key + 6);
            const int gv = std::atoi(val);
            if (gi > 0 && gi < SETTINGS_GESTURES_COUNT
                    && gv >= 0 && gv < (int)GestureAction::Count)
                gesture_action[gi] = (uint8_t)gv;
        } else if (std::strcmp(key, "touch_scroll_invert") == 0) {
            touch_scroll_invert = (std::atoi(val) != 0);
        } else if (std::strcmp(key, "touch_sensitivity") == 0) {
            int v = std::atoi(val); if (v >= 10 && v <= 200) touch_sensitivity = (uint32_t)v;
        } else if (std::strcmp(key, "auto_connect") == 0) {
            auto_connect = (val[0] == '1' || val[0] == 't');
        } else if (std::strcmp(key, "couper_si_hors_focus") == 0) {
            cut_when_unfocused = (val[0] == '1' || val[0] == 't');
        } else if (std::strcmp(key, "reconnexion_auto") == 0) {
            auto_reconnect = (val[0] == '1' || val[0] == 't');
        } else if (std::strcmp(key, "reconnexion_max") == 0) {
            int v = std::atoi(val); if (v >= 0 && v <= 20) reconnect_max = (uint32_t)v;
        } else if (std::strcmp(key, "decodage_materiel") == 0) {
            hw_decode = (val[0] == '1' || val[0] == 't');
        } else if (std::strcmp(key, "opus_materiel") == 0) {
            hw_opus = (val[0] == '1' || val[0] == 't');
        } else if (std::strcmp(key, "eq_profil_dock") == 0) {
            int v = std::atoi(val); if (v >= 0 && v < EQ_PRESET_COUNT) eq_preset_docked = (uint32_t)v;
        } else if (std::strcmp(key, "eq_profil_portable") == 0) {
            int v = std::atoi(val); if (v >= 0 && v < EQ_PRESET_COUNT) eq_preset_handheld = (uint32_t)v;
        } else if (std::strcmp(key, "eq_bandes") == 0) {
            eq_bands = val;
        } else if (std::strcmp(key, "eq_auto_trim") == 0) {
            eq_auto_trim = (val[0] == '1' || val[0] == 't');
        } else if (std::strcmp(key, "sons") == 0) {
            sounds = (val[0] == '1' || val[0] == 't');
        } else if (std::strcmp(key, "sons_volume") == 0) {
            int v = std::atoi(val); if (v >= 0 && v <= 100) sounds_volume = (uint32_t)v;
        } else if (std::strcmp(key, "journal_niveau") == 0) {
            /* Bounded to the values of `journal_severity_t`. We refuse 0 (ERROR
             * only): a log where even the fallbacks are invisible makes
             * diagnosis impossible, and nobody would choose that knowingly from
             * the screen - so a 0 comes from a damaged file. */
            int v = std::atoi(val);
            if (v >= 1 && v <= 4) log_level = (uint32_t)v;
        } else if (std::strcmp(key, "canal_dev") == 0) {
            dev_channel = (val[0] == '1' || val[0] == 't');
            dev_channel_known = true;   /* the setting exists: it can be applied */
        } else if (std::strcmp(key, "audio_volume") == 0) {
            int v = std::atoi(val); if (v >= 0 && v <= 300) audio_volume = (uint32_t)v;
        } else if (std::strcmp(key, "mouse_relative") == 0) {
            mouse_relative = (val[0] == '1' || val[0] == 't');
        } else if (std::strcmp(key, "native_mode") == 0) {
            /* Retired 2026-09-26 with the WebRTC path it chose against. Still
             * RECOGNISED, so a settings.txt written by an older build loads
             * without the key being reported as unknown; its value is moot. */
        } else if (std::strcmp(key, "gamepad_enabled") == 0) {
            gamepad_enabled = (val[0] == '1' || val[0] == 't');
        } else if (std::strcmp(key, "kb_layout") == 0) {
            int v = std::atoi(val);
            kb_layout = (v == 1) ? KbLayout::Qwerty : KbLayout::Azerty;
        } else if (std::strcmp(key, "auto_resolution") == 0) {
            auto_resolution = (val[0] == '1' || val[0] == 't');
        } else if (std::strcmp(key, "rear_touch") == 0) {
            rear_touch = (val[0] == '1' || val[0] == 't');
        } else if (std::strcmp(key, "rear_zone_tl") == 0) {
            int v = std::atoi(val); if (v >= 0 && v <= 6) rear_zone_tl = (uint32_t)v;
        } else if (std::strcmp(key, "rear_zone_tr") == 0) {
            int v = std::atoi(val); if (v >= 0 && v <= 6) rear_zone_tr = (uint32_t)v;
        } else if (std::strcmp(key, "rear_zone_bl") == 0) {
            int v = std::atoi(val); if (v >= 0 && v <= 6) rear_zone_bl = (uint32_t)v;
        } else if (std::strcmp(key, "rear_zone_br") == 0) {
            int v = std::atoi(val); if (v >= 0 && v <= 6) rear_zone_br = (uint32_t)v;
        } else if (std::strcmp(key, "rear_gest_tl") == 0) {
            int v = std::atoi(val); if (v >= 0 && v <= 2) rear_gest_tl = (uint32_t)v;
        } else if (std::strcmp(key, "rear_gest_tr") == 0) {
            int v = std::atoi(val); if (v >= 0 && v <= 2) rear_gest_tr = (uint32_t)v;
        } else if (std::strcmp(key, "rear_gest_bl") == 0) {
            int v = std::atoi(val); if (v >= 0 && v <= 2) rear_gest_bl = (uint32_t)v;
        } else if (std::strcmp(key, "rear_gest_br") == 0) {
            int v = std::atoi(val); if (v >= 0 && v <= 2) rear_gest_br = (uint32_t)v;
        } else if (std::strcmp(key, "rear_two_finger") == 0) {
            int v = std::atoi(val); if (v >= 0 && v <= 6) rear_two_finger = (uint32_t)v;
        } else if (std::strcmp(key, "rear_slide_pct") == 0) {
            int v = std::atoi(val); if (v >= 5 && v <= 90) rear_slide_pct = (uint32_t)v;
        } else if (std::strcmp(key, "pad_mouse_mode") == 0) {
            int v = std::atoi(val); if (v >= 0 && v <= 2) pad_mouse_mode = (uint32_t)v;
        } else if (std::strcmp(key, "pad_mouse_sens") == 0) {
            int v = std::atoi(val);
            pad_mouse_sens = (uint32_t)(v < 10 ? 10 : (v > 400 ? 400 : v));
        } else if (std::strcmp(key, "pad_mouse_invert_y") == 0) {
            pad_mouse_invert_y = (val[0] == '1' || val[0] == 't');
        } else if (std::strcmp(key, "pad_mouse_scroll_step") == 0) {
            int v = std::atoi(val);
            pad_mouse_scroll_step = (uint32_t)(v < 10 ? 10 : (v > 400 ? 400 : v));
        } else if (std::strcmp(key, "pad_mouse_gyro_source") == 0) {
            int v = std::atoi(val); if (v >= 0 && v <= 5) pad_mouse_gyro_source = (uint32_t)v;
        } else if (std::strcmp(key, "video_tcp") == 0) {
            video_tcp = (val[0] == '1' || val[0] == 't');
        } else if (std::strcmp(key, "bitrate_per_link") == 0) {
            bitrate_per_link = (val[0] == '1' || val[0] == 't');
        /* B1 - ONE clamp for the three bitrates, and it SNAPS onto the ladder.
         * These two were bounded 1..150 while the global one was bounded 0..500,
         * and a value between two rungs made every screen disagree about where
         * it was. Out-of-range no longer means "ignored" either: a 150 written
         * by the previous ladder came back as the default, not as the ceiling
         * the user had asked for. */
        } else if (std::strcmp(key, "bitrate_wifi_mbps") == 0) {
            bitrate_wifi_mbps = bitrate_clamp((uint32_t)std::strtoul(val, nullptr, 10));
        } else if (std::strcmp(key, "bitrate_eth_mbps") == 0) {
            bitrate_eth_mbps = bitrate_clamp((uint32_t)std::strtoul(val, nullptr, 10));
        } else if (std::strcmp(key, "pad_map") == 0) {
            pad_map = val;
        } else if (std::strcmp(key, "pad_invert_y") == 0) {
            pad_invert_y = (val[0] == '1' || val[0] == 't');
        } else if (std::strcmp(key, "pad_deadzone") == 0) {
            int v = std::atoi(val); if (v >= 0 && v <= 40) pad_deadzone = (uint32_t)v;
        } else if (std::strcmp(key, "pointeur_demo") == 0) {
            demo_pointer = (val[0] == '1' || val[0] == 't');
        } else if (std::strcmp(key, "curseur_source") == 0) {
            /* S113 - bounded: an out-of-range value in a hand-edited file must
             * not turn the cursor off silently, it falls back on the default. */
            int v = std::atoi(val); if (v >= 0 && v <= 2) cursor_source = (uint32_t)v;
        } else if (std::strcmp(key, "haptique_intensite") == 0) {
            haptics_strength = (uint32_t)std::strtoul(val, nullptr, 10);
            if (haptics_strength > 100) haptics_strength = 100;
        } else if (std::strcmp(key, "hud_opacite") == 0) {
            hud_opacity = (uint32_t)std::strtoul(val, nullptr, 10);
            if (hud_opacity < 10)  hud_opacity = 10;
            if (hud_opacity > 100) hud_opacity = 100;
        } else if (std::strcmp(key, "temps_total_s") == 0) {
            time_total_s = (uint32_t)std::strtoul(val, nullptr, 10);
        } else if (std::strcmp(key, "temps_mois_s") == 0) {
            time_month_s = (uint32_t)std::strtoul(val, nullptr, 10);
        } else if (std::strcmp(key, "temps_jour_s") == 0) {
            time_day_s = (uint32_t)std::strtoul(val, nullptr, 10);
        } else if (std::strcmp(key, "temps_jour") == 0) {
            time_day = (uint32_t)std::strtoul(val, nullptr, 10);
        } else if (std::strcmp(key, "temps_mois") == 0) {
            time_month = (uint32_t)std::strtoul(val, nullptr, 10);
        } else if (std::strcmp(key, "hud_lignes") == 0) {
            hud_rows = val;
        } else if (std::strcmp(key, "hud_positions") == 0) {
            hud_positions = val;
        } else if (std::strcmp(key, "hud_sections") == 0) {
            hud_sections = (uint32_t)std::strtoul(val, nullptr, 10);
        } else if (std::strcmp(key, "touch_mode") == 0) {
            int v = std::atoi(val);
            if (v >= 0 && v <= 2) touch_mode = static_cast<TouchMode>(v);
        }
        /* B1 2026-05-18 - quality params, persisted */
        else if (std::strcmp(key, "max_bitrate_mbps") == 0) {
            max_bitrate_mbps = bitrate_clamp((uint32_t)std::strtoul(val, nullptr, 10));
        } else if (std::strcmp(key, "target_fps") == 0) {
            int v = std::atoi(val); if (v >= 0 && v <= 240) target_fps = (uint32_t)v;
        } else if (std::strcmp(key, "profile_id") == 0) {
            int v = std::atoi(val); if (v >= 0 && v <= 10) profile_id = (uint32_t)v;
        } else if (std::strcmp(key, "display_width") == 0) {
            int v = std::atoi(val); if (v >= 320 && v <= 7680) display_width = (uint32_t)v;
        } else if (std::strcmp(key, "display_height") == 0) {
            int v = std::atoi(val); if (v >= 240 && v <= 4320) display_height = (uint32_t)v;
        }
    }
    std::fclose(f);

    adoptDevChannelFromDisk();
}

/* === CANAL-1 2026-09-12 - THE FILE IS THE STATE, THE SETTING MIRRORS IT ===
 *
 * `logsink.txt` is not the picture of a setting, it IS the channel: present means
 * armed, absent means off. So when the settings file says nothing about it, the
 * answer is not "false", it is "go and look".
 *
 * WHAT THIS REPAIRS, measured on Linux 2026-09-12. `save()` wrote `canal_dev=0`
 * even while the value had never been established; the file dropped by hand (or
 * by `tools/switch-logsink.sh setup`) therefore worked for exactly ONE launch,
 * and the next one read that invented 0 back as a deliberate choice and renamed
 * the destination away. The guard on `dev_channel_known` was written to prevent
 * precisely this and bought one launch, because nothing stopped the key being
 * WRITTEN before it was known. On console it is worse than on desktop: the file
 * arrives over FTP, the channel answers once, then goes mute with no message -
 * and the natural conclusion is that the network channel is broken.
 *
 * The setting still wins once it has been chosen on screen; this only decides
 * what to believe when nobody has chosen anything. */
void Settings::adoptDevChannelFromDisk() {
    if (dev_channel_known) return;          /* a deliberate choice is on file: it wins */

    char path[256];
    std::snprintf(path, sizeof(path), "%slogsink.txt", SHADOW_DATA_DIR);
    FILE *probe = std::fopen(path, "rb");
    if (probe) {
        std::fclose(probe);
        dev_channel = true;
    }
    dev_channel_known = true;
}

void Settings::save() const {
    /* === AF4 2026-09-10 - WRITTEN BESIDE, THEN MOVED OVER ===
     *
     * "w" truncated the file before the first line was written, and this runs
     * every time a setting changes. A cut in between - power button, flat
     * battery - left a short or empty file, which loads as factory settings
     * without a word. See shadow/atomic_file.h. Binary mode: `load()` strips
     * "\r\n" and "\n" alike, so the line-ending change on Windows is invisible
     * to it. */
    char tmp[256];
    FILE *f = atomic_file_open(kSettingsPath, tmp, sizeof tmp);
    if (!f) return;
    std::fprintf(f, "show_perf_stats=%d\n", show_perf_stats ? 1 : 0);
    std::fprintf(f, "vsync_flux=%d\n", vsync_stream ? 1 : 0);
    std::fprintf(f, "etirer_image=%d\n", stretch_image ? 1 : 0);
    std::fprintf(f, "verrou_au_reveil=%d\n", lock_on_wake ? 1 : 0);
    std::fprintf(f, "touch_mode=%d\n", static_cast<int>(touch_mode));
    std::fprintf(f, "hud_sections=%u\n", hud_sections);
    std::fprintf(f, "hud_positions=%s\n", hud_positions.c_str());
    std::fprintf(f, "hud_lignes=%s\n", hud_rows.c_str());
    std::fprintf(f, "temps_total_s=%u\n", time_total_s);
    std::fprintf(f, "temps_mois_s=%u\n",  time_month_s);
    std::fprintf(f, "temps_jour_s=%u\n",  time_day_s);
    std::fprintf(f, "temps_jour=%u\n",    time_day);
    std::fprintf(f, "temps_mois=%u\n",    time_month);
    std::fprintf(f, "hud_opacite=%u\n", hud_opacity);
    std::fprintf(f, "haptique_intensite=%u\n", haptics_strength);
    std::fprintf(f, "pointeur_demo=%d\n", demo_pointer ? 1 : 0);
    std::fprintf(f, "curseur_source=%u\n", cursor_source);
    std::fprintf(f, "ui_scale=%u\n", ui_scale);
    std::fprintf(f, "hud_refresh_ms=%u\n", hud_refresh_ms);
    std::fprintf(f, "hud_charts=%u\n", hud_charts);
    std::fprintf(f, "language=%s\n", language.c_str());
    std::fprintf(f, "rumble_intensite=%u\n", rumble_strength);
    std::fprintf(f, "gamepad_type=%u\n", gamepad_type);
    std::fprintf(f, "udp_register_input_v2=%d\n", udp_register_input ? 1 : 0);
    std::fprintf(f, "touch_sensitivity=%u\n", touch_sensitivity);
    std::fprintf(f, "touch_scroll_invert=%d\n", touch_scroll_invert ? 1 : 0);
    std::fprintf(f, "audio_hifi=%d\n", audio_hifi ? 1 : 0);
    std::fprintf(f, "couleur_444=%d\n", color_444 ? 1 : 0);
    for (int gi = 1; gi < SETTINGS_GESTURES_COUNT; gi++)
        std::fprintf(f, "geste_%d=%u\n", gi, (unsigned)gesture_action[gi]);
    std::fprintf(f, "mouse_relative=%d\n", mouse_relative ? 1 : 0);
    std::fprintf(f, "gamepad_enabled=%d\n", gamepad_enabled ? 1 : 0);
    std::fprintf(f, "kb_layout=%u\n", (unsigned)kb_layout);
    std::fprintf(f, "auto_resolution=%d\n", auto_resolution ? 1 : 0);
    std::fprintf(f, "rear_touch=%d\n", rear_touch ? 1 : 0);
    std::fprintf(f, "rear_zone_tl=%u\n", rear_zone_tl);
    std::fprintf(f, "rear_zone_tr=%u\n", rear_zone_tr);
    std::fprintf(f, "rear_zone_bl=%u\n", rear_zone_bl);
    std::fprintf(f, "rear_zone_br=%u\n", rear_zone_br);
    std::fprintf(f, "rear_gest_tl=%u\n", rear_gest_tl);
    std::fprintf(f, "rear_gest_tr=%u\n", rear_gest_tr);
    std::fprintf(f, "rear_gest_bl=%u\n", rear_gest_bl);
    std::fprintf(f, "rear_gest_br=%u\n", rear_gest_br);
    std::fprintf(f, "rear_two_finger=%u\n", rear_two_finger);
    std::fprintf(f, "rear_slide_pct=%u\n", rear_slide_pct);
    std::fprintf(f, "pad_mouse_mode=%u\n", pad_mouse_mode);
    std::fprintf(f, "pad_mouse_sens=%u\n", pad_mouse_sens);
    std::fprintf(f, "pad_mouse_invert_y=%d\n", pad_mouse_invert_y ? 1 : 0);
    std::fprintf(f, "pad_mouse_scroll_step=%u\n", pad_mouse_scroll_step);
    std::fprintf(f, "pad_mouse_gyro_source=%u\n", pad_mouse_gyro_source);
    std::fprintf(f, "video_tcp=%d\n", video_tcp ? 1 : 0);
    std::fprintf(f, "bitrate_per_link=%d\n", bitrate_per_link ? 1 : 0);
    std::fprintf(f, "bitrate_wifi_mbps=%u\n", bitrate_wifi_mbps);
    std::fprintf(f, "bitrate_eth_mbps=%u\n", bitrate_eth_mbps);
    std::fprintf(f, "pad_map=%s\n", pad_map.c_str());
    std::fprintf(f, "pad_invert_y=%d\n", pad_invert_y ? 1 : 0);
    std::fprintf(f, "audio_volume=%u\n", audio_volume);
    std::fprintf(f, "auto_connect=%d\n", auto_connect ? 1 : 0);
    std::fprintf(f, "couper_si_hors_focus=%d\n", cut_when_unfocused ? 1 : 0);
    std::fprintf(f, "reconnexion_auto=%d\n", auto_reconnect ? 1 : 0);
    std::fprintf(f, "reconnexion_max=%u\n", reconnect_max);
    std::fprintf(f, "decodage_materiel=%d\n", hw_decode ? 1 : 0);
    std::fprintf(f, "opus_materiel=%d\n", hw_opus ? 1 : 0);
    std::fprintf(f, "eq_profil_dock=%u\n", eq_preset_docked);
    std::fprintf(f, "eq_profil_portable=%u\n", eq_preset_handheld);
    std::fprintf(f, "eq_bandes=%s\n", eq_bands.c_str());
    std::fprintf(f, "eq_auto_trim=%d\n", eq_auto_trim ? 1 : 0);
    std::fprintf(f, "sons=%d\n", sounds ? 1 : 0);
    std::fprintf(f, "sons_volume=%u\n", sounds_volume);
    std::fprintf(f, "journal_niveau=%u\n", log_level);
    /* Not written while unknown: an invented 0 reads back as a deliberate choice
     * on the next launch, and disarms a channel nobody turned off (CANAL-1). */
    if (dev_channel_known) std::fprintf(f, "canal_dev=%d\n", dev_channel ? 1 : 0);
    std::fprintf(f, "pad_deadzone=%u\n", pad_deadzone);
    std::fprintf(f, "max_bitrate_mbps=%u\n", max_bitrate_mbps);
    std::fprintf(f, "target_fps=%u\n", target_fps);
    std::fprintf(f, "codec=%u\n", codec);
    std::fprintf(f, "profile_id=%u\n", profile_id);
    std::fprintf(f, "display_width=%u\n", display_width);
    std::fprintf(f, "display_height=%u\n", display_height);
    /* A failed write keeps the previous file: the last good settings rather
     * than half of the new ones. */
    atomic_file_commit(f, tmp, kSettingsPath, !std::ferror(f));
}


/* See the header for the why.
 *
 * `env.txt` keeps priority, but the guard CANNOT be a plain `getenv`: this
 * function is called again whenever a setting changes, and once we have set the
 * variable ourselves, `getenv` would find it and refuse every update. Changing
 * the codec in the Quality screen would then only take effect on the NEXT
 * launch - that is, a setting that lies.
 *
 * So we record ONCE, on the first call, which keys already came from outside:
 * those are the only ones we will never touch. */
/* === S90 - THE PRESET FOLLOWS THE CONSOLE'S MODE ===
 *
 * Reads the custom bands then applies the equaliser. Separated from
 * `applyToggles` because it is also called by `followConsoleMode`, sixty times a
 * second - so it must be short and free of side effects.
 *
 * `LinkType::Unknown` does not exist here: `isDocked()` returns a boolean, and on
 * the desktop it always returns false. The "handheld" preset would therefore be
 * permanently active there, which would be WRONG - a desktop has real speakers.
 * So we only apply it on console. */
/* A flat format: "type,freq,q,gain" per band, separated by ';'. Like the rest of
 * the settings file - it can be typed by hand without getting it wrong, which a
 * JSON does not allow.
 *
 * A malformed entry stops the reading of the FOLLOWING ones but keeps the
 * previous ones: a partial equaliser beats a mute equaliser whose silence nothing
 * explains. */
int Settings::eqReadBands(eq_band_t *out) const
{
    if (!out) return 0;
    for (int i = 0; i < EQ_BANDS; i++)
        out[i] = eq_band_t{ EQ_OFF, 1000.0f, 1.0f, 0.0f };

    int nb = 0;
    const char *c = eq_bands.c_str();
    while (*c && nb < EQ_BANDS) {
        int t = 0; float f = 0, q = 0, g = 0;
        if (std::sscanf(c, "%d,%f,%f,%f", &t, &f, &q, &g) != 4) break;
        if (t >= 0 && t < EQ_TYPE_COUNT) {
            out[nb] = eq_band_t{ (eq_type_t)t, f, q, g };
            nb++;
        }
        const char *pv = std::strchr(c, ';');
        if (!pv) break;
        c = pv + 1;
    }
    return nb;
}

void Settings::eqWriteBands(const eq_band_t *bands)
{
    if (!bands) return;
    std::string s;
    char b[64];
    for (int i = 0; i < EQ_BANDS; i++) {
        std::snprintf(b, sizeof b, "%d,%.0f,%.3f,%.1f", (int)bands[i].type,
                      (double)bands[i].freq, (double)bands[i].q,
                      (double)bands[i].gain_db);
        if (i) s += ';';
        s += b;
    }
    eq_bands = s;
    save();
    /* IMMEDIATE effect: it is a sound setting, it is judged by ear, and waiting
     * for the next session would make it impossible to tune. */
    applyEq();
}

void Settings::applyEq() const
{
    /* EQ-VITA 2026-09-26 - the Vita answers the question too (a handheld is
     * never docked, a PS TV always is: device_mode.cpp), but only the Switch
     * asked it, so a handheld Vita played the DOCKED profile through its own
     * speakers while the equaliser page said "handheld".
     * SHADOW_EQ_MODE_VITA=0 restores the docked profile on the Vita. */
#if defined(__SWITCH__)
    const bool dock = device::isDocked();
#elif defined(__vita__) || defined(__psp2__)
    const char *vita_mode = getenv("SHADOW_EQ_MODE_VITA");
    const bool dock = (vita_mode && atoi(vita_mode) == 0) ? true : device::isDocked();
#else
    const bool dock = true;    /* a desktop has no 3 cm speakers */
#endif
    const uint32_t p = dock ? eq_preset_docked : eq_preset_handheld;

    eq_band_t bands[EQ_BANDS];
    int nb = 0;

    if (p == (uint32_t)EQ_PRESET_CUSTOM) {
        nb = eqReadBands(bands);
        /* Never set: we start from the handheld preset rather than from
         * silence. An empty "Custom" screen would give five flat bands and the
         * impression that the setting does not work. */
        if (nb == 0) nb = eq_preset(EQ_PRESET_HANDHELD, bands);
    } else {
        nb = eq_preset((eq_preset_t)p, bands);
    }

    audio_eq_configure(nb > 0 ? bands : nullptr, nb, eq_auto_trim);
}

/* Called every frame. It does NOTHING until the mode changes: it is a boolean
 * comparison, and that is what makes it safe to call from the render path.
 *
 * Why poll rather than subscribe: `appletGetOperationMode` has no notification
 * usable here, and the `device_mode` module already caches it. Polling it is
 * therefore cheaper than the machinery it would take to avoid polling. */
/* B1 - see the header. The link is read HERE and nowhere else, so the three
 * screens and the connect path cannot disagree about which value is in force. */
uint32_t Settings::effectiveBitrateMbps() const
{
    bitrate_link_t link = BITRATE_LINK_UNKNOWN;
    switch (device::linkType()) {
        case device::LinkType::WiFi:     link = BITRATE_LINK_WIFI;     break;
        case device::LinkType::Ethernet: link = BITRATE_LINK_ETHERNET; break;
        default:                         link = BITRATE_LINK_UNKNOWN;  break;
    }
    return bitrate_effective(bitrate_per_link ? 1 : 0, link,
                             max_bitrate_mbps, bitrate_wifi_mbps, bitrate_eth_mbps);
}

void Settings::applyBitrateLive()
{
    const uint32_t mbps = effectiveBitrateMbps();
    /* Auto travels as the sentinel, not as 0: the session reads 0 as "leave
     * unchanged", which is precisely how choosing Auto used to change nothing
     * while the screen said it had. */
    ctrl_session_set_video_config(mbps ? mbps : SHADOW_VIDEO_CFG_DEFAULT);
}

void Settings::followConsoleMode()
{
#ifdef __SWITCH__
    static int dernier = -1;
    const int actuel = device::isDocked() ? 1 : 0;
    if (actuel == dernier) return;
    dernier = actuel;
    instance().applyEq();
#endif
}

void Settings::addSessionTime(uint32_t seconds)
{
    /* A session shorter than ten seconds is a failed attempt or a round trip
     * through the list: counting it would inflate the reading with noise. */
    if (seconds < 10) return;

    std::time_t brut = std::time(nullptr);
    std::tm t{};
#if defined(_WIN32)
    localtime_s(&t, &brut);
#else
    localtime_r(&brut, &t);
#endif
    const uint32_t day = (uint32_t)((t.tm_year + 1900) * 10000
                                     + (t.tm_mon + 1) * 100 + t.tm_mday);
    const uint32_t month = day / 100;

    /* The period rolls over BEFORE the addition: without that, the first
     * session of a new day would go into the previous day's total. */
    if (day != time_day) { time_day = day; time_day_s = 0; }
    if (month != time_month) { time_month = month; time_month_s = 0; }

    time_day_s  += seconds;
    time_month_s  += seconds;
    time_total_s += seconds;
    save();
}

/* See the header for the why. */
void Settings::resetToDefaults()
{
    /* The counters are put aside BEFORE the wipe rather than restored after it:
     * a field added to the class later joins the reset automatically, and only
     * what is named here escapes it. The opposite order - reset then re-assign -
     * would let a new counter be erased silently. */
    const uint32_t total = time_total_s, month_s = time_month_s, day_s = time_day_s;
    const uint32_t day = time_day, month = time_month;
    /* `dev_channel_known` is not a preference but a reading: it says whether
     * `logsink.txt` has been looked for. Losing it would make the application
     * believe it has never looked, and `applyToggles` returns early in that
     * case - so half the settings would not be applied by the very call that is
     * meant to put everything back in order. */
    const bool channel_known = dev_channel_known;

    *this = Settings();

    time_total_s = total; time_month_s = month_s; time_day_s = day_s;
    time_day = day; time_month = month;
    dev_channel_known = channel_known;

    save();
    applyToggles();
    /* The bitrate does not go through a variable but through the live session:
     * without this the picture would keep the rate that was in force, and the
     * screen would show the default. */
    applyBitrateLive();
}

void Settings::applyToggles() const
{
    /* Which keys came from outside now lives in `env_override`, taken as one
     * snapshot in `main` before any `setenv` of ours.
     *
     * It used to be a pair of function `static`s here, filled lazily on the
     * FIRST call. That worked, but it made the answer depend on when this
     * function happened to run first - the repo's most expensive defect family,
     * session state in a function `static`, and the one CLAUDE.md names first.
     * It also kept the answer private, so the screens could not say which of
     * their rows the session was ignoring. */
    auto forced = [](const char *key) { return env_override_active(key) != 0; };

    auto place = [&forced](const char *key, bool value) {
        if (forced(key)) return;                /* env.txt has priority */
        setenv(key, value ? "1" : "0", 1);
    };
    place("SHADOW_HWACCEL", hw_decode);
    place("SHADOW_HWOPUS",  hw_opus);

    /* === S81 - THE LOG LEVEL, SET EARLY AND EVERY TIME ===
     *
     * Placed HERE, right at the top, and not lower down: this function RETURNS
     * before the end when `dev_channel_known` is false (see the comment on that
     * return). A setting added after it would only be applied on machines that
     * have already written their settings file - that is, nowhere on the first
     * launch, and therefore exactly where one needs to understand what is going
     * on.
     *
     * `journal_set_level` ignores the call when `SHADOW_JOURNAL_NIVEAU` is set:
     * `env.txt`'s priority is held in the module, not here, so it holds for
     * callers that know nothing about that file too. */
    journal_set_level((journal_severity_t)log_level);

    /* S88 - the sounds. Set here like the log level, and for the same reason:
     * this function returns before the end when `dev_channel_known` is false, so
     * everything after it does not apply on the first launch. */
    ui::sfx::setEnabled(sounds);
    ui::sfx::setVolume(sounds_volume);
    /* S105 - set in the same place as the sound: the UI's two non-visual
     * feedbacks follow the same path, so a setting re-read at startup cannot
     * forget one of them. */
    ui::haptics::setStrength((int)haptics_strength);
    ui::pointer::setEnabled(demo_pointer);

    /* S90 - the equaliser follows the console's mode. Set here, hence on every
     * settings save; `followConsoleMode()` handles the rest. */
    applyEq();

    /* === 2026-08-27 - TWO SETTINGS THAT DID NOTHING ===
     *
     * "Codec" and "Profile" are saved, displayed in the Quality screen, and had
     * NO effect: `ctrl_session.c` only reads them from `SHADOW_CODEC` and
     * `SHADOW_PROFILE_ID`, and `ctrl_session_glue_params` does not carry them. On
     * console, where no variable is set, the user was therefore changing a
     * setting with no consequence - the worst case: believing you tried
     * something.
     *
     * We set them here, like the two decoders. The read on the session side tests
     * `atoi(e) > 0`, so the value 0 ("automatic") is correctly ignored and the
     * server keeps what it negotiates. */
    auto set_number = [&forced](const char *key, uint32_t value) {
        if (forced(key)) return;
        if (value == 0) {
            /* 0 = "automatic": we REMOVE the variable instead of setting 0. The
             * read on the session side tests `atoi(e) > 0`, so setting 0 would
             * work - but going back to "automatic" after choosing a value must
             * really erase the choice, not leave behind a variable someone will
             * one day read differently. */
            unsetenv(key);
            return;
        }
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%u", value);
        setenv(key, buf, 1);
    };
    set_number("SHADOW_CODEC",      codec);
    set_number("SHADOW_GAMEPAD_TYPE", gamepad_type);
    set_number("SHADOW_RUMBLE_PCT", rumble_strength);
    /* 1 = Opus, 2 = FLAC - the WIRE values, not a boolean. */
    set_number("SHADOW_AUDIO_CODEC", audio_hifi ? 2u : 1u);
#ifdef __SWITCH__
    /* On console 4:4:4 is refused by the hardware in four independent places;
     * we never ask for it, whatever the saved value. Asking for what we cannot
     * decode would give a black screen and triple the bitrate. */
    set_number("SHADOW_REG_F5", 0u);
#else
    set_number("SHADOW_REG_F5", color_444 ? 1u : 0u);
#endif
    set_number("SHADOW_PROFILE_ID", profile_id);

    /* === 2026-08-27 - "FORWARD THE GAMEPAD" DID NOTHING ON CONSOLE ===
     *
     * `SHADOW_GAMEPAD` only governs the Linux DESKTOP's evdev reader; on Switch
     * that function is a stub. The gamepad was therefore ALWAYS announced,
     * whatever the toggle said - while its own description warns that some games
     * ignore keyboard and mouse as soon as a gamepad exists on the remote machine
     * (G45).
     *
     * What really commands the announcement is `SHADOW_GAMEPAD_PLUG`, read by
     * `ctrl_gamepad_attach`: 0 forbids, 1 forces, absent = automatic. So we only
     * set the REFUSAL; leaving the variable absent preserves the automatic
     * detection, which is the right behaviour when the setting is on. */
    if (!forced("SHADOW_GAMEPAD_PLUG")) {
        if (gamepad_enabled) unsetenv("SHADOW_GAMEPAD_PLUG");
        else                 setenv("SHADOW_GAMEPAD_PLUG", "0", 1);
    }

    /* The development channel is not set through a variable: it exists if and
     * only if `logsink.txt` is present. The toggle WRITES or ERASES that file,
     * which makes it possible to arm it from the console instead of dropping it
     * over FTP from a computer.
     *
     * The address is NOT asked of the user: we have no comfortable keyboard for
     * typing an address, and being one digit off would give a mute channel with
     * no explanation. So we keep the one already there, and confine ourselves to
     * enabling or disabling. Placing the file the first time remains the job of
     * `tools/switch-logsink.sh setup`. */
    char path[256];
    std::snprintf(path, sizeof(path), "%slogsink.txt", SHADOW_DATA_DIR);

    /* === A SETTING MUST NOT DECIDE IN PLACE OF WHAT EXISTS ===
     * `dev_channel` is `false` by default. On the FIRST launch after the update,
     * no settings file contained it: it was therefore false, and this function
     * DISARMED a channel that was working - by renaming its destination file. The
     * diagnostic channel cut itself off, at the exact moment it was useful.
     * So we touch nothing until the setting has been WRITTEN at least once:
     * `dev_channel_known` only becomes true when the file is read or on a
     * deliberate change. */
    if (!dev_channel_known) return;

    if (!dev_channel) {
        char garde[256];
        std::snprintf(garde, sizeof(garde), "%slogsink.off", SHADOW_DATA_DIR);
        /* We RENAME instead of deleting: erasing the destination would force it
         * to be typed again, and that is exactly what we want to avoid.
         *
         * And we SAY it when the file was really there (CANAL-1): the setting is
         * entitled to win over a file dropped by hand, but disarming in silence
         * is what made this cost a session. The line lands in the log file, which
         * outlives the channel it is closing. */
        if (shadow_file_rename(path, garde) == 0)
            stlog("[channel] logsink.txt found but the setting says OFF - channel disarmed "
                  "(Settings > Advanced > Development channel to reopen it)");
    } else {
        char garde[256];
        std::snprintf(garde, sizeof(garde), "%slogsink.off", SHADOW_DATA_DIR);

        /* === CANAL-2 2026-09-12 - NEVER OVERWRITE A LIVE DESTINATION ===
         *
         * `rename` replaces its target on POSIX. So when BOTH files exist, this
         * buried the current address under the disarmed copy - which is the
         * older of the two by construction. Caught on the console: `logsink.txt`
         * held the address `tools/switch-logsink.sh setup` had just written, and
         * `logsink.off` a different one left from an earlier run. The app would
         * have used the leftover.
         *
         * On that day both addresses happened to be live (the same machine, one
         * per interface), so nothing broke - the observed defect is that the
         * DELIBERATE choice is discarded in silence, not that the channel died.
         * It dies the moment the leftover names an interface that is gone, and
         * the suspicion then falls on the network rather than on a rename.
         *
         * A destination that is already there wins: the guard copy is then
         * stale, and we drop it rather than let it come back later. */
        FILE *live = std::fopen(path, "rb");
        if (live) {
            std::fclose(live);
            shadow_file_remove(garde);            /* no-op when there is no guard copy */
        } else {
            shadow_file_rename(garde, path);
        }
    }
}
