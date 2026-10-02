/* BootView - see the header. */

#include "boot_view.hpp"

#include "../ui/paint.hpp"
#include "../ui/theme.hpp"
#include "../ui/type.hpp"
#include "../ui/i18n.hpp"
#include "../device_caps.h"                          /* CLIP5 - SHADOW_HAS_CLIPBOARD */
#include "../../../core/services/local_clipboard.h"  /* CLIP5 - the login code */
#include <cstdlib>
#include "core/version.h"          /* SHADOW_APP_NAME */

#include <chrono>

BootView::BootView()
{
#ifndef __SWITCH__
    /* UX9 - see the header. END = button released INSIDE the view's bounds;
     * the other states do not concern us - a click held down elsewhere must
     * not open a browser. */
    this->addGestureRecognizer(new brls::TapGestureRecognizer(
        [this](brls::TapGestureStatus status, brls::Sound *) {
            if (status.state != brls::GestureState::END) return;
            if (!pairing_ || url_.empty()) return;
            if (!urlHit(status.position.x, status.position.y)) return;
            brls::Application::getPlatform()->openBrowser(url_);
        }));
#endif
}

#ifndef __SWITCH__
bool BootView::urlHit(float px, float py) const
{
    if (url_w_ <= 0.0f) return false;   /* not drawn yet: nothing to aim at */
    /* Margin: a single line of text has a thin box, and pixel-exact aiming is
     * not a reasonable demand to make of a mouse. */
    const float m = 6.0f;
    return px >= url_x_ - m && px <= url_x_ + url_w_ + m &&
           py >= url_y_ - m && py <= url_y_ + url_h_ + m;
}
#endif

BootView::~BootView() = default;   /* the texture belongs to the NanoVG context */

void BootView::setStatus(const std::string &s) { status_ = s; error_.clear(); }
void BootView::setError(const std::string &s) { error_ = s; }

void BootView::setPairing(const std::string &url, const std::string &code,
                          const std::string &qr_path)
{
    url_ = url; code_ = code;
    if (qr_path != qr_path_) { qr_path_ = qr_path; qr_img_ = -1; qr_attempted_ = false; }
    pairing_ = true;

#if SHADOW_HAS_CLIPBOARD
    /* CLIP5 - see boot_view.hpp. Runs on the UI thread (the caller reaches us
     * through `brls::Threading::sync`), which is where Win32 clipboard calls
     * belong. */
    code_copied_ = false;
    {
        const char *off = std::getenv("SHADOW_LOGIN_CODE_CLIP");
        const bool on = !off || std::atoi(off) != 0;
        if (on && !code_.empty() && local_clipboard_available()) {
            /* The returned token is DISCARDED on purpose. The clipboard channel
             * is not open yet - there is no session at login - and it seeds its
             * own change token when it opens, by which time this write is in
             * the past. So there is nothing here for the echo guard to hold.
             *
             * And `code_copied_` is set from the RESULT: another application
             * can hold the clipboard, and claiming a copy that did not happen
             * would send the user to paste nothing. */
            code_copied_ = local_clipboard_set(code_.c_str(), code_.size(), nullptr);
        }
    }
#endif
}

void BootView::paint(NVGcontext *vg, float x, float y, float w, float h,
                  double t)
{

    ui::paint::shadowBg(vg, x, y, w, h);
    ui::paint::backgroundWaves(vg, x, y, w, h, t);
    nvgFontFace(vg, ui::theme::font());
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    const float cx = x + w * 0.5f;

    if (!pairing_) {
        nvgFontSize(vg, ui::type::HERO);
        nvgFillColor(vg, ui::theme::title);
        /* The app's name, not the service's: "Shadow" sat here after the
         * 2026-09-13 rename, presenting the client under someone else's mark. */
        nvgText(vg, cx, y + h * 0.34f, SHADOW_APP_NAME, nullptr);

        nvgFontSize(vg, ui::type::BODY);
        nvgFillColor(vg, ui::theme::label);
        nvgText(vg, cx, y + h * 0.34f + 42.0f, ui::tr("boot/tagline").c_str(), nullptr);

        const float sy = y + h * 0.60f;
        if (!error_.empty()) {
            nvgFontSize(vg, ui::type::BODY);
            nvgFillColor(vg, ui::theme::bad);
            /* UI10 - a boot error is written on two lines on purpose ("what
             * failed", then "what to try"), and `nvgText` drew the break as a
             * .notdef square. See paint.hpp. */
            ui::paint::textLines(vg, cx, sy, error_);
        } else if (!status_.empty()) {
            /* The spinner sits to the LEFT of the text, and the text stays
             * centred on the screen: centring the pair would shift the sentence
             * on every status change, which draws the eye for nothing. */
            ui::paint::spinner(vg, cx - ui::paint::textWidth(vg, status_.c_str()) * 0.5f - 22.0f,
                               sy, 9.0f, t);
            nvgFontSize(vg, ui::type::BODY);
            nvgFillColor(vg, ui::theme::hint);
            ui::paint::textLines(vg, cx, sy, status_);
        }
        return;
    }

    /* --- Pairing --------------------------------------------------------- */
    nvgFontSize(vg, ui::type::SCREEN);
    nvgFillColor(vg, ui::theme::title);
    nvgText(vg, cx, y + 56.0f, ui::tr("boot/grant_title").c_str(), nullptr);

    /* The QR is loaded on the FIRST frame that needs it: the NanoVG context
     * only exists during rendering, while the path arrives from the background
     * thread. One attempt only - an unreadable file stays unreadable. */
    if (qr_img_ < 0 && !qr_attempted_ && !qr_path_.empty()) {
        qr_attempted_ = true;
        qr_img_ = nvgCreateImage(vg, qr_path_.c_str(), 0);
    }

    const float qr_c = 220.0f;
    const float qr_x = cx - qr_c * 0.5f;
    const float qr_y = y + 96.0f;

    if (qr_img_ >= 0) {
        /* White backing under the QR, overflowing by a few pixels: a dark code
         * on a dark background is not read by phones, and the quiet zone is
         * part of the symbol specification. */
        nvgBeginPath(vg);
        nvgRoundedRect(vg, qr_x - 10.0f, qr_y - 10.0f, qr_c + 20.0f, qr_c + 20.0f, 8.0f);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 255));
        nvgFill(vg);

        NVGpaint p = nvgImagePattern(vg, qr_x, qr_y, qr_c, qr_c, 0.0f, qr_img_, 1.0f);
        nvgBeginPath(vg);
        nvgRect(vg, qr_x, qr_y, qr_c, qr_c);
        nvgFillPaint(vg, p);
        nvgFill(vg);
    } else {
        /* No QR: the address and the code are enough. We SAY so rather than
         * leaving an empty square that would be read as a rendering fault. */
        ui::paint::glassPanel(vg, qr_x, qr_y, qr_c, qr_c, ui::paint::PANEL_RADIUS);
        nvgFontSize(vg, ui::type::SECONDARY);
        nvgFillColor(vg, ui::theme::hint);
        nvgText(vg, cx, qr_y + qr_c * 0.5f, ui::tr("boot/grant_no_qr").c_str(), nullptr);
    }

    float ty = qr_y + qr_c + 40.0f;
    nvgFontSize(vg, ui::type::BODY);
#ifndef __SWITCH__
    /* UX9 - accent colour and underline: the two conventions a link carries
     * everywhere else. Without a visual cue the feature does not exist -
     * nobody clicks on text that looks like text. The underline is drawn BELOW
     * the baseline, so it moves nothing: the code and the countdown keep
     * exactly their place. */
    nvgFillColor(vg, ui::paint::accentVif);
#else
    nvgFillColor(vg, ui::theme::label);
#endif
    nvgText(vg, cx, ty, url_.c_str(), nullptr);
#ifndef __SWITCH__
    {
        float b[4];
        nvgTextBounds(vg, cx, ty, url_.c_str(), nullptr, b);
        url_x_ = b[0]; url_y_ = b[1];
        url_w_ = b[2] - b[0]; url_h_ = b[3] - b[1];
        nvgBeginPath(vg);
        nvgMoveTo(vg, b[0], b[3] + 2.0f);
        nvgLineTo(vg, b[2], b[3] + 2.0f);
        nvgStrokeColor(vg, ui::paint::accentVif);
        nvgStrokeWidth(vg, 1.5f);
        nvgStroke(vg);
    }
#endif

    ty += 42.0f;
    /* The code large and letter-spaced: it gets copied by hand onto a phone,
     * and it is what the eye looks for on coming back to the screen. */
    nvgFontSize(vg, ui::type::CODE);
    nvgFillColor(vg, ui::paint::accentVif);
    nvgText(vg, cx, ty, code_.c_str(), nullptr);

#if SHADOW_HAS_CLIPBOARD
    /* CLIP5 - said right under the code, in the hint colour: it accounts for a
     * clipboard the user did not change themselves, and it tells them they can
     * paste instead of read. Absent when the copy failed. */
    if (code_copied_) {
        ty += 26.0f;
        nvgFontSize(vg, ui::type::SECONDARY);
        nvgFillColor(vg, ui::theme::hint);
        nvgText(vg, cx, ty, ui::tr("boot/grant_copied").c_str(), nullptr);
    }
#endif

    if (!timer_.empty()) {
        ty += 38.0f;
        nvgFontSize(vg, ui::type::SECONDARY);
        nvgFillColor(vg, ui::theme::hint);
        nvgText(vg, cx, ty, timer_.c_str(), nullptr);
    }
    if (!error_.empty()) {
        nvgFontSize(vg, ui::type::SECONDARY);
        nvgFillColor(vg, ui::theme::bad);
        /* UI10 - anchored to the BOTTOM, so the block is raised by what it
         * needs beyond one line. Drawing it downward from here would push every
         * line after the first off the screen, which is a worse defect than the
         * square it replaces. */
        const float lh = ui::paint::textLinesHeight(vg, error_);
        const float one = ui::paint::textLinesHeight(vg, std::string("x"));
        ui::paint::textLines(vg, cx, y + h - 30.0f - (lh - one), error_);
    }
}
