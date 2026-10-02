/* BootView - the boot screen, drawn by the in-house framework.
 *
 * Two states, and that is all that sets it apart from the other screens:
 *
 *   HOME     the name, a tagline, and what the application is currently doing
 *            (checking the token, calling the service).
 *   PAIRING  when no token is valid: a QR code, the address to type in, the
 *            eight-character code and the time left.
 *
 * The QR is produced elsewhere, as a BMP on disk (shadow/qr_helper). We load it
 * here as a NanoVG texture. It is the ONLY image in the whole interface: all
 * the rest is drawn, which avoids shipping assets and depending on their scale.
 */
#pragma once

#include <borealis.hpp>

#include "../ui/screen_base.hpp"
#include <string>

class BootView : public ui::Screen {
public:
    /* DEVL-4: the name a script navigates by. Stable, never the title. */
    const char *devName() const override { return "demarrage"; }

    BootView();
    ~BootView() override;

    void setStatus(const std::string &s);
    void setError(const std::string &s);

    /* Switch to pairing mode. `qr_path` may be empty: we then show the address
     * and the code alone, which is still usable - a missing QR must not prevent
     * signing in. */
    void setPairing(const std::string &url, const std::string &code,
                    const std::string &qr_path);
    void setTimer(const std::string &t) { timer_ = t; }

    /* === CLIP5 2026-10-02 - THE LOGIN CODE, ALREADY COPIED =================
     *
     * The device grant shows an eight-character code to retype in a browser.
     * On console that is unavoidable: the code goes onto a phone, which is what
     * the QR above it is for. On a PC the browser is on the SAME machine - the
     * address beside it has been one click away since UX9 - so making the user
     * read eight characters off one window and type them into another is a
     * chore with nothing behind it.
     *
     * So `setPairing` puts the code on the clipboard and the screen says it
     * did. Saying it matters as much as doing it: a clipboard that changed
     * without the user asking is unsettling unless something accounts for it,
     * and a silent copy would also leave them typing anyway, having gained
     * nothing.
     *
     * Desktop only, via SHADOW_HAS_CLIPBOARD - a console has no clipboard to
     * put it on, and `local_clipboard_available()` says so at runtime too.
     * SHADOW_LOGIN_CODE_CLIP=0 turns it off. */
    bool code_copied_ = false;
    void exitPairing() { pairing_ = false; }

    void paint(NVGcontext *vg, float x, float y, float w, float h,
                 double t) override;

private:
#ifndef __SWITCH__
    /* === UX9 2026-09-10 - THE ADDRESS IS CLICKABLE ON DESKTOP ===
     *
     * On console this address gets copied onto a phone - that is what the QR
     * code just above is for. On a PC the browser is one click away, and
     * retyping a URL by hand with a mouse under your fingers is a chore
     * nothing justifies.
     *
     * The area is recorded WHEN DRAWN rather than computed on click: the
     * text's width depends on the font and the window scale, which only the
     * NanoVG context knows, and that context exists only while rendering. A
     * rectangle guessed here would be wrong from the first resize.
     *
     * None of this exists on Switch: `openBrowser` would open the console's
     * SYSTEM browser there, which nobody asked for. */
    bool urlHit(float px, float py) const;
    float url_x_ = 0, url_y_ = 0, url_w_ = 0, url_h_ = 0;
#endif

    std::string status_, error_, url_, code_, timer_, qr_path_;
    bool pairing_ = false;

    /* NanoVG handle of the QR texture. -1 = not loaded yet. The context only
     * exists while rendering, hence the lazy load: loading it when the path
     * arrives would do it on the background thread, where no context is
     * current. */
    int  qr_img_ = -1;
    bool qr_attempted_ = false;
};
