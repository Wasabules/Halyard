/* MainWindow - see main_window.hpp for the four states and the three threads. */
#include "main_window.hpp"

#include "auth_worker.hpp"
#include "core_scope.hpp"

extern "C" {
#include "core/services/config.h"   /* AUTH10 - SHADOW_OAUTH_CLIENT_ID */
}
#include "session_worker.hpp"
#include "step_list_widget.hpp"
#include "video_widget.hpp"
#include "settings_window.hpp"
#include "metrics_window.hpp"
#include "file_manager_window.hpp"
#include "about_dialog.hpp"
#include "account_window.hpp"
#include "machine_card.hpp"
#include "pairing_widgets.hpp"
#include "shortcuts.hpp"
#include "stream_overlay.hpp"
#include "theme.hpp"

extern "C" {
#include "core/version.h"
#include "core/input/kbd_hook_win.h"
#include "core/services/stats.h"
#include "core/protocol/session_caps.h"
#include "core/session/ctrl_session_glue.h"
}

/* === IN2 2026-10-03 — THE WINDOWS SYSTEM-SHORTCUT HOOK =====================
 *
 * `core/input/kbd_hook_win.c` installs a WH_KEYBOARD_LL hook that sends the keys
 * Windows keeps for itself - Alt+Tab, the Windows key, Ctrl+Esc, Alt+Esc - to
 * the VM and swallows them locally, but only while this weak symbol says the
 * stream owns the keyboard. Borealis defines it from its own focus state; this
 * is the Qt client's definition.
 *
 * It must be true ONLY when a key should go to the VM and not to this desktop:
 * a session is live, the main window is the active window, we are on the
 * streaming page, and the video surface holds the focus. Open the settings
 * window, alt-tab away, or go back to the machine list and it is false at once -
 * which is the escape hatch that stops the hook from locking the user out of
 * their own desktop. Evaluated on every keystroke on the GUI thread (the hook
 * is dispatched there), so touching Qt state here is safe. */
static MainWindow *g_main_window = nullptr;

#include <QStackedWidget>
#include <QScreen>
#include <QThread>
#include <QLabel>
#include <QApplication>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QGraphicsDropShadowEffect>
#include <QLineEdit>
#include <QScrollArea>
#include <QDateTime>
#include <QVariantMap>
#include <algorithm>
#include <QFrame>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFont>
#include <QClipboard>
#include <QGuiApplication>
#include <QWindow>
#include <QStatusBar>
#include <QMessageBox>
#include <QtConcurrent>
#include <QMenuBar>
#include <QKeySequence>
#include <QShortcut>
#include <QSettings>
#include <QTimer>
#include <QMoveEvent>
#include <QStandardPaths>
#include <QDateTime>
#include <QDir>
#include <QImage>
#include <QSignalBlocker>
#include <QGuiApplication>
#include <QEvent>
#include <QDesktopServices>
#include <QUrl>

using halyard::str;

extern "C" bool halyard_ui_keys_blocked(void)
{
    return g_main_window && g_main_window->keysGoToVm();
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    g_main_window = this;
    setWindowTitle(str(SHADOW_APP_NAME));
    setWindowIcon(halyard::theme::appTileIcon());
    resize(1280, 760);

    /* === D1 2026-10-03 — ONE HEADER, NOT THREE TOPS ======================
     *
     * Each page invented its own top: the pairing card had the mark and the
     * name, the machine list a title and a count, the connecting screen a
     * headline. So moving between them changed the whole window and nothing
     * said it was still the same application.
     *
     * A slim strip above the stack instead: the mark, where you are, and the
     * account. It is HIDDEN on the pairing page, which has the hero and would
     * otherwise show the mark twice, and on the streaming page, where every
     * pixel belongs to the picture. That is two exceptions out of four, which
     * is the sort of thing that argues against a shared header - but the two
     * that keep it are the two you move between while using the client. */
    auto *shell = new QWidget(this);
    auto *shellLay = new QVBoxLayout(shell);
    shellLay->setContentsMargins(0, 0, 0, 0);
    shellLay->setSpacing(0);

    header_ = new QWidget(shell);
    header_->setFixedHeight(52);
    {
        auto *hl = new QHBoxLayout(header_);
        hl->setContentsMargins(halyard::theme::SpacePage, 0,
                               halyard::theme::SpacePage, 0);
        hl->setSpacing(halyard::theme::SpaceRow);

        auto *mark = new QLabel(header_);
        /* ICON1 - the tile, not the strokes: at 22 px three palette-grey
         * lines were the same smudge the taskbar was showing. */
        mark->setPixmap(halyard::theme::appTileIcon().pixmap(22, 22));

        header_title_ = new QLabel(header_);
        header_title_->setProperty("h2", true);

        header_account_ = new QLabel(header_);
        header_account_->setProperty("dim", true);

        hl->addWidget(mark);
        hl->addWidget(header_title_);
        hl->addStretch(1);
        hl->addWidget(header_account_);

        /* A hairline under it rather than a filled bar: the bar competed with
         * the cards for being the brightest thing on the page. */
        header_->setStyleSheet(
            QStringLiteral("border-bottom: 1px solid %1;")
                .arg(halyard::theme::border(header_).name()));
    }
    shellLay->addWidget(header_);

    stack_ = new QStackedWidget(shell);
    shellLay->addWidget(stack_, 1);
    setCentralWidget(shell);

    /* ---------------------------------------------------- page 1: pairing
     *
     * === UI4 2026-10-03 - THE FIRST SCREEN =================================
     *
     * Two columns: a hero on the left that says what this application is, and
     * the work on the right. A single centred card on a grey window is a
     * DIALOG, and a dialog is what you dismiss - this is the screen someone
     * sees before they have decided to trust the thing.
     *
     * The right column is ordered by what is being waited for. Before a code
     * arrives: the three sign-in phases, moving, so a slow data centre is not
     * mistaken for a frozen window. Once it arrives: the QR first, because
     * scanning it removes the transfer entirely, then the code in cells for
     * whoever has no phone to hand, then the two buttons.
     *
     * See pairing_widgets.hpp for why the QR, the cells and the bar are each
     * painted rather than being a label. */
    {
        auto *page = new QWidget(this);
        auto *outer = new QVBoxLayout(page);
        outer->setAlignment(Qt::AlignCenter);

        auto *card = new QFrame(page);

        card->setProperty("card", true);
        card->setGraphicsEffect(halyard::theme::elevation(card, 24));
        card->setMaximumWidth(520);
        auto *lay = new QVBoxLayout(card);
        lay->setContentsMargins(32, 28, 32, 28);
        lay->setSpacing(halyard::theme::SpaceRow);

        /* === UI8 2026-10-03 — THE HERO IS GONE ==========================
         *
         * It was a 330 px blue column carrying a mark, a name, a tagline and
         * a version, and it was wrong twice.
         *
         * Wrong in code: the gradient was set with a SELECTOR-LESS
         * stylesheet, which Qt applies to the widget AND EVERY DESCENDANT -
         * so each label repainted its own slice of the gradient and the
         * panel came out as four lighter rectangles floating on a darker
         * one. The same trap as the step rows' triple left border, two days
         * apart.
         *
         * Wrong in design, and that is the part worth keeping: even painted
         * correctly it was a tall empty slab whose job was to say the name of
         * an application the person had already launched. The sign-in screen
         * has one thing to do and the card was already doing it.
         *
         * So the identity is a mark and a name at the top of the card, at the
         * size a heading deserves, and the window is one centred column. */
        {
            auto *mark = new QLabel(card);
            mark->setPixmap(halyard::theme::appTileIcon().pixmap(44, 44));
            mark->setAlignment(Qt::AlignCenter);
            auto *name = new QLabel(str(SHADOW_APP_NAME), card);
            name->setProperty("h1", true);
            name->setAlignment(Qt::AlignCenter);
            pair_tagline_ = new QLabel(card);
            pair_tagline_->setProperty("dim", true);
            pair_tagline_->setAlignment(Qt::AlignCenter);
            pair_tagline_->setWordWrap(true);
            lay->addWidget(mark);
            lay->addWidget(name);
            lay->addWidget(pair_tagline_);
            lay->addSpacing(halyard::theme::SpaceGroup);
        }

        pair_heading_ = new QLabel(card);
        pair_heading_->setProperty("h2", true);
        pair_heading_->setAlignment(Qt::AlignCenter);

        /* UI4 - the three phases, above everything, and hidden the moment a
         * code is in hand: by then they have nothing left to say and would
         * only be pushing the code down. */
        signin_steps_ = new SignInSteps(card);

        pair_hint_ = new QLabel(card);
        pair_hint_->setAlignment(Qt::AlignCenter);
        pair_hint_->setWordWrap(true);
        /* The hint carries the sign-in URL as a real link: rich text, opened in
         * the system browser. Without TextBrowserInteraction + openExternalLinks
         * an <a href> renders as blue text that does nothing. */
        pair_hint_->setTextFormat(Qt::RichText);
        pair_hint_->setTextInteractionFlags(Qt::TextBrowserInteraction);
        pair_hint_->setOpenExternalLinks(true);

        qr_ = new halyard::QrView(card);
        qr_->setVisible(false);
        qr_caption_ = new QLabel(card);
        qr_caption_->setProperty("dim", true);
        qr_caption_->setAlignment(Qt::AlignCenter);
        qr_caption_->setVisible(false);
        auto *qrRow = new QHBoxLayout;
        qrRow->addStretch(1);
        qrRow->addWidget(qr_);
        qrRow->addStretch(1);

        pair_cells_ = new halyard::CodeCells(card);
        pair_cells_->setVisible(false);

        /* === UI2 - THE DEADLINE, AND A WAY BACK FROM IT ====================
         *
         * A device code is good for 600 s and then the grant is dead. Two
         * things were missing, both of them reported: the deadline was only in
         * the status bar, where nobody looking at a code sees it; and when it
         * passed, the card said "sign-in failed" and that was the end - the
         * only way to try again was to kill the application.
         *
         * So the remaining time sits under the code, and the failure gets a
         * button. `AuthWorker::signIn` keeps no state across calls (it clears
         * its own stop flag and every handle is scoped), so a retry is the same
         * call the constructor makes - not a second code path. */
        pair_validity_ = new halyard::ValidityBar(card);
        pair_validity_->setVisible(false);
        pair_countdown_ = new QLabel(QString(), card);
        pair_countdown_->setAlignment(Qt::AlignCenter);
        pair_countdown_->setProperty("dim", true);
        pair_countdown_->setVisible(false);

        copy_code_btn_ = new QPushButton(card);
        open_page_btn_ = new QPushButton(card);
        open_page_btn_->setProperty("accent", true);
        retry_btn_     = new QPushButton(card);
        retry_btn_->setProperty("accent", true);
        copy_code_btn_->setVisible(false);
        open_page_btn_->setVisible(false);
        retry_btn_->setVisible(false);
        auto *btnRow = new QHBoxLayout;
        btnRow->setSpacing(halyard::theme::SpaceRow);
        btnRow->addStretch(1);
        btnRow->addWidget(copy_code_btn_);
        btnRow->addWidget(open_page_btn_);
        btnRow->addWidget(retry_btn_);
        btnRow->addStretch(1);

        connect(retry_btn_, &QPushButton::clicked, this, &MainWindow::restartSignIn);

        connect(copy_code_btn_, &QPushButton::clicked, this, [this] {
            if (QClipboard *cb = QGuiApplication::clipboard())
                cb->setText(pair_user_code_);
            statusBar()->showMessage(tr("Code copied to the clipboard."), 4000);
        });
        connect(open_page_btn_, &QPushButton::clicked, this, [this] {
            const QString u = !pair_uri_complete_.isEmpty() ? pair_uri_complete_
                                                            : pair_uri_;
            if (!u.isEmpty()) QDesktopServices::openUrl(QUrl(u));
        });

        lay->addWidget(pair_heading_);
        lay->addWidget(signin_steps_);
        lay->addWidget(pair_hint_);
        lay->addLayout(qrRow);
        lay->addWidget(qr_caption_);
        lay->addSpacing(4);
        lay->addWidget(pair_cells_);
        lay->addWidget(pair_validity_);
        lay->addWidget(pair_countdown_);
        lay->addSpacing(4);
        lay->addLayout(btnRow);

        outer->addWidget(card);
        stack_->addWidget(page);
    }

    /* --------------------------------------------------- page 2: machines
     *
     * UI1 - one card per machine in a scroll area, with the state as a pill and
     * Connect on the card itself. The old shared button at the bottom made you
     * infer its target from the selection. */
    {
        auto *page = new QWidget(this);
        auto *lay = new QVBoxLayout(page);
        lay->setContentsMargins(halyard::theme::SpacePage, halyard::theme::SpaceGroup,
                                halyard::theme::SpacePage, halyard::theme::SpaceGroup);
        lay->setSpacing(halyard::theme::SpaceRow);

        /* === UI8 2026-10-03 — THE PAGE SAID ITS NAME TWICE ================
         *
         * D1 put the page name in the shared header, and this row kept its
         * own copy; "Your machines" appeared twice, one line apart, and so
         * did the count (here and in the status bar). Shrinking the second
         * title to h2 made it look deliberate rather than fixing it.
         *
         * The title goes. The row stays because the filter and the count
         * hang off it, and the count stays HERE rather than in the header
         * because it belongs next to the thing it counts. */
        auto *head = new QHBoxLayout;
        machines_title_ = new QLabel(page);
        machines_title_->setVisible(false);
        machines_subtitle_ = new QLabel(page);
        machines_subtitle_->setProperty("dim", true);
        head->addWidget(machines_title_);
        head->addStretch(1);
        /* UI6 - a filter, shown only past six machines. Below that it is a
         * control that costs a row and saves nothing; past it, reading a
         * scrolling list to find one name is the slowest thing on the page. */
        machines_filter_ = new QLineEdit(page);
        machines_filter_->setClearButtonEnabled(true);
        machines_filter_->setMaximumWidth(240);
        machines_filter_->setVisible(false);
        connect(machines_filter_, &QLineEdit::textChanged, this,
                &MainWindow::applyMachineFilter);
        head->addWidget(machines_filter_);
        head->addWidget(machines_subtitle_);
        lay->addLayout(head);

        auto *scroll = new QScrollArea(page);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        /* UI5 - the arrows arrive here, because the scroll area is what has
         * the focus before any card does and what keeps it when a card is
         * removed. An event filter rather than a QScrollArea subclass: there
         * is nothing else to subclass it for. */
        machines_scroll_ = scroll;
        scroll->installEventFilter(this);
        scroll->setFocusPolicy(Qt::StrongFocus);
        machines_host_ = new QWidget(scroll);
        machines_lay_ = new QVBoxLayout(machines_host_);
        machines_lay_->setContentsMargins(0, 0, 0, 0);
        machines_lay_->setSpacing(halyard::theme::SpaceRow);
        machines_lay_->addStretch(1);
        scroll->setWidget(machines_host_);
        lay->addWidget(scroll, 1);

        /* The two states a list can be in besides "full", said plainly rather
         * than left as an empty rectangle. */
        machines_empty_ = new QLabel(page);
        machines_empty_->setAlignment(Qt::AlignCenter);
        machines_empty_->setProperty("dim", true);
        machines_empty_->setWordWrap(true);
        lay->addWidget(machines_empty_);

        /* === D5 — THE LIST CAN ALSO FAIL ==================================
         *
         * A refused token or a dead network produced an empty page and one
         * line in the status bar, so the two cases a person must tell apart -
         * "you have no machines" and "we could not ask" - looked identical.
         * This says which, and offers the retry, because a network blink is
         * the common cause and it needs no more than asking again. */
        machines_error_ = new QFrame(page);
        machines_error_->setProperty("card", true);
        machines_error_->setVisible(false);
        {
            auto *el = new QVBoxLayout(machines_error_);
            el->setContentsMargins(halyard::theme::SpaceGroup,
                                   halyard::theme::SpaceGroup,
                                   halyard::theme::SpaceGroup,
                                   halyard::theme::SpaceGroup);
            el->setSpacing(halyard::theme::SpaceRow);
            machines_error_text_ = new QLabel(machines_error_);
            machines_error_text_->setWordWrap(true);
            machines_error_text_->setTextInteractionFlags(Qt::TextSelectableByMouse);
            machines_error_retry_ = new QPushButton(machines_error_);
            machines_error_retry_->setProperty("accent", true);
            connect(machines_error_retry_, &QPushButton::clicked, this,
                    &MainWindow::listMachines);
            auto *er = new QHBoxLayout;
            er->addStretch(1);
            er->addWidget(machines_error_retry_);
            el->addWidget(machines_error_text_);
            el->addLayout(er);
        }
        lay->addWidget(machines_error_);

        stack_->addWidget(page);
    }


    /* ------------------------------------------------- page 3: connecting */
    steps_ = new StepListWidget(this);
    stack_->addWidget(steps_);

    /* UI3 - a bootstrap that fails is recoverable: the machine was asleep, the
     * video server was not up yet, the network blinked. Retry re-runs the SAME
     * `connectTo` with the machine we were given, so there is one path in. */
    connect(steps_, &StepListWidget::retryRequested, this, [this] {
        if (!last_machine_id_.isEmpty()) connectTo(last_machine_id_);
    });
    connect(steps_, &StepListWidget::backRequested, this, [this] {
        setPage(PageMachines);
        setMachinesBusy(false);
    });
    /* UI7 - give up while it is still running. The abort flag is the same one
     * a server-side end raises, so the teardown is the one path and not a
     * second one written for Cancel. */
    connect(steps_, &StepListWidget::cancelRequested, this, [this] {
        if (sess_) sess_->requestStop();
        if (boot_) boot_->requestStop();
        session_live_ = false;
        setMachinesBusy(false);
        setPage(PageMachines);
    });

    /* -------------------------------------------------- page 4: streaming */
    video_ = new VideoWidget(this);
    stack_->addWidget(video_);

    /* IN3 - the stream overlay's actions. Disconnect raises the session's abort
     * flag; the `finished` signal then does the teardown, the same path as a
     * server-side end, so there is one cleanup and not two. */
    connect(video_, &VideoWidget::requestFullscreenToggle, this,
            &MainWindow::toggleFullscreen);
    connect(video_, &VideoWidget::requestSettings, this, &MainWindow::openSettings);
    connect(video_, &VideoWidget::requestFiles, this, &MainWindow::openFileManager);
    connect(video_, &VideoWidget::requestDisconnect, this, [this] {
        if (session_live_ && sess_) sess_->requestStop();
    });

    /* === OV1 - the in-stream overlay (HUD + menu) ======================== *
     *
     * Two frameless top-level windows over the video (see stream_overlay.hpp for
     * why not children). Created once; shown and positioned while streaming. */
    {
        QSettings st;
        const int secMask = st.value(QStringLiteral("ui/hud_sections"),
                                     (int)halyard::SecDefault).toInt();
        const int chMask  = st.value(QStringLiteral("ui/hud_charts"),
                                     (int)halyard::ChDefault).toInt();
        stream_hud_ = new halyard::StreamHud(this);
        stream_hud_->setMasks(secMask, chMask);
        /* The one number core cannot know: what this client has actually put on
         * screen. Read through a callback so the HUD never holds the widget. */
        stream_hud_->setPresentedCounter([this] {
            return video_ ? video_->framesPresented() : 0;
        });
        /* OV7 - presentation and the saved block layout. */
        const int hudMs    = st.value(QStringLiteral("ui/hud_refresh_ms"), 500).toInt();
        const int hudScale = st.value(QStringLiteral("ui/hud_scale"), 100).toInt();
        const int hudOpac  = st.value(QStringLiteral("ui/hud_opacity"), 100).toInt();
        stream_hud_->setRefreshMs(hudMs);
        stream_hud_->setScalePercent(hudScale);
        stream_hud_->setOpacityPercent(hudOpac);
        stream_hud_->setLayoutString(
            st.value(QStringLiteral("ui/hud_layout")).toString());
        connect(stream_hud_, &halyard::StreamHud::layoutChanged, this, [this] {
            QSettings().setValue(QStringLiteral("ui/hud_layout"),
                                 stream_hud_->layoutString());
        });

        stream_overlay_ = new halyard::StreamOverlay(this);
        stream_overlay_->setInitialHud(secMask, chMask);
        stream_overlay_->setHudPresentation(hudMs, hudScale, hudOpac);
        connect(stream_overlay_, &halyard::StreamOverlay::hudRefreshChanged, this,
                [this](int ms) {
                    QSettings().setValue(QStringLiteral("ui/hud_refresh_ms"), ms);
                    stream_hud_->setRefreshMs(ms);
                });
        connect(stream_overlay_, &halyard::StreamOverlay::hudScaleChanged, this,
                [this](int p) {
                    QSettings().setValue(QStringLiteral("ui/hud_scale"), p);
                    stream_hud_->setScalePercent(p);
                });
        connect(stream_overlay_, &halyard::StreamOverlay::hudOpacityChanged, this,
                [this](int p) {
                    QSettings().setValue(QStringLiteral("ui/hud_opacity"), p);
                    stream_hud_->setOpacityPercent(p);
                });
        connect(stream_overlay_, &halyard::StreamOverlay::hudLayoutReset, this,
                [this] { stream_hud_->resetLayout(); });
        /* Edit mode: the HUD takes the mouse, so the overlay menu steps aside
         * and a toast says how to come back. */
        connect(stream_overlay_, &halyard::StreamOverlay::hudEditToggled, this,
                [this](bool on) {
                    stream_hud_->setEditing(on);
                    if (on) showToast(tr("Editing the HUD — press %1 to finish.")
                                          .arg(overlay_shortcut_->key()
                                                   .toString(QKeySequence::NativeText)));
                });
        connect(stream_overlay_, &halyard::StreamOverlay::hudMasksChanged, this,
                [this](int sec, int ch) {
                    QSettings s2;
                    s2.setValue(QStringLiteral("ui/hud_sections"), sec);
                    s2.setValue(QStringLiteral("ui/hud_charts"), ch);
                    stream_hud_->setMasks(sec, ch);
                    updateOverlayVisibility();
                });
        connect(stream_overlay_, &halyard::StreamOverlay::requestFullscreenToggle,
                this, &MainWindow::toggleFullscreen);
        connect(stream_overlay_, &halyard::StreamOverlay::requestDisconnect, this,
                [this] { if (session_live_ && sess_) sess_->requestStop(); });
        connect(stream_overlay_, &halyard::StreamOverlay::requestFiles, this,
                &MainWindow::openFileManager);
        connect(stream_overlay_, &halyard::StreamOverlay::requestMetrics, this,
                &MainWindow::openMetrics);
        connect(stream_overlay_, &halyard::StreamOverlay::requestSettings, this,
                &MainWindow::openSettings);
        connect(stream_overlay_, &halyard::StreamOverlay::requestScreenshot, this,
                &MainWindow::takeScreenshot);
        connect(stream_overlay_, &halyard::StreamOverlay::requestCopyDiagnostics,
                this, &MainWindow::copyDiagnostics);
        connect(stream_overlay_, &halyard::StreamOverlay::cursorSourceChanged, this,
                [this](int src) {
                    if (video_) video_->setCursorSource(src);
                    if (act_hide_cursor_) {
                        QSignalBlocker b(act_hide_cursor_);
                        act_hide_cursor_->setChecked(src == VideoWidget::CursorNone);
                    }
                });
        connect(stream_overlay_, &halyard::StreamOverlay::toast, this,
                &MainWindow::showToast);

        /* The hotkey is an application shortcut: it fires before the key reaches
         * the video surface, so it is NOT forwarded to the VM. Configurable in
         * Settings > General; rebuilt by applyShortcuts(). */
        /* === KEY1 - the two commands that have no menu entry ===============
         *
         * Overlay and screenshot exist only in the overlay's Tools tab, so
         * they need a `QShortcut` of their own. The other five are menu
         * entries and carry their key on the `QAction`; all seven are set from
         * the same table by `applyShortcuts()`.
         *
         * APPLICATION context on both, because the video widget holds the
         * keyboard while streaming and a widget-context shortcut would never
         * be reached. */
        overlay_shortcut_ = new QShortcut(this);
        overlay_shortcut_->setContext(Qt::ApplicationShortcut);
        connect(overlay_shortcut_, &QShortcut::activated, this,
                &MainWindow::toggleOverlay);

        screenshot_shortcut_ = new QShortcut(this);
        screenshot_shortcut_->setContext(Qt::ApplicationShortcut);
        connect(screenshot_shortcut_, &QShortcut::activated, this,
                &MainWindow::takeScreenshot);

        /* NOT applyShortcuts() here: it sets the key on five QActions that
         * the menu block below has not created yet. Called once the menus
         * exist, just before retranslate(). */

        /* OV3 - the overlays follow the APPLICATION's activity: alt-tab away
         * and they go, come back and they return. */
        connect(qApp, &QGuiApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState st) {
                    app_active_ = (st == Qt::ApplicationActive);
                    updateOverlayVisibility();
                });
        /* OV9 - re-evaluate whenever the focused window changes, so opening the
         * settings takes the HUD away at once and closing them brings it back. */
        connect(qApp, &QGuiApplication::focusWindowChanged, this,
                [this](QWindow *) { updateOverlayVisibility(); });
    }

    /* === QT3 - the menu bar, which is the point of leaving a console UI ====
     *
     * On console everything had to be inside one screen stack. Here the
     * settings and the metrics are WINDOWS: they open beside a running stream,
     * move to another monitor, and keep their place when closed.
     *
     * The shortcuts are the platform conventions (Qt maps Ctrl to Cmd on
     * macOS by itself), and `QKeySequence::Preferences` is what a Mac user
     * expects to find rather than whatever we would have invented. */
    {
        view_menu_ = menuBar()->addMenu(QString());

        QAction *act_settings = act_settings_ = view_menu_->addAction(QString());
        /* `QKeySequence::Preferences` is EMPTY on Windows and Linux - Qt binds
         * it only on macOS. So the portable shortcut is given explicitly and
         * the standard one is added on top, which is what a Mac user reaches
         * for. */
        /* KEY1 - the key itself comes from `applyShortcuts()`, which reads the
         * one table. The STANDARD sequence is added here because it is not a
         * user choice: Qt binds `Preferences` only on macOS, and a Mac user
         * reaches for it whatever else is configured. */
        connect(act_settings, &QAction::triggered, this, &MainWindow::openSettings);

        QAction *act_metrics = act_metrics_ = view_menu_->addAction(QString());
        connect(act_metrics, &QAction::triggered, this, &MainWindow::openMetrics);

        /* IN4 - the "Stream" menu: the pause-menu tools, in the menu bar, shown
         * only while a stream is open. The control bar on the video has the same
         * actions for fullscreen use; this is the discoverable home for them and
         * for the ones a bar has no room for (the pointer choice). */
        stream_menu_ = menuBar()->addMenu(QString());
        act_fs_ = stream_menu_->addAction(QString());
        connect(act_fs_, &QAction::triggered, this, &MainWindow::toggleFullscreen);
        act_hide_cursor_ = stream_menu_->addAction(QString());
        act_hide_cursor_->setCheckable(true);
        connect(act_hide_cursor_, &QAction::toggled, this, [this](bool on) {
            if (video_) video_->setLocalCursorHidden(on);
        });
        stream_menu_->addSeparator();
        act_stream_metrics_ = stream_menu_->addAction(QString());
        connect(act_stream_metrics_, &QAction::triggered, this, &MainWindow::openMetrics);
        stream_menu_->addSeparator();
        act_disconnect_ = stream_menu_->addAction(QString());
        connect(act_disconnect_, &QAction::triggered, this, [this] {
            if (session_live_ && sess_) sess_->requestStop();
        });
        stream_menu_->menuAction()->setVisible(false);   /* until streaming */

        /* FM1 - the file manager. Enabled only while a session is live, because
         * the SFTP channel and its credential exist only then; `session_live_`
         * drives it, set on connect and cleared on every session exit. */
        act_files_ = view_menu_->addAction(QString());
        act_files_->setEnabled(false);
        connect(act_files_, &QAction::triggered, this, &MainWindow::openFileManager);

        /* QT4 - Help, with About.
         *
         * `QAction::AboutRole` is what moves this item into the application
         * menu on macOS, where a Help menu entry called "About" is wrong. On
         * Windows and Linux the role is ignored and it stays here, so one line
         * is correct on all three. */
        /* ACC1 - what this account is entitled to. Next to Sign out, because
         * both are about the account rather than about a machine. */
        act_account_ = view_menu_->addAction(QString());
        connect(act_account_, &QAction::triggered, this, &MainWindow::openAccount);

        /* AUTH9 - sign out. In View and not in Help: it acts on the account,
         * which is what this menu is about, and it is destructive enough to
         * want a separator above it. */
        view_menu_->addSeparator();
        act_sign_out_ = view_menu_->addAction(QString());
        connect(act_sign_out_, &QAction::triggered, this, &MainWindow::signOut);

        help_menu_ = menuBar()->addMenu(QString());
        QAction *act_about = act_about_ = help_menu_->addAction(QString());
        act_about->setMenuRole(QAction::AboutRole);
        connect(act_about, &QAction::triggered, this, &MainWindow::openAbout);
    }

    /* KEY1 - after the menus, because five of the seven keys live on their
     * QActions. Before retranslate() only for tidiness; the order of those two
     * does not matter. */
    applyShortcuts();

    retranslate();
    setPage(PagePairing);

    /* ============================ the three workers ====================== */

    auth_thread_ = new QThread(this);
    auth_ = new AuthWorker;
    auth_->moveToThread(auth_thread_);
    connect(auth_thread_, &QThread::finished, auth_, &QObject::deleteLater);

    boot_thread_ = new QThread(this);
    boot_ = new BootstrapWorker;
    boot_->moveToThread(boot_thread_);
    connect(boot_thread_, &QThread::finished, boot_, &QObject::deleteLater);

    sess_thread_ = new QThread(this);
    sess_ = new SessionWorker;
    sess_->moveToThread(sess_thread_);
    connect(sess_thread_, &QThread::finished, sess_, &QObject::deleteLater);

    /* Every one QUEUED: all three emit from their own thread, and `frameReady`
     * from core's DECODE thread. */
    connect(auth_, &AuthWorker::datacentre, this,
            [this](const QString &name, const QString &, const QString &apiVer) {
                launcher_api_version_ = apiVer;
                /* UI6 - kept, because the cards show it: it is per account, so
                 * one value for the whole list. */
                datacentre_ = name;
                statusBar()->showMessage(tr("data centre: %1").arg(name));
            }, Qt::QueuedConnection);
    connect(auth_, &AuthWorker::progress, this, [this](const QString &w) {
        pair_hint_->setText(w);
    }, Qt::QueuedConnection);
    connect(auth_, &AuthWorker::stage, this, [this](int i) {
        signin_steps_->setStage(i);
        signin_steps_->setVisible(true);
    }, Qt::QueuedConnection);
    connect(auth_, &AuthWorker::pairingNeeded, this,
            &MainWindow::onPairingNeeded, Qt::QueuedConnection);
    connect(auth_, &AuthWorker::pairingProgress, this, &MainWindow::onPairingProgress,
            Qt::QueuedConnection);
    connect(auth_, &AuthWorker::succeeded, this,
            &MainWindow::onSignedIn, Qt::QueuedConnection);
    connect(auth_, &AuthWorker::failed, this, &MainWindow::onSignInFailed,
            Qt::QueuedConnection);

    connect(boot_, &BootstrapWorker::stepRunning, this, [this](int i, const QString &d) {
        steps_->setState(i, StepListWidget::State::Running, d);
    }, Qt::QueuedConnection);
    connect(boot_, &BootstrapWorker::stepDone, this, [this](int i, const QString &d) {
        steps_->setState(i, StepListWidget::State::Done, d);
    }, Qt::QueuedConnection);
    connect(boot_, &BootstrapWorker::stepFailed, this, [this](int i, const QString &d) {
        steps_->setState(i, StepListWidget::State::Failed, d);
        session_live_ = false;
        setMachinesBusy(false);
    }, Qt::QueuedConnection);
    connect(boot_, &BootstrapWorker::ready, this,
            &MainWindow::onBootstrapReady, Qt::QueuedConnection);
    /* === SSE1 — THE VM'S OWN EVENTS ======================================
     *
     * QUEUED, and the queue is the point: the sink runs on the keepalive's
     * curl thread and this lambda touches widgets.
     *
     * Only two events get a toast. `bsod` and `get-out` are things that
     * happen TO the person - the VM crashed, the server is ending the
     * session - and a stream that goes black with no explanation is the
     * worst version of either. The rest goes to the log, because the RE says
     * to expect roughly one event per session and a toast for every one of
     * them would be a notification for "the encoder is ready". */
    connect(boot_, &BootstrapWorker::vmEvent, this,
            [this](const QString &kind, const QString &sub, const QString &detail) {
                qInfo("[SSE1] %s%s%s%s%s", qPrintable(kind),
                      sub.isEmpty() ? "" : ".", qPrintable(sub),
                      detail.isEmpty() ? "" : " = ", qPrintable(detail));

                if (kind == QStringLiteral("bsod")) {
                    showToast(tr("The machine has crashed (blue screen). "
                                 "Shadow is restarting it."));
                } else if (kind == QStringLiteral("get-out")) {
                    showToast(tr("Shadow is ending this session."));
                } else if (kind == QStringLiteral("status-changed")
                           || kind == QStringLiteral("status_changed")) {
                    /* The machine's run state, which `/vms` never gives
                     * (`status: null`). Kept for the card to use on the next
                     * listing rather than repainted now: the stream only
                     * exists while a session does, and by then the list is
                     * not on screen. */
                    if (!detail.isEmpty() && !last_machine_id_.isEmpty())
                        vm_state_[last_machine_id_] = detail;
                }
            }, Qt::QueuedConnection);

    /* CAPS2 - what the account is allowed, kept for the session. */
    connect(boot_, &BootstrapWorker::capabilities, this,
            [this](const BootstrapWorker::Caps &c) { caps_ = c; },
            Qt::QueuedConnection);

    connect(sess_, &SessionWorker::frameReady, video_,
            &VideoWidget::presentFrame, Qt::QueuedConnection);
    connect(sess_, &SessionWorker::progress, this,
            [this](const QString &s, const QString &d) {
                /* CAPS2 - the session countdown rides along with the status,
                 * which is the one line that is already refreshed while a
                 * stream runs. A timer of its own would repaint the bar for
                 * a minute hand. */
                const QString left = sessionTimeLeft();
                video_->setStatus(left.isEmpty()
                                      ? s + QStringLiteral(" - ") + d
                                      : s + QStringLiteral(" - ") + d
                                            + QStringLiteral("   |   ") + left);
            }, Qt::QueuedConnection);
    connect(sess_, &SessionWorker::finished, this,
            [this](bool ok, const QString &why) {
                video_->setStatus((ok ? tr("session ended: ")
                                      : tr("session stopped: ")) + why);
                session_started_.invalidate();   /* CAPS2 - stop counting */
                /* VID1 - say so in the middle of the screen. Without this the
                 * last decoded frame stayed on display and a session that had
                 * ended looked like one that had frozen. */
                video_->showMessage(ok ? tr("Session ended") : tr("Session stopped"),
                                    why);
                session_live_ = false;
                /* FM1 - the SFTP credential died with the session, so the file
                 * manager is now pointing at nothing: close it and grey the
                 * menu entry until the next session grants a fresh channel. */
                act_files_->setEnabled(false);
                stream_menu_->menuAction()->setVisible(false);
                updateOverlayVisibility();   /* OV3 - session over: all gone */
                if (file_manager_) file_manager_->close();
                /* IN3/OV4 - do not strand the machine list in fullscreen, and
                 * put back every piece of chrome the immersive mode hid. */
                if (isFullScreen()) {
                    showNormal();
                    menuBar()->setVisible(true);
                    statusBar()->setVisible(true);
                    if (video_) {
                        video_->setChromeVisible(true);
                        video_->setFullscreenState(false);
                    }
                }
                setPage(PageMachines);
                setMachinesBusy(false);
            }, Qt::QueuedConnection);

    auth_thread_->start();
    boot_thread_->start();
    sess_thread_->start();

    /* IN2 - install the system-shortcut hook on THIS thread, the one with the
     * message loop (a WH_KEYBOARD_LL hook is dispatched to the installing
     * thread's queue). A no-op off Windows and when SHADOW_WIN_KBD_HOOK=0; inert
     * until `halyard_ui_keys_blocked()` says the stream owns the keyboard. */
    kbd_hook_win_install();

    QMetaObject::invokeMethod(auth_, "signIn", Qt::QueuedConnection);
}

bool MainWindow::keysGoToVm() const
{
    return session_live_ && isActiveWindow() &&
           stack_->currentIndex() == PageStreaming &&
           video_ && video_->hasFocus();
}

MainWindow::~MainWindow()
{
    kbd_hook_win_remove();       /* IN2 - before the window and video_ go */
    g_main_window = nullptr;
    /* Stop flags FIRST, then the loops: a worker blocked in HTTP leaves within
     * its own poll granularity, and `quit()` on a thread whose slot has not
     * returned does nothing at all. */
    if (auth_) auth_->requestStop();
    if (boot_) boot_->requestStop();
    if (sess_) sess_->requestStop();
    for (QThread *t : {auth_thread_, boot_thread_, sess_thread_}) {
        if (!t) continue;
        t->quit();
        t->wait(5000);
    }
}

/* QT5 - every STATIC text, set here so a language switch can set it again.
 * Dynamic texts (a status line, the pairing hint) are produced by tr() when
 * they happen and follow the new language from their next update. */
void MainWindow::retranslate()
{
    machines_filter_->setPlaceholderText(tr("Filter..."));
    retry_btn_->setText(tr("Start over"));
    pair_tagline_->setText(tr("Your Shadow cloud PC, on anything you own."));
    act_account_->setText(tr("&Account..."));
    act_sign_out_->setText(tr("Sign &out"));
    view_menu_->setTitle(tr("&View"));
    act_settings_->setText(tr("&Settings"));
    act_metrics_->setText(tr("&Metrics"));
    act_files_->setText(tr("&File transfer..."));
    stream_menu_->setTitle(tr("&Stream"));
    act_fs_->setText(tr("Fullscreen"));
    act_hide_cursor_->setText(tr("Hide the local mouse pointer"));
    act_stream_metrics_->setText(tr("Metrics..."));
    act_disconnect_->setText(tr("Disconnect"));
    help_menu_->setTitle(tr("&Help"));
    act_about_->setText(tr("&About %1").arg(str(SHADOW_APP_NAME)));
}

void MainWindow::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::LanguageChange) retranslate();
    else if (e->type() == QEvent::WindowStateChange ||
             e->type() == QEvent::ActivationChange)
        updateOverlayVisibility();   /* OV3 - minimise / focus */
    QMainWindow::changeEvent(e);
}

void MainWindow::openSettings()
{
    if (!settings_) {
        settings_ = new SettingsWindow(this);
        connect(settings_, &SettingsWindow::overlayHotkeyChanged, this,
                &MainWindow::applyShortcuts);
        connect(settings_, &SettingsWindow::settingChanged, this,
                [this](const QString &env, const QString &v, bool live) {
                    const QString shown = v.isEmpty() ? tr("(unset)") : v;
                    statusBar()->showMessage(
                        live ? tr("%1 = %2 - applied").arg(env, shown)
                             : tr("%1 = %2 - takes effect on the next session")
                                   .arg(env, shown), 6000);
                });
    }
    settings_->show();
    settings_->raise();
    settings_->activateWindow();
}

void MainWindow::openMetrics()
{
    if (!metrics_) metrics_ = new MetricsWindow(this);
    metrics_->show();
    metrics_->raise();
    metrics_->activateWindow();
}

void MainWindow::openFileManagerForced()
{
    const bool was = session_live_;
    session_live_ = true;   /* let openFileManager() through for --files */
    openFileManager();
    session_live_ = was;
}

void MainWindow::openFileManager()
{
    /* Built fresh each time: the SFTP credential lives only as long as the
     * session, so a window kept from a previous one would be dead. It connects
     * from core's live grant in its own constructor. */
    if (!session_live_) return;
    if (!file_manager_) {
        file_manager_ = new FileManagerWindow(this);
        file_manager_->setAttribute(Qt::WA_DeleteOnClose);
        connect(file_manager_, &QObject::destroyed, this,
                [this] { file_manager_ = nullptr; });
    }
    file_manager_->show();
    file_manager_->raise();
    file_manager_->activateWindow();
}

/* === SCR1 2026-10-03 — WHICH SCREEN ======================================
 *
 * `showFullScreen()` fills whatever screen the window currently sits on, and
 * nothing else was ever said about screens anywhere in this client. On one
 * monitor that is correct and invisible. On two it is wrong in the one way
 * that matters: the stream goes fullscreen where the window happened to be,
 * which after opening the settings on the other monitor is not where the
 * person is looking - and once it is fullscreen there is no way to move it,
 * because the window has no frame to drag.
 *
 * The preference is stored as the screen's NAME and not as its index. An
 * index is a position in a list that the system reorders when a monitor is
 * unplugged, so the choice would silently become a different screen; a name
 * that no longer exists simply does not match, and we fall back to the
 * window's own screen, which is the behaviour someone who changed their setup
 * would expect anyway.
 *
 * "Auto" (an empty setting) is the default and means exactly what it used to
 * do, so nobody with one monitor sees a change. */
QScreen *MainWindow::fullscreenTarget() const
{
    const QString want = QSettings().value(QStringLiteral("ui/fullscreen_screen"))
                             .toString();
    if (!want.isEmpty())
        for (QScreen *sc : QGuiApplication::screens())
            if (sc->name() == want) return sc;

    /* The window's own screen; `screen()` can be null very early, before the
     * window has been shown on anything. */
    return screen() ? screen() : QGuiApplication::primaryScreen();
}

void MainWindow::toggleFullscreen()
{
    /* IN3 - whole-window fullscreen. The menu bar goes with it; the stream's
     * own overlay menu stays reachable, which is the way back out (plus F11,
     * which the hook never swallows). Only meaningful while streaming. */
    const bool goFull = !isFullScreen();

    /* OV4 - a REAL fullscreen: every piece of chrome goes, not just the window
     * frame. Menu bar, status bar and the stream's control bar all hide, so the
     * picture is the screen. F11 and the overlay hotkey are the way back, and
     * both still work because the video surface keeps the focus. */
    menuBar()->setVisible(!goFull);
    statusBar()->setVisible(!goFull);
    if (video_) video_->setChromeVisible(!goFull);

    if (goFull) {
        /* SCR1 - move BEFORE going fullscreen. `setScreen` on an already
         * fullscreen window is honoured inconsistently across platforms;
         * placing the normal window inside the target's geometry first lets
         * the usual "fullscreen on the screen I am on" rule do the work, which
         * is the path every platform agrees about. */
        QScreen *target = fullscreenTarget();
        if (target && screen() != target) {
            normal_geometry_ = geometry();   /* so showNormal can come back */
            const QRect g = target->availableGeometry();
            move(g.center() - QPoint(width() / 2, height() / 2));
        }
        showFullScreen();
    } else {
        showNormal();
        /* Put the window back where it was before we moved it to another
         * screen, or it reappears centred on a monitor it never came from. */
        if (normal_geometry_.isValid()) {
            setGeometry(normal_geometry_);
            normal_geometry_ = QRect();
        }
    }
    if (video_) {
        video_->setFullscreenState(goFull);
        video_->setFocus(Qt::OtherFocusReason);
    }
    if (goFull) {
        /* Deferred: the new geometry is not final until the event loop has
         * processed the state change, and the toast positions against it. */
        const QString k = overlay_shortcut_->key().toString(QKeySequence::NativeText);
        QTimer::singleShot(0, this, [this, k] {
            showToast(tr("Fullscreen — F11 to leave, %1 for the menu.").arg(k));
        });
    }
    /* The overlay windows are positioned in screen coordinates, so they must
     * follow the window across the fullscreen change (deferred: the new geometry
     * is not final until the event loop has processed the state change). */
    QTimer::singleShot(0, this, [this] { repositionOverlays(); });
}

/* === OV1 - the overlay plumbing =========================================== */

QRect MainWindow::videoGlobalRect() const
{
    if (!video_) return QRect();
    return QRect(video_->mapToGlobal(QPoint(0, 0)), video_->size());
}

void MainWindow::repositionOverlays()
{
    const QRect r = videoGlobalRect();
    if (r.isNull()) return;
    if (stream_hud_ && stream_hud_->isVisible())     stream_hud_->placeOver(r);
    if (stream_overlay_ && stream_overlay_->isVisible()) stream_overlay_->placeOver(r);
}

/* === OV3 - WHEN THE OVERLAY WINDOWS MAY BE ON SCREEN ======================
 *
 * They are `Qt::Tool` + always-on-top, which is what lets them sit over a
 * fullscreen native video surface - and is also why they must be governed: left
 * to themselves they float over every other application and outlive the stream,
 * which is exactly what was reported.
 *
 * Four conditions, all required: a session is streaming, the streaming page is
 * the one shown, the window is not minimised, and the APPLICATION is active.
 * The last one is deliberately the application's state and not this window's:
 * while the overlay menu has focus, the main window is NOT the active window,
 * and testing that would hide the overlay the moment it opened. */
/* OV10 - is one of our OTHER windows sitting over the picture?
 *
 * Dropping `WindowStaysOnTopHint` fixes the ordering, but ordering alone is not
 * enough: the HUD is click-through, so a file manager underneath it is still
 * usable yet partly unreadable, and a translucent HUD over a file list reads as
 * a rendering fault. Overlap and not focus, because the window the person is
 * reading is not always the focused one - and overlap is also what lets the
 * file manager live on a second monitor with the HUD still up. */
/* === UI5 — THE ARROWS ON THE MACHINE LIST =================================
 *
 * Up/Down move between cards, Home/End jump to the ends, and the card itself
 * handles Enter (see machine_card.hpp for why the split). Implemented on the
 * CONTAINER because moving between siblings needs to know the set, and a card
 * that reached for its neighbours would have to know how they are laid out.
 *
 * `ensureWidgetVisible` is not optional: focus can move to a card below the
 * fold, and a focus ring you cannot see is worse than none - the list looks
 * like it has stopped responding. */
bool MainWindow::eventFilter(QObject *o, QEvent *ev)
{
    if (o == machines_scroll_ && ev->type() == QEvent::KeyPress
        && !machine_cards_.isEmpty()) {
        auto *ke = static_cast<QKeyEvent *>(ev);
        int cur = -1;
        for (int i = 0; i < machine_cards_.size(); i++)
            if (machine_cards_.at(i)->hasFocus()) { cur = i; break; }

        int want = cur;
        switch (ke->key()) {
        case Qt::Key_Down: want = (cur < 0) ? 0 : qMin(cur + 1, machine_cards_.size() - 1); break;
        case Qt::Key_Up:   want = (cur < 0) ? 0 : qMax(cur - 1, 0); break;
        case Qt::Key_Home: want = 0; break;
        case Qt::Key_End:  want = machine_cards_.size() - 1; break;
        default: return QMainWindow::eventFilter(o, ev);
        }
        if (want != cur) {
            machine_cards_.at(want)->setFocus(Qt::OtherFocusReason);
            machines_scroll_->ensureWidgetVisible(machine_cards_.at(want), 0, 12);
        }
        return true;
    }
    return QMainWindow::eventFilter(o, ev);
}

bool MainWindow::ownWindowOverVideo() const
{
    const QRect v = videoGlobalRect();
    if (v.isNull()) return false;

    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (!w || w == this || !w->isWindow() || !w->isVisible() || w->isMinimized())
            continue;
        /* Our own overlays are the thing being judged, not a judge of it; menus
         * and tooltips come and go in a frame and must not flicker the HUD. */
        if (w == stream_hud_ || w == stream_overlay_ || w == toast_) continue;
        const Qt::WindowType t = w->windowType();
        if (t == Qt::Popup || t == Qt::ToolTip || t == Qt::SplashScreen) continue;
        if (w->frameGeometry().intersects(v)) return true;
    }
    return false;
}

bool MainWindow::overlaysAllowed() const
{
    return session_live_ && app_active_ && !isMinimized()
        && stack_ && stack_->currentIndex() == PageStreaming
        && !ownWindowOverVideo();
}

/* === CAPS2 2026-10-03 — "5h55m restantes" ================================
 *
 * The official client shows a session countdown; nothing in the protocol
 * pushes one. `usage.max_session_length` from `/vms/{id}/capabilities` is the
 * ceiling (21600 s on the account measured), and the figure is that minus the
 * time since the session started - counted HERE, locally, because there is no
 * runtime endpoint and no SSE event carrying it.
 *
 * Empty when the server did not give a ceiling: a countdown invented from a
 * default would be the exact mistake the Borealis client refused to make
 * ("a figure one believes official and that is not is worse than no figure"),
 * and the difference is that this one IS official. */
QString MainWindow::sessionTimeLeft() const
{
    if (caps_.maxSessionLength <= 0 || !session_started_.isValid())
        return QString();
    const qint64 left = caps_.maxSessionLength - session_started_.elapsed() / 1000;
    if (left <= 0) return tr("session time is up");
    const qint64 h = left / 3600, m = (left % 3600) / 60;
    return h > 0 ? tr("%1h%2m left").arg(h).arg(m, 2, 10, QLatin1Char('0'))
                 : tr("%1 min left").arg(m);
}

void MainWindow::updateOverlayVisibility()
{
    const bool ok = overlaysAllowed();

    /* OV10 - a window can be dragged over the picture without any focus change,
     * so poll while a session is on the stream page. One rectangle test per
     * top-level, four times a second: cheaper than an application-wide event
     * filter and it catches moves, resizes and another monitor going away. */
    const bool watch = session_live_ && stack_ && stack_->currentIndex() == PageStreaming;
    if (!overlay_watch_) {
        overlay_watch_ = new QTimer(this);
        overlay_watch_->setInterval(250);
        connect(overlay_watch_, &QTimer::timeout, this,
                &MainWindow::updateOverlayVisibility);
    }
    if (watch && !overlay_watch_->isActive())      overlay_watch_->start();
    else if (!watch && overlay_watch_->isActive()) overlay_watch_->stop();
    if (stream_hud_)
        stream_hud_->setVisible(ok && stream_hud_->anything());
    if (!ok) {
        if (stream_overlay_) stream_overlay_->hide();
        if (toast_)          toast_->hide();
    }
    if (ok) repositionOverlays();
}

void MainWindow::toggleOverlay()
{
    if (!stream_overlay_ || !overlaysAllowed()) return;
    /* OV7 - while arranging the HUD the hotkey means "done", not "menu". */
    if (stream_hud_ && stream_hud_->editing()) {
        stream_hud_->setEditing(false);
        stream_overlay_->setHudEditing(false);
        return;
    }
    if (stream_overlay_->isVisible()) {
        stream_overlay_->hide();
    } else {
        stream_overlay_->placeOver(videoGlobalRect());
        stream_overlay_->show();
        stream_overlay_->raise();
        stream_overlay_->activateWindow();
    }
}

/* === KEY1 — EVERY KEY FROM THE ONE TABLE ==================================
 *
 * Called at construction and again whenever the settings window changes a
 * binding, so a new key takes effect without a restart. Reading all seven in
 * one place is also what makes a collision detectable at all: when they were
 * set at seven call sites, nothing ever had the set in front of it.
 *
 * `keySequenceFor` falls back to the table's default for a missing or empty
 * stored value, so a configuration file that predates a command still gets a
 * working key rather than none. */
QKeySequence MainWindow::keySequenceFor(int action) const
{
    const auto &a = halyard::keys::action(action);
    const QString stored = QSettings()
        .value(QString::fromUtf8(a.settingsKey)).toString();
    /* An explicitly cleared key is stored as "-": a QSettings value that is
     * merely absent means "never set" and must take the default, while a user
     * who deliberately unbound a command must get nothing. Distinguishing the
     * two is why the empty string is not used for either. */
    if (stored == QStringLiteral("-")) return QKeySequence();
    if (stored.isEmpty()) return QKeySequence(QString::fromUtf8(a.def));
    return QKeySequence(stored);
}

void MainWindow::applyShortcuts()
{
    using namespace halyard::keys;

    act_fs_->setShortcut(keySequenceFor(ActFullscreen));
    act_metrics_->setShortcut(keySequenceFor(ActMetrics));
    act_files_->setShortcut(keySequenceFor(ActFiles));
    /* Preferences stays alongside the configured one - see the note at the
     * menu. A list with an empty sequence in it is harmless; Qt ignores it. */
    act_settings_->setShortcuts({ keySequenceFor(ActSettings),
                                  QKeySequence(QKeySequence::Preferences) });
    act_disconnect_->setShortcut(keySequenceFor(ActDisconnect));

    /* The five menu actions get APPLICATION context too. The default is
     * `WindowShortcut`, which is not reached while the video widget has the
     * keyboard - and the Stream menu is hidden off a session anyway, so its
     * entries would otherwise be unreachable by key exactly when they matter.
     * Set here rather than at construction so one loop covers them all. */
    for (QAction *a : { act_fs_, act_metrics_, act_files_, act_settings_,
                        act_disconnect_ })
        a->setShortcutContext(Qt::ApplicationShortcut);

    overlay_shortcut_->setKey(keySequenceFor(ActOverlay));
    screenshot_shortcut_->setKey(keySequenceFor(ActScreenshot));

    /* UI5 - the stream's own F11 handler used to be a second declaration of
     * the same key. It now asks us, so there is one source. */
    if (video_) video_->setFullscreenKey(keySequenceFor(ActFullscreen));
}

/* OV2 - a transient note over the stream. A frameless tool window rather than
 * the status bar, because in fullscreen there is no status bar; it is
 * click-through so it can never swallow a shot in a game. */
void MainWindow::showToast(const QString &text)
{
    /* === OV3/D4 — WHERE A TOAST IS ALLOWED ================================
     *
     * Originally gated on `overlaysAllowed()`, which also means "on the
     * streaming page" - so every confirmation outside a stream went to the
     * status bar, which is where messages go to be missed.
     *
     * The gate that actually mattered was never the page: it was that an
     * unparented always-on-top tool window floats over every other
     * application for ever (reported). That is fixed by the parent and by
     * OV10's dropping of the topmost hint. What remains is the honest
     * condition - this application is in front, and not minimised. */
    if (!app_active_ || isMinimized()) return;
    if (!toast_) {
        /* Parented to the main window on purpose: a null parent makes it an
         * independent top-level that Windows keeps alive and on top whatever
         * has focus. */
        /* OV10 - no `WindowStaysOnTopHint`: it means topmost over the whole
         * desktop, our own other windows included. `Qt::Tool` + this parent is
         * what puts it above the video's native child surface. */
        toast_ = new QLabel(this, Qt::FramelessWindowHint | Qt::Tool |
                                  Qt::WindowTransparentForInput);
        toast_->setAttribute(Qt::WA_TranslucentBackground);
        toast_->setAttribute(Qt::WA_ShowWithoutActivating);
        toast_->setStyleSheet(QStringLiteral(
            "color:#f2f2f2; background:rgba(20,20,24,225);"
            "padding:9px 14px; border-radius:9px;"));
        toast_timer_ = new QTimer(this);
        toast_timer_->setSingleShot(true);
        connect(toast_timer_, &QTimer::timeout, this,
                [this] { if (toast_) toast_->hide(); });
    }
    toast_->setText(text);
    toast_->adjustSize();
    /* D4 - over the picture while streaming, else near the bottom of the
     * window. `videoGlobalRect()` is null off the streaming page, and a toast
     * placed from a null rectangle landed at the top left of the desktop. */
    QRect r = videoGlobalRect();
    if (r.isNull()) r = QRect(mapToGlobal(QPoint(0, 0)), size());
    toast_->move(r.center().x() - toast_->width() / 2,
                 r.bottom() - toast_->height() - 48);
    toast_->show();
    toast_->raise();
    toast_timer_->start(2600);
}

/* OV2 - a PNG of exactly what the stream is showing, from the sink's current
 * frame: the decoded picture, not a grab of the window, so no overlay or cursor
 * lands in it. */
void MainWindow::takeScreenshot()
{
    if (!video_) return;
    const QImage img = video_->currentFrameImage();
    if (img.isNull()) { showToast(tr("No picture to capture yet.")); return; }

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString path = QDir(dir).filePath(
        QStringLiteral("halyard-%1.png")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
    if (img.save(path)) showToast(tr("Screenshot saved: %1").arg(path));
    else                showToast(tr("Could not save the screenshot."));
}

void MainWindow::copyDiagnostics()
{
    shadow_session_caps c;
    const bool have = ctrl_session_caps(&c);
    session_stats_t s;
    session_stats_get(&s);
    const QString txt = QStringLiteral(
        "%1 %2 (%3)\n"
        "session: %4 s, %5 channels granted\n"
        "video: %6x%7, codec %8, %9 decoded, %10 decode errors\n"
        "net: %11 pkts, loss %12/%13, rtt %14 ms\n"
        "audio: %15 decoded, %16 lost")
        .arg(str(SHADOW_APP_NAME), str(SHADOW_VERSION), str(SHADOW_BUILD_HASH))
        .arg(s.session_seconds).arg(have ? c.n_granted : 0)
        .arg(s.h264_width).arg(s.h264_height)
        .arg(QString::fromUtf8(ctrl_session_glue_codec()))
        .arg(s.h264_frames_decoded).arg(s.h264_decode_errors)
        .arg(s.rtp_video_packets).arg(s.chunks_missing).arg(s.chunks_expected)
        .arg(s.ctrl_rtt_us / 1000)
        .arg(s.opus_decoded).arg(s.opus_lost);
    QGuiApplication::clipboard()->setText(txt);
    showToast(tr("Diagnostics copied to the clipboard."));
}

void MainWindow::moveEvent(QMoveEvent *e)
{
    QMainWindow::moveEvent(e);
    repositionOverlays();
}

void MainWindow::resizeEvent(QResizeEvent *e)
{
    QMainWindow::resizeEvent(e);
    repositionOverlays();
}

void MainWindow::openAbout()
{
    /* Built each time and not kept: it holds no state, and a dialog that is
     * constructed once keeps the palette it was born with - which is wrong the
     * moment the desktop switches to dark mode while the client is running. */
    halyard::AboutDialog dlg(this);
    dlg.exec();
}

/* D1 - what the header says, per page. */
void MainWindow::updateHeader(int index)
{
    if (!header_) return;
    /* The pairing page has the hero (the mark twice would be silly) and the
     * streaming page owns every pixel. */
    header_->setVisible(index == PageMachines || index == PageConnecting);
    header_title_->setText(index == PageConnecting ? tr("Connecting")
                                                   : tr("Your machines"));
    /* The account is not something we hold - no profile call is made, and
     * making one just to fill a header would be collecting a name we have no
     * other use for. The data centre is what we do know, and it is the part
     * that matters for a stream. */
    header_account_->setText(datacentre_);
}

void MainWindow::setPage(int index)
{
    const int from = stack_->currentIndex();
    stack_->setCurrentIndex(index);
    updateHeader(index);
    updateOverlayVisibility();   /* OV3 - off the stream page, no overlays */

    /* === UI1 — A 140 ms FADE IN ON THE PAGE THAT ARRIVES ==================
     *
     * The four pages are one window swapping its contents, and an instant swap
     * reads as a flash with no direction: people lost track of whether the code
     * screen had been replaced or the window had been redrawn.
     *
     * Only the INCOMING page animates, and only its opacity. Deliberately not
     * a slide or a cross-fade: a cross-fade needs both pages rendered at once,
     * which on the streaming page means compositing live video through a
     * `QGraphicsOpacityEffect` - that forces the video widget off its native
     * surface and cost ~11 ms a frame when tried (2026-10-03).
     *
     * The streaming page is therefore excluded outright, and the effect is
     * REMOVED when the animation ends rather than left at opacity 1: an effect
     * still installed keeps the widget on the slow path for every later
     * repaint. `SHADOW_QT_ANIM=0` turns the transitions off. */
    /* D3 - one gate for every animation in the client: our toggle, then the
     * system's "show animations" preference. */
    if (!halyard::theme::animationsEnabled() || from == index
        || index == PageStreaming)
        return;

    QWidget *page = stack_->widget(index);
    if (!page) return;

    auto *fx = new QGraphicsOpacityEffect(page);
    fx->setOpacity(0.0);
    page->setGraphicsEffect(fx);

    auto *a = new QPropertyAnimation(fx, "opacity", page);
    a->setDuration(140);
    a->setStartValue(0.0);
    a->setEndValue(1.0);
    a->setEasingCurve(QEasingCurve::OutCubic);
    connect(a, &QPropertyAnimation::finished, page, [page] {
        page->setGraphicsEffect(nullptr);   /* deletes the effect */
    });
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

void MainWindow::onPairingNeeded(const QString &userCode, const QString &uri,
                                 const QString &uriComplete, int expiresIn)
{
    pair_user_code_ = userCode;
    pair_uri_ = uri.isEmpty() ? QStringLiteral("https://shadow.tech/device") : uri;
    pair_uri_complete_ = uriComplete;

    /* CLIP5, the same reasoning as the Borealis client: the code is eight
     * characters to retype into a browser ON THIS MACHINE, so it is copied and
     * the screen SAYS so. Done here and not in the worker: this is the GUI
     * thread, which is where clipboard calls belong. */
    bool copied = false;
    if (QClipboard *cb = QGuiApplication::clipboard()) {
        cb->setText(userCode);
        copied = true;
    }

    /* UI4 - the phases are done with: a code is the thing they were leading
     * to, and leaving them up would push it down for nothing. */
    signin_steps_->setVisible(false);
    pair_heading_->setText(tr("Finish signing in"));

    /* UI4 - the QR encodes the COMPLETE URI when the server gave one, because
     * that form carries the code: scanning it then needs no typing at all. We
     * fall back to the plain URI, where the phone still saves the URL and only
     * the code has to be entered. */
    const QString qrText = !pair_uri_complete_.isEmpty() ? pair_uri_complete_
                                                          : pair_uri_;
    const bool haveQr = qr_->setText(qrText);
    qr_->setVisible(haveQr);
    qr_caption_->setVisible(haveQr);
    qr_caption_->setText(pair_uri_complete_.isEmpty()
                             ? tr("Scan with a phone, then enter the code")
                             : tr("Scan with a phone - the code is included"));

    /* The URL as a real, clickable link (rich text). The validity is no longer
     * written here: it is the bar and the countdown under the code, and saying
     * it a third time in a sentence that is read once was the version people
     * missed. */
    /* UI8 - the URL as text and not as a link to read: a 60-character
     * auth URL wrapped over two lines was the widest thing in the card and
     * the one nobody was going to retype. The button opens it; the line now
     * only says what to do. */
    pair_hint_->setText(
        haveQr ? tr("Or open the sign-in page and enter this code.%1")
                     .arg(copied ? tr(" It is already on your clipboard.")
                                 : QString())
               : tr("Open the sign-in page and enter this code.%1")
                     .arg(copied ? tr(" It is already on your clipboard.")
                                 : QString()));

    /* UI8 - short labels. "Open the sign-in page (code prefilled)" clipped
     * its own text at any card width that still fitted on a laptop, and the
     * parenthesis was repeating what the line under the QR already says. The
     * copy feedback moves to the hint for the same reason: a button whose
     * caption changes to past tense is a button that has stopped saying what
     * pressing it does. */
    copy_code_btn_->setText(tr("Copy the code"));
    open_page_btn_->setText(tr("Open the page"));
    pair_cells_->setCode(userCode);
    pair_cells_->setVisible(true);
    pair_validity_->setVisible(true);
    copy_code_btn_->setVisible(true);
    open_page_btn_->setVisible(true);
    retry_btn_->setVisible(false);
    pair_countdown_->setVisible(true);
    pair_expires_ = expiresIn > 0 ? expiresIn : 600;
    onPairingProgress(expiresIn);
}

/* UI2 - the remaining validity, on the card. Written as m:ss and not as a bare
 * second count: "417 s" is a number you have to divide before it means
 * anything, and the only question being asked is "do I have time". */
void MainWindow::onPairingProgress(int secondsLeft)
{
    if (secondsLeft < 0) secondsLeft = 0;
    const QString mmss = QStringLiteral("%1:%2")
                             .arg(secondsLeft / 60)
                             .arg(secondsLeft % 60, 2, 10, QLatin1Char('0'));
    pair_countdown_->setText(tr("this code expires in %1").arg(mmss));
    pair_validity_->setRemaining(secondsLeft, pair_expires_);
    /* Under a minute it stops being background information. */
    pair_countdown_->setStyleSheet(
        secondsLeft <= 60 ? halyard::theme::css(halyard::theme::warn(this)) : QString());
    pair_countdown_->setVisible(true);
    statusBar()->showMessage(tr("waiting for the browser - %1 left").arg(mmss));
}

void MainWindow::onSignInFailed(const QString &why)
{
    /* UI2 - a failure here is nearly always recoverable (the code ran out, the
     * browser was never opened, the network blinked), so the screen offers the
     * retry instead of ending at a sentence. The code and its two buttons go:
     * that code is dead, and leaving it on screen invites retyping it. */
    pair_hint_->setText(tr("Sign-in did not complete: %1").arg(why));
    pair_heading_->setText(tr("Sign-in failed"));
    signin_steps_->setFailed();
    signin_steps_->setVisible(true);
    pair_cells_->setVisible(false);
    pair_validity_->setVisible(false);
    qr_->setVisible(false);
    qr_caption_->setVisible(false);
    pair_countdown_->setVisible(false);
    copy_code_btn_->setVisible(false);
    open_page_btn_->setVisible(false);
    retry_btn_->setVisible(true);
    retry_btn_->setEnabled(true);
    retry_btn_->setFocus();
    statusBar()->showMessage(tr("sign-in failed"));
}

/* === AUTH9 — SIGN OUT =====================================================
 *
 * Confirmed, because it is not undoable: the stored refresh token is the only
 * thing standing between this machine and typing a device code into a browser
 * again, and nothing else in the application can put it back.
 *
 * A live session is stopped first. Signing out while streaming would leave a
 * session running against credentials the application has just thrown away -
 * it would keep working until the next token refresh and then fail somewhere
 * unrelated, which is the worst of both. */
/* ACC1 - the account window, built once and kept: it holds a scroll position
 * and a refresh in flight, and rebuilding it on every open would throw both
 * away. Fed the session each time, because the machine it asks about is
 * whichever one is current. */
void MainWindow::openAccount()
{
    if (!account_) account_ = new AccountWindow(this);
    account_->setSession(launcher_url_, bearer_,
                         last_machine_id_.isEmpty()
                             ? machine_ids_.value(0)
                             : last_machine_id_,
                         datacentre_, launcher_api_version_);
    account_->show();
    account_->raise();
    account_->activateWindow();
}

void MainWindow::signOut()
{
    const QMessageBox::StandardButton a = QMessageBox::question(
        this, tr("Sign out"),
        tr("Forget the saved session on this computer?\n\n"
           "You will have to sign in through a browser again at the next "
           "start. Nothing on the machines themselves is changed."),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (a != QMessageBox::Yes) return;

    if (session_live_ && sess_) sess_->requestStop();

    /* === AUTH10 2026-10-03 — TELL THE SERVER, THEN FORGET =================
     *
     * Deleting the token locally is what the person at this machine can see;
     * the token itself stayed valid at Shadow for months. Anybody holding a
     * copy - a backup, an old card, a log that should not have had it -
     * could keep refreshing it, and "sign out" had said nothing to anyone.
     *
     * Revoke FIRST: once the file is gone there is nothing left to revoke
     * with. On a worker because it is an HTTP round trip and this is the GUI
     * thread, and NOT waited on: the local forget must happen whether or not
     * the network is up, since a sign-out that left the credential on disk
     * because Wi-Fi was down is the worse of the two failures.
     *
     * The token is read, used and dropped inside the worker. It is never
     * logged, and `oauth_revoke_token` does not print it either. */
    {
        char *rt = nullptr;
        if (oauth_load_refresh(&rt) && rt && *rt) {
            const QByteArray tok(rt);
            (void)QtConcurrent::run([tok] {
                halyard::ScopedDiscovery d;
                long http = 0;
                if (oauth_discover(d.out(), &http))
                    (void)oauth_revoke_token(d.out(), SHADOW_OAUTH_CLIENT_ID,
                                             tok.constData());
            });
        }
        /* Zeroed rather than merely freed: it is a credential, and this is a
         * heap buffer that would otherwise sit in freed memory. */
        if (rt) { memset(rt, 0, strlen(rt)); free(rt); }
    }

    const bool ok = oauth_forget_refresh();
    bearer_.clear();
    launcher_url_.clear();
    machine_ids_.clear();
    machine_names_.clear();
    last_machine_id_.clear();
    clearMachineCards();
    act_files_->setEnabled(false);
    stream_menu_->menuAction()->setVisible(false);

    statusBar()->showMessage(ok ? tr("Signed out.")
                                : tr("Signed out, but the stored session could "
                                     "not be removed from disk."));
    restartSignIn();
}

void MainWindow::restartSignIn()
{
    /* The same call the constructor makes. `AuthWorker::signIn` clears its own
     * stop flag and holds every handle in a scope, so running it again is a
     * fresh attempt and not a resumption of the dead one. */
    retry_btn_->setEnabled(false);
    retry_btn_->setVisible(false);
    pair_cells_->setVisible(false);
    pair_validity_->setVisible(false);
    qr_->setVisible(false);
    qr_caption_->setVisible(false);
    pair_countdown_->setVisible(false);
    copy_code_btn_->setVisible(false);
    open_page_btn_->setVisible(false);
    pair_heading_->setText(tr("Signing in"));
    pair_hint_->setText(tr("Contacting Shadow..."));
    signin_steps_->reset();
    signin_steps_->setVisible(true);
    setPage(PagePairing);
    statusBar()->clearMessage();
    QMetaObject::invokeMethod(auth_, "signIn", Qt::QueuedConnection);
}

void MainWindow::onSignedIn(const QString &bearer, const QString &launcherUrl)
{
    bearer_ = bearer;            /* a credential: kept, never logged */
    launcher_url_ = launcherUrl;
    pair_hint_->setText(tr("signed in"));
    signin_steps_->setComplete();
    pair_cells_->setVisible(false);
    pair_validity_->setVisible(false);
    qr_->setVisible(false);
    qr_caption_->setVisible(false);
    pair_countdown_->setVisible(false);
    listMachines();
}

void MainWindow::listMachines()
{
    statusBar()->showMessage(tr("fetching your machines..."));
    showMachineSkeletons();
    setPage(PageMachines);   /* UI6 - so the skeletons are what is on screen */
    const QByteArray base = launcher_url_.toUtf8();
    const QByteArray tok  = bearer_.toUtf8();

    /* `launcher_list_vms` is blocking HTTP, so it does NOT run here. One short
     * call does not justify a fourth worker, so it goes to the thread pool and
     * the result comes back through a queued invocation. */
    (void)QtConcurrent::run([this, base, tok] {
        halyard::ScopedVmPage page;
        long http = 0;
        /* VMK1 - one list of rows rather than three parallel QStringLists.
         * Three was already one too many when the state joined the name; six
         * fields in six lists is how an off-by-one puts a machine's
         * maintenance flag on its neighbour. */
        QVariantList rows;
        bool fetched = false;
        if (launcher_list_vms(base.constData(), tok.constData(), 0, 50,
                              page.out(), &http)) {
            fetched = true;
            for (int i = 0; i < page->count; i++) {
                const VmInfo &v = page->items[i];
                QString label = str(v.alias);
                if (label.isEmpty()) label = str(v.name);
                if (label.isEmpty()) label = str(v.id);

                QVariantMap m;
                m[QStringLiteral("id")]    = str(v.id);
                m[QStringLiteral("name")]  = label;
                m[QStringLiteral("state")] = str(v.state);
                m[QStringLiteral("dc")]    = str(v.datacenter);
                m[QStringLiteral("hw")]    = str(v.hwconfig);
                m[QStringLiteral("tags")]  = str(v.tags);
                m[QStringLiteral("maint")] = v.maintenance;
                rows << m;
            }
        }
        if (!fetched) {
            /* D5 - the HTTP code goes with it: "could not be reached" and
             * "your session was refused" need different actions, and the
             * number is what tells them apart. */
            QMetaObject::invokeMethod(this, "onMachinesFailed", Qt::QueuedConnection,
                                      Q_ARG(int, int(http)));
            return;
        }
        QMetaObject::invokeMethod(this, "onMachinesFetched", Qt::QueuedConnection,
                                  Q_ARG(QVariantList, rows));
    });
}

void MainWindow::onMachinesFetched(const QVariantList &rows)
{
    clearMachineCards();

    QStringList ids, names;
    for (const QVariant &r : rows) {
        const QVariantMap m = r.toMap();
        ids   << m.value(QStringLiteral("id")).toString();
        names << m.value(QStringLiteral("name")).toString();
    }

    /* === UI6 — MOST RECENTLY USED FIRST =================================
     *
     * The server's order is whatever its database returns, which for someone
     * with four machines means the one they always use can be third. The
     * local record of last connections is the only thing that knows better,
     * and it is a stable sort so machines never used keep the server's order
     * among themselves rather than shuffling between launches. */
    QVector<int> order;
    for (int i = 0; i < ids.size(); i++) order.append(i);
    QSettings st;
    st.beginGroup(QStringLiteral("machines"));
    QVector<QDateTime> used(ids.size());
    for (int i = 0; i < ids.size(); i++)
        used[i] = st.value(ids.at(i) + QStringLiteral("/last_used")).toDateTime();
    st.endGroup();
    std::stable_sort(order.begin(), order.end(), [&used](int a, int b) {
        if (used[a].isValid() != used[b].isValid()) return used[a].isValid();
        if (!used[a].isValid()) return false;          /* both unknown: keep order */
        return used[a] > used[b];
    });

    machine_ids_.clear();
    machine_names_.clear();
    for (int k = 0; k < order.size(); k++) {
        const int i = order.at(k);
        const QVariantMap m = rows.at(i).toMap();
        machine_ids_   << ids.at(i);
        machine_names_ << names.value(i);

        MachineCard::Info info;
        info.id    = ids.at(i);
        info.name  = names.value(i);
        info.state = m.value(QStringLiteral("state")).toString();
        /* SSE1 - `/vms` sends `status: null`, so the only run state we ever
         * see is the one the event stream reported during a session. Used
         * only as a fallback: a value the server just gave beats one we
         * remembered. */
        if (info.state.isEmpty())
            info.state = vm_state_.value(ids.at(i));
        /* VMK1 - the machine's OWN data centre when the server gave one; the
         * account-wide name from TINAG only as a fallback, which is what the
         * first version used unconditionally. */
        info.datacentre = m.value(QStringLiteral("dc")).toString();
        if (info.datacentre.isEmpty()) info.datacentre = datacentre_;
        info.hwconfig    = m.value(QStringLiteral("hw")).toString();
        info.tags        = m.value(QStringLiteral("tags")).toString();
        info.maintenance = m.value(QStringLiteral("maint")).toBool();

        auto *card = new MachineCard(info, machines_host_);
        card->setLastUsed(used[i]);
        connect(card, &MachineCard::connectRequested, this, &MainWindow::connectTo);
        /* Before the trailing stretch, so the cards stay at the top. */
        machines_lay_->insertWidget(machines_lay_->count() - 1, card);
        machine_cards_.append(card);
        staggerIn(card, k);
    }
    machines_filter_->setVisible(machine_cards_.size() > 6);
    /* UI5 - the keyboard lands somewhere useful. The old list selected row 0
     * on arrival; focusing the first card is the same courtesy, and without it
     * the arrows have nothing to move FROM. */
    if (!machine_cards_.isEmpty()) machine_cards_.first()->setFocus(Qt::OtherFocusReason);

    machines_error_->setVisible(false);
    machines_empty_->setVisible(ids.isEmpty());
    machines_empty_->setText(tr("No machine on this account."));
    machines_subtitle_->setText(tr("%n machine(s)", "", ids.size()));
    /* UI8 - not in the status bar as well: the count was on screen twice,
     * fifteen pixels apart. */
    statusBar()->clearMessage();
    setPage(PageMachines);
}

/* UI6 - take the cards down, skeletons included. One place, because three
 * callers had three slightly different versions of it and one of them leaked
 * the placeholder widgets. */
void MainWindow::clearMachineCards()
{
    for (MachineCard *c : machine_cards_) { machines_lay_->removeWidget(c); delete c; }
    machine_cards_.clear();
    for (QWidget *w : machine_skeletons_) { machines_lay_->removeWidget(w); delete w; }
    machine_skeletons_.clear();
}

/* === UI6 — SKELETONS WHILE FETCHING ======================================
 *
 * The page went up empty and then jumped when the list arrived, which reads as
 * a glitch rather than as loading. Three grey blocks the size of a card say
 * "something is coming and it will look like this".
 *
 * Three and not one per machine: we do not know how many there are yet, and
 * guessing would make the jump worse when the guess is wrong. */
void MainWindow::showMachineSkeletons()
{
    clearMachineCards();
    machines_error_->setVisible(false);
    machines_empty_->setVisible(false);
    machines_filter_->setVisible(false);
    for (int i = 0; i < 3; i++) {
        auto *sk = new QFrame(machines_host_);
        sk->setProperty("card", true);
        sk->setFixedHeight(72);
        /* Fading, so it is visibly WAITING rather than broken. The animation
         * is owned by the widget, so deleting the skeleton stops it. */
        auto *fx = new QGraphicsOpacityEffect(sk);
        fx->setOpacity(0.45);
        sk->setGraphicsEffect(fx);
        auto *a = new QPropertyAnimation(fx, "opacity", sk);
        a->setDuration(900);
        a->setStartValue(0.25);
        a->setKeyValueAt(0.5, 0.6);
        a->setEndValue(0.25);
        a->setLoopCount(-1);
        a->start();
        machines_lay_->insertWidget(machines_lay_->count() - 1, sk);
        machine_skeletons_.append(sk);
    }
}

/* UI6 - fade and rise, 40 ms apart.
 *
 * Capped at six: beyond that the last card of a twenty-machine list would
 * arrive most of a second late, and a stagger that outlasts the reader's
 * attention is just a slow list. The effect is REMOVED on completion - one
 * left installed keeps the widget on the slow repaint path for ever. */
void MainWindow::staggerIn(QWidget *w, int index)
{
    if (!halyard::theme::animationsEnabled()) return;

    auto *fx = new QGraphicsOpacityEffect(w);
    fx->setOpacity(0.0);
    w->setGraphicsEffect(fx);

    auto *a = new QPropertyAnimation(fx, "opacity", w);
    a->setDuration(180);
    a->setStartValue(0.0);
    a->setEndValue(1.0);
    a->setEasingCurve(QEasingCurve::OutCubic);
    connect(a, &QPropertyAnimation::finished, w, [w] { w->setGraphicsEffect(nullptr); });

    const int delay = qMin(index, 6) * 40;
    QTimer::singleShot(delay, w, [a] { a->start(QAbstractAnimation::DeleteWhenStopped); });
}

/* UI6 - hide what does not match. Case-insensitive on the NAME only: the id is
 * a uuid nobody types, and matching it would make a search for "paris" hit a
 * machine whose id happens to contain those letters. */
void MainWindow::applyMachineFilter(const QString &text)
{
    const QString q = text.trimmed();
    int shown = 0;
    for (int i = 0; i < machine_cards_.size(); i++) {
        const bool match = q.isEmpty()
            || machine_names_.value(i).contains(q, Qt::CaseInsensitive);
        machine_cards_.at(i)->setVisible(match);
        if (match) shown++;
    }
    if (!q.isEmpty() && shown == 0) {
        machines_empty_->setText(tr("No machine matches \u201c%1\u201d.").arg(q));
        machines_empty_->setVisible(true);
    } else if (!machine_cards_.isEmpty()) {
        machines_empty_->setVisible(false);
    }
}

/* D5 - the list could not be fetched. */
void MainWindow::onMachinesFailed(int http)
{
    clearMachineCards();
    machines_empty_->setVisible(false);
    machines_filter_->setVisible(false);
    machines_subtitle_->clear();

    machines_error_text_->setText(
        http == 401 || http == 403
            ? tr("Your session was refused (HTTP %1). Signing out and back in "
                 "is usually what this needs.").arg(http)
            : http == 0
                ? tr("Shadow could not be reached. Check the connection, then "
                     "try again.")
                : tr("The machine list could not be fetched (HTTP %1).").arg(http));
    machines_error_retry_->setText(tr("Try again"));
    machines_error_->setVisible(true);
    setPage(PageMachines);
    statusBar()->showMessage(tr("the machine list could not be fetched"));
}

void MainWindow::setMachinesBusy(bool busy)
{
    for (MachineCard *c : machine_cards_) c->setBusy(busy);
}

void MainWindow::connectTo(const QString &id)
{
    const int row = machine_ids_.indexOf(id);
    if (row < 0) return;
    /* VMK1 - the card disables its own button, but `connectTo` is also
     * reached by Enter and by a double-click, and a guard at the one place
     * every path goes through beats three guards at the paths. */
    if (row < machine_cards_.size() && machine_cards_.at(row)->underMaintenance()) {
        statusBar()->showMessage(
            tr("Shadow reports this machine as under maintenance."), 6000);
        return;
    }
    last_machine_id_ = id;   /* UI3 - what Retry retries */

    /* UI6 - the local record the list is ordered by. Written on the ATTEMPT
     * and not on success: "the one I was last on" is what a person means, and
     * a connection that failed is still the one they tried. */
    {
        QSettings st;
        st.beginGroup(QStringLiteral("machines"));
        st.setValue(id + QStringLiteral("/last_used"), QDateTime::currentDateTime());
        st.endGroup();
    }

    /* Core's contract 1: one session per process. Refused here rather than
     * letting two worker threads into the same module state. */
    if (session_live_) {
        QMessageBox::information(this, tr("Already streaming"),
            tr("Only one session can run at a time: the core's session API is "
               "global. Leave the current stream first."));
        return;
    }
    session_live_ = true;
    setMachinesBusy(true);

    steps_->reset();
    steps_->setHeadline(tr("connecting to %1").arg(machine_names_.value(row, id)));
    setPage(PageConnecting);

    QMetaObject::invokeMethod(boot_, "start", Qt::QueuedConnection,
                              Q_ARG(QString, launcher_url_),
                              Q_ARG(QString, bearer_),
                              Q_ARG(QString, id));
}

void MainWindow::onBootstrapReady(const BootstrapWorker::Ready &r)
{
    steps_->setState(6, StepListWidget::State::Done,
                     QStringLiteral("%1:%2").arg(r.vmHost).arg(r.portBase));
    video_->setStatus(tr("opening the stream on :%1").arg(r.portBase + 11));
    if (video_) video_->beginSession();   /* VID1 - back to the waiting panel */
    session_started_.start();             /* CAPS2 - the countdown's origin */
    setPage(PageStreaming);

    /* FM1 - the file manager becomes reachable. The SFTP channel is granted a
     * moment later, during the control-channel announcements inside the session;
     * the window opened now would find it not-yet-ready and offers Reconnect, so
     * enabling here rather than waiting for a caps signal we do not have is
     * safe. */
    act_files_->setEnabled(true);
    stream_menu_->menuAction()->setVisible(true);
    /* OV1 - the corner HUD appears with the stream when any metric is selected;
     * the overlay menu waits for its hotkey. Positioned after the page is shown
     * so the video has its geometry. */
    QTimer::singleShot(0, this, [this] { updateOverlayVisibility(); });
    /* IN1 - the video surface takes the keyboard as soon as the stream shows,
     * so the first keystroke is forwarded without a click to focus it first. */
    video_->setFocus(Qt::OtherFocusReason);

    /* The SSE keepalives travel with it, and the session stops them - see
     * SessionWorker::runSession. */
    QMetaObject::invokeMethod(sess_, "runSession", Qt::QueuedConnection,
                              Q_ARG(BootstrapWorker::Ready, r));
}
