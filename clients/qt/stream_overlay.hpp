/* stream_overlay - the in-stream "gaming" overlay: a HUD and a menu.
 *
 * === OV1 2026-10-03 — WHY TWO TOP-LEVEL WINDOWS, NOT CHILDREN ===============
 *
 * On Windows `QVideoWidget` is a NATIVE child window: it paints over any
 * non-native sibling whatever the Qt stacking order (this already defeated a
 * floated button - IN3). An overlay drawn ON the stream therefore cannot be a
 * child of the video. It is instead a FRAMELESS, TRANSLUCENT TOP-LEVEL window
 * positioned over the video's screen rectangle, which composites above a native
 * surface and works the same in a window and in fullscreen.
 *
 * Two of them, because they have opposite input needs:
 *
 *   StreamHud  - the corner metrics. DISPLAY ONLY, and CLICK-THROUGH
 *     (Qt::WindowTransparentForInput): a HUD that ate clicks meant for the game
 *     would be unusable. Shown whenever the HUD is enabled.
 *
 *   StreamOverlay - the menu (volume, equaliser, which metrics to show, quick
 *     actions). INTERACTIVE, so it is a normal input window; it is shown on the
 *     configurable hotkey and takes the keyboard while open - which also stops
 *     the stream view from forwarding those keys to the VM, the behaviour a
 *     pause menu wants.
 *
 * Both follow the video's global geometry (MainWindow repositions them on move,
 * resize and fullscreen). Everything they drive is live core API: volume
 * (`audio_set_volume`, 0..300, mid-stream) and the five-band equaliser
 * (`audio_eq_configure`), both documented usable while a stream runs.
 */
#pragma once

#include <QPoint>
#include <QRect>
#include <QWidget>

#include "hud_model.hpp"
#include "overlay_paint.hpp"
#include "rate_meter.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QPushButton;
class QSlider;
class QTabWidget;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace halyard {

/* ------------------------------------------------------------ the HUD -----
 *
 * OV7: the Borealis layout model, in Qt. The HUD is ONE transparent window the
 * size of the video, holding one BLOCK per enabled section and per enabled
 * chart. Each block carries a normalised anchor in [0,1] so a layout survives a
 * resolution change, and may name another block as its LEADER - the two then
 * move as one panel. The model is deliberately poor, as Borealis's is: a block
 * has a leader or it has none, no nested groups.
 *
 * Normally the window is CLICK-THROUGH, so the HUD can never eat a shot in a
 * game. Edit mode re-creates it as an input window: blocks drag with the left
 * button, a drop near another block snaps them into one group, a right-click
 * detaches, and a drop near an edge magnetises to it.
 */
class StreamHud : public QWidget {
    Q_OBJECT
public:
    explicit StreamHud(QWidget *parent = nullptr);

    void setMasks(int sections, int charts);
    int  sections() const { return secMask_; }
    int  charts() const { return chartMask_; }
    bool anything() const { return secMask_ != 0 || chartMask_ != 0; }

    void setPresentedCounter(std::function<quint64()> fn) { presented_ = std::move(fn); }

    /* === HUD2 2026-10-03 — GLASS OVER OUR OWN PICTURE ====================
     *
     * `backdrop-filter` does not exist in Qt Widgets, and the Windows
     * acrylic attributes blur the DESKTOP rather than the video widget
     * underneath this tool window - so neither gives glass over the stream.
     * The blur is computed from the decoded frame, which is the only thing
     * that actually IS behind the HUD.
     *
     * A registered source rather than a pointer to the video widget: this
     * class must not know what a VideoWidget is, for the same reason core
     * hands its frames over by a sink. Returns a null QImage when there is
     * no picture yet, and the HUD then falls back to a flat fill. */
    void setFrameSource(std::function<QImage()> fn) { frameSource_ = std::move(fn); }

    /* HUD4 - the two quota figures, from `/vms/{id}/capabilities` by way of
     * the window. Seconds; 0 means the server did not say and the block then
     * says nothing rather than inventing a ceiling. */
    void setQuotas(int sessionCeilingSec, int sessionElapsedSec,
                   int periodAllowanceSec, int periodUsedSec);

    /* The HUD covers this rectangle; blocks are placed inside it. */
    void placeOver(const QRect &videoGlobalRect);

    /* OV7 - presentation. Refresh is the sampling cadence (and the charts'
     * sample period with it); scale and opacity are the Borealis knobs. */
    void setRefreshMs(int ms);
    int  refreshMs() const { return refreshMs_; }
    void setScalePercent(int p);
    int  scalePercent() const { return scalePct_; }
    void setOpacityPercent(int p);
    int  opacityPercent() const { return opacityPct_; }

    void setEditing(bool on);
    bool editing() const { return editing_; }

    /* "id:ax,ay,leader;..." - what the settings persist. */
    QString layoutString() const;
    void    setLayoutString(const QString &s);
    void    resetLayout();

signals:
    void layoutChanged();

protected:
    bool eventFilter(QObject *o, QEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private slots:
    void refresh();

private:
    /* HUD2 - recompute the blurred backdrop. Called on the refresh tick, not
     * on every paint: measured at 0.29 ms for one block and 0.77 ms for a
     * column of them (2560x1440 source, crop + 1/10 downscale + upscale), so
     * twice a second is ~1.5 ms of CPU per second. Per paint it would run on
     * every mouse move during a drag. */
    void stackDefaults();   /* HUD3 */
    void rebuildGlass();
    std::function<QImage()> frameSource_;
    QImage  glass_;        /* the blurred picture behind the whole HUD */
    int     qSessionCeil_ = 0, qSessionUsed_ = 0;   /* HUD4 */
    int     qPeriodCeil_ = 0,  qPeriodUsed_ = 0;
    QRect   glassRect_;    /* where in the video it was taken from */

private:
    struct RowW  { const HudRow *spec = nullptr; QWidget *host = nullptr;
                   QLabel *name = nullptr, *value = nullptr; };
    struct ChartW{ const HudChartSpec *spec = nullptr; QLabel *value = nullptr;
                   Sparkline *spark = nullptr; };

    /* A block: a section (title + rows) or one chart. */
    struct Blk {
        QString   id;
        bool      placed = false;   /* HUD3 - moved by the user */
        int       section = 0;          /* 0 for a chart block */
        int       chartBit = 0;         /* 0 for a section block */
        QWidget  *card = nullptr;
        QVBoxLayout *body = nullptr;
        QVector<RowW> rows;
        ChartW    chart;
        QVBoxLayout *latLay = nullptr;  /* the latency block builds per tick */

        /* === HUD4 2026-10-03 — THE BLOCKS THE DESIGN ASKED FOR ==========
         *
         * The HUD was a label/value list for every section, which is a
         * debug dump: four readings people actually watch - frames, latency,
         * bitrate, loss - were four rows among twenty, in the same size and
         * the same weight as "orphan chunks".
         *
         * `tiles` is the Performance block rebuilt as four big numbers, and
         * `quota` is new: the session countdown and the period's allowance,
         * which the client has known since CAPS2 and never showed in the
         * stream. */
        struct Tile { QLabel *value = nullptr; QLabel *unit = nullptr; };
        QVector<Tile> tiles;
        struct Quota {
            QLabel *sessionText = nullptr, *monthText = nullptr;
            QWidget *sessionBar = nullptr, *monthBar = nullptr;
            QWidget *sessionFill = nullptr, *monthFill = nullptr;
        } quota;
        QToolButton *detach = nullptr;
        double    ax = 0.0, ay = 0.0;
        QString   leader;               /* empty = standalone */
    };

    /* HUD4 - declared after `Blk`, which they take by reference. */
    QWidget *buildTiles(Blk &b);
    QWidget *buildQuota(Blk &b);
    void     refreshTiles(Blk &b, const HudSnap &s);
    void     refreshQuota(Blk &b);

    void rebuild();
    void layoutBlocks();
    void applyStyle();
    QRect usableRect(const QSize &blockSize) const;
    int   blockIndex(const QString &id) const;
    void  moveGroup(const QString &leaderId, double dax, double day);
    void  snapOrMagnetise(int idx);
    void  updateDetachButtons();
    bool  isLeader(const QString &id) const;

    QVector<Blk> blocks_;
    QTimer *timer_ = nullptr;
    int     secMask_ = SecDefault;
    int     chartMask_ = ChDefault;
    int     refreshMs_ = 500;
    int     scalePct_ = 100;
    int     opacityPct_ = 100;
    int     bubbleAlpha_ = 185;
    bool    editing_ = false;
    HudMeters meters_;
    std::function<quint64()> presented_;
    QRect   lastRect_;

    /* drag state (edit mode) */
    int     dragIdx_ = -1;
    QPoint  dragStart_;
    QPoint  dragOrigin_;
    bool    dragDetached_ = false;
};

/* -------------------------------------------------------- the menu -------- */
class StreamOverlay : public QWidget {
    Q_OBJECT
public:
    explicit StreamOverlay(QWidget *parent = nullptr);
    void placeOver(const QRect &videoGlobalRect);
    /* Sets the HUD checkboxes without emitting - MainWindow calls it once with
     * the persisted mask so the controls match what the HUD shows. */
    void setInitialHud(int sections, int charts);
    void setHudPresentation(int refreshMs, int scalePct, int opacityPct);
    void setHudEditing(bool on);

signals:
    void hudMasksChanged(int sections, int charts);  /* persisted + applied */
    void hudRefreshChanged(int ms);
    void hudScaleChanged(int percent);
    void hudOpacityChanged(int percent);
    void hudEditToggled(bool on);
    void hudLayoutReset();
    void requestFullscreenToggle();
    void requestDisconnect();
    void requestScreenshot();
    void requestFiles();
    void requestMetrics();
    void requestSettings();
    void requestCopyDiagnostics();
    void toast(const QString &text);    /* a transient note over the stream */
    void cursorSourceChanged(int src);   /* 0 none, 1 VM image, 2 local */
    void closed();

protected:
    void changeEvent(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void showEvent(QShowEvent *e) override;

private:
    void retranslate();
    void applyEq();
    void loadFromCore();      /* volume + eq_active reflected into the controls */

    /* The tab bodies. Each builds its own controls and wires them to the core
     * API or the SHADOW_* variable it drives. */
    QWidget *buildSoundTab();
    QWidget *buildVideoTab();
    QWidget *buildInputTab();
    QWidget *buildToolsTab();
    QWidget *buildHudTab();

    /* OV2 - writes a SHADOW_* variable the way the settings window does:
     * setenv + persist, refused when the environment set it from outside.
     * `liveNote` is the toast shown, so a row that only takes effect next
     * session says so rather than looking inert. */
    void writeEnv(const QString &env, const QString &value, bool live);

    QTabWidget *tabs_ = nullptr;

    /* audio */
    QSlider *volume_ = nullptr;
    QLabel  *volumeVal_ = nullptr;
    QPushButton *muteBtn_ = nullptr;
    int      premute_ = 100;
    EqCurve *eqCurve_ = nullptr;
    QCheckBox *eqOn_ = nullptr;
    QCheckBox *eqAutoTrim_ = nullptr;
    struct Band {
        QComboBox      *type = nullptr;
        QDoubleSpinBox *freq = nullptr;
        QDoubleSpinBox *q    = nullptr;
        QDoubleSpinBox *gain = nullptr;
    };
    Band    bands_[5];
    QComboBox *eqPreset_ = nullptr;

    /* HUD metric toggles */
    QCheckBox *secBox_[7] = { nullptr };
    QCheckBox *chartBox_[8] = { nullptr };
    QComboBox *refreshBox_ = nullptr;
    QSlider   *scaleSlider_ = nullptr, *opacitySlider_ = nullptr;
    QPushButton *editBtn_ = nullptr, *resetLayoutBtn_ = nullptr;
    QComboBox *cursorSrc_ = nullptr;
    QGroupBox *gCombos_ = nullptr;
    QPushButton *shotBtn_ = nullptr, *filesBtn_ = nullptr, *metBtn_ = nullptr,
                *setBtn_ = nullptr, *diagBtn_ = nullptr;

    QLabel      *titleAudio_ = nullptr, *titleEq_ = nullptr, *titleHud_ = nullptr;
    QGroupBox   *gAudio_ = nullptr, *gEq_ = nullptr, *gHud_ = nullptr;
    QPushButton *fsBtn_ = nullptr, *discBtn_ = nullptr, *closeBtn_ = nullptr;
};

}  // namespace halyard
