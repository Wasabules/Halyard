/* ui::Modal - see the header for the why. */

#include "modal.hpp"

#include "paint.hpp"
#include "theme.hpp"
#include "type.hpp"

namespace ui {

namespace {
const float MODAL_W  = 520.0f;
const float MODAL_H  = 190.0f;
const float BUTTON_H = 46.0f;
const float MARGIN   = 24.0f;
}

void Modal::open(std::string question, std::vector<Button> buttons)
{
    question_ = std::move(question);
    buttons_  = std::move(buttons);
    /* The focus starts on the FIRST button, which by convention is the harmless
     * action: a reflex press on A must not fire the destructive one. */
    sel_      = 0;
    open_     = true;
}

bool Modal::left()
{
    if (!open_) return false;
    if (sel_ > 0) sel_--;
    return true;   /* consumed even at the end: the screen below must not move */
}

bool Modal::right()
{
    if (!open_) return false;
    if (sel_ + 1 < (int)buttons_.size()) sel_++;
    return true;
}

bool Modal::activate()
{
    if (!open_) return false;

    /* Copy the action BEFORE closing: it may destroy the screen that contains
     * us (a disconnect, a shutdown), and reading `buttons_` afterwards would
     * mean reading a freed object. Same reasoning as for the views, applied to
     * ourselves. */
    std::function<void()> a;
    if (sel_ >= 0 && sel_ < (int)buttons_.size()) a = buttons_[(size_t)sel_].action;

    open_ = false;
    buttons_.clear();
    if (a) a();
    return true;
}

bool Modal::back()
{
    if (!open_) return false;
    /* B cancels: we close WITHOUT firing anything. Firing the first button
     * would be defensible, but someone pressing B wants to go back, not to
     * choose. */
    open_ = false;
    buttons_.clear();
    return true;
}

void Modal::draw(NVGcontext *vg, float x, float y, float w, float h, double t)
{
    if (!open_) return;

    /* The screen behind is DIMMED, not erased: you must still see what is being
     * discussed, while no longer being able to mistake it for what is active. */
    nvgBeginPath(vg);
    nvgRect(vg, x, y, w, h);
    nvgFillColor(vg, nvgRGBA(4, 7, 14, 190));
    nvgFill(vg);

    const float pw = (MODAL_W < w - 48.0f) ? MODAL_W : w - 48.0f;
    const float px = x + (w - pw) * 0.5f;
    const float py = y + (h - MODAL_H) * 0.5f;

    paint::glassPanel(vg, px, py, pw, MODAL_H, paint::PANEL_RADIUS);

    nvgFontFace(vg, theme::font());
    nvgFontSize(vg, type::BODY);
    nvgFillColor(vg, theme::title);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    paint::clampedText(vg, px + pw * 0.5f, py + 56.0f, pw - MARGIN * 2.0f,
                      py, MODAL_H, question_, false, t);

    if (buttons_.empty()) { nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE); return; }

    const float bw = (pw - MARGIN * 2.0f - 12.0f * (float)(buttons_.size() - 1))
                   / (float)buttons_.size();
    const float by = py + MODAL_H - MARGIN - BUTTON_H;

    for (size_t i = 0; i < buttons_.size(); i++) {
        const float bx = px + MARGIN + (bw + 12.0f) * (float)i;
        const bool  foc = ((int)i == sel_);

        nvgBeginPath(vg);
        nvgRoundedRect(vg, bx, by, bw, BUTTON_H, 10.0f);
        if (foc) {
            nvgFillColor(vg, buttons_[i].danger ? theme::rowDanger : paint::accent);
        } else {
            nvgFillColor(vg, paint::cardBgTop);
        }
        nvgFill(vg);
        if (!foc) {
            nvgStrokeWidth(vg, 1.0f);
            nvgStrokeColor(vg, paint::cardBorder);
            nvgStroke(vg);
        }

        nvgFontSize(vg, type::BODY);
        /* The label stays at full brightness even when not focused: dimming the
         * unchosen button would make it look unavailable. */
        nvgFillColor(vg, theme::title);
        paint::clampedText(vg, bx + bw * 0.5f, by + BUTTON_H * 0.5f, bw - 16.0f,
                          by, BUTTON_H, buttons_[i].label, false, t);
    }

    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
}

}  // namespace ui
