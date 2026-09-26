/* PadView - controller settings, and the TESTER.
 *
 * === WHY A TESTER ===
 *
 * A whole day of controller defects, not one of which shows up in a log:
 *   - four phantom controllers on the remote machine, because we replayed the
 *     plug-in announcement (G49);
 *   - axes scaled wrong: evdev hands out +/-32767 where we only forwarded
 *     0..255 (G48);
 *   - Start sent but INVISIBLE, press and release leaving in the same
 *     microsecond (G51).
 *
 * What they have in common: you have to compare what the CONSOLE reads against
 * what we SEND. Hence two controllers drawn side by side, and a single thing to
 * watch - if they diverge, the fault is OURS, between the read and the send.
 *
 * So the screen has two modes. LIST mode carries the settings; TEST mode
 * replaces all of it with the two silhouettes. A mode, not a sub-page: the test
 * only makes sense here, and one more page would add a level to cross when
 * getting here quickly is exactly the point.
 */
#pragma once

#include <borealis.hpp>

#include "../ui/screen_base.hpp"
#include <functional>

#include "../ui/screen.hpp"
#include "../ui/pad_draw.hpp"

class PadView : public ui::Screen {
public:
    /* DEVL-4: the name a script navigates by. Stable, never the title. */
    const char *devName() const override { return "manette"; }

    PadView();

    void rebuild();

    bool up()   override { return mode_test_ ? false : screen_.up(); }
    bool down()    override { return mode_test_ ? false : screen_.down(); }
    bool left() override;
    bool right() override;
    bool activate() override;

    /* Returns true if B was consumed (leaving test mode). False = the activity
     * may pop normally. */
    bool back() override;

    void paint(NVGcontext *vg, float x, float y, float w, float h,
                 double t) override;

    /* S97 - the footer of this screen is touchable. */
    ui::ListScreen *innerList() override { return &screen_; }

private:
    ui::ListScreen screen_;
    bool mode_test_ = false;
    /* PM5 - the controller-mouse tester, hosted here as well as in the pause
     * menu. Outside a session it is MORE useful than in one: nothing is sent to
     * the remote machine, so the sensitivity can be set before connecting. */
    bool mode_mouse_test_ = false;

    /* B3 - the amplitudes of the trial CURRENTLY running, so changing the
     * intensity re-applies THAT trial rather than a fixed 255/255. Without them
     * the level was re-applied on both motors: adjusting a "low motor" trial
     * silently started the high one, and what you felt was no longer what you
     * were testing. 0/0 = no trial running. */
    uint8_t essai_b_ = 0, essai_h_ = 0;

    /* B3 - applies the intensity at index `k`, then RE-APPLIES the trial that
     * is running so the change is felt on the spot. */
    void applyIntensity(int k);
    bool rearChoice(const ui::Item *it);
    /* Instant at which B started being held, 0 when released. A number, not a
     * timer: nothing to start or to stop, and a mode left while the button is
     * still down leaves nothing behind it. */
    double exit_hold_start_ = 0.0;

    void drawTest(NVGcontext *vg, float x, float y, float w, float h, double t);
    void drawMouseTest(NVGcontext *vg, float x, float y, float w, float h, double t);
};

enum PadId {
    PAD_TEST = 1000, PAD_MOUSE_TEST, PAD_INVERT, PAD_DEADZONE, PAD_RESET, PAD_TYPE,
    PAD_VIB_LOW, PAD_VIB_HIGH, PAD_VIB_BOTH, PAD_VIB_STOP, PAD_VIB_PCT,
    /* The rear panel. The four zone ids are CONTIGUOUS and in `rear_zone`
     * order, so one handler serves all four by subtraction rather than four
     * near-identical branches; same for the gestures. */
    REAR_ON,
    REAR_ACT0, REAR_ACT1, REAR_ACT2, REAR_ACT3,
    REAR_GES0, REAR_GES1, REAR_GES2, REAR_GES3,
    REAR_TWO, REAR_SLIDE,
    /* The key mappings take identifiers 0..N-1, in padmap::Btn order: that is
     * what lets us recover the key from the identifier without an extra
     * table. */
};
