/* ShutdownActivity — see the header. */

#include "activity/shutdown_activity.hpp"

#include "../ui/screen_base.hpp"
#include "../ui/paint.hpp"
#include "../ui/theme.hpp"
#include "../ui/type.hpp"
#include "../ui/i18n.hpp"
#include "../ui/shutdown.hpp"

#include <string>

/* The screen is drawn by our own framework, like every other one here: it
 * borrows the same glass and the same background so that closing does not look
 * like a different application taking over. */
class ShutdownView : public ui::Screen {
public:
    /* DEVL-4: the name a script navigates by. Stable, never the title. */
    const char *devName() const override { return "arret"; }

    void paint(NVGcontext *vg, float x, float y, float w, float h,
               double t) override
    {
        ui::paint::shadowBg(vg, x, y, w, h);
        ui::paint::backgroundWaves(vg, x, y, w, h, t);

        nvgFontFace(vg, ui::theme::font());
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

        const float cx = x + w * 0.5f;
        const float cy = y + h * 0.5f;

        nvgFontSize(vg, ui::type::SCREEN);
        nvgFillColor(vg, ui::theme::title);
        nvgText(vg, cx, cy - 34.0f, ui::tr("shutdown/title").c_str(), nullptr);

        /* The STEP, not a spinner. A spinner says "wait"; the step says what is
         * being waited on, and that is the whole point of this screen — closing
         * the session on the server side is what costs the seconds. */
        const char *step = ui::shutdown::step();
        if (step && *step) {
            nvgFontSize(vg, ui::type::BODY);
            nvgFillColor(vg, ui::theme::label);
            nvgText(vg, cx, cy + 10.0f, step, nullptr);
        }

        /* A moving dot rather than a progress bar: we do NOT know how long this
         * takes — a dead network turns two seconds into thirty — and a bar that
         * lies about its own progress is worse than no bar. */
        const float period = 1.2f;
        const float phase  = (float)(t - (double)(long long)(t / period) * period) / period;
        const float span   = 78.0f;
        const float dx     = (phase < 0.5f ? phase : 1.0f - phase) * 2.0f * span - span * 0.5f;
        nvgBeginPath(vg);
        nvgCircle(vg, cx + dx, cy + 48.0f, 3.5f);
        nvgFillColor(vg, ui::paint::accentVif);
        nvgFill(vg);
    }
};

brls::View *ShutdownActivity::createContentView()
{
    view = new ShutdownView();
    return view;
}
