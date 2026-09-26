/* ui::OverlayMenu - the pause menu drawn on top of the stream.
 *
 * Before: an array of labels rebuilt by hand every frame, an enum of indices,
 * and a switch in stream_activity.cpp that had to stay in step with the array
 * order. Adding one entry meant touching four places.
 *
 * Now each entry carries its own action, and entries are grouped into PAGES:
 * a short root, with submenus for anything secondary. Laying settings and
 * sections out flat buried "Resume" and "Quit" in the middle of a dozen lines.
 *
 * The view owns the menu and draws it; the activity fills it in - it alone
 * knows what "Quitter" means.
 *
 * Built on every platform: this is the end user's menu, not a developer tool.
 */
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nanovg.h>

#include "anim.h"
#include "theme.hpp"

namespace ui {

class Page;

class MenuEntry {
public:
    MenuEntry(std::string label, std::string description)
        : label_(std::move(label)), description_(std::move(description)) {}
    virtual ~MenuEntry() = default;

    const std::string &label() const       { return label_; }
    const std::string &description() const { return description_; }

    virtual bool        selectable() const { return true; }
    virtual bool        danger() const     { return false; }
    /* Text shown at the right end of the row (a toggle's state, a choice's
     * value, an information field). Empty = nothing on the right. */
    virtual std::string value() const      { return std::string(); }
    /* Returns true if the menu should close. */
    virtual bool        activate()         { return false; }
    /* Left/right arrows: -1 or +1. */
    virtual void        step(int)          {}
    /* Non-null for an entry that opens a page. */
    virtual Page       *opens() const      { return nullptr; }

    /* === S106 2026-08-29 - A SECOND ACTION, ON Y ===
     *
     * A toggle that opened a detail page on A would lose what is best about it:
     * a one-press round trip. But a section asks two questions - "do I show
     * it?" and "with which rows?" - and putting both on the same button forces
     * you to pick which one is the main one.
     *
     * So Y carries the second one. The footer announces it ONLY when the
     * focused row actually has one: a permanent hint for an action that exists
     * one time in three reads as a dead button. */
    virtual Page       *detail() const     { return nullptr; }
    /* Called JUST BEFORE opening the detail page: this is what lets a child
     * page depend on state that did not exist when the menu was built - a
     * section only knows its rows after a first render. */
    virtual void        prepareDetail()    {}

protected:
    std::string label_;
    std::string description_;
};

/* A page = one screen of the menu. The root is one, and so is every submenu.
 * Each remembers its selected row: going back puts the cursor where you left
 * it. */
class Page {
public:
    explicit Page(std::string title) : title_(std::move(title)) {}

    Page &action(std::string label, std::string desc,
                 std::function<void()> fn, bool closesMenu = true);
    /* Destructive action: highlighted in red (quit, disconnect). */
    Page &danger(std::string label, std::string desc, std::function<void()> fn);
    Page &toggle(std::string label, std::string desc,
                 std::function<bool()> get, std::function<void()> flip);
    /* S106 - a toggle WITH a detail page: A toggles, Y opens the returned page.
     * Returns the child page, for the caller to fill in. */
    Page &toggleDetail(std::string label, std::string desc,
                       std::function<bool()> get, std::function<void()> flip,
                       std::string detailTitle,
                       std::function<void(Page &)> fill = nullptr);
    /* Cyclic choice: the value shows between chevrons, and left/right plus A
     * cycle through it. */
    Page &choice(std::string label, std::string desc,
                 std::function<std::string()> value,
                 std::function<void(int)> step);
    Page &info(std::string label, std::function<std::string()> value);
    Page &separator();
    /* Empties the page. Used by detail pages, which are rebuilt every time they
     * are opened. */
    void  clear();

    /* Opens a child page and returns THAT page, for the caller to fill in. So
     * the fluent chain continues inside the submenu, not in the parent. */
    Page &submenu(std::string label, std::string desc = std::string());

    const std::string &title() const { return title_; }

private:
    friend class OverlayMenu;

    int firstSelectable(int from, int dir) const;
    /* Next selectable entry in direction `dir` (-1 / +1). The cursor STOPS at
     * the ends, it does not wrap - see the .cpp for why, and for the
     * counter-case of a page with no selectable entry at all. Returns -1 if the
     * page has none. The computation is delegated to nav.h. */
    int nextSelectable(int dir) const;

    std::string                             title_;
    std::vector<std::unique_ptr<MenuEntry>> entries_;
    std::vector<std::unique_ptr<Page>>      children_;   /* child pages */
    int                                     sel_ = 0;
};

class OverlayMenu {
public:
    OverlayMenu() : root_("Menu") {}

    /* --- construction --- */
    Page &root() { return root_; }
    /* Shorthands onto the root page: `menu.action(...).toggle(...)` stays
     * readable for a simple menu, and returns a Page& so the chain works inside
     * submenus too. */
    Page &action(std::string label, std::string desc,
                 std::function<void()> fn, bool closesMenu = true);
    Page &danger(std::string label, std::string desc, std::function<void()> fn);
    Page &toggle(std::string label, std::string desc,
                 std::function<bool()> get, std::function<void()> flip);
    Page &choice(std::string label, std::string desc,
                 std::function<std::string()> value,
                 std::function<void(int)> step);
    Page &info(std::string label, std::function<std::string()> value);
    Page &separator();
    Page &submenu(std::string label, std::string desc = std::string());
    void  clear();

    void setTitle(std::string t)  { root_.title_ = std::move(t); }
    /* Leave empty for the automatic hint (it says "retour" inside a submenu and
     * "fermer" at the root). */
    void setFooter(std::string f) { footer_ = std::move(f); }
    /* Same role as ui::Hud::setScale: keep the apparent size constant as the
     * window grows, and follow the magnification the user picked. */
    void setScale(float s) { scale_ = (s > 0.05f) ? s : 0.05f; }

    /* --- state --- */
    bool isOpen() const { return open_; }
    bool atRoot() const { return stack_.empty(); }
    void open();
    void close();

    /* --- navigation --- */
    void up();
    void down();
    void left();
    void right();
    /* Applies the selected entry: opens a page, or acts and closes the menu if
     * the entry asks for it. */
    void activate();
    /* Goes up one page. Returns false at the root: the caller closes then. */
    bool back();

    void draw(NVGcontext *vg, float x, float y, float width, float height);

    /* --- touch ---
     * This menu is drawn in immediate mode with nanovg: it is not a Borealis
     * view, so the library's touch system cannot see it and cannot hit it. That
     * was the cause of "touch does not work in the pause menu" (reported
     * 2026-08-25): there simply was no hit test at all.
     *
     * So we record the geometry of every SELECTABLE row while drawing, and
     * `tapAt` reads it back. The coordinates are the ones passed to `draw`,
     * i.e. the Borealis view space: the caller must bring the touch point into
     * it (divide the raw value by `Application::windowScale`, which is what the
     * library itself does).
     *
     * `hitTest` returns the index of the entry under the point, or -1. */
    int  hitTest(float vx, float vy) const;
    /* Selects the touched entry and activates it. Returns true if a press was
     * consumed - the caller must then not forward it to the stream. */
    bool tapAt(float vx, float vy);

    /* === S96 2026-08-29 - THE FINGER MOVES THE FOCUS FIRST ===
     *
     * `tapAt` selected AND activated on RELEASE. That was enough while it was
     * the only thing activating - but a press also fires the "A" path on the
     * current entry, and that path runs BEFORE the release: you touched one row
     * and the menu opened another, the one that had the keyboard focus.
     *
     * Setting the selection on the PRESS makes the question moot: whichever
     * path activates afterwards, it acts on the row you touched. This is
     * exactly the S84 fix, applied there to the list screens - the same
     * omission existed here, in a different screen.
     *
     * Called every frame while the finger is down: the highlight follows the
     * finger, which makes the gesture readable before you even let go. */
    bool selectAt(float vx, float vy);

    /* S106 - Y: opens the focused entry's detail page, if it has one. */
    bool detail();
    /* Does the focused entry have a detail page? The footer uses this. */
    bool hasDetail() const;
    /* true if the point falls inside the panel, even outside any row: a press
     * next to a row must not pass through to the remote desktop. */
    bool containsPoint(float vx, float vy) const;

private:
    Page       &current();
    const Page &current() const;
    std::string breadcrumb() const;

    Page                root_;
    std::vector<Page *> stack_;   /* pages opened above the root */
    std::string         footer_;
    float               scale_ = 1.0f;
    bool                open_ = false;

    /* Geometry recorded on the last draw, for touch. `index` refers to the
     * entry within the current page. */
    struct HitBox { int index; float x, y, w, h; };
    std::vector<HitBox> hits_;
    float               panel_x_ = 0, panel_y_ = 0, panel_w_ = 0, panel_h_ = 0;

    /* --- animations (2026-08-27) ---
     * These are VALUE FIELDS, not a list of live animations to maintain: an
     * `anim_t` is five numbers and a function of time (see anim.h). It copies
     * and it is thrown away, so there is nothing to destroy, nothing to remove
     * from a list, and above all no pointer to a view - the crash family this
     * framework was born to eliminate.
     *
     * The focus rectangle is animated in Y AND in HEIGHT: an entry with a
     * description is taller than a plain one, and a fixed height would make the
     * highlight spill onto the neighbouring row while it travels. */
    anim_t focus_y_ = anim_fixed(0.0f);
    anim_t focus_h_ = anim_fixed(0.0f);
    /* Opacity of the page content (breadcrumb, rows, bottom hint). Restarts
     * from 0 on every page change. */
    anim_t fade_    = anim_fixed(1.0f);

    /* Fingerprint of the page drawn on the previous frame, to tell "the cursor
     * moved" (slide) from "we changed page" (jump).
     *
     * Two INTEGERS, and deliberately not a `Page *`: `clear()` destroys every
     * child page, and it can be called while the menu is open. A pointer kept
     * across frames just for this comparison would then be DANGLING - the crash
     * family this framework was born to eliminate, reintroduced by a piece of
     * decoration. These two integers, on the other hand, are never
     * dereferenced.
     *
     * `anim_placed_` stays false until a page's first frame has placed the
     * rectangle: without it, reopening the menu would slide the highlight in
     * from wherever it sat when the menu was closed. */
    int  anim_depth_  = -1;
    int  anim_count_  = -1;
    bool anim_placed_ = false;
};

}  // namespace ui
