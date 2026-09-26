/* EqView - see the header for why the curve and the two profiles exist. */

#include "eq_view.hpp"

#include "settings.hpp"

extern "C" {
#include "../../../core/media/audio.h"
}

#include "../ui/i18n.hpp"
#include "../ui/paint.hpp"
#include "../ui/theme.hpp"
#include "../ui/type.hpp"
#include "../device_mode.hpp"

using ui::paint::textWidth;
/* Short names for the in-house framework's two namespaces. */
namespace paint = ui::paint;
namespace theme = ui::theme;

#include <cstdio>
#include <cmath>

namespace {

/* The offered values. These are NOTCHES and not continuous sliders: with a
 * controller, a slider forces you to hold a direction while watching a number
 * scroll by, which is tedious and imprecise. Notches are stepped through one
 * press at a time, and they can be read.
 *
 * The frequencies follow a roughly logarithmic progression - that is how the ear
 * perceives them, and a linear progression would give twenty notches in the
 * treble and three in the bass. */
const float FREQS[] = {
    30, 40, 60, 80, 100, 120, 160, 200, 250, 315, 400, 500, 630, 800,
    1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000,
    12500, 16000
};
const float QS[]    = { 0.3f, 0.5f, 0.707f, 1.0f, 1.4f, 2.0f, 3.0f, 4.0f, 6.0f };
const float GAINS[] = { -12, -9, -6, -4.5f, -3, -1.5f, 0, 1.5f, 3, 4.5f, 6, 9, 12 };

template <size_t N>
int nearest(const float (&table)[N], float v)
{
    int best = 0;
    float gap = 1e9f;
    for (size_t i = 0; i < N; i++) {
        const float d = fabsf(table[i] - v);
        if (d < gap) { gap = d; best = (int)i; }
    }
    return best;
}

std::string fmtFreq(float f)
{
    char b[24];
    if (f >= 1000.0f) std::snprintf(b, sizeof b, "%.1f kHz", (double)(f / 1000.0f));
    else              std::snprintf(b, sizeof b, "%.0f Hz", (double)f);
    return b;
}

std::string fmtGain(float g)
{
    char b[16];
    std::snprintf(b, sizeof b, "%+.1f dB", (double)g);
    return b;
}

std::string fmtQ(float q)
{
    char b[16];
    std::snprintf(b, sizeof b, "%.2f", (double)q);
    return b;
}

ui::Item titleItem(const std::string &t)
{
    ui::Item i; i.kind = ui::Kind::Title; i.title = t; i.actionable = false;
    return i;
}

ui::Item choiceItem(int id, const std::string &t, const std::string &desc,
                    std::vector<std::string> v, int idx)
{
    ui::Item i; i.kind = ui::Kind::Choice; i.id = id;
    i.title = t; i.subtitle = desc; i.choice = std::move(v); i.choice_index = idx;
    return i;
}

std::vector<std::string> presetLabels()
{
    std::vector<std::string> v;
    for (int p = 0; p < EQ_PRESET_COUNT; p++)
        v.push_back(ui::tr(eq_preset_key((eq_preset_t)p)));
    return v;
}

std::vector<std::string> typeLabels()
{
    std::vector<std::string> v;
    for (int t = 0; t < EQ_TYPE_COUNT; t++)
        v.push_back(ui::tr(eq_type_key((eq_type_t)t)));
    return v;
}

/* Next/previous notch in a table, WRAPPING. Wrapping avoids having to go all the
 * way round to step back by one: with thirteen gain values, a hard stop would
 * cost twelve presses to go from +12 to -12. */
template <size_t N>
int nextNotch(const float (&table)[N], float v, int dir)
{
    int i = nearest(table, v);
    const int n = (int)N;
    i = (i + (dir >= 0 ? 1 : n - 1)) % n;
    return i;
}

}  // namespace

EqView::EqView()
{
    loadBands();
    screen_.setTitle(ui::tr("eq/title"));
    screen_.setHints({ { "B", ui::tr("action/back") }, { "A", ui::tr("action/ok") } });
    rebuild();
}

/* Reads the bands from the settings file. If they do not exist we start from the
 * HANDHELD profile rather than from silence: a "Custom" screen with all five
 * bands switched off would give the impression that the setting does not work,
 * and would force building everything from nothing. */
void EqView::loadBands()
{
    /* S90b - the parsing lives in `Settings`: three places need it, and three
     * copies end up diverging. See settings.hpp. */
    if (Settings::instance().eqReadBands(bands_) == 0)
        eq_preset(EQ_PRESET_HANDHELD, bands_);
}

void EqView::saveBands()
{
    Settings::instance().eqWriteBands(bands_);
}

void EqView::rebuild()
{
    Settings &s = Settings::instance();
    std::vector<ui::Item> v;

    v.push_back(titleItem(ui::tr("eq/profiles")));
    v.push_back(choiceItem(EQ_ID_PROFILE_DOCK, ui::tr("eq/profile_docked"),
                           ui::tr("eq/profile_docked_desc"), presetLabels(),
                           (int)s.eq_preset_docked));
    v.push_back(choiceItem(EQ_ID_PROFILE_HANDHELD, ui::tr("eq/profile_handheld"),
                           ui::tr("eq/profile_handheld_desc"), presetLabels(),
                           (int)s.eq_preset_handheld));

    {
        ui::Item it; it.kind = ui::Kind::Toggle; it.id = EQ_ID_AUTO_TRIM;
        it.title = ui::tr("eq/auto_trim");
        it.subtitle = ui::tr("eq/auto_trim_desc");
        it.lit = s.eq_auto_trim;
        v.push_back(std::move(it));
    }

    /* The five bands appear ONLY if one of the two profiles is "Custom". Showing
     * them otherwise would suggest they are being tuned while a ready-made
     * preset replaces them - twenty rows with no effect, and nothing on screen
     * to say so. */
    const bool custom = s.eq_preset_docked == (uint32_t)EQ_PRESET_CUSTOM
                     || s.eq_preset_handheld == (uint32_t)EQ_PRESET_CUSTOM;
    /* S91 - ONE row, not twenty. The band detail lives in the graphic equalizer:
     * five settings that answer each other do not belong in a list, and twenty
     * rows of scrolling were unmanageable (reported in those terms). */
    if (custom) {
        ui::Item r; r.kind = ui::Kind::Action; r.id = EQ_ID_TUNE;
        r.title = ui::tr("eq/adjust");
        r.subtitle = ui::tr("eq/adjust_desc");
        v.push_back(std::move(r));

        ui::Item z; z.kind = ui::Kind::Action; z.id = EQ_ID_RESET;
        z.title = ui::tr("eq/reset");
        z.subtitle = ui::tr("eq/reset_desc");
        v.push_back(std::move(z));
    }

    screen_.setItems(std::move(v));
}

bool EqView::apply(const ui::Item &it)
{
    Settings &s = Settings::instance();

    if (it.id == EQ_ID_PROFILE_DOCK || it.id == EQ_ID_PROFILE_HANDHELD) {
        const uint32_t p = (uint32_t)((it.choice_index >= 0 && it.choice_index < EQ_PRESET_COUNT)
                                      ? it.choice_index : 0);
        if (it.id == EQ_ID_PROFILE_DOCK) s.eq_preset_docked = p;
        else                             s.eq_preset_handheld = p;
        s.save();
        s.applyEq();
        rebuild();               /* makes the bands appear or disappear */
        return true;
    }

    if (it.id >= EQ_ID_BAND_BASE) {
        const int k = it.id - EQ_ID_BAND_BASE;
        const int i = k / 4, p = k % 4;
        if (i < 0 || i >= EQ_BANDS) return false;

        switch (p) {
            case 0:
                if (it.choice_index >= 0 && it.choice_index < EQ_TYPE_COUNT)
                    bands_[i].type = (eq_type_t)it.choice_index;
                saveBands();
                /* Changing the type makes the band's three other rows appear or
                 * disappear. */
                rebuild();
                return true;
            case 1:
                if (it.choice_index >= 0
                    && it.choice_index < (int)(sizeof FREQS / sizeof FREQS[0]))
                    bands_[i].freq = FREQS[it.choice_index];
                break;
            case 2:
                if (it.choice_index >= 0
                    && it.choice_index < (int)(sizeof QS / sizeof QS[0]))
                    bands_[i].q = QS[it.choice_index];
                break;
            case 3:
                if (it.choice_index >= 0
                    && it.choice_index < (int)(sizeof GAINS / sizeof GAINS[0]))
                    bands_[i].gain_db = GAINS[it.choice_index];
                break;
            default: return false;
        }
        saveBands();
        return true;
    }
    return false;
}

/* === S91 - NAVIGATION DEPENDS ON THE MODE ===
 *
 * In graphic mode the directions no longer walk a list: they PICK a band and SET
 * its gain. That is the gesture the representation suggests - the bars sit side
 * by side, the gain is vertical - and a mapping that contradicts what you see
 * has to be relearned every time.
 *
 *   left / right : the band              up / down : its gain
 *   L / R        : its frequency         X : its type     Y : its width
 *   B            : leave the mode
 *
 * The triggers carry the frequency because they can be pressed without letting
 * go of the directions: you set a gain by ear, then move the band, without
 * changing your grip. */
void EqView::step(int param, int dir)
{
    if (sel_band_ < 0 || sel_band_ >= EQ_BANDS) return;
    eq_band_t &b = bands_[sel_band_];

    switch (param) {
    case 0: {                       /* type */
        int t = (int)b.type + (dir >= 0 ? 1 : -1);
        if (t < 0) t = EQ_TYPE_COUNT - 1;
        if (t >= EQ_TYPE_COUNT) t = 0;
        b.type = (eq_type_t)t;
        break;
    }
    case 1: b.freq    = FREQS[nextNotch(FREQS, b.freq, dir)];         break;
    case 2: b.q       = QS[nextNotch(QS, b.q, dir)];                  break;
    case 3:
        /* A high-pass or a low-pass has no gain: it cuts, it does not dose. The
         * engine ignores the field; letting it move on screen would suggest a
         * setting that has no effect. */
        if (b.type == EQ_HIGHPASS || b.type == EQ_LOWPASS) return;
        b.gain_db = GAINS[nextNotch(GAINS, b.gain_db, dir)];
        break;
    default: return;
    }
    saveBands();
}

bool EqView::up()
{
    if (!graphic_mode_) return screen_.up();
    step(3, +1);
    return true;
}

bool EqView::down()
{
    if (!graphic_mode_) return screen_.down();
    step(3, -1);
    return true;
}

bool EqView::left()
{
    if (graphic_mode_) {
        if (--sel_band_ < 0) sel_band_ = EQ_BANDS - 1;
        return true;
    }
    if (!screen_.left()) return false;
    const ui::Item *it = screen_.focusedItem();
    if (it) apply(*it);
    return true;
}

bool EqView::right()
{
    if (graphic_mode_) {
        if (++sel_band_ >= EQ_BANDS) sel_band_ = 0;
        return true;
    }
    if (!screen_.right()) return false;
    const ui::Item *it = screen_.focusedItem();
    if (it) apply(*it);
    return true;
}

bool EqView::triggerL() { if (!graphic_mode_) return false; step(1, -1); return true; }
bool EqView::triggerR() { if (!graphic_mode_) return false; step(1, +1); return true; }
bool EqView::buttonX()   { if (!graphic_mode_) return false; step(0, +1); return true; }
bool EqView::buttonY()   { if (!graphic_mode_) return false; step(2, +1); return true; }

/* B leaves the MODE, it does not pop the screen: returning `true` tells
 * `cablerNavigation` the screen had something of its own to close. Without it,
 * leaving the graphic equalizer would go straight back to the settings, skipping
 * the profiles page we had just opened. */
bool EqView::back()
{
    if (!graphic_mode_) return false;
    graphic_mode_ = false;
    rebuild();
    return true;
}

bool EqView::activate()
{
    if (graphic_mode_) return false;

    const ui::Item *it = screen_.focusedItem();
    if (!it) return false;

    if (it->id == EQ_ID_TUNE) {
        graphic_mode_ = true;
        return true;
    }
    if (it->id == EQ_ID_AUTO_TRIM) {
        if (!screen_.toggle()) return false;
        const ui::Item *after = screen_.focusedItem();
        if (after) {
            Settings &s = Settings::instance();
            s.eq_auto_trim = after->lit;
            s.save();
            s.applyEq();
        }
        return true;
    }
    if (it->id == EQ_ID_RESET) {
        eq_preset(EQ_PRESET_HANDHELD, bands_);
        saveBands();
        rebuild();
        return true;
    }
    return false;
}

/* === THE CURVE ===
 *
 * It comes from the ENGINE (`audio_eq_reponse_db`) and is not recomputed here.
 * That is what makes it honest: a curve redrawn from the settings would show
 * what the user ASKED FOR, not what the filter DOES - and the two differ as soon
 * as a value is clamped, a band is refused for instability, or the automatic
 * attenuation kicks in.
 *
 * The horizontal scale is logarithmic: that is how the ear perceives
 * frequencies, and a linear scale would squash the whole bass - half of the
 * tuning - into the first centimetre. */
void EqView::drawCurve(NVGcontext *vg, float x, float y, float w, float h)
{
    paint::glassPanel(vg, x, y, w, h, paint::PANEL_RADIUS);

    const float mx = 16.0f, my = 26.0f;
    const float gx = x + mx, gy = y + my;
    const float gw = w - mx * 2.0f, gh = h - my * 2.0f;
    if (gw <= 10.0f || gh <= 10.0f) return;

    const float DB_SCALE = 15.0f;   /* +/- 15 dB from top to bottom */
    const float f_lo = 30.0f, f_hi = 16000.0f;

    /* Guides: zero as a solid line, +/- 6 and 12 dB implicitly dashed (fainter).
     * Without them, a flat curve and a slightly domed one look alike. */
    for (int d = -12; d <= 12; d += 6) {
        const float ly = gy + gh * 0.5f - (float)d / DB_SCALE * gh * 0.5f;
        nvgBeginPath(vg);
        nvgMoveTo(vg, gx, ly);
        nvgLineTo(vg, gx + gw, ly);
        nvgStrokeWidth(vg, d == 0 ? 1.2f : 0.8f);
        nvgStrokeColor(vg, nvgRGBA(150, 176, 224, d == 0 ? 90 : 34));
        nvgStroke(vg);
    }
    for (float f : { 100.0f, 1000.0f, 10000.0f }) {
        const float lx = gx + gw * logf(f / f_lo) / logf(f_hi / f_lo);
        nvgBeginPath(vg);
        nvgMoveTo(vg, lx, gy);
        nvgLineTo(vg, lx, gy + gh);
        nvgStrokeWidth(vg, 0.8f);
        nvgStrokeColor(vg, nvgRGBA(150, 176, 224, 34));
        nvgStroke(vg);
    }

    nvgBeginPath(vg);
    for (int i = 0; i <= 120; i++) {
        const float t = (float)i / 120.0f;
        const float f = f_lo * powf(f_hi / f_lo, t);
        float dbv = audio_eq_reponse_db(f);
        if (!(dbv == dbv)) dbv = 0.0f;                 /* NaN: see nav.h */
        if (dbv >  DB_SCALE) dbv =  DB_SCALE;
        if (dbv < -DB_SCALE) dbv = -DB_SCALE;
        const float px = gx + gw * t;
        const float py = gy + gh * 0.5f - dbv / DB_SCALE * gh * 0.5f;
        if (i == 0) nvgMoveTo(vg, px, py);
        else        nvgLineTo(vg, px, py);
    }
    nvgStrokeWidth(vg, 2.2f);
    nvgLineCap(vg, NVG_ROUND);
    nvgLineJoin(vg, NVG_ROUND);
    nvgStrokeColor(vg, audio_eq_active() ? paint::accentVif
                                        : nvgRGBA(140, 150, 172, 160));
    nvgStroke(vg);

    nvgFontFace(vg, theme::font());
    nvgFontSize(vg, ui::type::CAPTION);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, theme::hint);
    nvgText(vg, gx, y + 14.0f, ui::tr("eq/response").c_str(), nullptr);

    /* The ACTIVE mode is named: it is the one playing, and without that mention
     * you cannot tell which of the two profiles the curve represents. */
    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, theme::sectionT);
    nvgText(vg, gx + gw, y + 14.0f,
            ui::tr(device::isDocked() ? "eq/mode_docked" : "eq/mode_handheld").c_str(),
            nullptr);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
}

/* === S91 - THE GRAPHIC EQUALIZER ===
 *
 * Five vertical bars under the curve they produce. It is the universal
 * representation of an equalizer, and it is universal because it makes visible
 * what a list hides: two bands that overlap, a band left at full tilt, the
 * overall shape.
 *
 * The bars sit at their FREQUENCY on the curve's axis, not at equal intervals.
 * That is the only honest choice: two bands set to 500 and 630 Hz are
 * neighbours, and spreading them apart artificially would suggest they act on
 * distinct regions - that is to say hide exactly what the representation exists
 * to show. */
void EqView::drawGraphic(NVGcontext *vg, float x, float y, float w, float h,
                         double t)
{
    (void)t;
    const float MARGIN = 40.0f;
    const float gx = x + MARGIN, gw = w - MARGIN * 2.0f;
    if (gw < 200.0f) return;

    /* --- Title and active mode ------------------------------------------ */
    nvgFontFace(vg, theme::font());
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, ui::type::SCREEN);
    nvgFillColor(vg, theme::title);
    nvgText(vg, gx, y + 40.0f, ui::tr("eq/adjust").c_str(), nullptr);

    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, ui::type::SECONDARY);
    nvgFillColor(vg, theme::sectionT);
    nvgText(vg, gx + gw, y + 40.0f,
            ui::tr(device::isDocked() ? "eq/mode_docked" : "eq/mode_handheld").c_str(),
            nullptr);

    /* --- The curve, on top ----------------------------------------------- */
    const float cy = y + 66.0f;
    const float ch = (h - 66.0f - 190.0f) < 120.0f ? 120.0f : (h - 66.0f - 190.0f);
    drawCurve(vg, gx, cy, gw, ch);

    /* --- The five bars --------------------------------------------------- */
    const float by = cy + ch + 18.0f;
    const float bh = 118.0f;
    const float DB_SCALE = 12.0f;
    const float f_lo = 30.0f, f_hi = 16000.0f;
    const float COL_W = 44.0f;

    for (int i = 0; i < EQ_BANDS; i++) {
        const eq_band_t &b = bands_[i];
        const bool sel = (i == sel_band_);
        const bool off = (b.type == EQ_OFF);
        /* Placed at the FREQUENCY, clamped so that a band at 30 Hz does not fall
         * off the left edge of the frame. */
        float u = logf(b.freq / f_lo) / logf(f_hi / f_lo);
        if (u < 0.0f) u = 0.0f;
        if (u > 1.0f) u = 1.0f;
        const float bx = gx + COL_W * 0.5f + u * (gw - COL_W);

        const float mid = by + bh * 0.5f;

        /* The track. Always drawn whole: it is what gives the scale, and without
         * it a gain of +3 dB and one of +12 dB look alike. */
        nvgBeginPath(vg);
        nvgRoundedRect(vg, bx - 4.0f, by, 8.0f, bh, 4.0f);
        nvgFillColor(vg, nvgRGBA(20, 26, 42, 220));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, bx - 10.0f, mid);
        nvgLineTo(vg, bx + 10.0f, mid);
        nvgStrokeWidth(vg, 1.0f);
        nvgStrokeColor(vg, nvgRGBA(150, 176, 224, 70));
        nvgStroke(vg);

        const NVGcolor tint = off ? nvgRGBA(110, 120, 142, 150)
                            : sel ? paint::accentVif
                                  : paint::accent;

        /* The bar starts from the CENTRE: an equalizer cuts as much as it boosts,
         * and a bar always rising from the bottom would read as an attenuator. */
        if (!off && b.type != EQ_HIGHPASS && b.type != EQ_LOWPASS) {
            float g = b.gain_db;
            if (g >  DB_SCALE) g =  DB_SCALE;
            if (g < -DB_SCALE) g = -DB_SCALE;
            const float hy = mid - g / DB_SCALE * bh * 0.5f;
            nvgBeginPath(vg);
            nvgRoundedRect(vg, bx - 4.0f, g >= 0.0f ? hy : mid, 8.0f,
                           fabsf(hy - mid), 4.0f);
            nvgFillColor(vg, tint);
            nvgFill(vg);

            /* The knob, at the value. It carries the focus: it is what the eye
             * follows while tuning. */
            nvgBeginPath(vg);
            nvgRoundedRect(vg, bx - 15.0f, hy - 5.0f, 30.0f, 10.0f, 5.0f);
            nvgFillColor(vg, sel ? nvgRGBA(230, 240, 255, 255)
                                 : nvgRGBA(150, 170, 205, 230));
            nvgFill(vg);
        } else if (!off) {
            /* High-pass or low-pass: no gain to show. We draw a step rather than
             * a bar - the shape says what the filter does, and does not suggest a
             * gain left at zero. */
            const float s = (b.type == EQ_HIGHPASS) ? 1.0f : -1.0f;
            nvgBeginPath(vg);
            nvgMoveTo(vg, bx - 13.0f, mid + 16.0f * s);
            nvgLineTo(vg, bx,        mid + 16.0f * s);
            nvgLineTo(vg, bx,        mid - 16.0f * s);
            nvgLineTo(vg, bx + 13.0f, mid - 16.0f * s);
            nvgStrokeWidth(vg, 3.0f);
            nvgLineCap(vg, NVG_ROUND);
            nvgLineJoin(vg, NVG_ROUND);
            nvgStrokeColor(vg, tint);
            nvgStroke(vg);
        }

        /* Selection halo: it surrounds the WHOLE column, not just the knob - it
         * is the band that was picked, not a value. */
        if (sel) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, bx - COL_W * 0.5f, by - 8.0f, COL_W, bh + 62.0f,
                           paint::CARD_RADIUS);
            nvgStrokeWidth(vg, 1.6f);
            nvgStrokeColor(vg, nvgTransRGBA(paint::accentVif, 150));
            nvgStroke(vg);
        }

        /* Under the bar: frequency, type, width. Three tight lines, because an
         * equalizer is tuned by looking at the numbers as much as at the
         * shape. */
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontSize(vg, ui::type::CAPTION);
        nvgFillColor(vg, sel ? theme::title : theme::label);
        nvgText(vg, bx, by + bh + 14.0f, fmtFreq(b.freq).c_str(), nullptr);

        nvgFillColor(vg, sel ? theme::sectionT : theme::hint);
        nvgText(vg, bx, by + bh + 30.0f,
                ui::tr(eq_type_key(b.type)).c_str(), nullptr);

        if (!off) {
            char q[24];
            std::snprintf(q, sizeof q, "Q %.2f", (double)b.q);
            nvgText(vg, bx, by + bh + 46.0f, q, nullptr);
        }

        /* The gain spelled out next to the knob: the bar gives the shape, the
         * number gives the value, and you need both to reproduce a setting. */
        if (!off && b.type != EQ_HIGHPASS && b.type != EQ_LOWPASS) {
            nvgFontSize(vg, ui::type::SECONDARY);
            nvgFillColor(vg, sel ? theme::title : theme::value);
            nvgText(vg, bx, by - 16.0f, fmtGain(b.gain_db).c_str(), nullptr);
        }
    }

    /* --- The help line, and it is indispensable --------------------------
     * Six commands on six different buttons cannot be guessed, and there is
     * neither hover nor context menu on a console. This is the only place where
     * you learn that X changes the type. */
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, ui::type::CAPTION);
    nvgFillColor(vg, theme::hint);
    nvgText(vg, x + w * 0.5f, y + h - 24.0f, ui::tr("eq/help").c_str(), nullptr);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
}

void EqView::paint(NVGcontext *vg, float x, float y, float w, float h, double t)
{
    if (graphic_mode_) {
        paint::shadowBg(vg, x, y, w, h);
        paint::backgroundWaves(vg, x, y, w, h, t);
        drawGraphic(vg, x, y, w, h, t);
        return;
    }

    /* The curve takes the right-hand side, the list the left. Below a certain
     * width we give ALL the room back to the list: a curve ten square
     * centimetres across can no longer be read, and it is the list that lets you
     * tune. */
    const float CURVE_W = 380.0f;
    const bool with_curve = w > CURVE_W * 2.2f;

    /* EQ-BG 2026-09-26 - the background spans the WHOLE page. The list used to
     * paint it over its own width only, so the curve's column showed the white
     * clear colour behind it - on every target, since the page exists. */
    paint::shadowBg(vg, x, y, w, h);
    paint::backgroundWaves(vg, x, y, w, h, t);
    screen_.setPaintsBackground(false);
    screen_.draw(vg, x, y, with_curve ? w - CURVE_W : w, h, t);

    if (with_curve) {
        const float ch = 220.0f;
        drawCurve(vg, x + w - CURVE_W, y + (h - ch) * 0.5f,
                  CURVE_W - 28.0f, ch);
    }

    if (screen_.touchRelease() >= 0) activate();
}
