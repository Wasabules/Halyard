/* ui::OverlayMenu - see overlay_menu.hpp. */
#include "overlay_menu.hpp"

#include "anim.h"
#include "i18n.hpp"
#include "nav.h"
#include "paint.hpp"
#include "type.hpp"

#include <chrono>

namespace ui {

namespace {

const float PANEL_W    = 620.0f;
const float PAD        = 28.0f;
const float TITLE_H    = 58.0f;
const float ROW_H      = 44.0f;   /* without description */
const float ROW_H_DESC = 58.0f;   /* with description */
const float ROW_GAP    = 6.0f;
const float SEP_H      = 14.0f;
const float FOOTER_H   = 34.0f;

/* --- concrete entries --------------------------------------------------- */

class Action : public MenuEntry {
public:
    Action(std::string l, std::string d, std::function<void()> fn,
           bool closes, bool danger)
        : MenuEntry(std::move(l), std::move(d)), fn_(std::move(fn)),
          closes_(closes), danger_(danger) {}
    bool danger() const override { return danger_; }
    bool activate() override
    {
        if (fn_) fn_();
        return closes_;
    }
private:
    std::function<void()> fn_;
    bool closes_, danger_;
};

class Toggle : public MenuEntry {
public:
    Toggle(std::string l, std::string d, std::function<bool()> get,
           std::function<void()> flip, Page *detail = nullptr,
           std::function<void(Page &)> fill = nullptr)
        : MenuEntry(std::move(l), std::move(d)), get_(std::move(get)),
          flip_(std::move(flip)), detail_(detail),
          fill_(std::move(fill)) {}
    /* S106 - this entry's second question, opened with Y. */
    Page *detail() const override { return detail_; }
    void  prepareDetail() override
    {
        /* Rebuilt on EVERY opening: a section's rows change with its content -
         * an error row only exists while there are errors. A page built once
         * would keep showing the state it had back then. */
        if (detail_ && fill_) { detail_->clear(); fill_(*detail_); }
    }
    std::string value() const override
    {
        /* Goes through the catalogue like everything else: these two words were
         * the last ones hardcoded inside the framework, and they came out in
         * French in the middle of an English menu. */
        return tr((get_ && get_()) ? "menu/on" : "menu/off");
    }
    bool activate() override
    {
        if (flip_) flip_();
        return false;   /* stay open: the effect has to be visible */
    }
    void step(int) override { if (flip_) flip_(); }
private:
    Page *detail_ = nullptr;
    std::function<void(Page &)> fill_;

    std::function<bool()> get_;
    std::function<void()> flip_;
};

class Choice : public MenuEntry {
public:
    Choice(std::string l, std::string d, std::function<std::string()> value,
           std::function<void(int)> step)
        : MenuEntry(std::move(l), std::move(d)), value_(std::move(value)),
          step_(std::move(step)) {}
    std::string value() const override
    {
        return value_ ? ("< " + value_() + " >") : std::string();
    }
    bool activate() override { step(+1); return false; }
    void step(int dir) override { if (step_) step_(dir); }
private:
    std::function<std::string()> value_;
    std::function<void(int)>     step_;
};

class Info : public MenuEntry {
public:
    Info(std::string l, std::function<std::string()> value)
        : MenuEntry(std::move(l), std::string()), value_(std::move(value)) {}
    bool        selectable() const override { return false; }
    std::string value() const override { return value_ ? value_() : std::string(); }
private:
    std::function<std::string()> value_;
};

/* An entry that opens a page. It "fait" nothing itself: OverlayMenu sees a
 * non-null `opens()` and pushes the page. */
class SubmenuEntry : public MenuEntry {
public:
    SubmenuEntry(std::string l, std::string d, Page *page)
        : MenuEntry(std::move(l), std::move(d)), page_(page) {}
    std::string value() const override { return "\u203a"; }
    Page       *opens() const override { return page_; }
private:
    Page *page_;
};

class Separator : public MenuEntry {
public:
    Separator() : MenuEntry(std::string(), std::string()) {}
    bool selectable() const override { return false; }
};

float rowHeight(const MenuEntry &e)
{
    if (!e.selectable() && e.label().empty() && e.value().empty()) return SEP_H;
    return e.description().empty() ? ROW_H : ROW_H_DESC;
}

}  // namespace

/* ---------------------------------------------------------- construction */

Page &Page::action(std::string label, std::string desc,
                   std::function<void()> fn, bool closesMenu)
{
    entries_.push_back(std::unique_ptr<MenuEntry>(
        new Action(std::move(label), std::move(desc), std::move(fn),
                   closesMenu, false)));
    return *this;
}

Page &Page::danger(std::string label, std::string desc, std::function<void()> fn)
{
    entries_.push_back(std::unique_ptr<MenuEntry>(
        new Action(std::move(label), std::move(desc), std::move(fn), true, true)));
    return *this;
}

Page &Page::toggle(std::string label, std::string desc,
                   std::function<bool()> get, std::function<void()> flip)
{
    entries_.push_back(std::unique_ptr<MenuEntry>(
        new Toggle(std::move(label), std::move(desc), std::move(get),
                   std::move(flip))));
    return *this;
}

Page &Page::toggleDetail(std::string label, std::string desc,
                         std::function<bool()> get, std::function<void()> flip,
                         std::string detailTitle,
                         std::function<void(Page &)> fill)
{
    /* The child page belongs to THIS page, just like a submenu: the page owns
     * the lifetime, and the entry only keeps a pointer to it. */
    children_.push_back(std::unique_ptr<Page>(new Page(std::move(detailTitle))));
    Page *child = children_.back().get();
    entries_.push_back(std::unique_ptr<MenuEntry>(
        new Toggle(std::move(label), std::move(desc), std::move(get),
                   std::move(flip), child, std::move(fill))));
    return *child;
}

Page &Page::choice(std::string label, std::string desc,
                   std::function<std::string()> value,
                   std::function<void(int)> step)
{
    entries_.push_back(std::unique_ptr<MenuEntry>(
        new Choice(std::move(label), std::move(desc), std::move(value),
                   std::move(step))));
    return *this;
}

Page &Page::info(std::string label, std::function<std::string()> value)
{
    entries_.push_back(std::unique_ptr<MenuEntry>(
        new Info(std::move(label), std::move(value))));
    return *this;
}

Page &Page::separator()
{
    entries_.push_back(std::unique_ptr<MenuEntry>(new Separator()));
    return *this;
}

void Page::clear()
{
    entries_.clear();
    sel_ = 0;
}

Page &Page::submenu(std::string label, std::string desc)
{
    children_.push_back(std::unique_ptr<Page>(new Page(label)));
    Page *child = children_.back().get();
    entries_.push_back(std::unique_ptr<MenuEntry>(
        new SubmenuEntry(std::move(label), std::move(desc), child)));
    return *child;
}

/* --- shorthands onto the root page --- */

Page &OverlayMenu::action(std::string l, std::string d, std::function<void()> f,
                          bool closesMenu)
{ return root_.action(std::move(l), std::move(d), std::move(f), closesMenu); }

Page &OverlayMenu::danger(std::string l, std::string d, std::function<void()> f)
{ return root_.danger(std::move(l), std::move(d), std::move(f)); }

Page &OverlayMenu::toggle(std::string l, std::string d, std::function<bool()> g,
                          std::function<void()> f)
{ return root_.toggle(std::move(l), std::move(d), std::move(g), std::move(f)); }

Page &OverlayMenu::choice(std::string l, std::string d,
                          std::function<std::string()> v,
                          std::function<void(int)> st)
{ return root_.choice(std::move(l), std::move(d), std::move(v), std::move(st)); }

Page &OverlayMenu::info(std::string l, std::function<std::string()> v)
{ return root_.info(std::move(l), std::move(v)); }

Page &OverlayMenu::separator() { return root_.separator(); }

Page &OverlayMenu::submenu(std::string l, std::string d)
{ return root_.submenu(std::move(l), std::move(d)); }

void OverlayMenu::clear()
{
    stack_.clear();
    root_.entries_.clear();
    root_.children_.clear();
    root_.sel_ = 0;
    /* The content changes entirely: the focus rectangle no longer has a row to
     * follow, so it must be PLACED again on the first frame rather than slide
     * in from a position that means nothing any more. */
    anim_placed_ = false;
}

Page       &OverlayMenu::current()       { return stack_.empty() ? root_ : *stack_.back(); }
const Page &OverlayMenu::current() const { return stack_.empty() ? root_ : *stack_.back(); }

/* ------------------------------------------------------------ navigation */

int Page::firstSelectable(int from, int dir) const
{
    const int n = (int)entries_.size();
    if (n == 0) return -1;
    for (int step = 0; step < n; step++) {
        int i = from + dir * step;
        if (i < 0 || i >= n) break;
        if (entries_[i]->selectable()) return i;
    }
    return -1;
}

/* Next selectable entry in direction `dir`, or -1.
 *
 * The cursor STOPS at the ends, it does not wrap. That is the menu's original
 * behaviour and it is kept on purpose: the last entry of the root page is
 * "Quit", a destructive action shown in red. Wrapping would mean that one "up"
 * press too many from "Resume" - the reflex gesture when you are looking for
 * the top of the list - would land on it. In a menu where everything fits on
 * screen, stopping costs nothing and avoids that trap.
 *
 * The `tries` loop is a TERMINATION guard: at either end `nav_deplacer` returns
 * the current index, and with no bound we would spin forever on a non-selectable
 * entry. */
int Page::nextSelectable(int dir) const
{
    nav_t n;
    n.nb        = (int)entries_.size();
    n.focus     = sel_;
    n.scroll    = 0.0f;   /* the menu does not scroll: it all fits in the panel */
    n.view_h     = 0.0f;
    n.content_h = 0.0f;

    int i = nav_clamp_focus(&n);
    if (i < 0) return -1;

    for (int tries = 0; tries < n.nb; tries++) {
        n.focus = i;
        i = nav_move(&n, dir);
        if (i < 0) return -1;
        if (entries_[(size_t)i]->selectable()) return i;
    }
    return -1;
}

void OverlayMenu::open()
{
    open_ = true;
    stack_.clear();
    int i = root_.firstSelectable(0, +1);
    root_.sel_ = (i >= 0) ? i : 0;
    /* Opening is not a cursor move: without this reset the highlight would
     * slide in from the row it was on when the menu was last closed, and the
     * menu's first frame would show a movement nobody asked for. The fade-in
     * restarts here too. */
    anim_placed_ = false;
}

void OverlayMenu::close()
{
    open_ = false;
    /* Next opening starts from the root: reopening the menu deep inside some
     * obscure submenu would be disorienting. */
    stack_.clear();
    anim_placed_ = false;
}

void OverlayMenu::up()
{
    if (!open_) return;
    Page &p = current();
    const int i = p.nextSelectable(-1);
    if (i >= 0) p.sel_ = i;
}

void OverlayMenu::down()
{
    if (!open_) return;
    Page &p = current();
    const int i = p.nextSelectable(+1);
    if (i >= 0) p.sel_ = i;
}

void OverlayMenu::left()
{
    if (!open_) return;
    Page &p = current();
    if (p.sel_ >= 0 && p.sel_ < (int)p.entries_.size()) p.entries_[p.sel_]->step(-1);
}

void OverlayMenu::right()
{
    if (!open_) return;
    Page &p = current();
    if (p.sel_ >= 0 && p.sel_ < (int)p.entries_.size()) p.entries_[p.sel_]->step(+1);
}

void OverlayMenu::activate()
{
    if (!open_) return;
    Page &p = current();
    if (p.sel_ < 0 || p.sel_ >= (int)p.entries_.size()) return;

    MenuEntry &e = *p.entries_[p.sel_];
    if (Page *child = e.opens()) {
        int i = child->firstSelectable(0, +1);
        child->sel_ = (i >= 0) ? i : 0;
        stack_.push_back(child);
        return;
    }
    /* The entry may ask for the menu to close; read its answer before touching
     * anything else. */
    if (e.activate()) close();
}

bool OverlayMenu::back()
{
    if (!open_ || stack_.empty()) return false;
    stack_.pop_back();
    return true;
}

std::string OverlayMenu::breadcrumb() const
{
    std::string s = root_.title();
    for (const Page *p : stack_) s += "  >  " + p->title();
    return s;
}

/* --------------------------------------------------------------- render */

/* === Overlong text (2026-08-27) ===
 * Labels and descriptions used to overflow their row: the text ran over the
 * value on the right, then outside the menu frame entirely.
 *
 * The rule now lives in ui::paint (textWidth / tronque / texteBorne) and
 * this file CALLS it. It used to keep its own copy, written the same day: two
 * copies of one rule drift apart - one day we would have fixed the UTF-8 cut
 * on one side only, and the menu would have shown replacement characters where
 * the machine list showed none. The version in paint is word for word the one
 * that was here, with a single improvement: it INTERSECTS the scissor instead
 * of replacing it, which is strictly better here (the menu sets none, so the
 * behaviour is identical) and necessary for its other caller.
 *
 * Both behaviours are unchanged:
 *   - SELECTED row: the text SCROLLS, pausing at each end;
 *   - other rows: cut with an ellipsis, because scrolling the whole page at
 *     once would make it unreadable.
 */

void OverlayMenu::draw(NVGcontext *vg, float x, float y, float width, float height)
{
    if (!vg || !open_) return;
    const Page &page = current();
    if (page.entries_.empty()) return;

    /* ONE clock reading per frame, handed to everything that moves: the focus
     * rectangle, the page fade and the scrolling texts. Two readings drift by a
     * frame, and that shows up precisely on a highlight meant to track a row. */
    const double tNow = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    /* The veil covers the whole view, at the original scale: scaling it with
     * the panel would leave the stream visible around the edges. */
    nvgBeginPath(vg);
    nvgRect(vg, x, y, width, height);
    nvgFillColor(vg, theme::backdrop);
    nvgFill(vg);

    const float k = scale_;
    nvgSave(vg);
    nvgScale(vg, k, k);
    x /= k; y /= k; width /= k; height /= k;

    nvgFontFace(vg, theme::font());

    float contentH = 0.0f;
    for (const auto &e : page.entries_) contentH += rowHeight(*e) + ROW_GAP;

    const float pw = (PANEL_W < width - 40.0f) ? PANEL_W : width - 40.0f;
    const float ph = TITLE_H + contentH + FOOTER_H + PAD;
    const float px = x + (width - pw) / 2.0f;
    const float py = y + (height - ph) / 2.0f;

    nvgBeginPath(vg);
    /* Recorded for touch (see overlay_menu.hpp): the panel here, then each
     * selectable row in the first loop below. */
    panel_x_ = px; panel_y_ = py; panel_w_ = pw; panel_h_ = ph;
    hits_.clear();

    /* === S69 2026-08-27 - SAME SKIN AS THE SCREENS ===
     * This panel was grey (18,20,26) while the screens are midnight blue: you
     * see them seconds apart, and the mismatch was noticeable. It now borrows
     * the same glass. It belongs here especially well: the menu sits ON the
     * video stream, and a translucent panel lets you make out what you just
     * paused. */
    paint::glassPanel(vg, px, py, pw, ph, paint::PANEL_RADIUS);

    const float ix = px + PAD;
    const float iw = pw - PAD * 2.0f;

    /* --- where the focus rectangle belongs on this frame ---
     * Computed BEFORE drawing any content: the focused row decides both the
     * animation's target and the rectangle's colour. */
    float focusY = 0.0f, focusH = 0.0f;
    bool  focusVisible = false;
    {
        float ry = py + TITLE_H;
        for (size_t i = 0; i < page.entries_.size(); i++) {
            const float rh = rowHeight(*page.entries_[i]);
            if ((int)i == page.sel_) {
                /* A page made only of information rows keeps sel_ on a
                 * non-selectable entry: highlighting nothing is better than
                 * putting the marker on something you cannot activate. */
                focusVisible = page.entries_[i]->selectable();
                focusY = ry;
                focusH = rh;
                break;
            }
            ry += rh + ROW_GAP;
        }
    }

    /* --- slide or jump? ---
     * Changing PAGE is not a cursor move: dragging the highlight across the
     * whole panel to get from a root row to a submenu row tells nobody
     * anything - it is no longer tracking anything. So we jump, and the fade
     * covers the jump. The page is identified by its stack depth and its entry
     * count, two INTEGERS - never by its address: see overlay_menu.hpp,
     * `clear()` destroys the child pages and a pointer kept here for this one
     * comparison would be dangling. */
    const int  depth       = (int)stack_.size();
    const int  entryCount  = (int)page.entries_.size();
    const bool pageChanged = (!anim_placed_ || depth != anim_depth_
                                            || entryCount != anim_count_);

    if (focusVisible) {
        if (pageChanged) {
            focus_y_ = anim_fixed(focusY);
            focus_h_ = anim_fixed(focusH);
        } else {
            focus_y_ = anim_towards(&focus_y_, focusY, tNow, ANIM_FOCUS_S);
            focus_h_ = anim_towards(&focus_h_, focusH, tNow, ANIM_FOCUS_S);
        }
    }
    if (pageChanged) {
        /* A light fade on arriving at a page. It carries the CONTENT only: the
         * panel itself stays put, otherwise the frame would flicker on every
         * round trip through the submenus. No slide, no scaling - on a console
         * you watch from a distance, so movement must say where you are, not
         * decorate. */
        const anim_t invisible = anim_fixed(0.0f);
        fade_        = anim_towards(&invisible, 1.0f, tNow, ANIM_FONDU_S);
        anim_depth_  = depth;
        anim_count_  = entryCount;
        anim_placed_ = true;
    }

    nvgSave(vg);
    nvgGlobalAlpha(vg, anim_value(&fade_, tNow));

    /* A breadcrumb rather than a bare title: inside a submenu you must be able
     * to see where you came from without having to remember the path. */
    std::string crumb = breadcrumb();
    nvgFontSize(vg, ui::type::SECTION);
    nvgFillColor(vg, theme::title);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(vg, px + PAD, py + TITLE_H / 2.0f + 4.0f, crumb.c_str(), nullptr);

    /* The content is drawn in THREE passes - backgrounds, focus rectangle,
     * texts - instead of row by row as before. The focus rectangle is animated:
     * while it travels it straddles two rows, so it must come AFTER every
     * background (or the row it is leaving repaints over it) and BEFORE every
     * text (or it erases the text it crosses). Drawing one whole row at a time
     * made that ordering impossible. */

    float iy = py + TITLE_H;
    for (size_t i = 0; i < page.entries_.size(); i++) {
        const MenuEntry &e  = *page.entries_[i];
        const float      ih = rowHeight(e);

        if (!e.selectable() && e.label().empty()) {
            nvgBeginPath(vg);
            nvgMoveTo(vg, ix + 4.0f, iy + ih / 2.0f);
            nvgLineTo(vg, ix + iw - 4.0f, iy + ih / 2.0f);
            nvgStrokeColor(vg, paint::cardBorder);
            nvgStroke(vg);
        } else if (e.selectable()) {
            hits_.push_back(HitBox{ (int)i, ix, iy, iw, ih });
            /* Every row gets the neutral background, including the focused one:
             * the animated rectangle covers it right afterwards. `paint::accent`
             * and `theme::rowDanger` are opaque, so once the movement ends the
             * result is exactly what it was before. */
            nvgBeginPath(vg);
            nvgRoundedRect(vg, ix, iy, iw, ih, 8.0f);
            nvgFillColor(vg, paint::cardBgTop);
            nvgFill(vg);
        }
        iy += ih + ROW_GAP;
    }

    if (focusVisible) {
        const MenuEntry &sel = *page.entries_[(size_t)page.sel_];
        nvgBeginPath(vg);
        nvgRoundedRect(vg, ix, anim_value(&focus_y_, tNow), iw,
                       anim_value(&focus_h_, tNow), 8.0f);
        nvgFillColor(vg, sel.danger() ? theme::rowDanger : paint::accent);
        nvgFill(vg);
    }

    iy = py + TITLE_H;
    for (size_t i = 0; i < page.entries_.size(); i++) {
        const MenuEntry &e  = *page.entries_[i];
        const float      ih = rowHeight(e);
        const bool       focused = ((int)i == page.sel_);

        if (!e.selectable() && e.label().empty()) { iy += ih + ROW_GAP; continue; }

        const bool  hasDesc = !e.description().empty();
        const float labelY  = hasDesc ? iy + 17.0f : iy + ih / 2.0f;

        /* The value is measured BEFORE the label: it is what decides how much
         * room is left on the left. Without that the label was written on top
         * of it, which is what you saw on entries with a long value. */
        const std::string v = e.value();
        float valueW = 0.0f;
        if (!v.empty()) {
            nvgFontSize(vg, ui::type::BODY);
            valueW = paint::textWidth(vg, v.c_str()) + 12.0f;
        }
        const float textX   = ix + 18.0f;
        const float textMax = iw - 18.0f - 18.0f - valueW;

        nvgFontSize(vg, ui::type::BODY);
        nvgFillColor(vg, e.selectable() ? theme::title : theme::label);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        paint::clampedText(vg, textX, labelY, textMax, iy, ih,
                          e.label(), focused, tNow);

        if (hasDesc) {
            nvgFontSize(vg, ui::type::CAPTION);
            nvgFillColor(vg, focused ? theme::value : theme::label);
            paint::clampedText(vg, textX, iy + ih - 16.0f, textMax, iy, ih,
                              e.description(), focused, tNow);
        }

        if (!v.empty()) {
            /* Centred on the ROW, not on the label: on an entry with a
             * description the label is raised to make room for the text below
             * it, and aligning the value on the label made it float too high -
             * very visible on the submenu chevron. */
            nvgFontSize(vg, ui::type::BODY);
            nvgFillColor(vg, focused ? theme::title : theme::value);
            nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
            nvgText(vg, ix + iw - 18.0f, iy + ih / 2.0f, v.c_str(), nullptr);
        }

        iy += ih + ROW_GAP;
    }

    /* The hint follows the page: at the root B closes, inside a submenu it goes
     * back up. Without it you cannot tell whether B will return you to the
     * stream. */
    std::string hint = footer_;
    if (hint.empty())
        hint = tr("menu/hint_nav",
                  tr(atRoot() ? "menu/hint_close" : "menu/hint_back"));
    nvgFontSize(vg, ui::type::CAPTION);
    nvgFillColor(vg, theme::hint);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgText(vg, px + pw / 2.0f, py + ph - FOOTER_H / 2.0f - 4.0f, hint.c_str(), nullptr);

    nvgRestore(vg);   /* fade */
    nvgRestore(vg);   /* scale */
}

/* --- touch: see the note in the header --- */

bool OverlayMenu::containsPoint(float vx, float vy) const
{
    if (!open_ || panel_w_ <= 0.0f) return false;
    /* S98 - same conversion as `hitTest`: the panel is recorded in the scaled
     * coordinate space. Without it, at 75 % a press outside the menu would be
     * taken for one inside, and the other way round. */
    const float k = (scale_ > 0.05f) ? scale_ : 1.0f;
    vx /= k; vy /= k;
    return vx >= panel_x_ && vx <= panel_x_ + panel_w_
        && vy >= panel_y_ && vy <= panel_y_ + panel_h_;
}

int OverlayMenu::hitTest(float vx, float vy) const
{
    if (!open_) return -1;
    /* === S98 2026-08-29 - THE MENU IS SCALED, THE FINGER IS NOT ===
     *
     * `draw` works in a coordinate space divided by `scale_` (the "interface
     * size" setting), and it is in THAT space that the hit boxes are recorded.
     * The finger's coordinates, on the other hand, arrive in screen pixels. At
     * 100 % the two coincide and all was well; at 75 % the gap grows with the
     * distance from the corner - hence a menu that opened a different row, the
     * further off the lower you aimed.
     *
     * Reported verbatim: "I had changed the interface size, I set it back to
     * 100 % and tapping works". The setting was not at fault, it only made a
     * missing conversion visible.
     *
     * The division happens HERE rather than in the caller: `hitTest` is the
     * only entry point into the menu's touch geometry, so it is the only place
     * where the omission cannot happen again. */
    const float k = (scale_ > 0.05f) ? scale_ : 1.0f;
    vx /= k; vy /= k;
    for (const HitBox &h : hits_)
        if (vx >= h.x && vx <= h.x + h.w && vy >= h.y && vy <= h.y + h.h)
            return h.index;
    return -1;
}

bool OverlayMenu::hasDetail() const
{
    if (!open_) return false;
    const Page &p = const_cast<OverlayMenu *>(this)->current();
    if (p.sel_ < 0 || p.sel_ >= (int)p.entries_.size()) return false;
    return p.entries_[p.sel_]->detail() != nullptr;
}

bool OverlayMenu::detail()
{
    if (!open_) return false;
    Page &p = current();
    if (p.sel_ < 0 || p.sel_ >= (int)p.entries_.size()) return false;
    Page *d = p.entries_[p.sel_]->detail();
    if (!d) return false;
    p.entries_[p.sel_]->prepareDetail();
    if (d->entries_.empty()) return false;   /* nothing to show: do not open */
    int i = d->firstSelectable(0, +1);
    d->sel_ = (i >= 0) ? i : 0;
    stack_.push_back(d);
    return true;
}

bool OverlayMenu::selectAt(float vx, float vy)
{
    if (!open_) return false;
    const int idx = hitTest(vx, vy);
    if (idx < 0) return false;
    current().sel_ = idx;
    return true;
}

bool OverlayMenu::tapAt(float vx, float vy)
{
    if (!open_) return false;
    const int idx = hitTest(vx, vy);
    if (idx >= 0) {
        /* Already set by `selectAt` on the press; we redo it here so this
         * function stays correct if it is called on its own. */
        current().sel_ = idx;
        activate();
        return true;
    }
    /* Press inside the panel but not on a row: consumed anyway, otherwise it
     * would pass through the menu to the remote desktop. */
    return containsPoint(vx, vy);
}

}  // namespace ui
