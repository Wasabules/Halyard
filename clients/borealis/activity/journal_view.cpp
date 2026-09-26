/* JournalView - see the header for why this screen and its columns exist. */

#include "journal_view.hpp"

#include "../device_mode.hpp"
#include "../ui/i18n.hpp"
#include "../ui/paint.hpp"
#include "../ui/sfx.hpp"
#include "../ui/theme.hpp"
#include "../ui/type.hpp"

extern "C" {
#include "../../../core/services/journal.h"
#include "../../../core/services/journal_line.h"
#include "../ui/touch.h"
}

namespace {

/* === THE ONLY TWO NUMBERS THAT SIZE THE READ ===
 *
 * `journal_dernieres_lignes` reads ONLY the end of the file, over a window the
 * caller sizes: the cost of this screen is therefore exactly what is written
 * here, and not the size of the session (several megabytes).
 *
 * 64 KB for 400 lines = 160 bytes per line on average, which matches this log's
 * format (`[12.345] I/video   ` is already twenty bytes of header). If the lines
 * are longer we render fewer than 400, and that is the right trade-off: better
 * fewer complete lines than a budget that follows a session's verbosity.
 *
 * Both buffers are LOCAL to the load and freed right away - see `reload()`. */
const size_t BUFFER_BYTES = 64u * 1024u;
const int    MAX_LINES    = 400;

/* The line height is a terminal's, not a settings list's: at 26 pixels a good
 * twenty fit on a 720-pixel screen, which is the minimum for READING a log
 * rather than leafing through it. */
const float LINE_H = 26.0f;

/* Columns, in PIXELS and not in characters: the application's font is
 * proportional (a single face is loaded, see `theme::font`). Aligning on a
 * character count would give ragged columns. */
const float COL_TIME = 82.0f;   /* timestamp, RIGHT-aligned within its column */
const float COL_SEV  = 18.0f;   /* the letter */
const float COL_CAT  = 86.0f;   /* the category */
const float COL_GAP  = 10.0f;

/* The severity tint. This is the whole point of this screen: a page of log is
 * read at a glance when severity is a COLOR, not a character lost in a column. */
NVGcolor severityTint(char s)
{
    switch (s) {
        case 'E': return ui::theme::bad;
        case 'A': return ui::theme::warn;
        case 'I': return ui::paint::accentVif;
        case 'D': return ui::theme::label;
        case 'T': return nvgTransRGBA(ui::theme::label, 140);
        default:  return ui::theme::hint;
    }
}

/* The MESSAGE follows the severity, but more discreetly.
 *
 * Coloring the letter alone is not enough: it is eighteen pixels wide at the
 * left edge, and an eye sweeping a page reads the messages, not the margin. So
 * an error carries its tint into its text - and, symmetrically, DEBUG and TRACE
 * recede. Without that recession, thirty lines of periodic measurements weigh
 * exactly as much visually as the single line explaining the failure, which is
 * the situation before this screen existed. */
NVGcolor messageTint(char s)
{
    switch (s) {
        case 'E': return ui::theme::bad;
        case 'A': return ui::theme::warn;
        case 'D': return nvgTransRGBA(ui::theme::value, 175);
        case 'T': return nvgTransRGBA(ui::theme::value, 125);
        default:  return ui::theme::value;   /* INFO and unrecognized: full brightness */
    }
}

}  // namespace

/* --- Loading ------------------------------------------------------------- */

JournalView::JournalView()
{
    /* The labels are resolved ONCE: `ui::tr` formats, therefore allocates, and
     * this screen redraws them sixty times per second. */
    title_    = ui::tr("log/title");
    subtitle_ = ui::tr("log/subtitle");
    hints_ = {
        { "B", ui::tr("action/back") },
        { "X", ui::tr("log/refresh") },
        { "Y", ui::tr("log/session") },
    };
    reload();
}

/* S101 - X WAS ANNOUNCED AND DID NOT EXIST. The footer had been showing
 * "X refresh" since the screen was created, and no function answered X: pressing
 * it did nothing, which reads as a frozen screen. One more promised button that
 * nothing served - same family as the "touching a line selects it" comments S84
 * found with no implementation behind them. */
bool JournalView::buttonX()
{
    reload();
    return true;
}

bool JournalView::buttonY()
{
    const int kept = journal_sessions_kept();
    /* It wraps: after the oldest we come back to the current session. A hard
     * stop would force a second button to go back, on a screen that has only
     * three free keys. */
    session_ = (session_ + 1) % (kept + 1);
    reload();
    return true;
}

void JournalView::reload()
{
    lines_.clear();
    message_.clear();

    /* === THE STORAGE IS OURS, AND IT IS TEMPORARY ===
     *
     * `journal_dernieres_lignes` allocates nothing: the buffer receives the
     * bytes and `raw` receives pointers INTO it. Both live for the duration of
     * this function, and each line is COPIED as it is split.
     *
     * Keeping the pointers and the buffer would have been tempting - zero copy -
     * and wrong twice over: we would carry 64 KB for the whole life of the
     * screen, and a reload (X button) would replace the buffer UNDER the lines
     * being displayed. Same family as the dangling views this whole framework
     * eliminates, dressed up as an optimization. */
    std::vector<char>         buffer(BUFFER_BYTES);
    std::vector<const char *> raw((size_t)MAX_LINES, nullptr);

    /* S101 - clamped on every read: the number of archives changes when the
     * application restarts or when they are purged, and an index kept from
     * before would point at a file that is gone. */
    const int kept = journal_sessions_kept();
    if (session_ < 0)      session_ = 0;
    if (session_ > kept)   session_ = kept;

    const int n = journal_last_lines_index(session_,
                                                 buffer.data(), buffer.size(),
                                                 raw.data(), MAX_LINES);
    if (n < 0) {
        message_ = ui::tr("log/unreadable");
    } else if (n == 0) {
        message_ = ui::tr("log/empty");
    } else {
        lines_.reserve((size_t)n);
        for (int i = 0; i < n; i++)
            if (raw[(size_t)i]) lines_.push_back(split(raw[(size_t)i]));
    }

    /* The status ALWAYS says which session is being read, even an empty one:
     * without it, an empty screen does not distinguish "this archive is empty"
     * from "I did not switch session". */
    const std::string which = (session_ == 0) ? ui::tr("log/session_current")
                                              : ui::tr("log/session_n", session_);
    status_ = lines_.empty()
            ? which
            : (which + "   " + ui::tr("log/lines", lines_.size()));

    /* A reload PUTS US BACK at the bottom. We just pressed "refresh", that is to
     * say asked "and now?": staying where we were would miss exactly the lines
     * we refreshed for. */
    go_to_bottom_ = true;
}

/* Splits `[12.345] I/video    message` into its four columns.
 *
 * The parsing itself is NOT here: it lives in `shadow/journal_line.h`, pure and
 * allocation-free, next to the `fprintf` in journal.c that produces this format
 * - writer and reader as neighbours, therefore modified together - and verified
 * offline by tests/test_journal_line.c, including on its degenerate inputs.
 * This function only does the COPY: from positions into `std::string`s the view
 * can keep after the buffer is freed. */
JournalView::Line JournalView::split(const char *raw)
{
    const journal_line_t d = journal_line_split(raw);

    Line l;
    l.sev = d.sev;
    l.timestamp.assign(raw + d.temps_pos, d.temps_len);
    l.cat.assign(raw + d.cat_pos, d.cat_len);
    l.text = raw + d.text_pos;
    return l;
}

/* --- Scrolling ----------------------------------------------------------- */

nav_t JournalView::state() const
{
    nav_t n;
    n.nb        = (int)lines_.size();
    n.focus     = -1;               /* no focus on this screen: see the header */
    n.scroll    = scroll_;
    /* `view_h_` comes from the LAST DRAWN FRAME, and that is the only possible
     * source: the height of the frame does not exist until the page has been
     * laid out.
     *
     * Consequence worth knowing: before the first frame `view_h_` is 0, so
     * `nav_scroll_max` returns the WHOLE height of the content and a scroll
     * could run well past the bottom. That has no visible effect because
     * `peindre()` re-clamps every frame with the real height - and that is
     * exactly why the clamping is redone there rather than done once here.
     * Removing it there would leave an empty page after a dock/handheld switch,
     * which changes the frame height under the scroll. */
    n.view_h     = view_h_;
    n.content_h = nav_content_h_uniform(n.nb, LINE_H, 0.0f);
    return n;
}

bool JournalView::scrollBy(int lines)
{
    if (lines_.empty()) return false;

    const nav_t n = state();
    const float before = scroll_;
    scroll_ = nav_scroll_clamp(&n, scroll_ + (float)lines * LINE_H);

    /* As soon as the user is steering we stop pulling them back to the bottom -
     * including when the move was refused because they were already there. */
    go_to_bottom_ = false;

    if (scroll_ == before) {
        /* Being at the end is not an error, it is an answer: without it, holding
         * the d-pad at the end of the log produces NOTHING and you cannot tell
         * whether you reached the end or the device stopped responding. Same
         * choice as `ui::ListScreen::moveBy` (S88). */
        ui::sfx::play(ui::sfx::Sound::NavigationEdge);
        return false;
    }
    return true;
}

/* A page is the frame MINUS two lines of overlap. Without that overlap, a page
 * jump makes the line you were reading disappear and you lose your place; it is
 * the convention of every text reader, and it matters all the more here because
 * the lines look alike. */
bool JournalView::scrollPage(int dir)
{
    int per_page = (int)(view_h_ / LINE_H) - 2;
    if (per_page < 1) per_page = 1;
    return scrollBy(dir * per_page);
}

/* --- Rendering ----------------------------------------------------------- */

void JournalView::paint(NVGcontext *vg, float x, float y, float w, float h,
                          double t)
{
    const float margin = device::isDocked() ? UI_MARGIN_DOCK : UI_MARGIN_HANDHELD;
    const float px = x + margin;
    const float pw = w - margin * 2.0f;

    ui::paint::shadowBg(vg, x, y, w, h);
    ui::paint::backgroundWaves(vg, x, y, w, h, t);

    /* Header and footer come from screen.hpp: redrawing them here would have
     * made them diverge from the list screens', and the title would jump by a
     * few pixels when moving from one to the other. */
    ui::drawHeader(vg, px, y, pw, title_, subtitle_, "", t);

    const float cy = y + UI_HEADER_H;
    float ch = h - UI_HEADER_H - UI_FOOTER_H;
    if (ch < 0.0f) ch = 0.0f;
    view_h_ = ch;

    /* The scroll state is built AFTER `view_h_`, never before: it is what
     * clamps, and clamping it with the PREVIOUS frame's height would make the
     * first frame after a dock/handheld switch jump. */
    nav_t n = state();

    if (go_to_bottom_) {
        /* Pinning to the bottom can only happen HERE: it needs the height of the
         * frame, which does not exist until the frame has been laid out. That is
         * also why it is a FLAG set by `reload()` and not a computation done
         * there - where the height is not known yet. */
        scroll_ = nav_scroll_max(&n);
        go_to_bottom_ = false;
    } else {
        scroll_ = nav_scroll_clamp(&n, scroll_);
    }
    n.scroll = scroll_;

    /* === The finger, handled INSIDE the rendering ===
     * The frame geometry has just been computed: the gesture therefore answers
     * THIS frame's layout. Deferring it by one frame would make it wrong as soon
     * as anything scrolled - correct at rest, offset afterwards.
     *
     * There is neither a threshold nor a drag/tap distinction here, unlike
     * `ui::ListScreen`: nothing is activatable on this screen, so a tap has no
     * consequence to tell apart from a two-pixel drag. */
    {
        const ui_touch_t d = ui_touch_current();
        const bool inside = d.active && d.x >= px && d.x <= px + pw
                         && d.y >= cy && d.y <= cy + ch;

        if (inside && !finger_down_) {
            finger_down_ = true;
            finger_y0_ = d.y;
            scroll_at_press_ = scroll_;
            go_to_bottom_ = false;
        } else if (d.active && finger_down_) {
            /* The finger drags the CONTENT: moving it up moves the text up, so it
             * INCREASES the scroll. The opposite would feel like pushing a
             * scrollbar, not like holding a page. */
            scroll_ = nav_scroll_clamp(&n, scroll_at_press_ + (finger_y0_ - d.y));
            n.scroll = scroll_;
        } else if (!d.active) {
            finger_down_ = false;
        }
    }

    if (lines_.empty()) {
        /* We DRAW the absence, in a panel that takes up the room: one line of
         * text stranded at the top of a 720-pixel screen reads as a stalled
         * load, not as an answer. Same choice as ListScreen. */
        const float bw = pw * 0.6f, bh = 120.0f;
        const float bx = px + (pw - bw) * 0.5f;
        const float by = cy + (ch - bh) * 0.5f;
        ui::paint::glassPanel(vg, bx, by, bw, bh, ui::paint::PANEL_RADIUS);
        nvgFontFace(vg, ui::theme::font());
        nvgFontSize(vg, ui::type::BODY);
        nvgFillColor(vg, ui::theme::label);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        ui::paint::clampedText(vg, bx + bw * 0.5f, by + bh * 0.5f, bw - 32.0f,
                              by, bh, message_, false, t);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    } else {
        nvgSave(vg);
        nvgScissor(vg, px, cy, pw, ch);
        nvgFontFace(vg, ui::theme::font());
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

        const float x_sev = px + COL_TIME + COL_GAP;
        const float x_cat = x_sev + COL_SEV;
        const float x_msg = x_cat + COL_CAT + COL_GAP;
        float msg_w = px + pw - x_msg;
        if (msg_w < 0.0f) msg_w = 0.0f;   /* narrow window: we cut, we do not overflow */

        const int nb = (int)lines_.size();
        for (int i = 0; i < nb; i++) {
            const Line &l = lines_[(size_t)i];
            const float ly = cy + nav_y_of_index(i, LINE_H, 0.0f) - scroll_;

            /* Outside the frame: we do not draw. That is what keeps the cost of a
             * frame CONSTANT, whether the log shows twenty lines or four
             * hundred. */
            if (ly + LINE_H < cy || ly > cy + ch) continue;

            const float mid = ly + LINE_H * 0.5f;

            if (!l.timestamp.empty()) {
                nvgFontSize(vg, ui::type::CAPTION);
                nvgFillColor(vg, nvgTransRGBA(ui::theme::hint, 190));
                nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
                const std::string tt = ui::paint::truncated(vg, l.timestamp, COL_TIME);
                nvgText(vg, px + COL_TIME, mid, tt.c_str(), nullptr);
                nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            }

            const char letter[2] = { l.sev, '\0' };
            nvgFontSize(vg, ui::type::SECONDARY);
            nvgFillColor(vg, severityTint(l.sev));
            nvgText(vg, x_sev, mid, letter, nullptr);

            if (!l.cat.empty()) {
                nvgFontSize(vg, ui::type::CAPTION);
                nvgFillColor(vg, ui::theme::sectionT);
                ui::paint::clampedText(vg, x_cat, mid, COL_CAT, ly, LINE_H,
                                      l.cat, false, t);
            }

            nvgFontSize(vg, ui::type::SECONDARY);
            nvgFillColor(vg, messageTint(l.sev));
            /* NEVER scroll the text: `texteBorne` can do it, and twenty lines
             * scrolling at once would be unreadable. A line that is too long is
             * cut with an ellipsis - the left-hand columns already carry enough
             * to find it again in the file. */
            ui::paint::clampedText(vg, x_msg, mid, msg_w, ly, LINE_H,
                                  l.text, false, t);
        }
        nvgRestore(vg);
    }

    ui::drawFooter(vg, px, y + h - UI_FOOTER_H, pw, UI_FOOTER_H,
                     hints_, status_, false, t);
}
