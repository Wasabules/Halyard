/* MainWindow - the four states, and the three threads behind them.
 *
 * === QT2 2026-10-03 — WHY A STACK AND NOT A NAVIGATION STACK ===============
 *
 * The client has exactly four states and they are a straight line:
 *
 *     pairing  ->  machines  ->  connecting  ->  streaming
 *
 * You never go back to pairing except by signing out, and never to connecting
 * except by leaving the stream. A `QStackedWidget` says that; a navigation
 * stack with push and pop would allow orders that do not exist, which is how
 * the Borealis client collected the S63/S66 family of crashes (a view popped
 * while something still held a pointer to it).
 *
 * === THE THREADS ===========================================================
 *
 * Three workers, three lifetimes, and this is the only object that knows all
 * of them:
 *
 *   AuthWorker        once per sign-in. Blocking HTTP and a poll loop.
 *   BootstrapWorker   once per connection attempt. Seven blocking steps.
 *   SessionWorker     once per stream, and it lasts - it also owns the two SSE
 *                     keepalives the bootstrap handed over.
 *
 * Every signal out of them is QUEUED, without exception: `AuthWorker` and
 * `BootstrapWorker` emit from their own thread, and `SessionWorker::frameReady`
 * from core's DECODE thread. A direct connection would run widget code off the
 * GUI thread, which is the one thing Qt does not forgive.
 *
 * ONE SESSION PER PROCESS (core's contract 1): the window refuses a second
 * connection while one is live rather than letting two worker threads into the
 * same module state.
 */
#pragma once

#include <QMainWindow>
#include <QKeySequence>
#include <QElapsedTimer>
#include <QHash>
#include <QRect>
#include <QVariantList>
#include <QVector>

#include "bootstrap_worker.hpp"

class QStackedWidget;
class QThread;
class QLabel;
class QFrame;
class QLineEdit;
class QScreen;
class QScrollArea;
class QVBoxLayout;
class MachineCard;
class QPushButton;
class QMenu;
class QAction;

class AuthWorker;
class SessionWorker;
class VideoWidget;
class StepListWidget;
class SignInSteps;
class SettingsWindow;
class MetricsWindow;
class AccountWindow;
class FileManagerWindow;
class QShortcut;
class QMoveEvent;
class QResizeEvent;
namespace halyard { class StreamHud; class StreamOverlay;
                    class QrView; class CodeCells; class ValidityBar; }

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

public slots:
    /* QT3: the ONE path to each window, used by the menu and by the
     * command-line flags alike. Two paths would be two things to test. */
    void openSettings();
    void openMetrics();
    void openFileManager();
    /* Opens it with no session, for `--files`: see main.cpp. */
    void openFileManagerForced();
    void openAbout();
    void toggleFullscreen();
    void toggleOverlay();
    void showToast(const QString &text);
    void takeScreenshot();
    void copyDiagnostics();

    /* IN2 - true when a keystroke should go to the VM, read by the Windows
     * system-shortcut hook through the halyard_ui_keys_blocked weak symbol. */
    bool keysGoToVm() const;

private slots:
    void onPairingNeeded(const QString &userCode, const QString &uri,
                         const QString &uriComplete, int expiresIn);
    void onSignedIn(const QString &bearer, const QString &launcherUrl);
    /* VMK1 - one row per machine, each a QVariantMap of the /vms fields.
     * A queued signal needs a registered type and a QVariantList is one
     * already; a struct would have wanted Q_DECLARE_METATYPE and a
     * registration for six strings. */
    void onMachinesFetched(const QVariantList &rows);
    void onMachinesFailed(int http);   /* D5 */
    void onPairingProgress(int secondsLeft);
    void onSignInFailed(const QString &why);
    /* UI2 - run the sign-in sequence again after it failed or expired. */
    void restartSignIn();
    /* AUTH9 - forget the stored refresh token and pair again. */
    void openAccount();   /* ACC1 */
    void signOut();
    void onBootstrapReady(const BootstrapWorker::Ready &r);
    void connectTo(const QString &id);

protected:
    void changeEvent(QEvent *e) override;
    void moveEvent(QMoveEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    /* UI5 - Up/Down/Home/End on the machine list. */
    bool eventFilter(QObject *o, QEvent *ev) override;

private:
    void retranslate();
    void setMachinesBusy(bool busy);
    /* UI6 - the machine list's furniture. */
    void clearMachineCards();
    void showMachineSkeletons();
    void staggerIn(QWidget *w, int index);
    void applyMachineFilter(const QString &text);
    void repositionOverlays();
    void updateOverlayVisibility();
    bool overlaysAllowed() const;
    /* OV10 - another of our windows is sitting over the picture. */
    bool ownWindowOverVideo() const;
    /* KEY1 - every shortcut, from the one table in shortcuts.hpp. Re-run
     * when the settings window changes a binding. */
    void updateHeader(int index);   /* D1 */
    QString sessionTimeLeft() const;   /* CAPS2 */
    /* LIM1 - the end-of-session warnings. */
    void    checkSessionLimit();
    void    showBanner(const QString &text, bool urgent);
    QString fmtLeft(int seconds) const;
    QScreen *fullscreenTarget() const;   /* SCR1 */
    void applyShortcuts();
    QKeySequence keySequenceFor(int action) const;
    QRect videoGlobalRect() const;
    void listMachines();
    void setPage(int index);

    enum Page { PagePairing = 0, PageMachines, PageConnecting, PageStreaming };

    QStackedWidget *stack_ = nullptr;

    /* pairing */
    QLabel *pair_hint_ = nullptr;
    QPushButton *copy_code_btn_ = nullptr;
    QLabel      *pair_countdown_ = nullptr;
    QLabel      *pair_tagline_ = nullptr;
    QLabel      *pair_heading_ = nullptr;
    QLabel      *qr_caption_   = nullptr;
    SignInSteps *signin_steps_ = nullptr;
    halyard::QrView     *qr_             = nullptr;
    halyard::CodeCells  *pair_cells_     = nullptr;
    halyard::ValidityBar *pair_validity_ = nullptr;
    int          pair_expires_ = 600;   /* UI4 - what the bar scales against */
    QPushButton *retry_btn_      = nullptr;
    QPushButton *open_page_btn_ = nullptr;
    QString pair_user_code_, pair_uri_, pair_uri_complete_;


    /* machines */
    QScrollArea *machines_scroll_ = nullptr;   /* UI5 - the arrows land here */
    QWidget     *machines_host_ = nullptr;
    QVBoxLayout *machines_lay_  = nullptr;
    QLabel      *machines_subtitle_ = nullptr;
    QLabel      *machines_empty_ = nullptr;
    QVector<MachineCard *> machine_cards_;
    QStringList  machine_names_;
    QWidget     *header_         = nullptr;   /* D1 */
    QLabel      *header_title_   = nullptr;
    QLabel      *header_account_ = nullptr;
    QLineEdit   *machines_filter_ = nullptr;
    QFrame      *machines_error_       = nullptr;   /* D5 */
    QLabel      *machines_error_text_  = nullptr;
    QPushButton *machines_error_retry_ = nullptr;
    QVector<QWidget *> machine_skeletons_;
    QString      datacentre_;        /* UI6 - shown on every card */
    QString      launcher_api_version_;   /* ACC1 - from TINAG */
    /* SSE1 - the run state the event stream reported, by machine id.
     * The only source there is: /vms answers `status: null`. */
    QHash<QString, QString> vm_state_;
    AccountWindow *account_ = nullptr;    /* ACC1 */
    BootstrapWorker::Caps caps_;     /* CAPS2 - what the account may do */
    QElapsedTimer session_started_;  /* CAPS2 - the session countdown */
    QString      last_machine_id_;   /* UI3 - what Retry retries */
    QRect        normal_geometry_;   /* SCR1 - where to return from another screen */
    /* QT5 - kept so retranslate() can re-label them. */
    QLabel  *machines_title_ = nullptr;
    QMenu   *view_menu_      = nullptr;
    QMenu   *help_menu_      = nullptr;
    QAction *act_settings_   = nullptr;
    QAction *act_metrics_    = nullptr;
    QAction *act_files_      = nullptr;
    QAction *act_about_      = nullptr;
    QMenu   *stream_menu_    = nullptr;
    QAction *act_fs_         = nullptr;
    QAction *act_hide_cursor_= nullptr;
    QAction *act_disconnect_ = nullptr;
    QAction *act_sign_out_   = nullptr;
    QAction *act_account_    = nullptr;   /* ACC1 */
    QAction *act_stream_metrics_ = nullptr;

    QStringList machine_ids_;

    /* connecting + streaming */
    StepListWidget *steps_ = nullptr;
    VideoWidget    *video_ = nullptr;

    /* The bearer is a CREDENTIAL. Kept for the session because every launcher
     * call needs it, never written to the log and never shown. */
    QString bearer_;
    QString launcher_url_;

    QThread *auth_thread_ = nullptr;
    QThread *boot_thread_ = nullptr;
    QThread *sess_thread_ = nullptr;
    AuthWorker      *auth_ = nullptr;
    BootstrapWorker *boot_ = nullptr;
    SessionWorker   *sess_ = nullptr;

    /* QT3: created on first use and KEPT, so position and page survive a
     * close - the whole reason they are windows and not dialogs. Parented to
     * `this` with Qt::Window, so they die with the main window and no manual
     * delete is needed. */
    SettingsWindow *settings_ = nullptr;
    MetricsWindow  *metrics_  = nullptr;
    FileManagerWindow *file_manager_ = nullptr;
    halyard::StreamHud     *stream_hud_ = nullptr;
    halyard::StreamOverlay *stream_overlay_ = nullptr;
    QShortcut              *overlay_shortcut_    = nullptr;
    QShortcut              *screenshot_shortcut_ = nullptr;
    QTimer                 *overlay_watch_    = nullptr;  /* OV10 - overlap poll */
    QLabel                 *toast_ = nullptr;
    QLabel                 *banner_ = nullptr;   /* LIM1 - it stays */
    int                     limit_state_ = 0;    /* LIM1 - thresholds done */
    QTimer                 *toast_timer_ = nullptr;
    bool                    app_active_ = true;

    bool session_live_ = false;
};
