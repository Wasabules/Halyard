/* EqView - the tone correction applied to the sound coming from the remote
 * machine.
 *
 * === WHAT THIS SCREEN SHOWS, AND WHY IT SHOWS A CURVE ===
 *
 * An equalizer tuned blind is an equalizer tuned badly. "Peak, 1800 Hz, Q 1.2,
 * +3 dB" tells nobody anything - not even someone who knows biquads, because
 * what matters is the SUM of the five bands, not each one taken apart. Two
 * neighbouring bands that overlap produce a bump nobody asked for, and no row of
 * the list shows it.
 *
 * The curve answers that: it is the REAL response of the cascade, read from the
 * engine (`audio_eq_reponse_db`) and not recomputed here. That is the only way
 * for it not to lie - a curve redrawn from the settings would show what the user
 * asked for, not what the filter does.
 *
 * === WHY TWO PROFILES, AND NOT ONE ===
 *
 * The problem to correct is not the same depending on the console's mode.
 * Docked, the sound goes out to a TV or an amplifier, which needs nothing. In
 * handheld mode it comes out of two speakers a few centimetres across that
 * render nothing below 250 Hz and clip when fed bass - so the bass you cannot
 * hear also degrades the mids you can.
 *
 * A single profile would force you to re-pick on every mode change, that is to
 * say never to do it. The switch is automatic (`Settings::suivreModeConsole`),
 * including mid-session.
 */
#pragma once

#include <borealis.hpp>

#include "../ui/screen_base.hpp"
#include "../ui/screen.hpp"

extern "C" {
#include "../../../core/protocol/eq.h"
}

class EqView : public ui::Screen {
public:
    /* DEVL-4: the name a script navigates by. Stable, never the title. */
    const char *devName() const override { return "egaliseur"; }

    EqView();

    void rebuild();

    /* === S91 2026-08-29 - TWO MODES, AND WHY ===
     *
     * Five bands with four parameters each make twenty list rows. That was
     * UNMANAGEABLE, reported in those terms: you scroll without ever seeing the
     * equalizer, and tuning one band means remembering where you are. A list is
     * made for INDEPENDENT settings; five bands that answer each other are not.
     *
     * The GRAPHIC mode shows all five at once, as vertical bars, under the curve
     * they produce. It is the universal representation of an equalizer, and it
     * is not decoration: it makes visible what a list hides - two neighbouring
     * bands that overlap, a band left at full tilt, the overall shape.
     *
     * Same pattern as the controller tester (`PadView::mode_test_`): a mode
     * INSIDE the screen, not one more activity. B leaves it, it does not pop. */
    bool up()     override;
    bool down()      override;
    bool left()   override;
    bool right()   override;
    bool activate()  override;
    bool back()   override;
    bool triggerL() override;
    bool triggerR() override;
    bool buttonX()  override;
    bool buttonY()  override;

    void paint(NVGcontext *vg, float x, float y, float w, float h,
                 double t) override;

    /* S97 - this screen's footer is touchable. */
    ui::ListScreen *innerList() override { return &screen_; }

private:
    ui::ListScreen screen_;

    /* The bands being edited. They live HERE and not in `Settings`: we write
     * them there on every change, but the screen needs to hold them in a usable
     * form while they are being modified, and re-reading the string from the
     * settings file on every press would be a pointless round trip. */
    eq_band_t bands_[EQ_BANDS];

    bool graphic_mode_ = false;
    int  sel_band_ = 0;

    void loadBands();
    void drawGraphic(NVGcontext *vg, float x, float y, float w, float h,
                     double t);
    /* Moves one parameter of the selected band by one notch. */
    void step(int param, int dir);
    void saveBands();
    bool apply(const ui::Item &it);
    void drawCurve(NVGcontext *vg, float x, float y, float w, float h);
};

/* Identifiers. The bands occupy a CONTIGUOUS range of four per band, which makes
 * (band, parameter) recoverable by a division - rather than twenty constants
 * that would have to be kept in agreement with the loop that builds them. */
enum EqId {
    EQ_ID_PROFILE_DOCK = 1,
    EQ_ID_PROFILE_HANDHELD,
    EQ_ID_AUTO_TRIM,
    EQ_ID_RESET,
    EQ_ID_TUNE,          /* S91 - enters the graphic equalizer */

    /* base + band * 4 + parameter (0 = type, 1 = freq, 2 = Q, 3 = gain) */
    EQ_ID_BAND_BASE = 100,
};
