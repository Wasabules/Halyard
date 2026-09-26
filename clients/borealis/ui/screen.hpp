/* ui::ListScreen - a list screen, described by DATA.
 *
 * === WHY THIS FRAMEWORK EXISTS ===
 *
 * Three crashes in a single day, all of the same family: in Borealis,
 * `Application::currentFocus` is a POINTER to a view. The moment a view holding
 * the focus is destroyed, the next virtual call jumps into a freed vtable -
 * Atmosphere reports it as "Instruction Abort". The guards that were supposed
 * to prevent this did nothing: `giveFocus(nullptr)` was a no-op, and
 * `Box::getDefaultFocus()` walks back down through `lastFocusedView`, so
 * "move the focus elsewhere" moved nothing at all. Three defects chained
 * together, and one of them in OUR code.
 *
 * The answer is not to add a fourth guard. It is to delete the category:
 *
 *   - NO view objects. A screen is a `std::vector<Item>`, nothing more.
 *   - NO focus pointer. The focus is an integer INDEX, re-clamped on every
 *     content change. An out-of-range index can be repaired; a dangling
 *     pointer crashes.
 *   - NO implicit lifetime. Nothing is retained between two frames: the screen
 *     is redrawn from its data.
 *
 * This is exactly what has kept `OverlayMenu` (the pause menu) immune since day
 * one, even though it is more complex than the machine list. We generalise what
 * already works.
 *
 * === WHAT THIS FILE DOES NOT DO ===
 *
 * It talks to neither the network nor any thread. A screen RECEIVES its data;
 * who produces it, and from which thread, is none of its business. That is what
 * makes it testable, and what prevents an asynchronous callback from holding a
 * reference to UI - the other half of the same trap.
 *
 * === WHY THIS .hpp STARTS OUT IN C ===
 *
 * Entry kinds made the list NON-UNIFORM: a section separator is not as tall as
 * a machine card, and a settings row without a description is shorter than one
 * with. `nav.h` only covers uniform layout (`nav_y_de_index`,
 * `nav_contenu_h_uniforme`, `nav_index_a_la_position`); a variable-height
 * version is therefore needed, and this is exactly the kind of arithmetic that
 * answers beside what it draws when nobody checks it.
 *
 * So that arithmetic is written in PLAIN C, at the top of the file, outside the
 * `#ifdef __cplusplus`. The price is a deliberate oddity - a `.hpp` a C
 * compiler can read - and the gain is that tests/test_ui_screen.c verifies it
 * offline, the way test_ui_nav.c and test_ui_anim.c do for their own modules:
 * no console, no GL context, no virtual machine. Everything after
 * `#ifdef __cplusplus` (colors, strings, rendering) stays C++.
 *
 * Created 2026-08-27.
 */
#ifndef UI_SCREEN_HPP
#define UI_SCREEN_HPP

#include "nav.h"

/* =========================================================================
 * PURE PART - no global state, no I/O, no allocation, no graphics type.
 * Verified by tests/test_ui_screen.c.
 * ========================================================================= */

/* Kind of an entry. The C++ side turns this into a `ui::Kind` (enum class)
 * just below, with THE SAME VALUES: that bridge is what keeps the pure part
 * verifiable from C without duplicating the list of kinds. */
typedef enum {
    UI_KIND_CARD   = 0,   /* machine: title, subtitle, badge, value */
    UI_KIND_TOGGLE = 1,   /* on/off setting, state carried by the entry */
    UI_KIND_CHOICE   = 2,   /* cycling value: left/right changes it */
    UI_KIND_ACTION  = 3,   /* fires, carries no state */
    UI_KIND_TITLE   = 4    /* section separator: NEVER focusable */
} ui_kind_t;

/* Heights, in pixels. They answer each other (see UI_GAP): changing one without
 * the other makes two entries touch.
 *
 * UI_H_CARD does NOT depend on whether a subtitle is present, unlike the
 * settings rows. That is deliberate: the machine inventory is reloaded
 * periodically and the server does not always fill the subtitle in. A height
 * that followed that field would make the whole list JUMP on every reload,
 * under the user's fingers.
 *
 * UI_H_TITLE (52) is taller than the text it carries: the air above the label
 * IS the section spacing. Putting it in the height rather than in a variable
 * gap keeps a single `gap` for the whole list, hence a single position formula
 * - two formulas always end up diverging. */
/* === A machine card is a BLOCK, not a row (2026-08-27) ===
 * At 84 pixels it had the silhouette of a settings row: "pick your machine" -
 * the application's primary gesture - looked no different from "flip a
 * checkbox". A machine deserves a tile you look at, like a game's cover art.
 * 168 = twice the old value: three still fit on screen at 720p, which keeps the
 * list browsable without scrolling for an ordinary account. */
#define UI_H_CARD        168.0f
#define UI_H_SETTING       60.0f   /* settings row without a description */
#define UI_H_SETTING_DESC  84.0f   /* ... with its dimmed 2nd line */
#define UI_H_TITLE         52.0f
#define UI_GAP             10.0f

/* === PAGE METRICS - shared by EVERY full-screen page (S86) ===
 *
 * They used to live in screen.cpp's anonymous namespace, hence reserved to
 * `ui::ListScreen`. The day a second full-screen page is written - the log
 * viewer - it must place its title and its button hints AT THE SAME heights,
 * otherwise moving from one page to the other makes the header jump a few
 * pixels: the kind of defect you see without being able to name it. Copying the
 * four values would have made them diverge on the first tweak; so they live
 * here, once.
 *
 * SAFE AREA. Many televisions OVERSCAN: they crop a few percent off each edge
 * without saying so. At 24 pixels from the edge a title gets cut - and the
 * defect is INVISIBLE in handheld mode, where the built-in screen shows the
 * whole image. So the margin is widened IN DOCKED MODE only, to ~2.5% of the
 * width, which is what broadcast guidelines recommend. */
#define UI_MARGIN_HANDHELD  24.0f
#define UI_MARGIN_DOCK      48.0f
#define UI_HEADER_H        96.0f   /* title + subtitle */
#define UI_FOOTER_H          44.0f   /* status bar and button hints */

/* === S79 2026-08-29 - THE SECTION RAIL ===
 *
 * A forty-entry settings page browses badly: you know neither where you are nor
 * what is left, and finding a setting again means re-reading everything. The
 * usual answer is a column of sections on the left and the detail on the right;
 * that is what the console itself does.
 *
 * The rail is a SECOND focus zone, and that is all it adds to the model: no
 * view, no pointer, one more index. The list does not know it has shrunk - it
 * simply receives a narrower band.
 *
 * THE TRAP, and the reason for these two functions. The list's horizontal band
 * must have the rail removed from it BOTH when drawing and when hit-testing
 * touch. Two separate computations always diverge: the day one of them changes,
 * a finger placed ON the rail activates the settings row hidden behind it. It
 * is the same mistake ui_index_at_position() documents for the vertical axis,
 * and it is silent - nothing complains, a setting just changes on its own.
 *
 * So they are pure, and verified by tests/test_ui_screen.c. */
#define UI_RAIL_W       268.0f   /* column width */
#define UI_RAIL_SPACING 18.0f   /* between the column and the list */
#define UI_RAIL_H_ITEM   58.0f   /* height of one section row */
#define UI_RAIL_GAP       6.0f

/* Left edge of the list. `rail_w` = 0 when there is no rail. */
static inline float ui_list_x(float cx, float rail_w)
{
    return (rail_w > 0.0f) ? cx + rail_w + UI_RAIL_SPACING : cx;
}

/* Width of the list. It never goes BELOW ZERO: on a narrow window (the desktop
 * build resizes freely) the rail alone can exceed the page, and a negative
 * width propagated into text measurement makes labels spill outside the frame
 * instead of being clipped. */
static inline float ui_list_w(float cw, float rail_w)
{
    if (rail_w <= 0.0f) return cw;
    const float w = cw - rail_w - UI_RAIL_SPACING;
    return w > 0.0f ? w : 0.0f;
}

/* Can an entry take the focus?
 *
 * COUNTER-CASE - THE SECTION SEPARATOR. A section title is decoration: it has
 * neither value nor action. If it could be focused, moving down a settings
 * screen would stop on it every other press and the A button would do nothing -
 * the screen would look broken. So the kind wins over `actionable`: a Titre is
 * never focusable, even if the caller left the flag true (which is its
 * default). */
static inline bool ui_focusable(ui_kind_t kind, bool actionable)
{
    if (kind == UI_KIND_TITLE) return false;
    return actionable;
}

/* Height of an entry given its kind. `has_description` = the entry carries a
 * dimmed 2nd line (the `sous_titre` field). */
static inline float ui_entry_height(ui_kind_t kind, bool has_description)
{
    switch (kind) {
        case UI_KIND_TITLE: return UI_H_TITLE;
        case UI_KIND_CARD: return UI_H_CARD;   /* see the comment above */
        default:             return has_description ? UI_H_SETTING_DESC
                                                    : UI_H_SETTING;
    }
}

/* Total content height, to be stored in `nav_t::contenu_h`.
 *
 * COUNTER-CASE - THE EXTRA SPACE AT THE END. There are `nb - 1` gaps, not `nb`:
 * counting a gap after the last entry lets the list scroll `gap` pixels into
 * the void, and the last element lifts off the bottom of the frame at the end
 * of travel - which reads as a missing element. Same trap, and same fix, as
 * nav_contenu_h_uniforme(). */
static inline float ui_content_h(const float *heights, int nb, float gap)
{
    if (!heights || nb <= 0) return 0.0f;

    float total = 0.0f;
    for (int i = 0; i < nb; i++) total += heights[i];
    return total + (float)(nb - 1) * gap;
}

/* Top position of entry `index`, in CONTENT coordinates.
 *
 * `index` is CLAMPED before the sum, never used raw. That is the same
 * discipline as nav_borner_focus() and for the same reason: the list may have
 * shrunk since the last move (background reload) without anyone being at fault,
 * and a stale index must RETURN a position, not read past the array. */
static inline float ui_y_of_index(const float *heights, int nb, int index,
                                  float gap)
{
    if (!heights || nb <= 0 || index <= 0) return 0.0f;
    if (index > nb) index = nb;

    float y = 0.0f;
    for (int i = 0; i < index; i++) y += heights[i] + gap;
    return y;
}

/* Index of the entry touched at `y_local` (VISIBLE FRAME coordinates: that is
 * what a touch event gives; the function adds the scroll itself). -1 if the
 * finger hit no entry.
 *
 * This is the variable-height version of nav_index_a_la_position(), and it
 * stays EQUIVALENT to it on a uniform list - the test checks that over a whole
 * list, because two geometries that diverge give a list that answers beside
 * what it draws.
 *
 * COUNTER-CASE - THE SPACE BETWEEN TWO CARDS. A finger landing exactly in the
 * void between two cards must trigger nothing: on this screen "activate" boots
 * a virtual machine, an expensive action to launch from a few pixels of
 * imprecision. So we check that the point falls INSIDE an entry and not in the
 * gap preceding it.
 *
 * COUNTER-CASE - NaN. A NaN coordinate fails BOTH `< 0` and `> vue_h`: it goes
 * through a naive clamp untouched. It can come from the finger, but above all
 * from SCROLLING, which is animated: (t - t0) / (t1 - t0) produces 0/0 when two
 * frames carry the same timestamp (applet resume after sleep).
 *
 * The two explicit tests below are deliberately redundant with the loop.
 * Verified by mutation on 2026-08-27: removing them changes NOTHING today,
 * because every comparison against NaN is false and the loop therefore falls
 * out the bottom, returning -1. But that is an accident of form, not a
 * property: written the way nav_index_a_la_position() is - a
 * `(long long)(y / step)` - the same function would convert the NaN to an
 * integer, which is UNDEFINED behaviour. So we keep the guard that survives the
 * rewrite. */
static inline int ui_index_at_position(const nav_t *n, float y_local,
                                         const float *heights, float gap)
{
    if (!n || !heights || n->nb <= 0) return -1;
    if (!(y_local == y_local)) return -1;                /* NaN */
    if (y_local < 0.0f || y_local > n->view_h) return -1;

    const float y = y_local + n->scroll;                 /* -> content coords */
    if (!(y == y)) return -1;                            /* scroll NaN */
    if (y < 0.0f) return -1;

    float top = 0.0f;
    for (int i = 0; i < n->nb; i++) {
        const float h = heights[i];
        if (y < top) return -1;                          /* lands in the gap */
        if (y <= top + h) return i;
        top += h + gap;
    }
    return -1;
}

/* First focusable index starting from `from` (inclusive), in direction `dir`.
 * -1 if none.
 *
 * COUNTER-CASE - THE SCREEN WITH NOTHING FOCUSABLE. A list made only of section
 * titles, or the "no machine" message, returns -1 and NOT 0: 0 would draw a
 * focus outline on a decorative entry, which the A button would then validate.
 * Same rule as nav_borner_focus() on an empty list.
 *
 * `from` is clamped instead of being rejected: this function is called with an
 * index that comes out of a computation, and failing silently (returning -1)
 * would claim "nothing focusable left" while the list is full of it. */
static inline int ui_next_focusable(const unsigned char *focusables,
                                          int nb, int from, int dir)
{
    if (!focusables || nb <= 0) return -1;
    if (dir == 0) dir = 1;
    if (from < 0)      from = 0;
    if (from > nb - 1) from = nb - 1;

    for (int i = from; i >= 0 && i < nb; i += dir)
        if (focusables[i]) return i;
    return -1;
}

/* New focus after moving by `delta` entries, SKIPPING what is not focusable.
 * Returns -1 if nothing is focusable.
 *
 * We count in INDICES and then skip, rather than counting in focusable entries:
 * for delta = +/-1 (the everyday case) the two are identical, and counting in
 * indices keeps navigation in agreement with the geometry that touch and
 * scrolling use.
 *
 * COUNTER-CASE - THE TITLE AT THE END OF THE LIST. If the last entry is a
 * separator, `nav_deplacer` stops on it and the skip "in the direction of
 * travel" finds nothing more. Without the fallback in the OPPOSITE direction,
 * navigation would lock up for good at the bottom of the screen, the focus
 * outline sitting on an entry that does not respond.
 *
 * COUNTER-CASE - THE HUGE DELTA. The computation goes through nav_deplacer(),
 * which sums in `long long`: `focus + delta` in `int` OVERFLOWS for a delta
 * near INT_MAX, and signed overflow is undefined behaviour from which the
 * compiler is entitled to conclude that the following bound always holds. */
static inline int ui_move_focus(const unsigned char *focusables, int nb,
                                    int focus, int delta)
{
    nav_t n;

    if (!focusables || nb <= 0) return -1;

    n.nb        = nb;
    n.focus     = focus;
    n.scroll    = 0.0f;      /* this computation only looks at the focus */
    n.view_h     = 0.0f;
    n.content_h = 0.0f;

    {
        int target = nav_move(&n, delta);
        if (target < 0) return -1;

        if (!focusables[target]) {
            const int dir = (delta >= 0) ? +1 : -1;
            int alt = ui_next_focusable(focusables, nb, target, dir);
            if (alt < 0) alt = ui_next_focusable(focusables, nb, target, -dir);
            target = alt;
        }
        return target;
    }
}

/* =========================================================================
 * C++ PART - screen data and rendering.
 * ========================================================================= */
#ifdef __cplusplus

#include "anim.h"

#include <nanovg.h>

#include <string>
#include <vector>

namespace ui {

/* The same values as `ui_genre_t`: see the `raw()` bridge below. */
enum class Kind {
    Card   = UI_KIND_CARD,
    Toggle = UI_KIND_TOGGLE,
    Choice   = UI_KIND_CHOICE,
    Action  = UI_KIND_ACTION,
    Title   = UI_KIND_TITLE,
};

/* Bridge to the pure part. It exists so the geometry computation stays
 * verifiable from C: without it the table of kinds would have to be duplicated,
 * and two tables end up diverging with nothing to signal it. */
inline ui_kind_t raw(Kind g) { return (ui_kind_t)(int)g; }

/* One list entry. Values only: it copies, compares and is thrown away without
 * consequence. Deliberately no pointer, no callback - the caller finds the
 * element again by its `id`. */
struct Item {
    /* The default is `Carte`: a caller written before the kinds existed - the
     * machine list - keeps behaving identically without being touched. */
    Kind       kind = Kind::Card;

    std::string title;
    /* 2nd line, smaller and dimmed. For a Carte it is the machine's subtitle;
     * for a setting it is ITS DESCRIPTION. One field for both, because it is
     * one and the same visual element: adding a second would give two ways to
     * write the same line, hence two places to fix when it overflows. The
     * presence of this field changes the HEIGHT of settings rows (see
     * ui_entry_height). */
    std::string subtitle;
    std::string value;       /* right-aligned (state, plan, ...) */
    std::string badge;        /* short pill: "on", "off" */
    NVGcolor    badge_tint = nvgRGBA(120, 130, 150, 255);

    /* A non-actionable entry is displayed but never takes the focus: a "no
     * machine" message has no business being selectable. A Kind::Titre is
     * never focusable anyway, whatever is put here. */
    bool        actionable = true;

    /* Visual shading (machine powered on). Changes nothing about navigation. */
    bool        active = false;

    /* --- Kind::Bascule -------------------------------------------------- */

    /* STATE of the setting, and nothing else. Distinct from `active`, which is
     * a rendering shade: conflating the two would mean that switching a setting
     * on also changed the color of its label, and the user would read a
     * malfunction where they had just ticked a box. `toggle()` is what
     * changes it, so that the switch has an animation to play. */
    bool        lit = false;

    /* --- Kind::Choix ---------------------------------------------------- */

    /* The possible values, in the order left/right cycles through them. */
    std::vector<std::string> choice;
    int         choice_index = 0;

    /* APPLICATION-level identifier, chosen by the caller. It is what we return
     * on activation, never an internal index: indices move when the list is
     * reloaded, identifiers do not. */
    int         id = 0;

    /* What gets shown on the right. An out-of-range `choix_index` falls back to
     * `valeur` instead of indexing the vector: the caller fills `choix` and
     * `choix_index` from two different places (a list of options and a setting
     * read back from disk), and nothing guarantees they agree. */
    const std::string &displayedValue() const
    {
        if (!choice.empty() && choice_index >= 0 && choice_index < (int)choice.size())
            return choice[(size_t)choice_index];
        return value;
    }
};

/* A button hint, bottom right: `A  Se connecter` (the reference locale's wording).
 *
 * Borealis used to draw these, in its frame's footer. Taking over the whole
 * screen lost them, and the screen went MUTE about what it can do - a defect
 * reported the first time it ran. On a console this matters more than
 * elsewhere: there is no hover and no context menu, those few words are the
 * ONLY place you learn that a button does something.
 *
 * They are DATA, like everything else: the screen receives them, it does not
 * derive them from its actions. Deriving would require the framework to know
 * Borealis' action system, which is precisely what we are getting rid of. */
struct Hint {
    std::string button;   /* "A", "B", "X", "Y", "+", "-" */
    std::string label;
};

/* === THE PAGE FRAME (S86, 2026-08-29) ===
 *
 * The header and the footer do not belong to the LIST: they belong to the
 * PAGE. As long as `ListScreen` was the only full-screen page the distinction
 * had no consequence. It has one from the second page on - the log viewer,
 * which has no list at all but does have a title and button hints.
 *
 * Copying them over there would have made them diverge on the first tweak, and
 * the divergence SHOWS: the title jumps a few pixels when you move from one
 * page to the other, with no obvious reason. So they live here, and both
 * screens call them.
 *
 * Stateless, like the rest of this frame: the time `t` arrives as a parameter,
 * read once per frame by the caller. */
void drawHeader(NVGcontext *vg, float px, float py, float pw,
                    const std::string &title, const std::string &subtitle,
                    const std::string &info, double t);

/* Footer: the separator line, the button hints on the RIGHT, the status on the
 * left in whatever space they leave.
 *
 * The order is not a detail: the hints are laid out FIRST because their width
 * depends on their text, so it is they that decide how much room is left. The
 * reverse would let the status write over the last hint as soon as it grew
 * longer. */
/* === S97 2026-08-29 - THE FOOTER HINTS ARE TAPPABLE ===
 *
 * "A Connect", "X Settings", "+ Quit" described buttons you had to HAVE.
 * Without a controller - and a touch-only console is a normal way to use it -
 * those actions were reachable by no means at all. So the footer makes them
 * clickable where they are already written, rather than inventing a button bar
 * the screen never asked for.
 *
 * `drawFooter` records the box of each hint; the screen hit-tests it on
 * release and routes the gesture to the SAME function as the physical button -
 * two paths that diverged would end up doing different things. */
struct HintBox { std::string button; float x, y, w, h; };

void drawFooter(NVGcontext *vg, float px, float py_top, float pw, float ph,
                  const std::vector<Hint> &hints,
                  const std::string &status, bool busy, double t,
                  std::vector<HintBox> *boxes = nullptr);

/* One section of the rail (S79). Like everything else: values only, and an
 * APPLICATION-level identifier - indices move when the list of sections
 * changes, identifiers do not. */
struct Section {
    std::string title;
    std::string mention;   /* dimmed 2nd line: a count, a state */
    int         id = 0;
};

class ListScreen {
public:
    /* --- Content ------------------------------------------------------- */

    void setTitle(std::string t)     { title_ = std::move(t); }
    void setSubtitle(std::string s) { subtitle_ = std::move(s); }
    void setInfo(std::string s)      { info_ = std::move(s); }

    /* Bottom bar. `busy` lights the waiting indicator. */
    void setStatus(std::string s, bool busy) { status_ = std::move(s); busy_ = busy; }

    /* Button hints, drawn bottom RIGHT in the given order. The order matters:
     * we read left to right, so the primary action goes last, closest to the
     * edge - that is the console's convention, and following it saves the user
     * from having to relearn it. */
    void setHints(std::vector<Hint> h) { hints_ = std::move(h); }

    /* What we show when the list is EMPTY. Drawn centred, in a panel that takes
     * up the space: one line of text stranded at the top of a 720-pixel screen
     * reads as a stalled load, not as an answer. Empty = nothing is shown at
     * all, which remains the right choice while loading. */
    void setEmptyMessage(std::string m) { message_vide_ = std::move(m); }

    /* Replaces all the content. The focus is RE-CLAMPED, never left out of
     * range: this is where the robustness is won, since this is the moment the
     * old content disappears. We try to keep the focused element by its `id` -
     * otherwise a periodic reload would drag the focus back to the top while
     * the user is navigating. */
    void setItems(std::vector<Item> items);

    /* === TILE layout (2026-08-27) ===
     *
     * A settings list is read top to bottom: each row is wide and short, you
     * read labels. A list of MACHINES is not read the same way - you pick among
     * a handful of items, the way you pick a game. The full-width band gave
     * that choice the silhouette of a checkbox.
     *
     * In tile mode, entries become NEARLY SQUARE blocks laid side by side and
     * centred, browsed left/right. That is the idiom of game libraries, and it
     * says what you are doing.
     *
     * Opt-in explicitly: the framework does not guess. A screen knows whether
     * it is presenting an inventory or settings; the framework does not. */
    void setTileLayout(bool on) { tiles_ = on; }

    /* === Section rail (S79) ==============================================
     *
     * Empty = no rail, and the screen behaves exactly as before. That is what
     * leaves the five other screens untouched.
     *
     * `actif_id` names the open section. An unknown identifier falls back to
     * the first one: the caller rebuilds its sections on every settings change,
     * and nothing guarantees the previous one still exists. */
    void setSections(std::vector<Section> r, int active_id);

    /* Identifier of the open section, or -1 if the rail is empty. */
    int  activeSection() const;

    /* === WHY THE RAIL IS NOT A FOCUS ZONE ===
     *
     * The obvious version makes it a second zone, entered by pressing LEFT and
     * left by pressing RIGHT. It was written, then removed, for a reason that
     * is clearer once stated: on a `Kind::Choix`, left and right ALREADY
     * belong to the value. The same gesture would then mean two things
     * depending on which row you are on and - more insidiously - leaving the
     * rail rightwards would drop the caller onto an entry it would believe had
     * been modified: on the "Language" row, that displayed the restart warning
     * with nothing having changed.
     *
     * So the rail is a TAB BAR laid out vertically: it says where you are, it
     * changes with the triggers or with a tap, and it never takes the focus. No
     * extra zone, no extra state, no ambiguous gesture - and the list's focus
     * does not move out from under the user.
     *
     * Moves the open section. This is what the L and R triggers do: a path
     * available from any row, a Choix included. */
    bool shiftSection(int dir);

    /* Returns the identifier of the section just chosen, or -1, and RESETS.
     * Same discipline as touchRelease(): the caller polls it after rendering
     * and rebuilds its list then, rather than receiving a callback while we are
     * drawing. */
    int  sectionConsumed();

    void clear() { setItems({}); }

    /* --- Reading ------------------------------------------------------- */

    int  focus() const { return focus_; }
    bool empty()  const { return items_.empty(); }
    size_t size() const { return items_.size(); }

    /* Returns nullptr if nothing is focused. The caller MUST test it - this is
     * the only way to get an element, and it cannot lie. */
    const Item *focusedItem() const;

    /* --- Navigation ---------------------------------------------------- */

    bool up()          { return moveBy(-1); }
    bool down()           { return moveBy(+1); }
    bool pageUp()        { return moveBy(-5); }
    bool pageDown()      { return moveBy(+5); }
    bool moveBy(int delta);

    /* Left/right: they only do something on a Kind::Choix, and return false
     * everywhere else. That false is what lets the caller hand the event back
     * to the parent screen (leaving the page towards a neighbouring tab, for
     * instance) instead of swallowing it silently. */
    bool left()        { return cycle(-1); }
    bool right()        { return cycle(+1); }

    /* Toggles the focused entry if it is a Kind::Bascule; false otherwise.
     * The state lives in the Item (`allume`) - the caller reads it back through
     * focusedItem() afterwards. Going through this method rather than writing
     * the field directly is what gives the switch its animation. */
    bool toggle();

    /* Touch: returns the `id` of the element touched, or -1. Uses the geometry
     * of the LAST frame drawn - a screen never drawn answers nothing. */
    int  touchAt(float x, float y) const;

    /* === Finger scrolling ================================================
     *
     * Three gestures, in the order they arrive:
     *   `fingerDown`  records the starting point;
     *   `fingerMove`  makes the list follow;
     *   `fingerUp`    decides whether that was a DRAG or a TAP.
     *
     * That distinction is the only delicate point, and it is settled by the
     * threshold: a finger never lands perfectly still, so treating a single
     * pixel as a drag would make selection impossible; conversely, too high a
     * threshold selects the element under the finger by accident at the end of
     * a scroll. `fingerUp` returns the touched `id` ONLY if the finger never
     * crossed the threshold. */
    void fingerDown(float x, float y);
    void fingerMove(float x, float y);
    /* Returns the `id` to activate, or -1 if it was a drag. */
    int  fingerUp(float x, float y);

    /* Returns the `id` of an element the finger just activated, or -1, and
     * RESETS. The caller polls it after rendering: firing an action from
     * `draw()` would make the screen act while it is drawing itself, which is
     * exactly the kind of reentrancy we avoid everywhere else. */
    int  touchRelease();

    /* === S97 - THE FOOTER HINT THAT WAS JUST TAPPED ===
     *
     * Returns the BUTTON label ("A", "X", "+", ...) and RESETS, like
     * `touchRelease`. Empty if nothing was tapped.
     *
     * These hints described buttons you had to HAVE. Without a controller - and
     * a touch-only console is a normal way to use it - "Connect", "Settings"
     * and "Quit" were reachable by no means at all. We make them clickable
     * where they are already written, rather than adding a button bar the
     * screen never asked for. */
    std::string hintReleased();

    /* --- Rendering ----------------------------------------------------- */

    /* `t` is the time in seconds, supplied by the caller: one clock read per
     * frame, otherwise the page's animations drift apart from each other. */
    void draw(NVGcontext *vg, float x, float y, float w, float h, double t);

private:
    std::vector<Item> items_;
    int    focus_   = -1;

    /* Two views DERIVED from `items_`, recomputed in the one function that
     * changes `items_`. They are not session state: they are the arguments the
     * pure part expects, cached so as not to rebuild them sixty times a second.
     * Entries that change afterwards (toggle, cycle) touch neither the kind
     * nor the subtitle, so they cannot make these stale. */
    std::vector<float>         heights_;
    std::vector<unsigned char> focusables_;

    /* Scrolling: a TARGET, and the animated position catching up to it. The
     * distinction matters - see moveBy(), which aims from the target and not
     * from the current position. */
    float  scroll_target_ = 0.0f;

    /* State of the gesture in progress. Numbers only: nothing to free, and an
     * interrupted gesture (finger dragged off screen, applet put to sleep)
     * leaves `finger_down_` stuck true with no consequence - the next gesture
     * resets it. */
    /* A finger on THE RAIL. Distinct from the list gesture: it has neither
     * threshold nor drag, a section is picked with one tap. */
    bool   finger_rail_ = false;
    bool   finger_down_ = false;
    bool   finger_dragged_ = false;   /* the threshold has been crossed */
    float  finger_y0_ = 0.0f;         /* where the finger landed */
    float  finger_x0_ = 0.0f;
    float  scroll_at_press_ = 0.0f;
    float  last_x_ = 0.0f, last_y_ = 0.0f;
    int    pending_touch_ = -1;
    float  scroll_       = 0.0f;

    std::string title_, subtitle_, info_, status_;
    std::vector<Hint> hints_;
    /* S97 - boxes of the footer hints, recorded while drawing. */
    std::vector<HintBox> hint_boxes_;
    std::string          pending_hint_;
    bool                 last_active_ = false;
    /* S100 - the finger is holding the scrollbar. */
    bool                 finger_bar_ = false;
    /* === S104 2026-08-29 - TILES HAVE THEIR OWN GEOMETRY ===
     * In tile mode, entries are HORIZONTAL tiles centred vertically, not
     * stacked rows. Recomputing their position in the touch test would be a
     * second truth that would drift from the first; so we RECORD the rectangles
     * while drawing, as we do for the footer. */
    struct TileBox { int index; float x, y, w, h; };
    std::vector<TileBox> tile_boxes_;
    std::string message_vide_;

    /* --- Rail (S79) ------------------------------------------------------
     * One vector and TWO indices, like the list: the index of the open section,
     * and a zone flag. Nothing that can dangle. */
    std::vector<Section> sections_;
    int    section_index_     = 0;
    int    section_consumed_ = -1;
    float  rail_scroll_        = 0.0f;
    /* Geometry of the last frame, for touch. NUMBERS. */
    float  rail_x_ = 0, rail_y_ = 0, rail_w_ = 0, rail_h_ = 0;
    /* The open section's highlight slides from one section to the next: that is
     * what shows the PATH travelled when you change tab with the triggers. One
     * animated value only - as with the tiles, animating each section
     * separately would let them drift apart. */
    anim_t anim_rail_y_ = anim_fixed(0.0f);

    bool   railMove(int delta);
    /* Index of the section under (x, y), or -1. Uses the geometry of the LAST
     * frame drawn, like touchAt(). */
    int    railAtPosition(float x, float y) const;
    void   drawRail(NVGcontext *vg, float x, float y, float w, float h,
                    double t);

    bool tiles_ = false;
    /* Position of the tile ROW, animated. One value for all of them: animating
     * each tile separately would let them drift apart. */
    anim_t anim_tiles_ = anim_fixed(0.0f);

    void drawTiles(NVGcontext *vg, float x, float y, float w, float h,
                   double t);
    bool   busy_  = false;

    /* Geometry of the last frame: used by touch and by scrolling. These fields
     * are NUMBERS, not views - copying them creates no lifetime dependency. */
    float  view_x_ = 0, view_y_ = 0, view_w_ = 0, view_h_ = 0;
    /* The list's BAND: `list_x_/list_w_` are narrowed by the rail, and it is
     * that band - not the whole screen - that the touch test queries. Keeping
     * them here rather than recomputing them in touchAt() is what guarantees
     * they are the ones of the frame that was DRAWN. */
    float  list_x_ = 0, list_w_ = 0;
    float  list_y_ = 0, list_h_ = 0;

    /* The animations are VALUE FIELDS, not a list to maintain: five numbers and
     * a function of time (see anim.h). There is nothing to remove when they end,
     * and a forgotten animation costs nothing.
     *
     * The focus outline is tracked in CONTENT coordinates (not screen ones):
     * otherwise an in-flight scroll would be subtracted twice and the outline
     * would move against the list. Its HEIGHT is animated too, because entries
     * no longer all have the same one. */
    anim_t anim_foc_y_  = anim_fixed(0.0f);
    anim_t anim_foc_h_  = anim_fixed(UI_H_CARD);
    anim_t anim_scroll_ = anim_fixed(0.0f);

    /* ONE switch animation, plus the index it belongs to. An array of
     * per-entry animations would be exactly the "list of live animations" this
     * frame refuses; and it would be pointless, since only one setting can be
     * toggled at a time - the focused one. The other switches are drawn at
     * their fixed state. */
    anim_t anim_toggle_       = anim_fixed(0.0f);
    int    anim_toggle_index_ = -1;

    /* Navigation state rebuilt AT EVERY use rather than kept in a field: the
     * list may have changed between two calls, and a stale `nav_t` is exactly
     * the kind of state that lies. `scroll` is a parameter because callers do
     * not all need the same one: touch reasons about what the user SEES
     * (animated position), navigation about where the list is GOING (target). */
    nav_t  navState(float scroll) const;

    /* Rebuilds heights_ / focusables_ from items_. */
    void   recomputeGeometry();

    bool   cycle(int dir);

    /* S84 - puts the focus on the entry carrying this identifier. Called when
     * the finger lifts: without it, `activer()` acted on the focused entry and
     * not on the one that had just been touched. */
    void   focusById(int id);

    /* First focusable index starting from `from`, in direction `dir`. */
    int    nextActionable(int from, int dir) const;
};

}  // namespace ui

#endif  /* __cplusplus */
#endif  /* UI_SCREEN_HPP */
