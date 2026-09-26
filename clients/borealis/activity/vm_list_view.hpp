/* VmListView - the machine list, drawn by our own framework.
 *
 * === WHY A SINGLE VIEW ===
 *
 * The old screen was a tree of Borealis views: a scrolling frame holding one
 * focusable card per machine, all rebuilt on every reload. That is what
 * produced the S63 crash - `Application::currentFocus` pointed at a freed card,
 * and the next virtual call jumped into a dead vtable ("Instruction Abort").
 *
 * Here the whole screen is ONE view, and it is the only focusable one. It lives
 * as long as the activity. Reloading the list no longer destroys any view: it
 * replaces a `std::vector<ui::Item>`. The internal focus is an INDEX, re-clamped
 * on every frame. The defect category disappears instead of being guarded
 * against.
 *
 * Borealis stays underneath for what it does well - window, GL context, input,
 * applet lifecycle - and no longer has a say in the appearance.
 */
#pragma once

#include <borealis.hpp>

#include "../ui/screen_base.hpp"

#include <functional>
#include <string>
#include <vector>

#include "../ui/screen.hpp"
#include "../ui/modal.hpp"

/* What the screen needs to know about a machine, and nothing more. Deliberate:
 * the view knows neither the network nor `ShadowApp`, it receives values. */
struct VmRow {
    std::string id;
    std::string name;
    std::string subtitle;
    std::string badge;
    bool        lit = false;
};

class VmListView : public ui::Screen {
public:
    /* DEVL-4: the name a script navigates by. Stable, never the title. */
    const char *devName() const override { return "liste-vm"; }

    VmListView();

    /* Replaces the content. Callable at any time FROM THE UI THREAD. */
    void setVms(const std::vector<VmRow> &vms);
    void setHeader(std::string title, std::string subtitle, std::string info);
    void setStatus(const std::string &s, bool busy);
    void setHints(std::vector<ui::Hint> h) { screen_.setHints(std::move(h)); }

    /* === S102 2026-08-29 - X GOES THROUGH THE VIEW ===
     *
     * CORRECTION. I first explained the inert X by "the first `registerAction`
     * wins". That is FALSE: `View::registerAction` REPLACES the existing entry
     * for a given button (`*it = {...}`), so the last one wins, and the
     * activity's action was indeed live. The log viewer's X, on the other hand,
     * really was missing - two different causes that I had merged into one.
     *
     * What remains true: going through the VIEW makes the two paths converge -
     * the physical button and a tap on the footer hint call the same function.
     * That is the reason to keep this wiring, not the one I had given. */
    void setOnRefresh(std::function<void()> f) { on_refresh_ = std::move(f); }
    bool buttonX() override;
    void setEmptyMessage(std::string m) { screen_.setEmptyMessage(std::move(m)); }

    /* This screen presents an INVENTORY, not settings: its entries are tiles
     * browsed left/right, like a game library.
     * See ui::ListScreen::setTileLayout. */
    void enableTiles() { screen_.setTileLayout(true); }

    /* Activating a machine: hands back its identifier (not a pointer, not an
     * index - indices move on reload). */
    void setOnActivate(std::function<void(const std::string &)> cb) { on_activate_ = std::move(cb); }

    /* A question put to the user. This is NOT a pushed activity but a STATE of
     * this screen: see ui/modal.hpp - that is what removes the extra pop of S66
     * by construction. */
    void ask(std::string question, std::vector<ui::Modal::Button> b)
    { modal_.open(std::move(question), std::move(b)); }

    /* An open dialog SWALLOWS navigation: without this the screen underneath
     * would keep responding while we are asking it a question. */
    bool up()    override { return modal_.opened() ? true : screen_.up(); }
    bool down()     override { return modal_.opened() ? true : screen_.down(); }
    /* In tile mode, left/right walk the row - that is the reading direction.
     * The dialog comes first: as long as it is open, it has the hand. */
    bool left()  override { return modal_.opened() ? modal_.left() : screen_.left(); }
    bool right()  override { return modal_.opened() ? modal_.right() : screen_.right(); }
    bool back()  override { return modal_.back(); }
    bool activate() override;                       /* returns false when nothing is focused */
    bool touch(float x, float y);       /* selects + activates */

    void paint(NVGcontext *vg, float x, float y, float w, float h,
                 double t) override;

    /* S97 - this screen's footer is touchable. */
    ui::ListScreen *innerList() override { return &screen_; }

private:
    ui::ListScreen screen_;
    ui::Modal      modal_;
    /* The identifiers, kept in parallel with the entries: `ui::Item` carries
     * only an integer `id`, while the application handles strings. */
    std::vector<std::string> ids_;
    std::function<void(const std::string &)> on_activate_;
    std::function<void()> on_refresh_;
};
