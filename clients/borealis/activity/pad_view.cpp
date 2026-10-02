/* PadView - see the header for why the tester exists. */

#include "pad_view.hpp"

#include "../device_caps.h"

#include "settings.hpp"

extern "C" {
#include "../../../core/protocol/rumble_hid.h"
#include "../../../core/services/log.h"
/* S81 - this module's log category. See shadow/journal.h: it is declared here,
 * never inferred from the text of the messages. */
#define pvlog(...) JOURNAL_INFO_(JOURNAL_CAT_GAMEPAD, __VA_ARGS__)
#define pvdbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_GAMEPAD, __VA_ARGS__)

}

#include "../ui/pad_test.hpp"
#include "../ui/mouse_test.hpp"   /* PM5 */

#include "../../../core/input/pad_map.hpp"
#include "../ui/i18n.hpp"
#include "../ui/pad_label.hpp"
#include "../ui/rear_label.hpp"
#include "../ui/paint.hpp"
#include "../ui/theme.hpp"
#include "../ui/type.hpp"

#include <chrono>

#ifdef __SWITCH__
#include <switch.h>
#endif

extern "C" {
#include "../../../core/protocol/ctrl_gamepad.h"
}

namespace {

/* === B3 2026-09-02 - THE INTENSITY LADDER, IN TEN-PERCENT STEPS ===
 *
 * It was {0, 25, 50, 75, 100}: four usable steps for a setting that is judged by
 * FEEL, on a linear actuator whose perceived strength is anything but linear.
 * Between 25 % and 50 % there was nowhere to stand. Ten-percent steps give
 * eleven rungs, which is what calibrating by hand needs.
 *
 * ONE table, read by the three places that need it (building the row, left,
 * right) - the same lesson as B1 on the bitrate, where three private ladders
 * disagreed and every screen misread the others' choice. */
const uint32_t RUMBLE_LEVELS[] = { 0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };
const int RUMBLE_LEVEL_N = (int)(sizeof RUMBLE_LEVELS / sizeof RUMBLE_LEVELS[0]);

/* S83 - the labels are those of the controller ANNOUNCED to the VM, not those
 * of the protocol. The target's number never changes; only its name does, so
 * that the same physical key is named in the vocabulary the game will show. */
std::vector<std::string> targetLabels()
{
    const padmap::Family f = padmap::currentFamily();
    std::vector<std::string> v;
    v.push_back(ui::tr("pad/unmapped"));
    for (int t = 0; t < padmap::TARGET_COUNT; t++)
        v.push_back(ui::padTargetLabel(t, f));
    return v;
}

/* === THE REAR PANEL'S SETTINGS, READ AND WRITTEN IN ONE PLACE ===========
 *
 * The four zones are four IDENTICAL rows differing only by which field they
 * read, so they are addressed by index rather than written out four times.
 * `settings.hpp` stores them as separate named fields (a text file on a memory
 * card outlives any array layout), and these two functions are the only place
 * the two shapes meet. */
uint32_t *rearAction(Settings &s, int z)
{
    switch (z) {
    case 0:  return &s.rear_zone_tl;
    case 1:  return &s.rear_zone_tr;
    case 2:  return &s.rear_zone_bl;
    default: return &s.rear_zone_br;
    }
}

uint32_t *rearGesture(Settings &s, int z)
{
    switch (z) {
    case 0:  return &s.rear_gest_tl;
    case 1:  return &s.rear_gest_tr;
    case 2:  return &s.rear_gest_bl;
    default: return &s.rear_gest_br;
    }
}

/* The travel ladder, in percent of the panel. One table read by the row, by
 * left and by right - the B1/B3 lesson: three private ladders disagree. */
const uint32_t REAR_SLIDE_PCT[] = { 10, 15, 20, 25, 28, 35, 45, 60, 80 };
const int REAR_SLIDE_N = (int)(sizeof REAR_SLIDE_PCT / sizeof REAR_SLIDE_PCT[0]);

ui::Item heading(const std::string &t)
{
    ui::Item i; i.kind = ui::Kind::Title; i.title = t; i.actionable = false;
    return i;
}

}  // namespace

PadView::PadView()
{
    padmap::load();
    screen_.setTitle(ui::tr("pad/title"));
    rebuild();
}

void PadView::rebuild()
{
    const std::vector<std::string> targets = targetLabels();
    std::vector<ui::Item> v;

    /* The tester comes FIRST: it is what we come here for most often, and
     * putting it below sixteen mappings would force a trip across the whole
     * page to reach it. */
    {
        ui::Item t; t.kind = ui::Kind::Action; t.id = PAD_TEST;
        t.title = ui::tr("pad/test");
        t.subtitle = ui::tr("pad/test_desc");
        v.push_back(std::move(t));
    }
    /* PM5 - and the pointing tester, right beside it: they answer the same kind
     * of question about the same hardware, and separating them would mean
     * looking for one of the two in another screen. */
    {
        ui::Item t; t.kind = ui::Kind::Action; t.id = PAD_MOUSE_TEST;
        t.title = ui::tr("menu/pad_mouse_test");
        t.subtitle = ui::tr("menu/pad_mouse_test_desc");
        v.push_back(std::move(t));
    }

    /* === G56 2026-08-28 - WHICH CONTROLLER DOES THE VM THINK IT SEES? ===
     * This choice sits BEFORE the key mapping because it logically precedes it:
     * Windows does not assign the same buttons to an Xbox pad and to a
     * DualShock.
     * The enum is the wire's, confirmed on console. The server refuses anything
     * past 5: it answers with a kReply carrying its error flag and creates no
     * device at all. So we only expose the six valid values. */
    {
        ui::Item it; it.kind = ui::Kind::Choice; it.id = PAD_TYPE;
        it.title = ui::tr("pad/type");
        it.subtitle = ui::tr("pad/type_desc");
        it.choice = { "Xbox 360 (XInput)", "Xbox One", "DualShock 4",
                     ui::tr("pad/type_left"), ui::tr("pad/type_right"),
                     ui::tr("pad/type_default") };
        const uint32_t t = Settings::instance().gamepad_type;
        it.choice_index = (t <= 5) ? (int)t : 0;
        v.push_back(std::move(it));
    }

    v.push_back(heading(ui::tr("pad/mapping") + " — "
                        + ui::padFamilyName(padmap::currentFamily())));
    for (size_t i = 0; i < padmap::count(); i++) {
        const padmap::Btn b = (padmap::Btn)i;
        if (!ui::padSourceExists(b)) continue;   /* not on this console */
        ui::Item it; it.kind = ui::Kind::Choice; it.id = (int)i;
        it.title = ui::padSourceLabel(b);
        it.choice = targets;
        it.choice_index = padmap::target(b) + 1;   /* -1 (none) -> index 0 */
        v.push_back(std::move(it));
    }

    v.push_back(heading(ui::tr("pad/sticks")));
    {
        ui::Item it; it.kind = ui::Kind::Toggle; it.id = PAD_INVERT;
        it.title = ui::tr("pad/invert_y");
        it.subtitle = ui::tr("menu/pad_invert_y_desc");
        it.lit = padmap::invertY();
        v.push_back(std::move(it));
    }
    {
        std::vector<std::string> zones;
        for (int z = 0; z <= 40; z += 5) zones.push_back(std::to_string(z) + " %");
        ui::Item it; it.kind = ui::Kind::Choice; it.id = PAD_DEADZONE;
        it.title = ui::tr("pad/deadzone");
        it.subtitle = ui::tr("menu/pad_deadzone_desc");
        it.choice = std::move(zones);
        it.choice_index = (int)(padmap::deadzone() / 5);
        v.push_back(std::move(it));
    }

    /* === THE REAR TOUCH PANEL, WHERE THE MISSING BUTTONS LIVE ==========
     *
     * This console has no ZL, no ZR and no stick clicks. Those four are exactly
     * where a PC game puts aiming, sprinting and melee, so without them a large
     * part of the library is not merely awkward but unplayable. The rear panel
     * is the hardware that can carry them, and it is already under the fingers
     * that would press them.
     *
     * The section is placed after the mapping and before the motors because it
     * is about which buttons EXIST, not how they are aimed - it belongs with
     * the mapping it extends. It appears only where there is a panel: an
     * unmapped zone is a control that responds to nothing, which is the defect
     * the rumble comment below records.
     *
     * The lower two zones default to unmapped ON PURPOSE. That is where a hand
     * holding the console rests, and a zone that fires when you merely hold the
     * machine is worse than one that does nothing. */
#if SHADOW_HAS_REAR_PAD
    v.push_back(heading(ui::tr("pad/rear")));
    {
        ui::Item it; it.kind = ui::Kind::Toggle; it.id = REAR_ON;
        it.title = ui::tr("pad/rear_on");
        it.subtitle = ui::tr("pad/rear_on_desc");
        it.lit = Settings::instance().rear_touch;
        v.push_back(std::move(it));
    }
    /* The rest of the section is shown only when the panel is on. A row that
     * changes a setting nothing reads is the same defect as a motor control on
     * a console with no motor - it teaches the user it works. */
    if (Settings::instance().rear_touch) {
        const std::vector<std::string> acts = ui::rearActionLabels();
        const std::vector<std::string> gess = ui::rearGestureLabels();
        Settings &st = Settings::instance();
        for (int z = 0; z < REAR_ZONE_COUNT; z++) {
            {
                ui::Item it; it.kind = ui::Kind::Choice; it.id = REAR_ACT0 + z;
                it.title = ui::rearZoneLabel(z);
                it.subtitle = ui::tr("pad/rear_zone_desc");
                it.choice = acts;
                const uint32_t a2 = *rearAction(st, z);
                it.choice_index = (int)(a2 < (uint32_t)REAR_ACT_COUNT ? a2 : 0);
                v.push_back(std::move(it));
            }
            /* The gesture row is offered only for a zone that DOES something -
             * "how is nothing triggered" is not a question. */
            if (*rearAction(st, z) != REAR_ACT_NONE) {
                ui::Item it; it.kind = ui::Kind::Choice; it.id = REAR_GES0 + z;
                it.title = ui::tr("pad/rear_gesture", ui::rearZoneLabel(z));
                it.subtitle = ui::tr("pad/rear_gesture_desc");
                it.choice = gess;
                const uint32_t g = *rearGesture(st, z);
                it.choice_index = (int)(g <= (uint32_t)REAR_GEST_SLIDE ? g : 0);
                v.push_back(std::move(it));
            }
        }
        {
            ui::Item it; it.kind = ui::Kind::Choice; it.id = REAR_TWO;
            it.title = ui::tr("pad/rear_two");
            it.subtitle = ui::tr("pad/rear_two_desc");
            it.choice = acts;
            const uint32_t a2 = st.rear_two_finger;
            it.choice_index = (int)(a2 < (uint32_t)REAR_ACT_COUNT ? a2 : 0);
            v.push_back(std::move(it));
        }
        /* Only worth asking when a trigger is actually driven by travel. */
        bool analog = (st.rear_two_finger == REAR_ACT_ZL_ANALOG
                       || st.rear_two_finger == REAR_ACT_ZR_ANALOG);
        for (int z = 0; z < REAR_ZONE_COUNT && !analog; z++) {
            const uint32_t a2 = *rearAction(st, z);
            analog = (a2 == REAR_ACT_ZL_ANALOG || a2 == REAR_ACT_ZR_ANALOG);
        }
        if (analog) {
            ui::Item it; it.kind = ui::Kind::Choice; it.id = REAR_SLIDE;
            it.title = ui::tr("pad/rear_slide");
            it.subtitle = ui::tr("pad/rear_slide_desc");
            it.choice_index = 4;
            for (int k = 0; k < REAR_SLIDE_N; k++) {
                it.choice.push_back(std::to_string(REAR_SLIDE_PCT[k]) + " %");
                if (REAR_SLIDE_PCT[k] == st.rear_slide_pct) it.choice_index = k;
            }
            v.push_back(std::move(it));
        }
    }
#endif

    /* === G57c 2026-08-28 - EXERCISING THE MOTORS WITHOUT THE WIRE ===
     * "It does not rumble" does not separate four causes: the protocol delivers
     * nothing, the state never crosses the threads, the handle is not obtained,
     * or the console refuses. These four actions short-circuit the protocol and
     * touch the hardware directly: if they rumble, the fault is upstream; if
     * they do nothing, it is here - and the log says which of the two
     * (`[G57] poignees ... rc=`, `[G57] envoi ... applique`).
     *
     * The motors are tested SEPARATELY because which one is which is the only
     * part of the decoding still at [C80]: feeling which one answers settles
     * what the disassembly could not. */
    /* === NO MOTORS ON THIS CONSOLE, SO NO MOTOR SETTINGS ===============
     *
     * A handheld PS Vita has no rumble hardware at all: `SHADOW_HAS_RUMBLE` is
     * 0 there. These six rows were offered anyway, and every one of them did
     * nothing - an intensity you can set that changes nothing, and three test
     * buttons that never buzz. That is worse than an absent section, because a
     * control that responds to input teaches the user it works.
     *
     * PS TV is the interesting case and it is knowingly left out: it drives a
     * DualShock, which DOES rumble. Nothing here detects PS TV, so wiring this
     * back means detecting the model first and answering `SHADOW_HAS_RUMBLE`
     * from that, not from the compile target. Until then, absent on both. */
#if SHADOW_HAS_RUMBLE
    v.push_back(heading(ui::tr("pad/rumble")));
    /* === G57d - INTENSITY, PLACED JUST ABOVE THE TRIALS ===
     * The trials HONOUR this setting: that is what lets you calibrate it right
     * here, by feel rather than by guess. Accepted consequence: at "off" the
     * trial buttons do nothing - and that is the correct answer, since the line
     * just above says so in as many words. */
    {
        ui::Item it; it.kind = ui::Kind::Choice; it.id = PAD_VIB_PCT;
        it.title = ui::tr("pad/rumble_intensity");
        it.subtitle = ui::tr("pad/rumble_intensity_desc");
        const uint32_t cur = Settings::instance().rumble_strength;
        it.choice_index = RUMBLE_LEVEL_N - 1;
        for (int k = 0; k < RUMBLE_LEVEL_N; k++) {
            it.choice.push_back(RUMBLE_LEVELS[k] == 0 ? ui::tr("pad/rumble_off")
                                          : std::to_string(RUMBLE_LEVELS[k]) + " %");
            if (RUMBLE_LEVELS[k] == cur) it.choice_index = k;
        }
        v.push_back(std::move(it));
    }
    {
        ui::Item it; it.kind = ui::Kind::Action; it.id = PAD_VIB_LOW;
        it.title = ui::tr("pad/rumble_low");
        it.subtitle = ui::tr("pad/rumble_low_desc");
        v.push_back(std::move(it));
    }
    {
        ui::Item it; it.kind = ui::Kind::Action; it.id = PAD_VIB_HIGH;
        it.title = ui::tr("pad/rumble_high");
        it.subtitle = ui::tr("pad/rumble_high_desc");
        v.push_back(std::move(it));
    }
    {
        ui::Item it; it.kind = ui::Kind::Action; it.id = PAD_VIB_BOTH;
        it.title = ui::tr("pad/rumble_both");
        v.push_back(std::move(it));
    }
    {
        ui::Item it; it.kind = ui::Kind::Action; it.id = PAD_VIB_STOP;
        it.title = ui::tr("pad/rumble_stop");
        it.subtitle = ui::tr("pad/rumble_stop_desc");
        v.push_back(std::move(it));
    }
#endif  /* SHADOW_HAS_RUMBLE */

    {
        ui::Item it; it.kind = ui::Kind::Action; it.id = PAD_RESET;
        it.title = ui::tr("pad/reset");
        v.push_back(std::move(it));
    }

    screen_.setItems(std::move(v));
    screen_.setHints({ { "B", ui::tr("action/back") }, { "A", ui::tr("action/ok") } });
}

/* === B3 - THE INTENSITY MUST BE FELT ON THE TRIAL THAT IS RUNNING ===
 *
 * Reported: "I change the intensity and I feel no difference on the real
 * trials." Two causes, and only the second one was visible in this file.
 *
 * The first was in `rumble_hid.c`: the percentage was read ONCE into a function
 * `static`, so it was frozen before the user could touch it - the setting did
 * nothing at all until the application was restarted.
 *
 * The second is here: the old code re-applied a fixed `255, 255`, that is, BOTH
 * motors at full scale. Adjusting the intensity while testing the low motor
 * therefore started the high one too, and what you felt was no longer the trial
 * you were running - which is its own reason to conclude the setting has no
 * effect. We re-apply the amplitudes OF THE RUNNING TRIAL.
 *
 * With no trial running we still pulse both motors: the level has to be
 * feelable while it is being chosen, otherwise it is set blind. At zero this is
 * a stop order, not a silence - the protocol carries no duration, so a rumble
 * already under way would hold forever. */
void PadView::applyIntensity(int k)
{
    if (k < 0 || k >= RUMBLE_LEVEL_N) return;
    Settings &st = Settings::instance();
    st.rumble_strength = RUMBLE_LEVELS[k];
    st.save();
    st.applyToggles();          /* pushes SHADOW_RUMBLE_PCT, re-read on each use */

    const bool en_essai = (essai_b_ || essai_h_);
    rumble_hid_apply(en_essai ? essai_b_ : 255,
                     en_essai ? essai_h_ : 255);
}

/* === ONE HANDLER FOR BOTH DIRECTIONS ===================================
 *
 * `left()` and `right()` above are already near-duplicates, and the PAD_TYPE
 * block is copied verbatim between them. That duplication is what this avoids
 * for the rear panel: the screen list has already moved the index by the time
 * we are called, so both directions do exactly the same thing - read the index
 * back, store it, save.
 *
 * Returns true if the row was one of ours. A change of ACTION rebuilds the
 * list, because it decides whether the gesture and travel rows exist at all -
 * and `it` points into the vector that rebuild() replaces, so nothing may
 * touch it afterwards. */
bool PadView::rearChoice(const ui::Item *it)
{
#if SHADOW_HAS_REAR_PAD
    Settings &st = Settings::instance();
    const int idx = it->choice_index;
    if (idx < 0) return false;

    if (it->id >= REAR_ACT0 && it->id <= REAR_ACT3) {
        *rearAction(st, it->id - REAR_ACT0) =
            (uint32_t)(idx < REAR_ACT_COUNT ? idx : 0);
        st.save();
        rebuild();
        return true;
    }
    if (it->id >= REAR_GES0 && it->id <= REAR_GES3) {
        *rearGesture(st, it->id - REAR_GES0) =
            (uint32_t)(idx <= REAR_GEST_SLIDE ? idx : 0);
        st.save();
        return true;
    }
    if (it->id == REAR_TWO) {
        st.rear_two_finger = (uint32_t)(idx < REAR_ACT_COUNT ? idx : 0);
        st.save();
        rebuild();          /* it may have just added or removed the travel row */
        return true;
    }
    if (it->id == REAR_SLIDE) {
        st.rear_slide_pct = REAR_SLIDE_PCT[idx < REAR_SLIDE_N ? idx : 4];
        st.save();
        return true;
    }
#else
    (void)it;
#endif
    return false;
}

bool PadView::left()
{
    if (mode_test_ || mode_mouse_test_ || !screen_.left()) return mode_test_ || mode_mouse_test_;
    const ui::Item *it = screen_.focusedItem();
    if (!it) return true;

    if (it->id == PAD_VIB_PCT) { applyIntensity(it->choice_index); return true; }
    if (rearChoice(it)) return true;

    if (it->id == PAD_TYPE) {
        /* Takes effect on the NEXT session: the plug-in announcement is the
         * channel's first message and is never replayed (G49 - each
         * announcement creates one more controller on the VM side). The
         * description says so. */
        Settings &st = Settings::instance();
        st.gamepad_type = (uint32_t)((it->choice_index >= 0 && it->choice_index <= 5)
                                     ? it->choice_index : 0);
        st.save();
        st.applyToggles();
        /* S83 - the mapping renames itself RIGHT NOW, under your eyes: the
         * sixteen lines below name the same physical keys, in the vocabulary of
         * the controller just chosen. This is the only moment at which the
         * setting can be checked - waiting for the next session would make it
         * invisible.
         *
         * Rebuild LAST: `it` points into the list that `reconstruire()`
         * replaces. The focus is recovered by its identifier, so it stays on
         * this line. */
        rebuild();
        return true;
    } else if (it->id == PAD_DEADZONE) {
        padmap::setDeadzone((uint32_t)(it->choice_index * 5));
    } else if (it->id >= 0 && it->id < (int)padmap::count()) {
        padmap::setTarget((padmap::Btn)it->id, it->choice_index - 1);
        padmap::save();
    }
    return true;
}

bool PadView::right()
{
    if (mode_test_ || mode_mouse_test_ || !screen_.right()) return mode_test_ || mode_mouse_test_;
    const ui::Item *it = screen_.focusedItem();
    if (!it) return true;

    if (it->id == PAD_VIB_PCT) { applyIntensity(it->choice_index); return true; }
    if (rearChoice(it)) return true;

    if (it->id == PAD_TYPE) {
        /* Takes effect on the NEXT session: the plug-in announcement is the
         * channel's first message and is never replayed (G49 - each
         * announcement creates one more controller on the VM side). The
         * description says so. */
        Settings &st = Settings::instance();
        st.gamepad_type = (uint32_t)((it->choice_index >= 0 && it->choice_index <= 5)
                                     ? it->choice_index : 0);
        st.save();
        st.applyToggles();
        /* S83 - the mapping renames itself RIGHT NOW, under your eyes: the
         * sixteen lines below name the same physical keys, in the vocabulary of
         * the controller just chosen. This is the only moment at which the
         * setting can be checked - waiting for the next session would make it
         * invisible.
         *
         * Rebuild LAST: `it` points into the list that `reconstruire()`
         * replaces. The focus is recovered by its identifier, so it stays on
         * this line. */
        rebuild();
        return true;
    } else if (it->id == PAD_DEADZONE) {
        padmap::setDeadzone((uint32_t)(it->choice_index * 5));
    } else if (it->id >= 0 && it->id < (int)padmap::count()) {
        padmap::setTarget((padmap::Btn)it->id, it->choice_index - 1);
        padmap::save();
        /* IMMEDIATE trial while a session is running: one pulse on the new
         * target. It is the only way to check a mapping without leaving the
         * screen - and it is HELD (G51), otherwise no game would see it. */
        const int t = padmap::target((padmap::Btn)it->id);
        if (ctrl_gamepad_active() && t >= 0 && t <= SHADOW_PAD_GUIDE)
            ctrl_gamepad_button_pulse(t);
    }
    return true;
}

bool PadView::activate()
{
    if (mode_test_ || mode_mouse_test_) return false;
    const ui::Item *it = screen_.focusedItem();
    if (!it) return false;

    if (it->id == PAD_MOUSE_TEST) {
        mode_mouse_test_ = true;
        /* Same reason as the gamepad tester: the page reads the controller
         * itself, so Borealis must stop consuming B as "leave the screen". */
        brls::Application::setGlobalQuit(false);
        return true;
    }
    if (it->id == PAD_TEST) {
        mode_test_ = true;
        /* === S72 2026-08-27 - DISARMING THE "GLOBAL QUIT" ===
         * Outside the stream, Borealis binds START to `Application::quit()`. In
         * a controller tester, pressing Start therefore did not merely display
         * nothing: it CLOSED THE APPLICATION. The one button we could not test
         * was also the most destructive one.
         * We disarm it for the duration of the test, and give it back on the
         * way out. */
        brls::Application::setGlobalQuit(false);
        exit_hold_start_ = 0;
        return true;
    }

    /* G57c - the four motor trials. We touch the hardware DIRECTLY, without
     * going through `rumble_state`: that is the whole point, the protocol is
     * short-circuited. The log then says which of the two links gave way. */
    if (it->id == PAD_VIB_LOW || it->id == PAD_VIB_HIGH
        || it->id == PAD_VIB_BOTH || it->id == PAD_VIB_STOP) {
        const uint8_t b = (it->id == PAD_VIB_LOW || it->id == PAD_VIB_BOTH) ? 255 : 0;
        const uint8_t h = (it->id == PAD_VIB_HIGH || it->id == PAD_VIB_BOTH) ? 255 : 0;
        /* B3 - remembered so a later intensity change re-applies THIS trial and
         * not both motors. `Stop` clears it, which is what makes the pulse on
         * an idle screen go back to using both. */
        essai_b_ = b; essai_h_ = h;
        const bool ok = rumble_hid_apply(b, h);
        pvlog("[G57c] manual test: low=%u high=%u -> %s",
                   (unsigned)b, (unsigned)h, ok ? "poignee OK" : "NO HANDLE");
        /* No automatic shutoff: the protocol carries no duration, and a rumble
         * that stops by itself would hide a motor that only answers for an
         * instant. "Stop" is a separate action, on purpose. */
        return true;
    }

#if SHADOW_HAS_REAR_PAD
    if (it->id == REAR_ON) {
        if (!screen_.toggle()) return false;
        const ui::Item *after = screen_.focusedItem();
        Settings &st = Settings::instance();
        st.rear_touch = after ? after->lit : false;
        st.save();
        /* Switching it on reveals the zone rows, switching it off hides them:
         * the section IS the answer to whether the panel is in use. */
        rebuild();
        return true;
    }
#endif
    if (it->id == PAD_INVERT) {
        if (!screen_.toggle()) return false;
        const ui::Item *after = screen_.focusedItem();
        if (after) padmap::setInvertY(after->lit);
        return true;
    }
    if (it->id == PAD_RESET) {
        padmap::resetDefaults();
        padmap::save();
        /* We rebuild the list instead of popping then re-pushing the screen, as
         * the old version did: that cycle destroyed views underneath the focus,
         * the S63 family of crashes. */
        rebuild();
        return true;
    }
    return false;
}

bool PadView::back()
{
    /* In tester mode, B is CONSUMED without closing anything: it has to show up
     * like any other button. It is the HOLD that exits, and it is measured in
     * ui/pad_test, where the button's real state is already read. */
    return mode_test_ || mode_mouse_test_;
}

/* `lire()`, `emis()` and the drawing moved into ui/pad_test: the tester is
 * SHARED with the pause menu, where it truly earns its keep since the
 * controller channel only exists during a session. Copying them over would let
 * the two versions drift apart - the mistake this repo paid for three times
 * today. */
/* Both the drawing AND the exit-hold measurement live in ui/pad_test: two
 * hosts, one behaviour. All we still do here is close the mode when it says
 * so. */
void PadView::drawMouseTest(NVGcontext *vg, float x, float y, float w, float h, double t)
{
    if (ui::mousetest::draw(vg, x, y, w, h, t)) {
        mode_mouse_test_ = false;
        brls::Application::setGlobalQuit(true);   /* give back what we took */
    }
}

void PadView::drawTest(NVGcontext *vg, float x, float y, float w, float h, double t)
{
    /* S87 - `Mode::Simple`: outside a session there is nothing to compare, so
     * the half showing what we send would be dark all the time. One controller,
     * drawn large. */
    if (ui::padtest::draw(vg, x, y, w, h, t, ui::padtest::Mode::Simple)) {
        mode_test_ = false;
        brls::Application::setGlobalQuit(true);   /* give back what we took */
    }
}

void PadView::paint(NVGcontext *vg, float x, float y, float w, float h,
                 double t)
{

    if (mode_test_) drawTest(vg, x, y, w, h, t);
    else if (mode_mouse_test_) drawMouseTest(vg, x, y, w, h, t);
    else            screen_.draw(vg, x, y, w, h, t);
}
