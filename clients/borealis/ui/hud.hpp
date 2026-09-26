/* ui::Hud - the stream's metrics panel.
 *
 * Replaces ~150 lines of `nvgText` calls piled up inside StreamView::draw,
 * where every measurement repeated its own snprintf, its own color and its own
 * vertical offset. Here we only describe WHAT WE MEASURE; the panel takes care
 * of layout, colors and sizing.
 *
 * The user picks which sections are shown - on Switch too, where the screen is
 * small and nobody wants everything, all the time. The selection is a bitmask
 * the caller persists (Settings::hud_sections).
 */
#pragma once

#include <map>
#include <set>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nanovg.h>

#include "theme.hpp"

namespace ui {

/* Buffer refilled every frame. The strings are fixed-size: the panel is redrawn
 * 60 times per second and has no reason to allocate. */
/* === S106 2026-08-29 - EVERY ROW CARRIES AN ID ===
 *
 * To remember "that row, I do not want to see it", the row has to be NAMEABLE.
 * The label will not do: it is translated, so switching language would forget
 * the choices, and rewording a label would forget them too. Neither will the
 * index: some rows only appear under a condition (decode errors are shown only
 * when there are any), so the ranks shift underneath.
 *
 * The id is therefore EXPLICIT and stable. For the forty-one rows that already
 * carried a translation key (`hud/fps`), it is taken from that key as is:
 * nothing to invent, nothing to keep in sync in two places. */
class HudBuilder {
public:
    /* Plain row: "Libelle              valeur". `id` is stable and untranslated -
     * it is what remembers the user's choice. */
    void row(const char *id, const char *label, const char *fmt, ...);
    /* Same, but the value is colored according to how good it is. */
    void graded(const char *id, const char *label, Grade g, const char *fmt, ...);
    /* Same, with a bar under the row - `ratio` is clamped to [0,1]. */
    void gauge(const char *id, const char *label, float ratio, Grade g,
               const char *fmt, ...);

private:
    friend class Hud;

    struct Row {
        char  id[28];
        char  label[28];
        char  value[44];
        Grade grade;
        float ratio;
        bool  hasGauge;
    };

    void push(const char *id, const char *label, const char *value,
              Grade g, float ratio, bool gauge);

    std::vector<Row> rows_;
};

/* A time series: a ring of values, sampled at a fixed rate, drawn as a curve.
 * This is what a counter cannot give you - an average does not say whether the
 * stream is steady or swinging between two extremes. */
class Chart {
public:
    static const size_t CAPACITY = 180;   /* 90 s at 2 Hz */

    Chart(std::string id, std::string title, std::string unit,
          std::function<float()> sample, float floorMax)
        : id_(std::move(id)), title_(std::move(title)), unit_(std::move(unit)),
          sample_(std::move(sample)), floorMax_(floorMax) {}

    const std::string &title() const { return title_; }

private:
    friend class Hud;

    void  push();
    float at(size_t i) const;      /* i = 0 -> oldest */
    size_t size() const { return count_; }
    float  latest() const;
    float  upperBound() const;     /* top of the vertical scale */

    std::string            id_, title_, unit_;
    std::function<float()> sample_;
    float                  floorMax_;              /* smallest scale allowed */
    float                  ring_[CAPACITY] = { 0 };
    size_t                 head_  = 0;
    size_t                 count_ = 0;
};

class Hud {
public:
    /* `id` is a stable key: it is what pins the bit in the mask, so the
     * declaration order can change without invalidating saved preferences. */
    void addSection(std::string id, std::string title,
                    std::function<void(HudBuilder &)> fill);

    /* Charts have their own list and their own mask: you often want a curve
     * without the matching block of numbers, or the other way round. */
    void addChart(std::string id, std::string title, std::string unit,
                  std::function<float()> sample, float floorMax);
    size_t             chartCount() const { return charts_.size(); }
    const std::string &chartTitle(size_t i) const;
    bool               chartEnabled(size_t i) const;
    void               toggleChart(size_t i);
    uint32_t           chartMask() const { return chartMask_; }
    void               setChartMask(uint32_t m) { chartMask_ = m; }

    /* === S106 - THE ROWS OF ONE SECTION ===
     *
     * The catalog holds EVERY row a section has produced, hidden ones included:
     * without them there would be no way to turn a row back on, which would
     * make hiding a one-way trip.
     *
     * It is only filled after a first render - a section does not know its rows
     * until it has been filled. A screen that queries it earlier gets an empty
     * list, which is the honest answer. */
    size_t             rowCount(size_t section) const;
    const std::string &rowLabel(size_t section, size_t i) const;
    bool               rowVisible(size_t section, size_t i) const;
    void               toggleRow(size_t section, size_t i);

    /* Hidden rows, "sectionId/rowId;..." - what the settings keep. */
    std::string        hiddenRows() const;
    void               setHiddenRows(const std::string &s);

    size_t             sectionCount() const { return sections_.size(); }
    const std::string &sectionTitle(size_t i) const;
    bool               sectionEnabled(size_t i) const;
    void               toggleSection(size_t i);

    uint32_t enabledMask() const { return mask_; }
    void     setEnabledMask(uint32_t m) { mask_ = m; }
    bool     visible() const
    {
        return (mask_ != 0 && !sections_.empty())
            || (chartMask_ != 0 && !charts_.empty());
    }

    /* How often the values are refreshed. The measurements are running
     * averages: reformatting them every frame would make the decimals dance and
     * the panel unreadable. So the computed rows are kept for `ms`, which also
     * freezes the panel width. */
    void setRefreshMs(int ms) { refreshMs_ = ms; }

    /* Panel scale factor.
     *
     * Borealis draws in a logical space of fixed width: enlarging the window
     * enlarges everything drawn in it, and the panel ended up covering the same
     * fraction of a screen twice as large. The caller passes the inverse of the
     * window scale here (times the magnification the user picked) so the panel
     * keeps its apparent size. */
    void setScale(float s) { scale_ = (s > 0.05f) ? s : 0.05f; }

    /* Block opacity, in percent (10..100). The panel background is already
     * translucent by construction; this setting multiplies WHAT IS LEFT, text
     * included - a panel whose background alone fades leaves floating text,
     * which is worse than either extreme. */
    void setOpacity(int percent)
    {
        if (percent < 10)  percent = 10;
        if (percent > 100) percent = 100;
        opacity_ = (float)percent / 100.0f;
    }

    /* Draws the enabled blocks, each in its place. */
    void draw(NVGcontext *vg, float x, float y, float width, float height);

    /* === S93 2026-08-29 - BLOCKS YOU PLACE, NOT ONE PANEL ===
     *
     * The panel used to be ONE rectangle in the top right corner, where
     * everything stacked. With four sections that was readable; with per-stage
     * latency, network, audio and the charts it covers half the picture - and
     * you cannot watch the game UNDER your own measurements.
     *
     * So each section becomes its own block, placed wherever you want. Edit
     * mode makes them grabbable by finger; outside that mode they receive no
     * event at all, otherwise you would drag them around while playing.
     *
     * The position is a NORMALIZED pair in [0,1], not pixels: the console draws
     * at 1280x720 in handheld mode and 1920x1080 docked, and a position in
     * pixels would throw the blocks off-screen when the mode changes. */
    void  setEditing(bool on) { editing_ = on; grabbed_ = SIZE_MAX; }
    bool  editing() const     { return editing_; }

    /* Returns true if the gesture was consumed. `phase`: 0 = press,
     * 1 = drag, 2 = release. Coordinates in pixels of the area. */
    bool  touch(float px, float py, int phase,
                  float zx, float zy, float zw, float zh);

    /* Positions, as "id:x,y;..." - what the settings persist. */
    std::string positions() const;
    void        setPositions(const std::string &s);
    void        resetPositions() { anchors_.clear(); leader_.clear(); }

private:
    struct Section {
        std::string                       id;
        std::string                       title;
        std::function<void(HudBuilder &)> fill;
    };

    /* A block = one section title and the slice of rows that follows it.
     * `chart` is SIZE_MAX for a section block, otherwise it is the chart index.
     * `w`/`h` are measured at every refresh: a block sizes itself on ITS OWN
     * content, no longer on the widest of them all. */
    struct Block {
        size_t first, count, section;
        size_t chart = SIZE_MAX;
        float  w = 0, h = 0, x = 0, y = 0;   /* x/y: pixels, computed at draw time */
    };

    /* Normalized anchor of a block, in [0,1]. */
    struct Anchor { float ax, ay; };

    /* === S94 2026-08-29 - BLOCKS THAT SNAP TOGETHER, AND COME APART ===
     *
     * Placing eight blocks one by one gets tedious fast, and the eye wants them
     * grouped anyway: latency WITH network, audio on its own. So two blocks
     * released close enough to each other snap together - they become a single
     * panel and move as one.
     *
     * But snapping without unsnapping is a trap: once two blocks are joined,
     * nothing would let you separate them, and correcting a few pixels would
     * mean resetting everything. Hence the LONG PRESS, which detaches the block
     * under the finger and leaves it exactly where it is.
     *
     * The model is deliberately poor: a block has a LEADER, or it has none. No
     * nested groups, no free ordering - the complexity would cost us at use
     * time without buying a metrics panel anything. */
    /* Snap distance. It used to be 28 px: two blocks set side by side joined
     * without being asked to, and it took a long press to undo a gesture that
     * felt harmless. At 6 px they practically have to overlap - snapping is an
     * INTENT you express, no longer a side effect of tidying up. Edge magnetism
     * keeps its own, wider tolerance: aiming at an edge is not as demanding as
     * aiming at a block. */
    static constexpr float SNAP_PX    = 6.0f;
    static constexpr float MAGNET_PX  = 26.0f;
    static constexpr int64_t LONG_PRESS_MS = 500;
    static constexpr float MOVE_PX    = 10.0f;  /* past this, it is no longer a press */
    static constexpr int64_t DOUBLE_TAP_MS = 320;  /* two taps = hide */

    /* The usable rectangle: the area minus a margin on each edge. A block
     * magnetized to an edge therefore lands there WITH its margin, instead of
     * hugging the pixel - which reads as overflow rather than as a choice.
     * One function for both directions: three conversions copied into three
     * places would have diverged at the first adjustment. */
    static void usableArea(float zx, float zy, float zw, float zh,
                           float bw, float bh,
                           float &ox, float &oy, float &lx, float &ly);

    const std::string &blockId(const Block &b) const;
    Anchor             anchorOf(const Block &b, size_t rank) const;

    void refresh(NVGcontext *vg);

    std::vector<Section> sections_;
    uint32_t             mask_ = 0;
    HudBuilder           builder_;   /* reused from one frame to the next */
    std::vector<Chart>   charts_;
    uint32_t             chartMask_  = 0;
    int64_t              lastSampleMs_ = 0;
    float                scale_      = 1.0f;
    float                opacity_    = 1.0f;
    std::vector<Block>   blocks_;
    std::map<std::string, Anchor> anchors_;
    /* Attached block -> its leader. A block absent from this table stands alone
     * (or is a leader itself) and then carries its own anchor. */
    std::map<std::string, std::string> leader_;
    /* Per-section row catalog: (id, label), all of them, hidden ones included. */
    std::vector<std::vector<std::pair<std::string, std::string>>> catalog_;
    std::set<std::string> hidden_rows_;
    bool                 editing_ = false;
    size_t               grabbed_ = SIZE_MAX;  /* block currently being moved */
    float                grab_dx_ = 0, grab_dy_ = 0;
    float                grab_x0_ = 0, grab_y0_ = 0;  /* initial press point */
    int64_t              grab_ms_ = 0;
    bool                 grab_moved_ = false;
    bool                 grab_detached_ = false;
    std::string          snap_preview_;   /* leader aimed at while dragging */
    /* === S107 2026-08-29 - THE ABSORBED BLOCK SLIDES INTO PLACE ===
     *
     * An instant merge makes a block VANISH from one spot and appear at
     * another: the eye loses the link between the two, and you have to reread
     * the panel to work out what just happened. Making it SLIDE from where it
     * was to its place in the group gives that link through the motion itself.
     *
     * We remember the position BEFORE; the draw pass, which alone knows the
     * position after, derives the offset and absorbs it. The reverse -
     * computing the arrival position at merge time - would mean redoing the
     * layout outside the draw pass, hence maintaining two versions of it. */
    std::map<std::string, std::pair<float, float>> merge_from_;
    int64_t              merge_ms_ = 0;
    int64_t              tap_ms_ = 0;       /* last press, for double-tap detection */
    std::string          tap_id_;
    /* A block was hidden by a double tap, pending the caller saving it: we need
     * to know WHAT to save without the Hud knowing anything about settings. */
    bool                 masks_changed_ = false;

public:
    /* True if a double tap has hidden a block since the last call - to be saved
     * by the caller, who alone knows the settings file. */
    bool takeMaskChanges() { bool v = masks_changed_; masks_changed_ = false; return v; }
private:

    std::string leaderOf(const std::string &id) const;
    /* Leader of the group `block` would merge into if released here, or an
     * empty string. Used TWICE: for the preview while dragging, and for the
     * snap on release. Two separate computations would eventually announce a
     * merge that does not happen. */
    std::string findSnapTarget(size_t block) const;
    void        moveGroup(const std::string &leader, float ax, float ay);
    void        snapIfClose(size_t block);
    void        detach(size_t block, float zx, float zy, float zw, float zh);
    void        drawBlock(NVGcontext *vg, const Block &b);
    int                  refreshMs_  = 500;
    int64_t              lastFillMs_ = 0;
    uint32_t             filledMask_ = 0;
};

}  // namespace ui
