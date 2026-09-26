/* ui::ListScreen - see screen.hpp for why the architecture is what it is, and
 * for the pure arithmetic (kinds, heights, focus) this file merely calls.
 * Here: the data, the events and the rendering.
 */

#include "haptics.hpp"
#include "../../../core/services/journal.h"
#include "screen.hpp"

#include "../device_mode.hpp"
#include "key_hint.hpp"   /* K21: the footer names keys when there is no pad */
#include "utf8.h"
#include "touch.h"    /* direct finger read, see its header */
#include "paint.hpp"
#include "sfx.hpp"
#include "theme.hpp"
#include "type.hpp"

namespace ui {

/* Page metrics. The ENTRY heights, on the other hand, live in screen.hpp
 * (UI_H_*) because the pure part needs them to compute the geometry: having
 * them twice, here and there, would make the list answer beside what it draws
 * on the first divergence. */
namespace {

/* Page metrics - DEFINED IN screen.hpp (see the safe area for televisions that
 * overscan). They were moved up there in S86 because the log viewer must place
 * its header at the same height: two copies would have made the title jump when
 * moving from one page to the other. */
const float MARGIN_HANDHELD = UI_MARGIN_HANDHELD;
const float MARGIN_DOCKED   = UI_MARGIN_DOCK;
const float HEADER_H        = UI_HEADER_H;
const float FOOTER_H        = UI_FOOTER_H;
const float VIEW_MARGIN = 12.0f;  /* breathing room at the scrolling frame edge */
const float PAD_X       = 18.0f;  /* inner edge of a card */
const float RIGHT_GAP   = 14.0f;  /* between two right-aligned elements */

/* Toggle switch. These proportions are those of a switch recognisable from two
 * metres away: the track is nearly twice as wide as the knob, otherwise you read
 * a dot sitting on a bar rather than a two-position mechanism. */
const float SWITCH_W = 52.0f;
const float SWITCH_H = 28.0f;

/* Chevrons of the Kind::Choix. */
const float CHEVRON  = 9.0f;

/* A toggle switch, `p` = 0 (off) to 1 (on).
 *
 * WHY A DRAWING AND NOT A WORD. This is the defect we just fixed elsewhere:
 * with Borealis, a setting's state was a label, and the row's descriptive text
 * ended up writing over it. A shape cannot be overwritten by text, and it reads
 * from a distance, controller in hand, without having to decipher an
 * eight-letter word.
 *
 * `p` is CLAMPED: it comes from an animation, hence from a clock, and a
 * progress value outside [0,1] on applet resume would put the knob outside its
 * track. */
void toggleSwitch(NVGcontext *vg, float x, float y, float w, float h, float p)
{
    p = anim_clamp01(p);

    const float r = h * 0.5f;
    const NVGcolor track_off = nvgRGBA(64, 72, 92, 255);
    const NVGcolor edge_off  = nvgRGBA(126, 138, 164, 130);

    nvgSave(vg);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, r);
    nvgFillColor(vg, nvgLerpRGBA(track_off, paint::accent, p));
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, r);
    nvgStrokeWidth(vg, 1.2f);
    nvgStrokeColor(vg, nvgLerpRGBA(edge_off, paint::accentVif, p));
    nvgStroke(vg);

    /* The knob keeps 3 px of visible track on each side: flush against the
     * edge, it merges with the outline and the two positions look alike. */
    const float margin = 3.0f;
    const float rb     = r - margin;
    const float cxb    = x + margin + rb + p * (w - 2.0f * (margin + rb));

    nvgBeginPath(vg);
    nvgCircle(vg, cxb, y + r, rb);
    nvgFillColor(vg, nvgLerpRGBA(nvgRGBA(198, 206, 222, 255),
                                 nvgRGBA(255, 255, 255, 255), p));
    nvgFill(vg);

    nvgRestore(vg);
}

/* A chevron. `dir` = -1 points left, +1 points right. */
void chevron(NVGcontext *vg, float cx, float cy, float size, int dir,
             NVGcolor tint)
{
    const float tip  = cx + (float)dir * size * 0.5f;
    const float back = cx - (float)dir * size * 0.5f;

    nvgSave(vg);
    nvgLineCap(vg, NVG_ROUND);
    nvgLineJoin(vg, NVG_ROUND);
    nvgBeginPath(vg);
    nvgMoveTo(vg, back, cy - size * 0.62f);
    nvgLineTo(vg, tip,  cy);
    nvgLineTo(vg, back, cy + size * 0.62f);
    nvgStrokeWidth(vg, 2.2f);
    nvgStrokeColor(vg, tint);
    nvgStroke(vg);
    nvgRestore(vg);
}

/* === S78 2026-08-29 - WHO HAS A STATE TO SHOW? ===
 *
 * Two distinct questions, long conflated into a single `vivante` variable:
 *
 *   "should the background be lit?"  -> isLit()
 *   "does it need a status dot?"     -> hasStatusDot()
 *
 * A setting has no state of that kind - its switch, its choice or its label
 * already say it - and the grey dot it used to carry read as "unavailable".
 *
 * Same reasoning for the background: a settings row keeps a SOLID background
 * whatever its state. Darkening it when the toggle is off gave an ordinary
 * settings page - half the toggles are off by default - the look of a
 * half-disabled page.
 *
 * === S84 2026-08-29 - AND THE MACHINE DOES NOT HAVE ONE EITHER ===
 *
 * S78 had kept the dot on machine cards, because a machine really is running or
 * not. Yet it was permanently GREY on screen, for a simple reason:
 * `VmRow::allumee` and `VmRow::badge` **are filled in nowhere**.
 * `vm_list_activity.cpp` only sets `id`, `nom` and `sous_titre`; the two other
 * fields exist, are copied into the item, drive the rendering - and always hold
 * their default value.
 *
 * So the dot was not showing a state: it was showing the ABSENCE of any
 * computation of that state, in a form that reads as "off". Same family as V12,
 * where five displayed counters were never written.
 *
 * We do not replace it with a guess derived from `vm.state` - we do not know
 * the full set of values that field takes. The machine's state is already
 * spelled out in the card's subtitle. One way of saying a thing beats two, one
 * of which is wrong. */
bool isLit(const Item &it)
{
    (void)it;
    return true;
}

bool hasStatusDot(const Item &it)
{
    (void)it;
    return false;
}

/* Badge then value, right-aligned. Returns the new available right edge.
 *
 * Measuring the right side BEFORE writing the title is what lets the title be
 * CLIPPED rather than write over the state - the defect fixed in the pause
 * menu. */
float drawBadgeAndValue(NVGcontext *vg, const Item &it, float right,
                        float cy, bool foc)
{
    if (!it.badge.empty()) {
        const float bw = paint::badgeWidth(vg, it.badge.c_str());
        paint::badge(vg, right - bw, cy - paint::BADGE_HEIGHT / 2.0f,
                     it.badge.c_str(), it.badge_tint);
        right -= bw + RIGHT_GAP;
    }
    if (!it.value.empty()) {
        nvgFontSize(vg, ui::type::SECONDARY);
        nvgFillColor(vg, foc ? theme::title : theme::value);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgText(vg, right, cy, it.value.c_str(), nullptr);
        right -= paint::textWidth(vg, it.value.c_str()) + RIGHT_GAP;
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    }
    return right;
}

}  // namespace

/* ----------------------------------------------------------------- state */

nav_t ListScreen::navState(float scroll) const
{
    nav_t n;
    n.nb        = (int)items_.size();
    n.focus     = focus_;
    n.scroll    = scroll;
    n.view_h     = list_h_;
    n.content_h = ui_content_h(heights_.data(), (int)heights_.size(), UI_GAP);
    return n;
}

void ListScreen::recomputeGeometry()
{
    heights_.resize(items_.size());
    focusables_.resize(items_.size());

    for (size_t i = 0; i < items_.size(); i++) {
        const Item &it = items_[i];
        heights_[i]    = ui_entry_height(raw(it.kind), !it.subtitle.empty());
        focusables_[i] = ui_focusable(raw(it.kind), it.actionable) ? 1u : 0u;
    }
}

int ListScreen::nextActionable(int from, int dir) const
{
    return ui_next_focusable(focusables_.data(), (int)focusables_.size(),
                                   from, dir);
}

/* --------------------------------------------------------------- content */

void ListScreen::setItems(std::vector<Item> items)
{
    /* We remember the focused IDENTIFIER, not its index: the list is reloaded
     * periodically, and falling back on the index would move the focus
     * elsewhere as soon as a machine appears or disappears above it. The user
     * would see their selection jump on its own. */
    int focused_id = -1;
    if (focus_ >= 0 && focus_ < (int)items_.size())
        focused_id = items_[(size_t)focus_].id;

    items_ = std::move(items);
    recomputeGeometry();

    int fresh = -1;
    if (focused_id >= 0) {
        for (size_t i = 0; i < items_.size(); i++)
            if (items_[i].id == focused_id && focusables_[i]) {
                fresh = (int)i;
                break;
            }
    }
    /* Gone (or first time): we take the first focusable entry. A list made only
     * of non-focusable entries - a "no machine" message, or a page that is
     * nothing but section titles - returns -1, and nothing is drawn as
     * focused. */
    if (fresh < 0) fresh = nextActionable(0, +1);
    focus_ = fresh;

    /* The focus outline animation is RESET FLAT, not replayed: after a reload
     * the focused entry may have moved ten rows, and watching it slide across
     * the page reads as navigation the user did not ask for. The scroll, on the
     * other hand, keeps catching up to its target: it only moved if the content
     * changed height, and the movement is then short and explainable. */
    if (focus_ >= 0) {
        anim_foc_y_ = anim_fixed(ui_y_of_index(heights_.data(), (int)heights_.size(),
                                              focus_, UI_GAP));
        anim_foc_h_ = anim_fixed(heights_[(size_t)focus_]);
    }

    /* The switch being animated belonged to the OLD list: its index no longer
     * designates anything reliable. We forget it; the switches then redraw at
     * their fixed state, which is always correct. */
    anim_toggle_index_ = -1;

    /* The content changed height: a scroll offset valid for the old list may
     * point into the void. */
    const nav_t n = navState(scroll_target_);
    scroll_target_ = nav_scroll_clamp(&n, scroll_target_);
}

const Item *ListScreen::focusedItem() const
{
    if (focus_ < 0 || focus_ >= (int)items_.size()) return nullptr;
    return &items_[(size_t)focus_];
}

/* ------------------------------------------------------------------ rail */

void ListScreen::setSections(std::vector<Section> r, int active_id)
{
    sections_ = std::move(r);

    /* We find the section by its IDENTIFIER, never by its index: the caller
     * rebuilds its sections on every settings change, and a conditional section
     * appearing above would shift everything. */
    int idx = -1;
    for (size_t i = 0; i < sections_.size(); i++)
        if (sections_[i].id == active_id) { idx = (int)i; break; }
    /* Unknown: the first one. Returning -1 would leave the screen with no open
     * section, hence no content, which reads as a broken page. */
    if (idx < 0) idx = 0;
    section_index_ = sections_.empty() ? 0 : idx;
}

int ListScreen::activeSection() const
{
    if (sections_.empty()) return -1;
    const int i = (section_index_ < 0) ? 0
                : (section_index_ >= (int)sections_.size())
                      ? (int)sections_.size() - 1
                      : section_index_;
    return sections_[(size_t)i].id;
}

int ListScreen::sectionConsumed()
{
    const int id = section_consumed_;
    section_consumed_ = -1;
    return id;
}

/* Moves the open section WITHOUT moving the focus zone.
 *
 * We STOP at the ends instead of wrapping. A rail is a hierarchy, not a
 * carousel: silently returning to the top after the last section feels like
 * having missed something. Same rule as moveBy() on the inventory, and for
 * the same reason. */
bool ListScreen::railMove(int delta)
{
    const int nb = (int)sections_.size();
    if (nb <= 0) return false;

    nav_t n;
    n.nb = nb; n.focus = section_index_;
    n.scroll = 0.0f; n.view_h = 0.0f; n.content_h = 0.0f;

    const int target = nav_move(&n, delta);
    if (target < 0 || target == section_index_) return false;

    section_index_     = target;
    section_consumed_ = sections_[(size_t)target].id;
    sfx::play(sfx::Sound::Section);
    return true;
}

bool ListScreen::shiftSection(int dir)
{
    return railMove(dir);
}

int ListScreen::railAtPosition(float x, float y) const
{
    /* Never drawn: we answer nothing rather than answering at random. Same rule
     * as touchAt(). */
    if (sections_.empty() || rail_w_ <= 0.0f || rail_h_ <= 0.0f) return -1;
    if (x < rail_x_ || x > rail_x_ + rail_w_) return -1;

    nav_t n;
    n.nb        = (int)sections_.size();
    n.focus     = section_index_;
    n.scroll    = rail_scroll_;
    n.view_h     = rail_h_;
    n.content_h = nav_content_h_uniform(n.nb, UI_RAIL_H_ITEM, UI_RAIL_GAP);

    return nav_index_at_position(&n, y - rail_y_, UI_RAIL_H_ITEM, UI_RAIL_GAP);
}

/* ------------------------------------------------------------ navigation */

bool ListScreen::moveBy(int delta)
{
    if (items_.empty()) return false;

    const int nb = (int)items_.size();

    /* Stops at the ends and skips section titles: on an inventory, wrapping
     * returns to the top without warning and a user holding the direction never
     * sees the end go by. The pause menu does wrap - everything there fits on
     * screen. */
    const int target = ui_move_focus(focusables_.data(), nb, focus_, delta);
    if (target < 0 || target == focus_) {
        /* === S88 - THE SOUND FOLLOWS THE RESULT, NOT THE GESTURE ===
         * Having nothing beyond is not an error: it is an answer, and it
         * deserves to be heard. Without it, holding the direction at the bottom
         * of a list produces NOTHING - the user cannot tell whether they are at
         * the end or the device has stopped responding. */
        sfx::play(sfx::Sound::NavigationEdge);
        haptics::play(haptics::Intent::Limit);   /* S105: the end stop is FELT */
        return false;
    }

    focus_ = target;
    sfx::play(sfx::Sound::Navigation);

    /* We aim from the scroll TARGET, not from the animated position. Two quick
     * presses would otherwise query a frame that has not arrived yet: the
     * function would answer "already visible" for an entry that will no longer
     * be once the movement ends, and the second card would end up off screen.
     *
     * `nav_scroll_pour_rendre_visible` does not move if the entry is already
     * there: no jitter on every press. */
    const nav_t n = navState(scroll_target_);
    scroll_target_ = nav_scroll_to_reveal(
        &n, ui_y_of_index(heights_.data(), nb, focus_, UI_GAP),
        heights_[(size_t)focus_], VIEW_MARGIN);
    return true;
}

/* In tile mode, left/right MOVE the selection: that is the reading direction of
 * a row. In list mode they cycle the focused entry's value. The same gesture
 * cannot mean two things, so the layout decides. */
bool ListScreen::cycle(int dir)
{
    if (tiles_) return moveBy(dir);

    if (focus_ < 0 || focus_ >= (int)items_.size()) return false;

    Item &it = items_[(size_t)focus_];
    if (it.kind != Kind::Choice) return false;

    /* We reuse nav.h's wrapping rather than writing a modulo here: C's `%`
     * returns a NEGATIVE remainder for a negative dividend, so a hand-written
     * `(idx - 1) % n` would index the vector at -1 on the very first left press
     * on the first choice. The same trap is documented in nav_deplacer_boucle(),
     * which already handles it.
     *
     * Intended side effect: a `choix_index` that is already out of range (the
     * caller fills `choix` and `choix_index` from two different places) is
     * CLAMPED on the way through, hence repaired, instead of being propagated. */
    nav_t n;
    n.nb        = (int)it.choice.size();
    n.focus     = it.choice_index;
    n.scroll    = 0.0f;
    n.view_h     = 0.0f;
    n.content_h = 0.0f;

    const int idx = nav_move_wrap(&n, dir);
    if (idx < 0 || idx == it.choice_index) return false;   /* 0 or 1 choice: nothing to cycle */

    it.choice_index = idx;
    sfx::play(sfx::Sound::Value);
    return true;
}

bool ListScreen::toggle()
{
    if (focus_ < 0 || focus_ >= (int)items_.size()) return false;

    Item &it = items_[(size_t)focus_];
    if (it.kind != Kind::Toggle) return false;

    it.lit = !it.lit;
    sfx::play(it.lit ? sfx::Sound::ToggleOn : sfx::Sound::ToggleOff);
    haptics::play(it.lit ? haptics::Intent::ToggleOn
                              : haptics::Intent::ToggleOff);

    /* If the animation belonged to ANOTHER setting, we reposition it on the
     * state BEFORE this toggle: without that the knob would appear at the
     * previous setting's position before sliding, which reads as if both had
     * changed. If it is the same setting being toggled again mid-travel, we
     * touch nothing - anim_vers() then starts from the CURRENT position, and
     * the knob turns back without jumping (anim.h). */
    if (anim_toggle_index_ != focus_) {
        anim_toggle_index_ = focus_;
        anim_toggle_       = anim_fixed(it.lit ? 0.0f : 1.0f);
    }
    return true;
}

/* === Finger scrolling (see screen.hpp) ===============================
 *
 * The threshold is in PIXELS and not in a fraction of the screen: it is a
 * property of the finger, not of the page. Twelve pixels roughly match the
 * tremor of a hand pressing without meaning to move - below that we made
 * selection impossible, above it we selected by accident at the end of a
 * scroll. */
static const float DRAG_THRESHOLD = 12.0f;

void ListScreen::fingerDown(float x, float y)
{
    finger_down_      = true;
    finger_dragged_   = false;
    finger_x0_        = x;
    finger_y0_        = y;
    scroll_at_press_  = scroll_target_;
}

void ListScreen::fingerMove(float x, float y)
{
    if (!finger_down_) return;

    const float dy = y - finger_y0_;
    const float dx = x - finger_x0_;
    if (!finger_dragged_ && (dy * dy + dx * dx) > DRAG_THRESHOLD * DRAG_THRESHOLD)
        finger_dragged_ = true;
    if (!finger_dragged_) return;

    /* The list follows the finger EXACTLY: an acceleration factor would make
     * the content feel like it slides under the hand instead of being held by
     * it. We subtract because a finger moving down pulls the content up. */
    const nav_t n = navState(scroll_at_press_);
    const float wanted = scroll_at_press_ - dy;
    scroll_target_ = nav_scroll_clamp(&n, wanted);
    /* During the gesture there is NO animation: the position follows the hand
     * frame by frame, and interpolating would add a lag you would feel. */
    scroll_ = scroll_target_;
}

/* S84 - puts the focus on the entry carrying this `id`.
 *
 * By IDENTIFIER and not by index, as everywhere else in this frame: indices
 * move when the list is rebuilt, identifiers do not. An entry that cannot be
 * found or is not focusable moves NOTHING - which is the right behaviour for a
 * finger landing on a section separator. */
void ListScreen::focusById(int id)
{
    for (size_t i = 0; i < items_.size(); i++) {
        if (items_[i].id != id) continue;
        if (i >= focusables_.size() || !focusables_[i]) return;
        if ((int)i == focus_) return;               /* already there: nothing to animate */
        focus_ = (int)i;

        /* The scroll follows, as it does after a d-pad move: an entry touched at
         * the edge of the frame must end up fully visible. */
        const nav_t n = navState(scroll_target_);
        scroll_target_ = nav_scroll_to_reveal(
            &n, ui_y_of_index(heights_.data(), (int)heights_.size(), focus_, UI_GAP),
            heights_[(size_t)focus_], VIEW_MARGIN);
        return;
    }
}

std::string ListScreen::hintReleased()
{
    std::string b;
    b.swap(pending_hint_);
    return b;
}

int ListScreen::touchRelease()
{
    const int id = pending_touch_;
    pending_touch_ = -1;
    return id;
}

int ListScreen::fingerUp(float x, float y)
{
    if (!finger_down_) return -1;
    finger_down_ = false;
    if (finger_dragged_) return -1;   /* that was a scroll, not a selection */
    return touchAt(x, y);
}

int ListScreen::touchAt(float x, float y) const
{
    /* === S104 - IN TILE MODE, THE VERTICAL LIST DOES NOT EXIST ===
     *
     * This test used to query `heights_` and `ui_index_at_position`, that is,
     * the geometry of a STACKED list - while the machine screen draws
     * horizontal, vertically centred tiles. So it was answering about a layout
     * absent from the screen.
     *
     * Exact consequence, measured by the `[S98]` trace over two sessions: with a
     * single machine, the finger activated the card as long as `y_local` stayed
     * under 168 - the height of a list row - and stopped responding beyond,
     * whereas the tile itself extends much further down. "The tap only works on
     * the top of the card" was the exact description of the phenomenon.
     *
     * We now test the rectangles RECORDED WHILE DRAWING. They cannot diverge
     * from what is displayed, which no recomputation can guarantee. */
    if (tiles_) {
        for (const TileBox &b : tile_boxes_)
            if (x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h) {
                if (b.index < 0 || b.index >= (int)items_.size()) return -1;
                if (!focusables_[(size_t)b.index]) return -1;
                return items_[(size_t)b.index].id;
            }
        return -1;
    }

    /* A screen never drawn has no geometry: it answers nothing rather than
     * answering at random. */
    if (list_w_ <= 0.0f || list_h_ <= 0.0f) return -1;
    /* S79 - the list's BAND, not the screen. Without this bound, a finger
     * landing on the rail activated the settings row hidden behind it: the rail
     * is drawn ON TOP, so nothing would have hinted at it. */
    if (x < list_x_ || x > list_x_ + list_w_) return -1;

    /* The finger aims at what the user SEES, hence the ANIMATED scroll position
     * - not its target. Using the target would activate, during the two tenths
     * of a second of catch-up, an entry that is not under the finger. */
    const nav_t n = navState(scroll_);
    const int idx = ui_index_at_position(&n, y - list_y_, heights_.data(), UI_GAP);
    if (idx < 0 || idx >= (int)items_.size()) return -1;
    if (!focusables_[(size_t)idx]) return -1;
    return items_[(size_t)idx].id;
}

/* --------------------------------------------------------------- drawing */

/* === TILE layout ===
 *
 * Nearly square blocks, laid side by side and centred on the chosen element.
 * We do not scroll: we SLIDE the row under a fixed point, which is the
 * difference between browsing a list and picking from a library.
 *
 * The chosen element is larger than its neighbours - that is what gives depth
 * without having to blur anything, and what makes the choice legible at a
 * glance even without reading the names. */
void ListScreen::drawTiles(NVGcontext *vg, float x, float y, float w,
                           float h, double t)
{
    const int nb = (int)items_.size();
    if (nb <= 0) return;

    /* COVER-ART proportions (2 by 3, like a game box) and not a square: that is
     * the silhouette you recognise without reading. Large enough to dominate the
     * screen - this is the application's primary gesture. */
    const float TILE_W = 300.0f, TILE_H = 400.0f, TILE_GAP = 36.0f;
    const float step  = TILE_W + TILE_GAP;
    const float cy    = list_y_ + list_h_ * 0.5f;
    const float center_x = x + w * 0.5f;

    /* The row slides to bring the chosen element to the centre. We animate the
     * position of the ROW, not that of the tiles: a single value to interpolate,
     * and nothing can drift apart between neighbours. */
    const float target = -(float)focus_ * step;
    anim_tiles_ = anim_towards(&anim_tiles_, target, t, ANIM_SCROLL_S);
    const float slide = anim_value(&anim_tiles_, t);

    tile_boxes_.clear();
    for (int i = 0; i < nb; i++) {
        const Item &it = items_[(size_t)i];
        const float vx_center = center_x + slide + (float)i * step;

        /* Off screen: we do not draw. With a step of 268 px, two tiles on each
         * side are enough to fill a 1280-wide screen. */
        if (vx_center < x - step || vx_center > x + w + step) continue;

        const bool foc = (i == focus_);
        /* Distance from the centre drives the size: the chosen tile is full
         * size, its neighbours shrink. The computation goes through the REAL
         * distance and not through the index, so the scale follows the slide
         * instead of snapping on arrival. */
        const float d = fabsf(vx_center - center_x) / step;
        float scale = 1.0f - 0.14f * (d > 1.0f ? 1.0f : d);

        const float vw = TILE_W * scale, vh = TILE_H * scale;
        const float vx = vx_center - vw * 0.5f, vy = cy - vh * 0.5f;

        /* S104 - the exact rectangle of THIS tile, as drawn. That is what the
         * finger sees; so that is what it must hit. */
        tile_boxes_.push_back(TileBox{ i, vx, vy, vw, vh });

        /* S84 - no status dot here either: see isLit(). The machine's state is
         * written in the banner, not encoded in a grey dot. */
        paint::card(vg, vx, vy, vw, vh, foc, true, false, t);

        /* === Monogram: the initial as a watermark ===
         * The body of the tile is empty - the server gives us no artwork per
         * machine. A bare fill reads as an image that failed to load; a large
         * initial reads as a CHOICE. It is the usual solution for libraries
         * without cover art, and it has a practical merit: two machines are
         * told apart at a glance without reading their names.
         *
         * In UTF-8 the first letter can span several bytes - the machine name
         * `Emile` written with its accent is TWO bytes on its E alone: so we
         * copy the first CHARACTER, not the first byte, otherwise we would
         * display a replacement character. */
        if (!it.title.empty()) {
            /* Counter-case: a name starting with an accented capital - the
             * one that broke the monogram - spans TWO bytes on its first
             * letter; cutting at the first byte gives a replacement character.
             * See ui/utf8.h: this computation was redone three times before
             * being filed there. */
            const std::string initial =
                it.title.substr(0, ui_utf8_len(it.title.data(), it.title.size(), 0));

            nvgSave(vg);
            nvgIntersectScissor(vg, vx, vy, vw, vh);
            nvgFontSize(vg, vh * 0.42f);
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            /* Very dim: it occupies the space without competing with the name. */
            nvgFillColor(vg, nvgRGBA(255, 255, 255, foc ? 40 : 26));
            nvgText(vg, vx_center, vy + vh * 0.40f, initial.c_str(), nullptr);
            nvgRestore(vg);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        }

        /* === The name AT THE BOTTOM, on a banner ===
         * It used to float in the middle of an empty tile, which made the tile
         * read as an empty box rather than as cover art. On a sleeve the title
         * sits at the bottom, on a fill that detaches it from the image - even
         * when there is no image yet, that layout says what the tile IS. */
        const float banner_h = vh * 0.22f;
        const float by2 = vy + vh - banner_h;

        nvgSave(vg);
        nvgIntersectScissor(vg, vx, by2, vw, banner_h);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, vx, vy, vw, vh, paint::CARD_RADIUS);
        nvgFillColor(vg, nvgRGBA(6, 10, 20, 190));
        nvgFill(vg);
        nvgRestore(vg);

        nvgFontSize(vg, foc ? ui::type::SECTION : ui::type::BODY);
        nvgFillColor(vg, foc ? theme::title : theme::value);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        paint::clampedText(vg, vx_center, by2 + banner_h * 0.5f, vw - 24.0f,
                          by2, banner_h, it.title, foc, t);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    }
}

/* === S79 - the column of sections ===
 *
 * A tab bar laid out vertically: the open section carries an accent highlight
 * that SLIDES from one row to the next, the others stay muted. The slide is not
 * an ornament - it is what shows that the tab changed and in which direction,
 * information a mere colour change loses entirely when you chain two trigger
 * presses.
 *
 * The rail never takes the focus (see screen.hpp): it therefore has neither
 * outline nor glow, which would say "the cursor is here" while it is in the
 * list. */
void ListScreen::drawRail(NVGcontext *vg, float x, float y, float w, float h,
                          double t)
{
    const int nb = (int)sections_.size();
    if (nb <= 0) return;

    /* === S99 2026-08-29 - THE RAIL HAS A TOP MARGIN, LIKE ITS SIDES ===
     *
     * The accent highlight was already inset by 8 px left and right, but its
     * VERTICAL ORIGIN was the panel's very edge: on the first section it
     * therefore touched the top of the glass, while its two other sides had room
     * to breathe. A single side without a margin is more noticeable than a
     * general absence of margin.
     *
     * The margin is applied to the CONTENT ORIGIN, not to the highlight alone:
     * label drawing, scrolling and the touch test all share it, so they cannot
     * diverge. The usable height loses both margins, otherwise the last section
     * would overflow the bottom by exactly what we just gained at the top. */
    const float RAIL_PAD = 8.0f;
    rail_x_ = x; rail_y_ = y + RAIL_PAD;
    rail_w_ = w; rail_h_ = (h > RAIL_PAD * 2.0f) ? h - RAIL_PAD * 2.0f : h;
    const float ry0 = y + RAIL_PAD;

    /* Clamped every frame, as for the list: the section table is rebuilt on
     * every settings change and may have shrunk. */
    if (section_index_ < 0)   section_index_ = 0;
    if (section_index_ >= nb) section_index_ = nb - 1;

    nav_t n;
    n.nb        = nb;
    n.focus     = section_index_;
    n.scroll    = rail_scroll_;
    n.view_h     = rail_h_;
    n.content_h = nav_content_h_uniform(nb, UI_RAIL_H_ITEM, UI_RAIL_GAP);

    /* The rail scrolls if it does not fit: eight sections fit at 720p, a ninth
     * would not, and a section you cannot reach is worse than a missing one. */
    rail_scroll_ = nav_scroll_clamp(&n, nav_scroll_to_reveal(
        &n, nav_y_of_index(section_index_, UI_RAIL_H_ITEM, UI_RAIL_GAP),
        UI_RAIL_H_ITEM, VIEW_MARGIN));
    n.scroll = rail_scroll_;

    paint::glassPanel(vg, x, y, w, h, paint::PANEL_RADIUS);

    nvgSave(vg);
    nvgIntersectScissor(vg, x, y, w, h);

    /* The accent highlight, at its ANIMATED position, under the labels. */
    anim_rail_y_ = anim_towards(&anim_rail_y_,
                             nav_y_of_index(section_index_, UI_RAIL_H_ITEM,
                                            UI_RAIL_GAP),
                             t, ANIM_FOCUS_S);
    {
        const float by = ry0 + anim_value(&anim_rail_y_, t) - rail_scroll_;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x + 8.0f, by, w - 16.0f, UI_RAIL_H_ITEM,
                       paint::CARD_RADIUS);
        nvgFillColor(vg, nvgRGBA(49, 130, 246, 54));
        nvgFill(vg);

        /* A bright bar FLUSH with the left edge: it is the landmark the eye
         * follows when moving back down from the column to the content. */
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x + 8.0f, by + 10.0f, 3.0f,
                       UI_RAIL_H_ITEM - 20.0f, 1.5f);
        nvgFillColor(vg, paint::accentVif);
        nvgFill(vg);
    }

    nvgFontFace(vg, theme::font());
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

    for (int i = 0; i < nb; i++) {
        const float ry = ry0 + nav_y_of_index(i, UI_RAIL_H_ITEM, UI_RAIL_GAP)
                       - rail_scroll_;
        if (ry + UI_RAIL_H_ITEM < y || ry > y + h) continue;

        const Section &r = sections_[(size_t)i];
        const bool open = (i == section_index_);
        const float tx = x + 26.0f;
        const float maxw = w - 26.0f - 16.0f;
        const float my = r.mention.empty() ? ry + UI_RAIL_H_ITEM * 0.5f
                                           : ry + 20.0f;

        nvgFontSize(vg, ui::type::BODY);
        nvgFillColor(vg, open ? theme::title : theme::label);
        paint::clampedText(vg, tx, my, maxw, ry, UI_RAIL_H_ITEM, r.title,
                          false, t);

        if (!r.mention.empty()) {
            nvgFontSize(vg, ui::type::CAPTION);
            nvgFillColor(vg, open ? theme::sectionT : theme::hint);
            paint::clampedText(vg, tx, ry + 40.0f, maxw, ry, UI_RAIL_H_ITEM,
                              r.mention, false, t);
        }
    }

    nvgRestore(vg);
}

/* === UI7c 2026-09-14 - WHICH PART OF THE LIST DRAW =====================
 *
 * Measured four ways from inside Borealis' frame: the GPU is idle, the swap is
 * 0.1 ms, and 286 ms is spent walking the view tree. Cutting the animated
 * background from 4096 stroked segments to 440 made the flush cheaper (2.1 ->
 * 0.6 ms) and the total WORSE - so the waves were never the dominant cost, and
 * eleven other hypotheses are already dead.
 *
 * So this times the parts of our own list draw rather than reasoning about
 * them. Same switch as the rest, off by default. */
namespace {
inline double ui7c_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
int    ui7c_on = -1;
double ui7c_acc[6] = { 0, 0, 0, 0, 0, 0 };
unsigned ui7c_n = 0;
double ui7c_win = 0.0;
}  // namespace

void ListScreen::draw(NVGcontext *vg, float x, float y, float w, float h, double t)
{
    if (ui7c_on < 0) {
        const char *e = getenv("SHADOW_UI_FRAME_MS");
        ui7c_on = e ? atoi(e) : 0;
        ui7c_win = ui7c_ms();
    }
    const double c0 = ui7c_on ? ui7c_ms() : 0.0;
    view_x_ = x; view_y_ = y; view_w_ = w; view_h_ = h;

    if (paints_bg_) paint::shadowBg(vg, x, y, w, h);
    const double c1 = ui7c_on ? ui7c_ms() : 0.0;
    if (paints_bg_) paint::backgroundWaves(vg, x, y, w, h, t);
    const double c2 = ui7c_on ? ui7c_ms() : 0.0;

    const float MARGIN = device::isDocked() ? MARGIN_DOCKED : MARGIN_HANDHELD;
    const float page_x = x + MARGIN;
    const float page_w = w - MARGIN * 2.0f;

    /* S79 - the rail takes its width out of the list. Both functions come from
     * screen.hpp and are the SAME ones the touch test uses: that is the whole
     * point, a finger landing on the rail cannot activate the row hidden behind
     * it. Tiles keep the whole page: an inventory has no sections. */
    const float rail_w = (!sections_.empty() && !tiles_) ? UI_RAIL_W : 0.0f;
    const float cx = ui_list_x(page_x, rail_w);
    const float cw = ui_list_w(page_w, rail_w);

    /* --- Header ----------------------------------------------------------
     * The header and the footer span the WHOLE PAGE, not the list's band: the
     * screen title and the button hints belong to the page, not to the panel on
     * the right. Aligning them on the list would make them jump 286 pixels
     * depending on whether a rail is present. */
    drawHeader(vg, page_x, y, page_w, title_, subtitle_, info_, t);
    const double c3 = ui7c_on ? ui7c_ms() : 0.0;

    /* --- Scrolling frame -------------------------------------------------- */
    list_x_ = cx;
    list_w_ = cw;
    list_y_ = y + HEADER_H;
    list_h_ = h - HEADER_H - FOOTER_H;
    if (list_h_ < 0.0f) list_h_ = 0.0f;

    /* The rail is drawn BEFORE the list: it occupies its own band and overlaps
     * nothing, but the order stays that of reading - column, then content. When
     * there are no sections, `rail_w_` stays at zero and the rail's touch test
     * answers nothing. */
    if (rail_w > 0.0f)
        drawRail(vg, page_x, list_y_, UI_RAIL_W, list_h_, t);
    else
        rail_w_ = 0.0f;

    /* The focus is re-clamped EVERY FRAME, before drawing the accent: the list
     * may have shrunk since the last move (background reload) without anyone
     * being at fault. This is the primitive that makes an incorrect index
     * harmless where a pointer would have crashed. */
    {
        nav_t n  = navState(scroll_target_);
        focus_   = nav_clamp_focus(&n);
        n.focus  = focus_;

        scroll_target_ = nav_scroll_clamp(&n, scroll_target_);

        /* Re-aiming at the same target restarts nothing (anim.h): calling
         * anim_vers every frame is therefore free, and it is what makes the
         * animation SELF-REPAIRING - if the clamping above just moved the focus
         * or the scroll, the movement follows without anyone having to notify
         * it. */
        anim_scroll_ = anim_towards(&anim_scroll_, scroll_target_, t, ANIM_SCROLL_S);

        /* EVERY intermediate value goes back through nav_scroll_borne. That is
         * where its NaN guard protects us: an animated scroll is exactly the
         * computation (t - t0) / (t1 - t0), and two frames carrying the same
         * timestamp - applet resume after sleep - produce 0/0 in it. A NaN fails
         * BOTH `< 0` and `> max`, so it would go through a naive clamp untouched
         * and the list would disappear without a message. */
        scroll_ = nav_scroll_clamp(&n, anim_value(&anim_scroll_, t));

        if (focus_ >= 0) {
            /* The outline is tracked in CONTENT coordinates: the scroll is
             * subtracted at drawing time, once. */
            anim_foc_y_ = anim_towards(&anim_foc_y_,
                                    ui_y_of_index(heights_.data(), (int)heights_.size(),
                                                  focus_, UI_GAP),
                                    t, ANIM_FOCUS_S);
            anim_foc_h_ = anim_towards(&anim_foc_h_, heights_[(size_t)focus_],
                                    t, ANIM_FOCUS_S);
        }

        if (anim_toggle_index_ >= 0 && anim_toggle_index_ < (int)items_.size()) {
            anim_toggle_ = anim_towards(&anim_toggle_,
                                      items_[(size_t)anim_toggle_index_].lit ? 1.0f : 0.0f,
                                      t, ANIM_FOCUS_S);
        }
    }

    /* === The finger is handled HERE, inside the rendering ===
     * The list geometry has just been computed right above: the gesture
     * therefore answers to THIS frame's layout. Deferring it by one frame would
     * be enough to make it wrong as soon as you have scrolled - correct at rest,
     * offset afterwards, and invisible on a short list. */
    {
        const ui_touch_t d = ui_touch_current();

        /* === S79 - the rail is picked with the finger ===
         * Handled BEFORE the list gesture, and separately: a section is picked
         * with one tap, there is nothing to drag. A finger landing on the rail
         * therefore starts no scroll gesture - otherwise dragging from the
         * column would scroll the list next to it, which makes no visual
         * sense. */
        const bool in_rail = d.active && rail_w_ > 0.0f
                          && d.x >= rail_x_ && d.x <= rail_x_ + rail_w_;
        if (in_rail) {
            finger_rail_ = true;
        } else if (finger_rail_ && !d.active) {
            finger_rail_ = false;
            const int idx = railAtPosition(last_x_, last_y_);
            if (idx >= 0 && idx != section_index_) {
                section_index_     = idx;
                section_consumed_ = sections_[(size_t)idx].id;
                sfx::play(sfx::Sound::Section);
            }
        }

        /* === S100 - THE SCROLLBAR CAN BE GRABBED ===
         * Tested BEFORE the list: the bar is drawn OVER the right edge of the
         * rows, so a finger landing there must grab it rather than activate the
         * row underneath - otherwise you would select an entry while meaning to
         * scroll, which is the opposite of what a scrollbar is for.
         *
         * The hit zone is wider than the bar itself: four pixels cannot be aimed
         * at with a finger. */
        const nav_t nbar = navState(scroll_);
        const bool scrollable = !tiles_ && nbar.content_h > nbar.view_h + 1.0f
                             && list_h_ > 0.0f;
        /* S101 - the hit zone is the WHOLE right margin, from the edge of the
         * cards to the edge of the screen. So it encroaches on nothing: a finger
         * landing there can only be aiming at the bar. That is also what makes
         * it reachable - four pixels cannot be aimed at with a finger. */
        const bool in_bar = d.active && scrollable && !in_rail
                         && d.x >= list_x_ + list_w_
                         && d.y >= list_y_ && d.y <= list_y_ + list_h_;
        if (in_bar || (finger_bar_ && d.active)) {
            finger_bar_ = true;
            /* The thumb CENTRES itself under the finger: you grab a position in
             * the whole, not a point on the thumb. That is what lets you cross a
             * long list in a single gesture. */
            const float HMIN = 34.0f;
            float ch = list_h_ * (list_h_ / nbar.content_h);
            if (ch < HMIN) ch = HMIN;
            const float free_travel = list_h_ - ch;
            float f = (free_travel > 1.0f) ? (d.y - list_y_ - ch * 0.5f) / free_travel : 0.0f;
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            scroll_target_ = nav_scroll_clamp(&nbar, f * (nbar.content_h - nbar.view_h));
            scroll_ = scroll_target_;
            if (d.active) { last_x_ = d.x; last_y_ = d.y; }
            last_active_ = d.active;
        }
        else {
        if (finger_bar_ && !d.active) finger_bar_ = false;

        /* S104 - in tile mode the sensitive band is that of the tiles, not the
         * list's column: without this a finger landing on a tile would not even
         * open a gesture. */
        const bool in_list = d.active && !in_rail
                          && d.x >= list_x_ && d.x <= list_x_ + list_w_
                          && d.y >= list_y_ && d.y <= list_y_ + list_h_;
        if (in_list && !finger_down_)      fingerDown(d.x, d.y);
        else if (d.active && finger_down_ && !in_rail) fingerMove(d.x, d.y);
        else if (!d.active && !finger_down_ && last_active_) {
            /* The finger lifted OUTSIDE the list: it may be a footer hint.
             * Handled separately because the footer does not belong to the list
             * - doing it in the same branch would force us to start a scroll
             * gesture on a zone that has none. */
            for (const HintBox &bb : hint_boxes_)
                if (last_x_ >= bb.x && last_x_ <= bb.x + bb.w
                    && last_y_ >= bb.y && last_y_ <= bb.y + bb.h) {
                    pending_hint_ = bb.button;
                    break;
                }
        }
        else if (!d.active && finger_down_) {
            /* The finger lifted: we record the element to activate, but we do
             * not activate it from here - rendering must not fire actions. The
             * caller picks it up through `touchRelease()`. */
            pending_touch_ = fingerUp(last_x_, last_y_);

            /* === S98 2026-08-29 - WHY ONLY THE TOP OF A CARD RESPONDS ===
             * Reported on the machine screen: the finger activates the card when
             * it touches the upper part, and does nothing lower down. Reading
             * the code was not enough to settle it - the drawing geometry and
             * the test geometry are computed by the SAME functions, with the
             * same scroll. So we record both sides once and for all: if the
             * returned index does not match the card aimed at, the discrepancy
             * will read straight off these numbers. DEBUG level: silent in
             * normal use. */
            if (journal_enabled(JOURNAL_DEBUG, JOURNAL_CAT_UI))
                JOURNAL_DEBUG_(JOURNAL_CAT_UI,
                    "[S98] doigt leve y=%.0f liste_y=%.0f liste_h=%.0f "
                    "y_local=%.0f scroll=%.0f -> id=%d (glisse=%d, %d entrees)",
                    (double)last_y_, (double)list_y_, (double)list_h_,
                    (double)(last_y_ - list_y_), (double)scroll_,
                    pending_touch_, finger_dragged_ ? 1 : 0,
                    (int)items_.size());

            /* === S84 2026-08-29 - THE FINGER MOVES THE FOCUS ===
             *
             * It did NOT, and that defect had two faces.
             *
             * On the settings side, the caller chains `touchRelease()` then
             * `activer()` - and `activer()` acts on the FOCUSED entry, not on
             * the one just touched. So tapping a row opened whichever row
             * already held the cursor. Reported verbatim: "if I tap on a menu
             * but another menu is highlighted, it opens the highlighted one".
             *
             * On the gestures and quality screens it was quieter: their comments
             * claim "touching a row SELECTS it", and nobody had implemented it.
             * The finger did nothing there at all.
             *
             * Fixing it HERE rather than in the four callers is what guarantees
             * that both gestures - the finger and the d-pad - leave the screen
             * in the SAME state. It is also what the hand expects: after
             * touching, the cursor is where you touched. */
            if (pending_touch_ >= 0) focusById(pending_touch_);
        }
        if (d.active) { last_x_ = d.x; last_y_ = d.y; }
        last_active_ = d.active;
        }   /* end of the list gesture, when the bar did not take the finger */
    }

    /* === EMPTY state, drawn (2026-08-27) ===
     * An empty list used to show a single line of text stranded at the top of a
     * 720-pixel page - it read as a stalled load rather than as an answer. So we
     * draw the absence: a centred panel that TAKES UP the space, so the screen
     * looks finished. */
    if (items_.empty() && !message_vide_.empty()) {
        const float pw = cw * 0.6f, ph = 120.0f;
        const float pxx = cx + (cw - pw) * 0.5f;
        const float pyy = list_y_ + (list_h_ - ph) * 0.5f;
        paint::glassPanel(vg, pxx, pyy, pw, ph, paint::PANEL_RADIUS);
        nvgFontSize(vg, ui::type::BODY);
        nvgFillColor(vg, theme::label);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        paint::clampedText(vg, pxx + pw * 0.5f, pyy + ph * 0.5f, pw - 32.0f,
                          pyy, ph, message_vide_, false, t);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    }

    /* In tile mode the row replaces the vertical list. The footer and the header
     * stay identical: it is the same page, not another screen. */
    if (tiles_) {
        drawTiles(vg, x, y, w, h, t);
    } else {
    nvgSave(vg);
    /* The scissor bounds the list's BAND and not the whole screen: without that
     * a card drawn with a negative width (window narrower than the rail) would
     * spill over the column. */
    nvgScissor(vg, cx, list_y_, cw, list_h_);

    const int nb = (int)items_.size();

    /* Three passes, and the order carries the whole effect:
     *   1. the backgrounds, at their exact place;
     *   2. the focus outline, at its ANIMATED position, over the backgrounds;
     *   3. the texts and the controls, over the outline.
     * Doing the texts in pass 1 would let the outline cover them while it
     * slides - the highlighted entry would become unreadable for two tenths of
     * a second on every press. */
    for (int i = 0; i < nb; i++) {
        const Item &it = items_[(size_t)i];
        const float ih = heights_[(size_t)i];
        const float iy = list_y_ + ui_y_of_index(heights_.data(), nb, i, UI_GAP) - scroll_;

        /* Outside the frame: we do not draw. On a short list this changes
         * nothing, but it is what keeps the cost constant if an account exposes
         * fifty machines. */
        if (iy + ih < list_y_ || iy > list_y_ + list_h_) continue;
        if (it.kind == Kind::Title) continue;   /* a separator has no card */

        paint::card(vg, cx, iy, cw, ih, false, isLit(it), hasStatusDot(it), t);
    }

    if (focus_ >= 0 && focus_ < nb) {
        const float fy = list_y_ + anim_value(&anim_foc_y_, t) - scroll_;
        const float fh = anim_value(&anim_foc_h_, t);
        const Item &it = items_[(size_t)focus_];

        if (fh > 0.0f && fy + fh > list_y_ && fy < list_y_ + list_h_)
            paint::card(vg, cx, fy, cw, fh, true, isLit(it), hasStatusDot(it), t);
    }

    for (int i = 0; i < nb; i++) {
        const Item &it = items_[(size_t)i];
        const float ih = heights_[(size_t)i];
        const float iy = list_y_ + ui_y_of_index(heights_.data(), nb, i, UI_GAP) - scroll_;
        if (iy + ih < list_y_ || iy > list_y_ + list_h_) continue;

        const bool  foc = (i == focus_);
        const float cy  = iy + ih * 0.5f;

        nvgFontFace(vg, theme::font());
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

        /* --- Section separator ------------------------------------------- */
        if (it.kind == Kind::Title) {
            /* The label is aligned on the cards' TEXT, not on their edge: a
             * section offset by eighteen pixels from what it announces reads as
             * one more element, not as a heading. */
            const float tx = cx + PAD_X;
            const float ty = iy + ih - 15.0f;   /* the air is ABOVE: that is the section spacing */

            nvgFontSize(vg, ui::type::SECONDARY);
            nvgFillColor(vg, theme::sectionT);
            const std::string label = paint::truncated(vg, it.title, cw - PAD_X * 2.0f);
            nvgText(vg, tx, ty, label.c_str(), nullptr);

            const float end = tx + paint::textWidth(vg, label.c_str()) + 12.0f;
            if (end < cx + cw - 4.0f) {
                nvgBeginPath(vg);
                nvgMoveTo(vg, end, ty + 0.5f);
                nvgLineTo(vg, cx + cw, ty + 0.5f);
                nvgStrokeWidth(vg, 1.0f);
                nvgStrokeColor(vg, nvgTransRGBA(theme::sectionT, 60));
                nvgStroke(vg);
            }
            continue;
        }

        /* --- Right-aligned controls --------------------------------------- */
        float right = cx + cw - PAD_X;

        switch (it.kind) {
            case Kind::Toggle: {
                /* Only one switch is animated at a time - the one just toggled.
                 * The others draw at their fixed state, which avoids keeping one
                 * animation per entry alive (see screen.hpp). */
                const float p = (i == anim_toggle_index_)
                                    ? anim_value(&anim_toggle_, t)
                                    : (it.lit ? 1.0f : 0.0f);
                toggleSwitch(vg, right - SWITCH_W, cy - SWITCH_H * 0.5f,
                             SWITCH_W, SWITCH_H, p);
                right -= SWITCH_W + RIGHT_GAP;
                break;
            }
            case Kind::Choice: {
                const std::string &val = it.displayedValue();
                /* The chevrons only light up on the focused row: they say "here,
                 * left and right do something". Lit everywhere, they would say
                 * nothing at all. */
                const NVGcolor tint = foc ? paint::accentVif
                                          : nvgTransRGBA(theme::hint, 130);

                chevron(vg, right - CHEVRON * 0.5f, cy, CHEVRON, +1, tint);
                right -= CHEVRON + 12.0f;

                nvgFontSize(vg, ui::type::BODY);
                nvgFillColor(vg, foc ? theme::title : theme::value);
                nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
                nvgText(vg, right, cy, val.c_str(), nullptr);
                right -= paint::textWidth(vg, val.c_str()) + 12.0f;
                nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

                chevron(vg, right - CHEVRON * 0.5f, cy, CHEVRON, -1, tint);
                right -= CHEVRON + RIGHT_GAP;
                break;
            }
            default:
                right = drawBadgeAndValue(vg, it, right, cy, foc);
                break;
        }

        /* --- Label and description ---------------------------------------- */
        const float txw = right - (cx + PAD_X);

        /* A setting's label stays at full brightness whatever its state:
         * dimming it when the setting is off would make half a settings page
         * unreadable from a distance, when the label is exactly what you are
         * looking for. Only the Carte keeps the old contrast, where the grey
         * carries the "machine off" information. */
        const bool card_off = (it.kind == Kind::Card) && !it.active;

        /* === The title is placed according to the entry's HEIGHT ===
         * A fixed position 30 px from the top assumed two things: that a
         * subtitle would come underneath, and that the entry was one row tall.
         * Both are false now that machine cards are 168 px BLOCKS: the name
         * floated at the very top of a tile, far from the optical centre.
         * So a card carries its name centred; a settings row keeps its top
         * anchor, because its description must come right below. */
        const bool block = (it.kind == Kind::Card);
        const float ty = block                   ? iy + ih * 0.5f
                       : it.subtitle.empty()   ? iy + ih * 0.5f
                                                 : iy + 30.0f;

        nvgFontSize(vg, it.kind == Kind::Card ? 21.0f : 20.0f);
        nvgFillColor(vg, card_off ? theme::label : theme::title);
        paint::clampedText(vg, cx + PAD_X, ty, txw, iy, ih, it.title, foc, t);

        if (!it.subtitle.empty()) {
            nvgFontSize(vg, ui::type::CAPTION);
            nvgFillColor(vg, theme::label);
            paint::clampedText(vg, cx + PAD_X, iy + 56.0f, txw, iy, ih,
                              it.subtitle, foc, t);
        }
    }

    nvgRestore(vg);
    }

    /* === S100 2026-08-29 - SAY THERE IS MORE, AND LET PEOPLE GO THERE ===
     *
     * A list longer than the screen said so by no sign at all: you only
     * discovered the rest by trying to move down, and nothing showed where you
     * were in the whole. The scrollbar answers both - it appears ONLY when there
     * really is something to scroll, otherwise it would become a permanent
     * ornament that no longer means anything.
     *
     * The thumb has a MINIMUM height: proportional all the way down, a
     * fifty-entry list would reduce it to a hairline you can neither see nor
     * grab. */
    {
        const nav_t nb2 = navState(scroll_);
        /* S104 - never in tile mode: the entries' cumulative height exceeds the
         * view from the sixth machine on, while nothing scrolls VERTICALLY - we
         * would show a bar that commands nothing. */
        if (!tiles_ && nb2.content_h > nb2.view_h + 1.0f && list_h_ > 0.0f) {
            /* === S101 2026-08-29 - THE BAR LIVES IN THE MARGIN, NOT ON THE
             *     CARDS ===
             * Placed inside the list, it overlapped the right edge of the
             * sections. Shrinking the cards to make room would have changed
             * their width depending on whether scrolling is possible - the
             * layout would move when adding one entry, and that shows.
             *
             * But the page already leaves a margin at the screen edge (24 px in
             * handheld mode, 48 docked) and that band belongs to nothing. The
             * bar centres itself in it: the cards keep their width, whatever the
             * length of the list. */
            const float BW = 4.0f, HMIN = 34.0f;
            const float bx = list_x_ + list_w_ + (MARGIN - BW) * 0.5f;
            float ch = list_h_ * (list_h_ / nb2.content_h);
            if (ch < HMIN) ch = HMIN;
            const float travel = nb2.content_h - nb2.view_h;
            float f = (travel > 0.0f) ? scroll_ / travel : 0.0f;
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            const float cy = list_y_ + f * (list_h_ - ch);

            nvgBeginPath(vg);
            nvgRoundedRect(vg, bx, list_y_, BW, list_h_, BW * 0.5f);
            nvgFillColor(vg, nvgRGBA(150, 176, 224, 30));
            nvgFill(vg);

            nvgBeginPath(vg);
            nvgRoundedRect(vg, bx, cy, BW, ch, BW * 0.5f);
            nvgFillColor(vg, finger_bar_ ? paint::accentVif
                                         : nvgRGBA(150, 176, 224, 120));
            nvgFill(vg);
        }
    }

    /* --- Footer: status on the left, button hints on the right ------------ */
    const double c4 = ui7c_on ? ui7c_ms() : 0.0;
    drawFooter(vg, page_x, y + h - FOOTER_H, page_w, FOOTER_H,
                 hints_, status_, busy_, t, &hint_boxes_);
    if (ui7c_on) {
        const double c5 = ui7c_ms();
        ui7c_acc[0] += c1 - c0;   /* shadowBg */
        ui7c_acc[1] += c2 - c1;   /* backgroundWaves */
        ui7c_acc[2] += c3 - c2;   /* header */
        ui7c_acc[3] += c4 - c3;   /* rail + the items */
        ui7c_acc[4] += c5 - c4;   /* footer */
        ui7c_n++;
        if (c5 - ui7c_win >= 5000.0 && ui7c_n > 4) {
            const double n = (double)ui7c_n;
            journal_write(JOURNAL_INFO, JOURNAL_CAT_SYSTEM,
                "[UI7c] liste: fond %.1f + vagues %.1f + entete %.1f + "
                "items %.1f + pied %.1f ms | n=%u",
                ui7c_acc[0]/n, ui7c_acc[1]/n, ui7c_acc[2]/n,
                ui7c_acc[3]/n, ui7c_acc[4]/n, ui7c_n);
            for (int k = 0; k < 6; k++) ui7c_acc[k] = 0.0;
            ui7c_n = 0; ui7c_win = c5;
        }
    }
}

/* ----------------------------------------------------- page frame (S86) */

void drawHeader(NVGcontext *vg, float px, float py, float pw,
                    const std::string &title, const std::string &subtitle,
                    const std::string &info, double t)
{
    nvgFontFace(vg, theme::font());
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

    nvgFontSize(vg, ui::type::SCREEN);
    nvgFillColor(vg, theme::title);
    paint::clampedText(vg, px, py + 38.0f, pw, py, HEADER_H, title, false, t);

    if (!subtitle.empty()) {
        nvgFontSize(vg, ui::type::BODY);
        nvgFillColor(vg, theme::label);
        paint::clampedText(vg, px, py + 68.0f, pw, py, HEADER_H, subtitle, false, t);
    }
    if (!info.empty()) {
        /* === S84 2026-08-29 - THE INFO ALIGNS ON THE TITLE ===
         *
         * It used to sit at the SUBtitle's height, on the right. On the machine
         * screen, the plan line ("Plan : Boost 2025 - actif depuis...")
         * therefore landed thirty pixels below the title `vm/title` ("Mes
         * machines Shadow"), facing nothing: the eye looks for a line and finds
         * none. The quoted values stay French - they are what the reference
         * locale actually holds, and an invented English one greps back to
         * nothing.
         *
         * Both belong to the same header line - the page name on the left, the
         * account state on the right - and that is what the layout now says. The
         * size stays smaller: same line does not mean same importance. */
        nvgFontSize(vg, ui::type::SECONDARY);
        nvgFillColor(vg, theme::sectionT);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgText(vg, px + pw, py + 38.0f, info.c_str(), nullptr);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    }
}

void drawFooter(NVGcontext *vg, float px, float py_top, float pw, float ph,
                  const std::vector<Hint> &hints,
                  const std::string &status, bool busy, double t,
                  std::vector<HintBox> *boxes)
{
    if (boxes) boxes->clear();
    /* A separator line first: without it the footer floats above the background
     * and the eye cannot tell where the content stops. */
    const float py = py_top + ph / 2.0f;
    nvgBeginPath(vg);
    nvgMoveTo(vg, px, py_top);
    nvgLineTo(vg, px + pw, py_top);
    nvgStrokeWidth(vg, 1.0f);
    nvgStrokeColor(vg, nvgRGBA(150, 176, 224, 46));
    nvgStroke(vg);

    /* The font and the alignment are set HERE and not left to the caller: this
     * function runs at the end of rendering, after the content has changed both
     * for its own needs. The footer would then inherit a state that is none of
     * its business - and the defect would stay invisible for as long as the last
     * element drawn happened to leave the right alignment behind. */
    nvgFontFace(vg, theme::font());
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

    /* The hints are laid out FIRST, starting from the right edge and working
     * back: their width depends on their text, so it is they that decide how
     * much room is left. The reverse would let the status write over the last
     * hint as soon as it grew longer - exactly the defect we had fixed on the
     * pause menu rows. */
    float edge = px + pw;
    for (size_t i = hints.size(); i-- > 0; ) {
        const Hint &hh = hints[i];
        /* K21 2026-09-12 - what is DRAWN may be a key name ("Esc"), what is
         * RECORDED stays the logical id ("B"): the touch hit-test below pairs
         * on it, and so do the screens that route a tap to the same function as
         * the physical button. Translating at the call site would have broken
         * that pairing at every one of the fifteen of them. */
        const std::string glyph = hintGlyph(hh.button);
        const float lw = paint::hintWidth(vg, glyph.c_str(), hh.label.c_str());
        /* We only draw what fits. A hint half off the screen is worse than no
         * hint: it reads as a display fault. */
        if (edge - lw < px) break;
        edge -= lw;
        /* The box spans the footer's full height, not the text's: aiming at a
         * 14-point line with a finger demands a precision nobody has, and the
         * dead zone above it belongs to nothing else. */
        if (boxes) boxes->push_back(HintBox{ hh.button, edge, py_top, lw, ph });
        paint::hintButton(vg, edge, py, glyph.c_str(), hh.label.c_str());
        edge -= 22.0f;   /* breathing room between two hints */
    }

    if (!status.empty() || busy) {
        float tx = px;
        if (busy) {
            paint::spinner(vg, px + 10.0f, py, 9.0f, t);
            tx += 30.0f;
        }
        const float avail = edge - 16.0f - tx;
        nvgFontSize(vg, ui::type::SECONDARY);
        nvgFillColor(vg, theme::hint);
        paint::clampedText(vg, tx, py, avail, py_top, ph, status, false, t);
    }
}

}  // namespace ui
