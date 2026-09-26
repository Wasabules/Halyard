/* QualityView - stream quality settings, drawn by the in-house framework.
 *
 * Five cyclic choices, nothing else. It is the simplest screen of the lot, and
 * that is exactly why it was ported first: it validates the whole path
 * (XML-less activity -> single view -> ui::ListScreen) on a case where a defect
 * would show up immediately.
 *
 * The five option tables are copied verbatim from the old screen, comments
 * included: several of their values are reverse-engineering HYPOTHESES (the
 * codec identifiers, the profile), and losing them would pass a guess off as a
 * fact.
 */
#pragma once

#include <borealis.hpp>

#include "../ui/screen_base.hpp"
#include "../ui/screen.hpp"

class QualityView : public ui::Screen {
public:
    /* DEVL-4: the name a script navigates by. Stable, never the title. */
    const char *devName() const override { return "qualite"; }

    QualityView();

    void rebuild();

    bool up()   override { return screen_.up(); }
    bool down()    override { return screen_.down(); }
    bool left() override;
    bool right() override;

    /* === B2 2026-09-02 - THIS SCREEN HAD NO `A` AT ALL ===
     * It was never overridden, so pressing A fell through to
     * `ui::Screen::activate()`, which returns false and does nothing. The screen
     * got away with it as long as it held only cycling choices - those are
     * driven by left/right. The day two TOGGLES were added to it (vertical sync,
     * 4:4:4 colour), they became inert: you press A, nothing happens, and there
     * is no ON/OFF to see. Reported as "impossible to disable Vertical sync". */
    bool activate() override;

    void paint(NVGcontext *vg, float x, float y, float w, float h,
                 double t) override;

    /* S97 - the footer of this screen is touchable. */
    ui::ListScreen *innerList() override { return &screen_; }

private:
    ui::ListScreen screen_;
    bool apply(const ui::Item &it);
};

enum QualId { QUAL_BITRATE = 1, QUAL_FPS, QUAL_RES, QUAL_CODEC, QUAL_PROFILE,
              QUAL_444, QUAL_VSYNC };
