/* ui::Modal - a confirmation prompt, drawn by our own framework.
 *
 * === WHY IT REPLACES brls::Dialog ===
 *
 * `brls::Dialog::open()` PUSHES AN ACTIVITY, and `buttonClick` pops it before
 * calling the button's code. Our four dialogs called `close()` inside that
 * code: a second pop, which took the screen UNDERNEATH with it. That is the
 * cause of S66 - "I quit and I end up on the home page" - and it survived three
 * wrong hypotheses before it was found.
 *
 * The S66 fix removed those `close()` calls. This one removes the CATEGORY:
 * here a dialog is not an activity, it is a STATE of the screen showing it.
 * There is no stack left to pop, so no extra pop is possible any more - whatever
 * code someone writes in a button tomorrow.
 *
 * Secondary consequence, but a visible one: the Borealis dialog stayed grey in
 * the middle of midnight-blue screens. This one borrows the same glass.
 */
#pragma once

#include <nanovg.h>

#include <functional>
#include <string>
#include <vector>

namespace ui {

class Modal {
public:
    struct Button {
        std::string           label;
        std::function<void()> action;   /* may be empty: the "cancel" button */
        bool                  danger = false;
    };

    /* Opens the prompt. The first button is the one that gets the focus: by
     * convention the HARMLESS action goes first, so that a reflex press on A
     * does not fire the destructive one. */
    void open(std::string question, std::vector<Button> buttons);

    bool opened() const { return open_; }
    void close()        { open_ = false; }

    /* Navigation. These return `true` when the event is CONSUMED - an open
     * dialog swallows everything, otherwise the screen underneath would keep
     * responding while we are asking it a question. */
    bool left();
    bool right();
    bool activate();
    bool back();   /* B cancels, like the first button */

    void draw(NVGcontext *vg, float x, float y, float w, float h, double t);

private:
    bool                open_ = false;
    std::string         question_;
    std::vector<Button> buttons_;
    int                 sel_ = 0;
};

}  // namespace ui
