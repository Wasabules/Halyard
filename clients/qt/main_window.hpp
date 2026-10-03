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

#include "bootstrap_worker.hpp"

class QStackedWidget;
class QThread;
class QLabel;
class QListWidget;
class QPushButton;
class QMenu;
class QAction;

class AuthWorker;
class SessionWorker;
class VideoWidget;
class StepListWidget;
class SettingsWindow;
class MetricsWindow;
class FileManagerWindow;

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

    /* IN2 - true when a keystroke should go to the VM, read by the Windows
     * system-shortcut hook through the halyard_ui_keys_blocked weak symbol. */
    bool keysGoToVm() const;

private slots:
    void onPairingNeeded(const QString &userCode, const QString &uri,
                         const QString &uriComplete, int expiresIn);
    void onSignedIn(const QString &bearer, const QString &launcherUrl);
    void onMachinesFetched(const QStringList &ids, const QStringList &labels);
    void onBootstrapReady(const BootstrapWorker::Ready &r);
    void connectSelected();

protected:
    void changeEvent(QEvent *e) override;

private:
    void retranslate();
    void listMachines();
    void setPage(int index);

    enum Page { PagePairing = 0, PageMachines, PageConnecting, PageStreaming };

    QStackedWidget *stack_ = nullptr;

    /* pairing */
    QLabel *pair_code_ = nullptr;
    QLabel *pair_hint_ = nullptr;

    /* machines */
    QListWidget *machines_ = nullptr;
    QPushButton *connect_  = nullptr;
    /* QT5 - kept so retranslate() can re-label them. */
    QLabel  *machines_title_ = nullptr;
    QMenu   *view_menu_      = nullptr;
    QMenu   *help_menu_      = nullptr;
    QAction *act_settings_   = nullptr;
    QAction *act_metrics_    = nullptr;
    QAction *act_files_      = nullptr;
    QAction *act_about_      = nullptr;
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

    bool session_live_ = false;
};
