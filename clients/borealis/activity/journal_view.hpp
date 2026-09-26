/* JournalView - the log viewing screen (S86, 2026-08-29).
 *
 * === WHY THIS IS NOT A `ui::ListScreen` ===
 *
 * That was the first idea, and it is a bad one for three reasons that add up:
 *
 *   1. HEIGHT. A `ListScreen` entry is 60 to 168 pixels tall - it is sized for
 *      a setting you aim at with a controller. A log line is ONE line. At 60
 *      pixels we would show eight lines on a 720-pixel screen where a good
 *      twenty fit: you no longer read a log, you leaf through it.
 *   2. FOCUS. `ListScreen` draws a highlight on the focused entry and the A
 *      button activates it. Here NOTHING is activatable: putting a cursor on a
 *      log line promises an action that does not exist. That is exactly the
 *      counter-case `ui_focusable()` documents for section separators - a
 *      screen that reads as broken.
 *   3. COST. `ListScreen` maintains two derived vectors and three animations
 *      per frame. For four hundred inert lines that is work for nothing.
 *
 * So this screen has NO focus at all: it only has SCROLLING. The geometry comes
 * from `nav.h` - `nav_contenu_h_uniforme`, `nav_scroll_max`, `nav_scroll_borne`,
 * `nav_y_de_index` - which is pure, uniform and already verified offline by
 * tests/test_ui_nav.c. We do not rewrite the scrolling arithmetic: that one
 * carries the NaN guard and the clamping for content shorter than the view.
 *
 * The header and the footer come from `ui::drawHeader` / `ui::drawFooter`
 * (screen.hpp): they are the PAGE's, not the list's, and copying them here
 * would have made them diverge on the first tweak.
 *
 * === WHAT THE SCREEN SHOWS, AND IN WHICH ORDER ===
 *
 * The last lines of the CURRENT session, most recent AT THE BOTTOM, like a
 * terminal - and the screen opens ALREADY SCROLLED ALL THE WAY DOWN.
 *
 * COUNTER-CASE: opening at the top would show the application starting up, that
 * is to say the exact opposite of what this screen is opened for. You come here
 * to see what just happened, not what happened twenty minutes ago.
 *
 * === THE COLUMNS ARE THE POINT ===
 *
 * `journal.c` writes `[12.345] I/video    message`: the timestamp, the severity
 * LETTER, the category padded to eight characters, then the text. Those are
 * COLUMNS, and they are what makes a page of log readable at a glance - provided
 * they are treated as such. So the severity letter is colored, and an error or
 * warning line carries its tint into its message, while DEBUG and TRACE recede
 * into grey. Without that, thirty lines of periodic measurements weigh exactly
 * as much visually as the single line explaining the failure - which is the
 * situation this screen replaced.
 *
 * The splitting happens ONCE, at load time, never in the rendering: searching
 * for four separators across four hundred lines sixty times per second is
 * precisely the G47 mistake (`ctrl_gamepad_present()` called from an `info()`,
 * interface down to 5 fps). The translated labels and the status line are cached
 * for the same reason - `ui::tr` formats, therefore allocates.
 *
 * A line that does not follow this format is NOT dropped: it is displayed whole
 * as the message, with an unknown severity. A log that hides what it does not
 * understand is worse than a raw one.
 */
#pragma once

#include <borealis.hpp>

#include "../ui/screen_base.hpp"
#include "../ui/screen.hpp"   /* ui::Hint, ui::drawHeader/drawFooter, nav.h */

#include <string>
#include <vector>

class JournalView : public ui::Screen {
public:
    /* DEVL-4: the name a script navigates by. Stable, never the title. */
    const char *devName() const override { return "journal"; }

    JournalView();

    /* Re-reads the log from the card. Called when the screen opens and on the X
     * button. NEVER from the rendering: it flushes the write buffer and reads
     * the SD card (see `journal_dernieres_lignes`), which is exactly the kind of
     * I/O a Borealis `info()` would perform sixty times per second. */
    void reload();

    /* Scrolling, and nothing else - there is no focus on this screen. Up/down
     * move a few lines, the triggers a whole page: four hundred lines are
     * otherwise crossed by holding the d-pad for twenty seconds. */
    bool up()      override { return scrollBy(-LINE_STEP); }
    bool down()       override { return scrollBy(+LINE_STEP); }
    /* S101 - Y switches session: the current one, then the archives from the
     * most recent to the oldest. It is almost always the PREVIOUS one you want
     * to re-read - you notice a defect, you quit, you come back, and the current
     * log now holds nothing but the startup. */
    bool buttonX() override;   /* refresh */
    bool buttonY() override;   /* switch session */

    bool triggerL() override { return scrollPage(-1); }
    bool triggerR() override { return scrollPage(+1); }

    void paint(NVGcontext *vg, float x, float y, float w, float h,
                 double t) override;

private:
    /* An ALREADY SPLIT line. Values only: it copies and it is thrown away, like
     * the `ui::Item`s. No pointer into the read buffer - that buffer is freed as
     * soon as loading is done, and a view holding pointers into it would be a
     * use-after-free dressed up as an optimization. */
    struct Line {
        std::string timestamp;  /* "12.345", without the brackets */
        std::string cat;        /* "video" */
        std::string text;       /* the message */
        char        sev = '?';  /* E A I D T, or ? if the line is off-format */
    };

    static Line split(const char *raw);

    std::vector<Line> lines_;
    std::string       message_;   /* shown instead of the lines when there are none */

    /* Cached labels: see the file header, `ui::tr` formats therefore allocates. */
    std::string           title_;
    std::string           subtitle_;
    std::string           status_;
    std::vector<ui::Hint> hints_;
    int                   session_ = 0;   /* 0 = current, 1..N = archives */

    float scroll_        = 0.0f;
    float view_h_        = 0.0f;   /* frame of the LAST rendered image, for page jumps */
    bool  go_to_bottom_  = true;   /* the next render pins the scroll to the bottom */

    /* Gesture in progress. Numbers only - nothing to free, and an interrupted
     * gesture (finger off the screen, applet suspended) leaves `finger_down_`
     * true, without consequence: the next gesture resets it. */
    bool  finger_down_ = false;
    float finger_y0_ = 0.0f;
    float scroll_at_press_ = 0.0f;

    static const int LINE_STEP = 4;

    /* Scroll state REBUILT on each use rather than kept in a field: the content
     * may have changed between two calls (a reload), and a stale `nav_t` is
     * exactly the kind of state that lies. */
    nav_t state() const;

    bool scrollBy(int lines);
    bool scrollPage(int dir);
};
