/* ui::Hud - see hud.hpp. */
#include <cstdlib>
#include "haptics.hpp"
#include <cmath>
#include "hud.hpp"
#include "type.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <chrono>

namespace ui {

namespace {

/* Layout metrics. */
const float PAD        = 14.0f;   /* panel inner margin */
const float ROW_H      = 19.0f;
const float GAUGE_H    = 3.0f;
const float GAUGE_GAP  = 5.0f;
const float SECTION_H  = 22.0f;   /* height of a section title */
const float FONT_ROW   = 15.0f;
const float FONT_TITLE = 12.0f;
const float MIN_W      = 300.0f;
const float MARGIN     = 16.0f;   /* distance to the edge of the video area */
const float CHART_H    = 34.0f;   /* height of the curve alone */
const float CHART_HEAD = 18.0f;   /* title line + current value */
const float CHART_GAP  = 6.0f;
const int   SAMPLE_MS  = 500;     /* series sampling rate */

float textW(NVGcontext *vg, const char *s, float size)
{
    if (!s || !*s) return 0.0f;
    nvgFontSize(vg, size);
    float b[4] = {0, 0, 0, 0};
    nvgTextBounds(vg, 0, 0, s, nullptr, b);
    return b[2] - b[0];
}

}  // namespace

/* -------------------------------------------------------------- builder */

void HudBuilder::push(const char *id, const char *label, const char *value,
                      Grade g, float ratio, bool gauge)
{
    Row r;
    snprintf(r.id,    sizeof(r.id),    "%s", id ? id : "");
    snprintf(r.label, sizeof(r.label), "%s", label ? label : "");
    snprintf(r.value, sizeof(r.value), "%s", value ? value : "");
    r.grade    = g;
    r.ratio    = ratio < 0.0f ? 0.0f : (ratio > 1.0f ? 1.0f : ratio);
    r.hasGauge = gauge;
    rows_.push_back(r);
}

void HudBuilder::row(const char *id, const char *label, const char *fmt, ...)
{
    char buf[44];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    push(id, label, buf, Grade::Neutral, 0.0f, false);
}

void HudBuilder::graded(const char *id, const char *label, Grade g, const char *fmt, ...)
{
    char buf[44];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    push(id, label, buf, g, 0.0f, false);
}

void HudBuilder::gauge(const char *id, const char *label, float ratio, Grade g,
                       const char *fmt, ...)
{
    char buf[44];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    push(id, label, buf, g, ratio, true);
}

/* ---------------------------------------------------------------- series */

void Chart::push()
{
    if (!sample_) return;
    ring_[head_] = sample_();
    head_ = (head_ + 1) % CAPACITY;
    if (count_ < CAPACITY) count_++;
}

float Chart::at(size_t i) const
{
    if (i >= count_) return 0.0f;
    /* head_ points at the next write: once the ring is full, the oldest value
     * sits right behind it. */
    size_t start = (count_ == CAPACITY) ? head_ : 0;
    return ring_[(start + i) % CAPACITY];
}

float Chart::latest() const
{
    if (count_ == 0) return 0.0f;
    return ring_[(head_ + CAPACITY - 1) % CAPACITY];
}

float Chart::upperBound() const
{
    /* Scale set on the observed maximum, with a per-series floor: without it a
     * curve flat at zero would jump to full height at the first tremor, and a
     * latency of 2 ms would look catastrophic. */
    float m = floorMax_;
    for (size_t i = 0; i < count_; i++)
        if (ring_[i] > m) m = ring_[i];
    return (m > 0.0f) ? m * 1.15f : 1.0f;
}

/* ------------------------------------------------------------------ hud */

void Hud::addSection(std::string id, std::string title,
                     std::function<void(HudBuilder &)> fill)
{
    /* Past 32 sections the mask would no longer be enough. We will never have
     * that many; better to refuse than to corrupt the preferences. */
    if (sections_.size() >= 32) return;
    Section s;
    s.id    = std::move(id);
    s.title = std::move(title);
    s.fill  = std::move(fill);
    sections_.push_back(std::move(s));
}

void Hud::addChart(std::string id, std::string title, std::string unit,
                   std::function<float()> sample, float floorMax)
{
    if (charts_.size() >= 32) return;
    charts_.push_back(Chart(std::move(id), std::move(title), std::move(unit),
                            std::move(sample), floorMax));
}

const std::string &Hud::chartTitle(size_t i) const
{
    static const std::string empty;
    return i < charts_.size() ? charts_[i].title() : empty;
}

bool Hud::chartEnabled(size_t i) const
{
    return i < charts_.size() && (chartMask_ & (1u << i)) != 0;
}

void Hud::toggleChart(size_t i)
{
    if (i < charts_.size()) chartMask_ ^= (1u << i);
}

const std::string &Hud::sectionTitle(size_t i) const
{
    static const std::string empty;
    return i < sections_.size() ? sections_[i].title : empty;
}

bool Hud::sectionEnabled(size_t i) const
{
    return i < sections_.size() && (mask_ & (1u << i)) != 0;
}

void Hud::toggleSection(size_t i)
{
    if (i < sections_.size()) mask_ ^= (1u << i);
}

namespace {

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

/* === S93 2026-08-29 - EACH BLOCK SIZES ITSELF ON ITS OWN CONTENT ===
 *
 * The old measurement took the width of the widest of ALL blocks and imposed it
 * on every one of them: that was right as long as they lived inside a single
 * rectangle. Once they are placed separately, a block of two short rows has no
 * reason to be as wide as the per-stage latency block. */
void Hud::refresh(NVGcontext *vg)
{
    blocks_.clear();
    builder_.rows_.clear();

    catalog_.assign(sections_.size(), {});
    for (size_t i = 0; i < sections_.size(); i++) {
        if (!sectionEnabled(i) || !sections_[i].fill) continue;
        size_t first = builder_.rows_.size();
        sections_[i].fill(builder_);

        /* === S106 - CATALOG FIRST, FILTER SECOND ===
         * We record EVERY row produced before removing any, otherwise a hidden
         * row would also vanish from the menu that turns it back on. */
        for (size_t r = first; r < builder_.rows_.size(); r++)
            catalog_[i].push_back({ builder_.rows_[r].id, builder_.rows_[r].label });

        /* Drop the hidden rows by compacting in place: a block designates a
         * contiguous SLICE of `rows_`, so there is no other way to remove one
         * from the middle. */
        size_t keep = first;
        for (size_t r = first; r < builder_.rows_.size(); r++) {
            const std::string key = sections_[i].id + "/" + builder_.rows_[r].id;
            if (hidden_rows_.count(key)) continue;
            if (keep != r) builder_.rows_[keep] = builder_.rows_[r];
            keep++;
        }
        builder_.rows_.resize(keep);

        size_t count = builder_.rows_.size() - first;
        if (count == 0) continue;

        float w = MIN_W - PAD * 2, h = SECTION_H;
        for (size_t r = first; r < first + count; r++) {
            const HudBuilder::Row &row = builder_.rows_[r];
            h += ROW_H + (row.hasGauge ? GAUGE_H + GAUGE_GAP : 0.0f);
            float need = textW(vg, row.label, FONT_ROW) + 24.0f
                       + textW(vg, row.value, FONT_ROW);
            if (need > w) w = need;
        }
        Block b{first, count, i};
        b.w = w + PAD * 2;
        b.h = h + PAD * 2 - GAUGE_GAP;
        blocks_.push_back(b);
    }

    /* A chart is a block like any other: we want to be able to drop a lone
     * curve in a corner, without the slab of numbers that goes with it. */
    for (size_t i = 0; i < charts_.size(); i++) {
        if (!chartEnabled(i)) continue;
        Block b{0, 0, SIZE_MAX};
        b.chart = i;
        b.w = 260.0f + PAD * 2;
        b.h = CHART_HEAD + CHART_H + PAD * 2;
        blocks_.push_back(b);
    }
}

size_t Hud::rowCount(size_t section) const
{
    return (section < catalog_.size()) ? catalog_[section].size() : 0;
}

const std::string &Hud::rowLabel(size_t section, size_t i) const
{
    static const std::string empty;
    if (section >= catalog_.size() || i >= catalog_[section].size()) return empty;
    return catalog_[section][i].second;
}

bool Hud::rowVisible(size_t section, size_t i) const
{
    if (section >= catalog_.size() || i >= catalog_[section].size()) return false;
    return hidden_rows_.count(sections_[section].id + "/"
                              + catalog_[section][i].first) == 0;
}

void Hud::toggleRow(size_t section, size_t i)
{
    if (section >= catalog_.size() || i >= catalog_[section].size()) return;
    const std::string key = sections_[section].id + "/" + catalog_[section][i].first;
    if (hidden_rows_.count(key)) hidden_rows_.erase(key);
    else                         hidden_rows_.insert(key);
    /* The next render has to rebuild its blocks: the row slice just changed
     * size, so the geometry did too. */
    blocks_.clear();
}

std::string Hud::hiddenRows() const
{
    std::string out;
    for (const std::string &c : hidden_rows_) out += c + ";";
    return out;
}

void Hud::setHiddenRows(const std::string &s)
{
    hidden_rows_.clear();
    size_t i = 0;
    while (i < s.size()) {
        size_t end = s.find(';', i);
        if (end == std::string::npos) end = s.size();
        if (end > i) hidden_rows_.insert(s.substr(i, end - i));
        i = end + 1;
    }
    blocks_.clear();
}

const std::string &Hud::blockId(const Block &b) const
{
    static const std::string empty;
    if (b.chart != SIZE_MAX) return charts_[b.chart].id_;
    return (b.section < sections_.size()) ? sections_[b.section].id : empty;
}

/* The stored anchor, or the default slot. The default stacks to the right, in
 * declaration order: exactly the old layout, so that upgrading moves nothing
 * until the user touches it. */
/* Returns the STORED anchor, or {-1,-1} when there is none: the draw pass then
 * stacks this block after the previous ones. Mixing "fraction" and "pixels" in
 * the same pair would have made a value whose meaning depends on a threshold -
 * the kind of ambiguity you pay for three months later. */
Hud::Anchor Hud::anchorOf(const Block &b, size_t rank) const
{
    (void)rank;
    auto it = anchors_.find(blockId(b));
    if (it != anchors_.end()) return it->second;
    return Anchor{-1.0f, -1.0f};
}

std::string Hud::positions() const
{
    std::string out;
    for (const auto &kv : anchors_) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s:%.4f,%.4f;",
                 kv.first.c_str(), kv.second.ax, kv.second.ay);
        out += buf;
    }
    /* Group attachments ride the same string, with '>' instead of ':'. One
     * setting rather than two: two strings that must stay consistent always end
     * up not being. */
    for (const auto &kv : leader_) out += kv.first + ">" + kv.second + ";";
    return out;
}

void Hud::setPositions(const std::string &s)
{
    anchors_.clear();
    leader_.clear();
    size_t i = 0;
    while (i < s.size()) {
        size_t end = s.find(';', i);
        if (end == std::string::npos) end = s.size();
        size_t caret = s.find('>', i);
        if (caret != std::string::npos && caret < end) {
            std::string member   = s.substr(i, caret - i);
            std::string leaderId = s.substr(caret + 1, end - caret - 1);
            /* A block cannot be its own leader: such a line comes from a
             * damaged file and would loop forever at draw time. */
            if (!member.empty() && !leaderId.empty() && member != leaderId)
                leader_[member] = leaderId;
            i = end + 1;
            continue;
        }
        size_t colon = s.find(':', i);
        size_t comma = s.find(',', i);
        if (colon != std::string::npos && comma != std::string::npos
            && colon < comma && comma < end) {
            std::string id = s.substr(i, colon - i);
            float ax = strtof(s.c_str() + colon + 1, nullptr);
            float ay = strtof(s.c_str() + comma + 1, nullptr);
            /* A position outside [0,1] comes from a damaged file or an older
             * version: clamp it rather than lose the block off-screen, where a
             * finger could never reach it again. */
            if (ax < 0.0f) ax = 0.0f; if (ax > 1.0f) ax = 1.0f;
            if (ay < 0.0f) ay = 0.0f; if (ay > 1.0f) ay = 1.0f;
            if (!id.empty()) anchors_[id] = Anchor{ax, ay};
        }
        i = end + 1;
    }
}

static float magnetize(float a, float tol);

void Hud::usableArea(float zx, float zy, float zw, float zh,
                     float bw, float bh,
                     float &ox, float &oy, float &lx, float &ly)
{
    ox = zx + MARGIN;
    oy = zy + MARGIN;
    lx = zw - bw - MARGIN * 2.0f;
    ly = zh - bh - MARGIN * 2.0f;
    if (lx < 0.0f) lx = 0.0f;
    if (ly < 0.0f) ly = 0.0f;
}

std::string Hud::leaderOf(const std::string &id) const
{
    auto it = leader_.find(id);
    return (it == leader_.end()) ? id : it->second;
}

/* Moves the LEADER: the members follow at draw time, they have no anchor. */
void Hud::moveGroup(const std::string &leader, float ax, float ay)
{
    anchors_[leader] = Anchor{ax, ay};
}

/* The group `block` would merge into if it were released right now.
 *
 * We compare the RECTANGLES, not the centers: two blocks of very different
 * sizes have distant centers while their edges are already brushing, and the
 * edge is what the eye aims at. */
std::string Hud::findSnapTarget(size_t block) const
{
    if (block >= blocks_.size()) return std::string();
    const std::string myLeader = leaderOf(blockId(blocks_[block]));

    float ax0 = 1e9f, ay0 = 1e9f, ax1 = -1e9f, ay1 = -1e9f;
    for (const Block &b : blocks_) {
        if (leaderOf(blockId(b)) != myLeader) continue;
        if (b.x < ax0) ax0 = b.x;
        if (b.y < ay0) ay0 = b.y;
        if (b.x + b.w > ax1) ax1 = b.x + b.w;
        if (b.y + b.h > ay1) ay1 = b.y + b.h;
    }
    if (ax1 < ax0) return std::string();

    std::string best;
    float best_d = SNAP_PX;
    for (const Block &b : blocks_) {
        const std::string c = leaderOf(blockId(b));
        if (c == myLeader) continue;
        float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
        for (const Block &o : blocks_) {
            if (leaderOf(blockId(o)) != c) continue;
            if (o.x < bx0) bx0 = o.x;
            if (o.y < by0) by0 = o.y;
            if (o.x + o.w > bx1) bx1 = o.x + o.w;
            if (o.y + o.h > by1) by1 = o.y + o.h;
        }
        const float dx = (ax0 > bx1) ? ax0 - bx1 : ((bx0 > ax1) ? bx0 - ax1 : 0.0f);
        const float dy = (ay0 > by1) ? ay0 - by1 : ((by0 > ay1) ? by0 - ay1 : 0.0f);
        const float d  = (dx > dy) ? dx : dy;
        if (d < best_d) { best_d = d; best = c; }
    }
    return best;
}

/* On release: we apply what the preview announced, not a second computation.
 * Two separate computations would eventually announce a merge that never
 * happens. */
void Hud::snapIfClose(size_t block)
{
    if (block >= blocks_.size()) return;
    const std::string best = findSnapTarget(block);
    if (best.empty()) return;
    const std::string myLeader = leaderOf(blockId(blocks_[block]));

    /* It is the DRAGGED group that joins the other one, never the reverse:
     * otherwise the block nobody touched would jump across the screen, and no
     * one would understand what just happened. */
    /* S107 - record where each block starts from BEFORE attaching it: after
     * that, its position is the group's and the origin is lost. */
    merge_from_.clear();
    merge_ms_ = 0;
    for (const Block &b : blocks_) {
        const std::string id = blockId(b);
        if (leaderOf(id) != myLeader) continue;
        merge_from_[id] = { b.x, b.y };
        leader_[id] = best;
        anchors_.erase(id);
    }
}

/* Long press: the block leaves its group and stays where it is. Without this,
 * two snapped blocks would stay snapped forever. */
void Hud::detach(size_t block, float zx, float zy, float zw, float zh)
{
    if (block >= blocks_.size()) return;
    const Block &b = blocks_[block];
    const std::string id = blockId(b);
    if (leader_.find(id) == leader_.end()) return;   /* already standalone */
    leader_.erase(id);
    /* It keeps EXACTLY its current position, converted to a fraction:
     * detaching must move nothing, otherwise you lose sight of the block at the
     * very moment you are acting on it. */
    float ox, oy, free_x, free_y;
    usableArea(zx, zy, zw, zh, b.w, b.h, ox, oy, free_x, free_y);
    float ax = (free_x > 1.0f) ? (b.x - ox) / free_x : 0.0f;
    float ay = (free_y > 1.0f) ? (b.y - oy) / free_y : 0.0f;
    if (ax < 0.0f) ax = 0.0f; if (ax > 1.0f) ax = 1.0f;
    if (ay < 0.0f) ay = 0.0f; if (ay > 1.0f) ay = 1.0f;
    anchors_[id] = Anchor{ax, ay};
}

bool Hud::touch(float px, float py, int phase,
                  float zx, float zy, float zw, float zh)
{
    if (!editing_ || blocks_.empty()) return false;
    /* The blocks are drawn in a scaled space; the finger arrives in screen
     * pixels. Without this division a block would be grabbable next to where it
     * is seen, the further off the more the scale departs from 1. */
    const float k = (scale_ > 0.05f) ? scale_ : 1.0f;
    px /= k; py /= k; zx /= k; zy /= k; zw /= k; zh /= k;

    if (phase == 0) {
        /* Last to first: reverse draw order, hence the one the eye means when
         * two blocks overlap. */
        for (size_t i = blocks_.size(); i-- > 0; ) {
            const Block &b = blocks_[i];
            if (px >= b.x && px <= b.x + b.w && py >= b.y && py <= b.y + b.h) {
                /* === S95 - DOUBLE TAP = HIDE THIS BLOCK ===
                 * Removing a section from the panel meant opening the menu,
                 * descending through two submenus and unchecking a row - in the
                 * middle of placing blocks, with the block right under your
                 * finger. The gesture is reversible: the section is rechecked
                 * from the menu, and the mode description says so. */
                const std::string id_i = blockId(b);
                if (id_i == tap_id_ && nowMs() - tap_ms_ <= DOUBLE_TAP_MS) {
                    if (b.chart == SIZE_MAX) toggleSection(b.section);
                    else                     toggleChart(b.chart);
                    masks_changed_ = true;
                    tap_ms_ = 0; tap_id_.clear();
                    grabbed_ = SIZE_MAX;
                    blocks_.clear();        /* force a rebuild: the indices just
                                             * changed under our feet */
                    return true;
                }
                tap_ms_ = nowMs();
                tap_id_ = id_i;
                grabbed_        = i;
                grab_dx_        = px - b.x;
                grab_dy_        = py - b.y;
                grab_x0_        = px;
                grab_y0_        = py;
                grab_ms_        = nowMs();
                grab_moved_     = false;
                grab_detached_  = false;
                return true;
            }
        }
        return false;
    }
    if (grabbed_ >= blocks_.size()) return false;
    Block &b = blocks_[grabbed_];

    if (phase == 1) {
        const float travel = fabsf(px - grab_x0_) + fabsf(py - grab_y0_);
        if (travel > MOVE_PX) grab_moved_ = true;

        /* LONG PRESS WITHOUT MOVING = detach. Tested before the drag: a finger
         * that shakes by two pixels must still detach, otherwise the gesture
         * works one time in three and comes across as capricious. */
        if (!grab_moved_ && !grab_detached_
            && nowMs() - grab_ms_ >= LONG_PRESS_MS) {
            detach(grabbed_, zx, zy, zw, zh);
            /* S105 - detaching is the least visible gesture of the mode: the
             * block does not move, only its attachment changes. Without
             * feedback, you cannot tell whether it took. */
            haptics::play(haptics::Intent::Mode);
            grab_detached_ = true;
            /* The grab stays valid: you can chain straight into a drag, which
             * is the natural gesture right after peeling a block off. */
            return true;
        }
        if (!grab_moved_) return true;   /* still waiting */

        /* We move the GROUP, not the block alone: otherwise dragging a snapped
         * block would silently tear it away, and the explicit detach gesture
         * would be pointless. */
        const std::string leader = leaderOf(blockId(b));
        float ox, oy, free_x, free_y;
        usableArea(zx, zy, zw, zh, b.w, b.h, ox, oy, free_x, free_y);
        float ax = (free_x > 1.0f) ? (px - grab_dx_ - ox) / free_x : 0.0f;
        float ay = (free_y > 1.0f) ? (py - grab_dy_ - oy) / free_y : 0.0f;
        if (ax < 0.0f) ax = 0.0f; if (ax > 1.0f) ax = 1.0f;
        if (ay < 0.0f) ay = 0.0f; if (ay > 1.0f) ay = 1.0f;
        moveGroup(leader, ax, ay);
        /* The merge preview, computed by the SAME function as the snap:
         * announcing a merge that would not happen would be worse than
         * announcing nothing at all. */
        snap_preview_ = findSnapTarget(grabbed_);
        return true;
    }

    /* Release: this is where, and only where, we snap. Snapping during the drag
     * would make the block jump under the finger as soon as it brushed a
     * neighbour, and you could no longer set it down beside one without it
     * latching on. */
    snap_preview_.clear();
    if (grab_moved_) {
        snapIfClose(grabbed_);
        /* Edge magnetism, AFTER the snap: snapping changes the group's size,
         * hence the fraction that puts it against the edge. Magnetizing first
         * would be aiming at a target that then moves. */
        const std::string leader = leaderOf(blockId(blocks_[grabbed_]));
        auto it = anchors_.find(leader);
        if (it != anchors_.end()) {
            /* Tolerance given in PIXELS then converted: expressed as a
             * fraction, it would be worth twice as much at 720p as at 1080p and
             * magnetism would feel erratic depending on the console's mode. */
            const float tol_x = (zw > 1.0f) ? (MAGNET_PX / zw) : 0.05f;
            const float tol_y = (zh > 1.0f) ? (MAGNET_PX / zh) : 0.05f;
            it->second.ax = magnetize(it->second.ax, tol_x);
            it->second.ay = magnetize(it->second.ay, tol_y);
        }
    }
    grabbed_ = SIZE_MAX;
    return true;
}

static float magnetize(float a, float tol)
{
    if (a < tol)             return 0.0f;
    if (a > 1.0f - tol)      return 1.0f;
    if (fabsf(a - 0.5f) < tol) return 0.5f;
    return a;
}

void Hud::draw(NVGcontext *vg, float x, float y, float width, float height)
{
    if (!vg || !visible()) return;

    const float k = scale_;
    nvgSave(vg);
    nvgScale(vg, k, k);
    x /= k; y /= k; width /= k; height /= k;

    nvgFontFace(vg, theme::font());

    /* Opacity applies to the WHOLE panel at once, through nanovg's global
     * alpha: applying it color by color would mean touching the thirty fill
     * calls, and the first one forgotten would leave an opaque element in the
     * middle of a faded block. */
    if (opacity_ < 0.999f) nvgGlobalAlpha(vg, opacity_);

    {
        int64_t t = nowMs();
        if (t - lastSampleMs_ >= SAMPLE_MS) {
            lastSampleMs_ = t;
            for (Chart &c : charts_) c.push();
        }
    }

    int64_t t = nowMs();
    if (blocks_.empty() || mask_ != filledMask_ || t - lastFillMs_ >= refreshMs_) {
        refresh(vg);
        lastFillMs_ = t;
        filledMask_ = mask_;
    }
    if (blocks_.empty()) { nvgRestore(vg); return; }

    /* --- Groups: one leader, its members stacked below, a single panel ---
     * The order follows that of `blocks_`, hence declaration order: two
     * successive sessions arrange the same blocks the same way. */
    std::vector<std::vector<size_t>> groups;
    std::vector<std::string>         leaders;
    for (size_t i = 0; i < blocks_.size(); i++) {
        const std::string c = leaderOf(blockId(blocks_[i]));
        size_t g = 0;
        for (; g < leaders.size(); g++) if (leaders[g] == c) break;
        if (g == leaders.size()) { leaders.push_back(c); groups.push_back({}); }
        groups[g].push_back(i);
    }

    /* Leader of the group being dragged: the preview has to tint BOTH sides of
     * the merge, otherwise you cannot tell which one joins which. */
    const std::string grabbedLeader = (editing_ && grabbed_ < blocks_.size())
                                    ? leaderOf(blockId(blocks_[grabbed_])) : std::string();

    float autoY = MARGIN;
    for (size_t g = 0; g < groups.size(); g++) {
        /* The group takes the width of its widest member: a panel whose blocks
         * do not share a right edge reads as a defect. */
        float gw = 0, gh = 0;
        for (size_t i : groups[g]) {
            if (blocks_[i].w > gw) gw = blocks_[i].w;
            gh += blocks_[i].h;
        }

        auto it = anchors_.find(leaders[g]);
        float gx, gy;
        if (it == anchors_.end()) {
            gx = x + width - gw - MARGIN;
            gy = y + autoY;
            autoY += gh + 8.0f;
        } else {
            float ox, oy, free_x, free_y;
            usableArea(x, y, width, height, gw, gh, ox, oy, free_x, free_y);
            gx = ox + it->second.ax * free_x;
            gy = oy + it->second.ay * free_y;
        }

        /* === S95 - SAY IT WILL MERGE, BEFORE IT MERGES ===
         * Without a preview you snap by accident: you set a block down near
         * another, they join, and it takes a long press to undo what you never
         * asked for. So both groups involved change color during the drag, and
         * a second border encloses them together - what we show is the RESULT,
         * not just the intent. */
        const bool targeted = editing_ && !snap_preview_.empty()
                           && (leaders[g] == snap_preview_ || leaders[g] == grabbedLeader);

        nvgBeginPath(vg);
        nvgRoundedRect(vg, gx, gy, gw, gh, 8.0f);
        nvgFillColor(vg, theme::panelBg);
        nvgFill(vg);
        if (targeted) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, gx, gy, gw, gh, 8.0f);
            nvgFillColor(vg, nvgRGBA(95, 199, 155, 46));
            nvgFill(vg);
        }
        nvgStrokeColor(vg, targeted ? nvgRGBA(95, 199, 155, 255)
                                    : (editing_ ? theme::sectionT : theme::panelBorder));
        nvgStrokeWidth(vg, targeted ? 3.0f : (editing_ ? 2.0f : 1.0f));
        nvgStroke(vg);

        float by = gy;
        for (size_t idx = 0; idx < groups[g].size(); idx++) {
            Block &b = blocks_[groups[g][idx]];
            b.x = gx; b.y = by; b.w = gw;

            /* S107 - the arrival slide. The duration is short (160 ms): beyond
             * that you wait for the panel instead of reading it, and an
             * animation you wait for is one animation too many. */
            if (!merge_from_.empty()) {
                static const int64_t DURATION = 160;
                if (merge_ms_ == 0) merge_ms_ = nowMs();
                const int64_t age = nowMs() - merge_ms_;
                auto it2 = merge_from_.find(blockId(b));
                if (age >= DURATION) {
                    merge_from_.clear();
                    merge_ms_ = 0;
                } else if (it2 != merge_from_.end()) {
                    /* Ease-out: the block leaves fast and settles. A linear
                     * move reads as a mechanical slide, not as something coming
                     * to rest in its place. */
                    float u = (float)age / (float)DURATION;
                    const float remaining = (1.0f - u) * (1.0f - u) * (1.0f - u);
                    b.x += (it2->second.first  - b.x) * remaining;
                    b.y += (it2->second.second - b.y) * remaining;
                }
            }

            /* Hairline between two snapped blocks: without it, two sections
             * stacked in the same panel read as a single list. */
            if (idx > 0) {
                nvgBeginPath(vg);
                nvgMoveTo(vg, gx + PAD, by);
                nvgLineTo(vg, gx + gw - PAD, by);
                nvgStrokeColor(vg, theme::panelBorder);
                nvgStrokeWidth(vg, 1.0f);
                nvgStroke(vg);
            }
            drawBlock(vg, b);
            by += b.h;
        }

        if (editing_) {
            const float r = 5.0f, cx = gx + gw - PAD, cy = gy + PAD;
            for (int i = 0; i < 3; i++) {
                nvgBeginPath(vg);
                nvgCircle(vg, cx, cy + (float)i * r, 1.6f);
                nvgFillColor(vg, theme::sectionT);
                nvgFill(vg);
            }
        }
    }

    nvgRestore(vg);
}

/* The contents of a block, without its background: the panel is drawn by the
 * group. */
void Hud::drawBlock(NVGcontext *vg, const Block &b)
{
    const float px = b.x, pw = b.w;
    float ty = b.y + PAD;

    if (b.chart == SIZE_MAX) {
        const std::string &title = sections_[b.section].title;
        nvgFontSize(vg, FONT_TITLE);
        nvgFillColor(vg, theme::sectionT);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        float tw = textW(vg, title.c_str(), FONT_TITLE);
        nvgText(vg, px + PAD, ty + SECTION_H / 2.0f, title.c_str(), nullptr);
        nvgBeginPath(vg);
        nvgMoveTo(vg, px + PAD + tw + 8.0f, ty + SECTION_H / 2.0f);
        nvgLineTo(vg, px + pw - PAD, ty + SECTION_H / 2.0f);
        nvgStrokeColor(vg, theme::panelBorder);
        nvgStrokeWidth(vg, 1.0f);
        nvgStroke(vg);
        ty += SECTION_H;

        for (size_t r = b.first; r < b.first + b.count; r++) {
            const HudBuilder::Row &row = builder_.rows_[r];
            nvgFontSize(vg, FONT_ROW);
            nvgFillColor(vg, theme::label);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgText(vg, px + PAD, ty + ROW_H / 2.0f, row.label, nullptr);
            nvgFillColor(vg, theme::forGrade(row.grade));
            nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
            nvgText(vg, px + pw - PAD, ty + ROW_H / 2.0f, row.value, nullptr);
            ty += ROW_H;
            if (row.hasGauge) {
                float gw2 = pw - PAD * 2;
                nvgBeginPath(vg);
                nvgRoundedRect(vg, px + PAD, ty, gw2, GAUGE_H, GAUGE_H / 2.0f);
                nvgFillColor(vg, theme::rowBg);
                nvgFill(vg);
                if (row.ratio > 0.0f) {
                    nvgBeginPath(vg);
                    nvgRoundedRect(vg, px + PAD, ty, gw2 * row.ratio, GAUGE_H,
                                   GAUGE_H / 2.0f);
                    nvgFillColor(vg, theme::forGrade(row.grade));
                    nvgFill(vg);
                }
                ty += GAUGE_H + GAUGE_GAP;
            }
        }
        return;
    }

    const Chart &c = charts_[b.chart];
    nvgFontSize(vg, FONT_TITLE);
    nvgFillColor(vg, theme::sectionT);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(vg, px + PAD, ty + CHART_HEAD / 2.0f, c.title_.c_str(), nullptr);

    char cur[32];
    snprintf(cur, sizeof(cur), "%.1f %s", c.latest(), c.unit_.c_str());
    nvgFontSize(vg, FONT_ROW);
    nvgFillColor(vg, theme::value);
    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
    nvgText(vg, px + pw - PAD, ty + CHART_HEAD / 2.0f, cur, nullptr);
    ty += CHART_HEAD;

    const float gx = px + PAD, gw2 = pw - PAD * 2;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, gx, ty, gw2, CHART_H, 3.0f);
    nvgFillColor(vg, theme::rowBg);
    nvgFill(vg);

    const size_t n = c.size();
    if (n < 2) return;
    const float top  = c.upperBound();
    const float step = gw2 / (float)(Chart::CAPACITY - 1);
    const float x0   = gx + gw2 - step * (float)(n - 1);

    /* === LAT-V2 2026-09-26 - AT MOST 60 POINTS PER CURVE ==================
     *
     * Every chart was drawn from all of its samples (up to 180), twice - the
     * area and the line - on EVERY frame. A curve 90 s wide on a few hundred
     * pixels needs no more than 60 points: neighbouring samples are averaged
     * into buckets. Measured on a PS Vita with the panel open: the charts cost
     * 1-2 ms per frame after this, the whole panel ~1 ms less than before.
     * NOT the main cost of the panel: that is its text rows (5.7 ms when it
     * opens, 17 ms once every row is filled), which is what took a Vita stream
     * from 60 to 45 images per second (KB §9 LAT-V2).
     * SHADOW_HUD_CHART_POINTS sets the cap; 0 draws every sample, as before. */
    static int max_pts = -1;
    if (max_pts < 0) {
        const char *e = getenv("SHADOW_HUD_CHART_POINTS");
        max_pts = e ? atoi(e) : 60;
        if (max_pts < 0) max_pts = 60;
    }
    const size_t npts = (max_pts == 0 || n <= (size_t)max_pts) ? n : (size_t)max_pts;
    float cx[Chart::CAPACITY], cy[Chart::CAPACITY];
    for (size_t k = 0; k < npts; k++) {
        const size_t a = k * n / npts, b = (k + 1) * n / npts;   /* [a, b) */
        float sum = 0.0f;
        for (size_t j = a; j < b; j++) sum += c.at(j);
        float v = (sum / (float)(b - a)) / top;
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        /* the bucket's centre, so the curve keeps its place on the time axis */
        cx[k] = x0 + step * ((float)(a + b - 1) * 0.5f);
        cy[k] = ty + CHART_H - v * CHART_H;
    }
    cx[0] = x0;                                /* both ends stay where they were */
    cx[npts - 1] = x0 + step * (float)(n - 1);

    nvgBeginPath(vg);
    nvgMoveTo(vg, x0, ty + CHART_H);
    for (size_t k = 0; k < npts; k++) nvgLineTo(vg, cx[k], cy[k]);
    nvgLineTo(vg, gx + gw2, ty + CHART_H);
    nvgClosePath(vg);
    nvgFillColor(vg, nvgRGBA(56, 96, 168, 90));
    nvgFill(vg);

    nvgBeginPath(vg);
    for (size_t k = 0; k < npts; k++) {
        if (k == 0) nvgMoveTo(vg, cx[k], cy[k]); else nvgLineTo(vg, cx[k], cy[k]);
    }
    nvgStrokeColor(vg, theme::sectionT);
    nvgStrokeWidth(vg, 1.4f);
    nvgStroke(vg);

    char top_lbl[24];
    snprintf(top_lbl, sizeof(top_lbl), "%.0f", top);
    nvgFontSize(vg, ui::type::CAPTION);
    nvgFillColor(vg, theme::label);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgText(vg, gx + 4.0f, ty + 2.0f, top_lbl, nullptr);
}

}  // namespace ui
