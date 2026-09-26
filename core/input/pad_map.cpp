/* padmap - see pad_map.hpp. */
#include "pad_map.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "settings.hpp"

extern "C" {
#include "../protocol/ctrl_gamepad.h"
}

namespace padmap {

namespace {

struct Entry { const char *key; int deflt; };

/* Order = display order. The default values follow the buttons' physical
 * position, not their letter (see the header). */
const Entry TABLE[(size_t)Btn::COUNT] = {
    { "pad/btn_a",      SHADOW_PAD_CIRCLE   },
    { "pad/btn_b",      SHADOW_PAD_CROSS    },
    { "pad/btn_x",      SHADOW_PAD_TRIANGLE },
    { "pad/btn_y",      SHADOW_PAD_SQUARE    },
    { "pad/btn_l",      SHADOW_PAD_L1       },
    { "pad/btn_r",      SHADOW_PAD_R1       },
    /* ZL/ZR are all-or-nothing on Switch whereas L2/R2 are analog on Shadow's
     * side: we send them fully pressed or at zero. */
    { "pad/btn_zl",     TARGET_L2           },
    { "pad/btn_zr",     TARGET_R2           },
    { "pad/btn_minus",  SHADOW_PAD_SELECT   },
    { "pad/btn_plus",   SHADOW_PAD_START    },
    { "pad/btn_lstick", SHADOW_PAD_L3       },
    { "pad/btn_rstick", SHADOW_PAD_R3       },
    { "pad/btn_up",     TARGET_DPAD         },
    { "pad/btn_down",   TARGET_DPAD         },
    { "pad/btn_left",   TARGET_DPAD         },
    { "pad/btn_right",  TARGET_DPAD         },
};

/* === S83 — LES QUATRE VOCABULAIRES ===
 *
 * One ROW per family, one COLUMN per protocol identifier. The column order is
 * that of `SHADOW_PAD_*` (ctrl_gamepad.h), which is an order
 * PlayStation : carre, triangle, croix, cercle, R3, L3, R1, L1, Start, Select,
 * Guide, then L2, R2 and the d-pad.
 *
 * The mapping is by PHYSICAL POSITION, never by letter. The first identifier is
 * the LEFT button of the diamond: that is X on an Xbox, Square on a PlayStation,
 * **Y** on a Nintendo. Aligning the letters (A onto A) would give buttons that
 * read crossed over on screen compared with what the finger expects - that is
 * already the rule governing the default table above, and it is the same here.
 *
 * An EMPTY entry means "this name is localised": the function then looks it up in
 * the catalogue. There are few of them, and they are the only ones that are not
 * initialisms. */
struct Vocabulary {
    const char *name;                   /* the family's name, for the title */
    const char *target[TARGET_COUNT];
};

const Vocabulary VOCAB[] = {
    /* Xbox - "Menu" and "View" are the official names since the Xbox One; the
     * 360 said "Start" and "Back". We keep the current names, they are the ones
     * today's games display. */
    { "Xbox",
      { "X", "Y", "A", "B", "RS", "LS", "RB", "LB",
        "Menu", "", "Guide", "LT", "RT", "" } },
    { "PlayStation",
      /* The four face buttons are localised (pad/ps_*): they were French
       * words in every language. */
      { "", "", "", "", "R3", "L3", "R1", "L1",
        "Options", "Share", "PS", "L2", "R2", "" } },
    { "Nintendo",
      { "Y", "X", "B", "A", "", "", "R", "L",
        "+", "-", "HOME", "ZL", "ZR", "" } },
    { "",   /* Generic: we name the POSITIONS, not letters */
      { "", "", "", "", "", "", "", "",
        "", "", "", "", "", "" } },
};

/* Catalogue keys, used wherever `VOCAB` leaves an entry empty. The Generic
 * family uses them ALL: it has no initialisms to offer, only the positions it
 * describes. */
const char *GENERIC_KEYS[TARGET_COUNT] = {
    "pad/pos_left", "pad/pos_top", "pad/pos_bottom", "pad/pos_right",
    "pad/pos_stick_r", "pad/pos_stick_l", "pad/pos_shoulder_r", "pad/pos_shoulder_l",
    "pad/pos_menu", "pad/pos_view", "pad/pos_guide",
    "pad/pos_trigger_l", "pad/pos_trigger_r", "pad/sh_dpad",
};

int  g_target[(size_t)Btn::COUNT];
bool g_loaded = false;

}  // namespace

size_t count() { return (size_t)Btn::COUNT; }

const char *key(Btn b)
{
    size_t i = (size_t)b;
    return i < count() ? TABLE[i].key : "";
}

void resetDefaults()
{
    for (size_t i = 0; i < count(); i++) g_target[i] = TABLE[i].deflt;
#if defined(__vita__) || defined(__psp2__)
    /* === PADV-1 2026-09-26 - ON THE VITA, "A" IS THE BOTTOM BUTTON ===
     *
     * The table above is the Switch's: its A sits on the RIGHT of the diamond,
     * so A goes to the right-hand target. The Vita's shim reads CROSS as A
     * (Borealis' convention, which makes Cross confirm in the menus), and Cross
     * is the BOTTOM button - so by default Cross was sent as Circle, Circle as
     * Cross, Square as Triangle and Triangle as Square, in every game. By
     * position on this console the mapping is the identity.
     *
     * Only the defaults change: a mapping the user saved (settings.txt
     * `pad_map`) is read on top and kept. SHADOW_VITA_PAD_BY_POSITION=0 restores
     * the Switch table. */
    const char *e = std::getenv("SHADOW_VITA_PAD_BY_POSITION");
    if (!e || std::atoi(e) != 0) {
        g_target[(size_t)Btn::A] = SHADOW_PAD_CROSS;
        g_target[(size_t)Btn::B] = SHADOW_PAD_CIRCLE;
        g_target[(size_t)Btn::X] = SHADOW_PAD_SQUARE;
        g_target[(size_t)Btn::Y] = SHADOW_PAD_TRIANGLE;
    }
#endif
    g_loaded = true;
}

int target(Btn b)
{
    if (!g_loaded) load();
    size_t i = (size_t)b;
    return i < count() ? g_target[i] : TARGET_NONE;
}

void setTarget(Btn b, int t)
{
    if (!g_loaded) load();
    size_t i = (size_t)b;
    if (i >= count()) return;
    if (t < TARGET_NONE || t >= TARGET_COUNT) return;
    g_target[i] = t;
}

void cycleTarget(Btn b, int dir)
{
    /* The list wraps from "none" (-1) to the last target, in both directions: any
     * value can therefore be reached without going all the way round. */
    const int span = TARGET_COUNT + 1;              /* +1 for "none" */
    int cur = target(b) + 1;                        /* 0 = aucune */
    cur = (cur + (dir >= 0 ? 1 : span - 1)) % span;
    setTarget(b, cur - 1);
}

Family currentFamily()
{
    /* Enum du FIL (G56) : Xbox360=0, XboxOne=1, Dualshock4=2, ControllerLeft=3,
     * ControllerRight=4, Default=5. An out-of-range value returns Xbox - the
     * setting's default - rather than Generic: showing "Bottom / Right" for a
     * corrupted value would be less useful than showing the default. */
    switch (Settings::instance().gamepad_type) {
        case 0: case 1: return Family::Xbox;
        case 2:         return Family::Playstation;
        case 3: case 4: return Family::Nintendo;
        case 5:         return Family::Generic;
        default:        return Family::Xbox;
    }
}

const char *familyNameRaw(Family f)
{
    const int i = (int)f;
    if (i < 0 || i >= (int)(sizeof VOCAB / sizeof VOCAB[0])) return "";
    return VOCAB[i].name;      /* "" = the caller must localise */
}

const char *targetSigle(int t, Family f)
{
    if (t < 0 || t >= TARGET_COUNT) return "";

    int i = (int)f;
    if (i < 0 || i >= (int)(sizeof VOCAB / sizeof VOCAB[0])) i = 0;

    /* An empty entry = this name is localised. That covers every name that is
     * not an initialism, and the whole Generic family. */
    const char *sigle = VOCAB[i].target[t];
    return (sigle && sigle[0]) ? sigle : "";
}

const char *targetGenericKey(int t)
{
    if (t < 0 || t >= TARGET_COUNT) return "pad/unmapped";
    return GENERIC_KEYS[t];
}

void load()
{
    resetDefaults();
    g_loaded = true;

    const std::string &s = Settings::instance().pad_map;
    if (s.empty()) return;

    /* Format: as many comma-separated integers as there are buttons. A shorter or
     * longer table comes from a different version: we then keep the default
     * values rather than apply a
     * correspondance a moitie decalee. */
    size_t n = 0;
    int tmp[(size_t)Btn::COUNT];
    const char *p = s.c_str();
    while (*p && n < count()) {
        char *end = nullptr;
        long v = strtol(p, &end, 10);
        if (end == p) break;
        tmp[n++] = (int)v;
        p = (*end == ',') ? end + 1 : end;
    }
    if (n != count() || *p) return;
    for (size_t i = 0; i < n; i++)
        if (tmp[i] >= TARGET_NONE && tmp[i] < TARGET_COUNT) g_target[i] = tmp[i];
}

void save()
{
    if (!g_loaded) load();
    std::string s;
    char buf[16];
    for (size_t i = 0; i < count(); i++) {
        snprintf(buf, sizeof(buf), "%d", g_target[i]);
        if (i) s += ',';
        s += buf;
    }
    Settings::instance().pad_map = s;
    Settings::instance().save();
}

bool     invertY()               { return Settings::instance().pad_invert_y; }
void     setInvertY(bool v)      { Settings::instance().pad_invert_y = v;
                                   Settings::instance().save(); }
uint32_t deadzone()              { return Settings::instance().pad_deadzone; }
void     setDeadzone(uint32_t v) { Settings::instance().pad_deadzone = (v > 40) ? 40 : v;
                                   Settings::instance().save(); }

}  // namespace padmap
