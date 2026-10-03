/* MainWindow - see main_window.hpp for the four states and the three threads. */
#include "main_window.hpp"

#include "auth_worker.hpp"
#include "core_scope.hpp"
#include "session_worker.hpp"
#include "step_list_widget.hpp"
#include "video_widget.hpp"
#include "settings_window.hpp"
#include "metrics_window.hpp"
#include "file_manager_window.hpp"
#include "about_dialog.hpp"
#include "theme.hpp"

extern "C" {
#include "core/version.h"
}

#include <QStackedWidget>
#include <QThread>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFont>
#include <QClipboard>
#include <QGuiApplication>
#include <QStatusBar>
#include <QMessageBox>
#include <QtConcurrent>
#include <QMenuBar>
#include <QKeySequence>
#include <QEvent>

using halyard::str;

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(str(SHADOW_APP_NAME));
    setWindowIcon(halyard::theme::appIcon());
    resize(1280, 760);

    stack_ = new QStackedWidget(this);
    setCentralWidget(stack_);

    /* ---------------------------------------------------- page 1: pairing */
    {
        auto *page = new QWidget(this);
        auto *lay = new QVBoxLayout(page);
        lay->setAlignment(Qt::AlignCenter);

        auto *title = new QLabel(tr("Halyard"), page);
        QFont tf = title->font();
        tf.setPointSize(tf.pointSize() + 14);
        tf.setBold(true);
        title->setFont(tf);
        title->setAlignment(Qt::AlignCenter);

        pair_hint_ = new QLabel(tr("signing in..."), page);
        pair_hint_->setAlignment(Qt::AlignCenter);
        pair_hint_->setWordWrap(true);

        pair_code_ = new QLabel(QString(), page);
        QFont cf = pair_code_->font();
        cf.setPointSize(cf.pointSize() + 20);
        cf.setBold(true);
        cf.setLetterSpacing(QFont::AbsoluteSpacing, 6);
        pair_code_->setFont(cf);
        pair_code_->setAlignment(Qt::AlignCenter);
        pair_code_->setTextInteractionFlags(Qt::TextSelectableByMouse);

        lay->addWidget(title);
        lay->addSpacing(24);
        lay->addWidget(pair_hint_);
        lay->addSpacing(16);
        lay->addWidget(pair_code_);
        stack_->addWidget(page);
    }

    /* --------------------------------------------------- page 2: machines */
    {
        auto *page = new QWidget(this);
        auto *lay = new QVBoxLayout(page);
        machines_title_ = new QLabel(page);
        QFont lf = machines_title_->font();
        lf.setPointSize(lf.pointSize() + 6);
        machines_title_->setFont(lf);

        machines_ = new QListWidget(page);
        connect_ = new QPushButton(page);
        connect_->setEnabled(false);

        auto *row = new QHBoxLayout;
        row->addStretch(1);
        row->addWidget(connect_);

        lay->addWidget(machines_title_);
        lay->addWidget(machines_, 1);
        lay->addLayout(row);
        stack_->addWidget(page);

        connect(machines_, &QListWidget::currentRowChanged, this, [this](int r) {
            connect_->setEnabled(r >= 0 && !session_live_);
        });
        connect(machines_, &QListWidget::itemDoubleClicked, this,
                [this] { connectSelected(); });
        connect(connect_, &QPushButton::clicked, this, &MainWindow::connectSelected);
    }

    /* ------------------------------------------------- page 3: connecting */
    steps_ = new StepListWidget(this);
    stack_->addWidget(steps_);

    /* -------------------------------------------------- page 4: streaming */
    video_ = new VideoWidget(this);
    stack_->addWidget(video_);

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
        act_settings->setShortcuts({ QKeySequence(QStringLiteral("Ctrl+,")),
                                     QKeySequence::Preferences });
        connect(act_settings, &QAction::triggered, this, &MainWindow::openSettings);

        QAction *act_metrics = act_metrics_ = view_menu_->addAction(QString());
        act_metrics->setShortcut(QKeySequence(QStringLiteral("Ctrl+M")));
        connect(act_metrics, &QAction::triggered, this, &MainWindow::openMetrics);

        /* FM1 - the file manager. Enabled only while a session is live, because
         * the SFTP channel and its credential exist only then; `session_live_`
         * drives it, set on connect and cleared on every session exit. */
        act_files_ = view_menu_->addAction(QString());
        act_files_->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));
        act_files_->setEnabled(false);
        connect(act_files_, &QAction::triggered, this, &MainWindow::openFileManager);

        /* QT4 - Help, with About.
         *
         * `QAction::AboutRole` is what moves this item into the application
         * menu on macOS, where a Help menu entry called "About" is wrong. On
         * Windows and Linux the role is ignored and it stays here, so one line
         * is correct on all three. */
        help_menu_ = menuBar()->addMenu(QString());
        QAction *act_about = act_about_ = help_menu_->addAction(QString());
        act_about->setMenuRole(QAction::AboutRole);
        connect(act_about, &QAction::triggered, this, &MainWindow::openAbout);
    }

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
            [this](const QString &name, const QString &) {
                statusBar()->showMessage(tr("data centre: %1").arg(name));
            }, Qt::QueuedConnection);
    connect(auth_, &AuthWorker::progress, this, [this](const QString &w) {
        pair_hint_->setText(w);
    }, Qt::QueuedConnection);
    connect(auth_, &AuthWorker::pairingNeeded, this,
            &MainWindow::onPairingNeeded, Qt::QueuedConnection);
    connect(auth_, &AuthWorker::pairingProgress, this, [this](int left) {
        statusBar()->showMessage(tr("waiting for the browser - %1 s left").arg(left));
    }, Qt::QueuedConnection);
    connect(auth_, &AuthWorker::succeeded, this,
            &MainWindow::onSignedIn, Qt::QueuedConnection);
    connect(auth_, &AuthWorker::failed, this, [this](const QString &why) {
        pair_hint_->setText(tr("sign-in failed: %1").arg(why));
        pair_code_->clear();
    }, Qt::QueuedConnection);

    connect(boot_, &BootstrapWorker::stepRunning, this, [this](int i, const QString &d) {
        steps_->setState(i, StepListWidget::State::Running, d);
    }, Qt::QueuedConnection);
    connect(boot_, &BootstrapWorker::stepDone, this, [this](int i, const QString &d) {
        steps_->setState(i, StepListWidget::State::Done, d);
    }, Qt::QueuedConnection);
    connect(boot_, &BootstrapWorker::stepFailed, this, [this](int i, const QString &d) {
        steps_->setState(i, StepListWidget::State::Failed, d);
        session_live_ = false;
        connect_->setEnabled(machines_->currentRow() >= 0);
    }, Qt::QueuedConnection);
    connect(boot_, &BootstrapWorker::ready, this,
            &MainWindow::onBootstrapReady, Qt::QueuedConnection);

    connect(sess_, &SessionWorker::frameReady, video_,
            &VideoWidget::presentFrame, Qt::QueuedConnection);
    connect(sess_, &SessionWorker::progress, this,
            [this](const QString &s, const QString &d) {
                video_->setStatus(s + QStringLiteral(" - ") + d);
            }, Qt::QueuedConnection);
    connect(sess_, &SessionWorker::finished, this,
            [this](bool ok, const QString &why) {
                video_->setStatus((ok ? tr("session ended: ")
                                      : tr("session stopped: ")) + why);
                session_live_ = false;
                /* FM1 - the SFTP credential died with the session, so the file
                 * manager is now pointing at nothing: close it and grey the
                 * menu entry until the next session grants a fresh channel. */
                act_files_->setEnabled(false);
                if (file_manager_) file_manager_->close();
                setPage(PageMachines);
                connect_->setEnabled(machines_->currentRow() >= 0);
            }, Qt::QueuedConnection);

    auth_thread_->start();
    boot_thread_->start();
    sess_thread_->start();

    QMetaObject::invokeMethod(auth_, "signIn", Qt::QueuedConnection);
}

MainWindow::~MainWindow()
{
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
    machines_title_->setText(tr("Your machines"));
    connect_->setText(tr("Connect"));
    view_menu_->setTitle(tr("&View"));
    act_settings_->setText(tr("&Settings"));
    act_metrics_->setText(tr("&Metrics"));
    act_files_->setText(tr("&File transfer..."));
    help_menu_->setTitle(tr("&Help"));
    act_about_->setText(tr("&About %1").arg(str(SHADOW_APP_NAME)));
}

void MainWindow::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::LanguageChange) retranslate();
    QMainWindow::changeEvent(e);
}

void MainWindow::openSettings()
{
    if (!settings_) {
        settings_ = new SettingsWindow(this);
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

void MainWindow::openAbout()
{
    /* Built each time and not kept: it holds no state, and a dialog that is
     * constructed once keeps the palette it was born with - which is wrong the
     * moment the desktop switches to dark mode while the client is running. */
    halyard::AboutDialog dlg(this);
    dlg.exec();
}

void MainWindow::setPage(int index) { stack_->setCurrentIndex(index); }

void MainWindow::onPairingNeeded(const QString &userCode, const QString &uri,
                                 const QString &uriComplete, int expiresIn)
{
    pair_code_->setText(userCode);

    /* CLIP5, the same reasoning as the Borealis client: the code is eight
     * characters to retype into a browser ON THIS MACHINE, so it is copied and
     * the screen SAYS so - a clipboard that changed without being asked is
     * unsettling unless something accounts for it. Done here and not in the
     * worker: this is the GUI thread, which is where clipboard calls belong. */
    bool copied = false;
    if (QClipboard *cb = QGuiApplication::clipboard()) {
        cb->setText(userCode);
        copied = true;
    }

    pair_hint_->setText(
        tr("Open %1 and enter this code.%2\nIt is valid for %3 s.")
            .arg(uri.isEmpty() ? QStringLiteral("shadow.tech/device") : uri,
                 copied ? tr("  The code is on your clipboard - paste it.")
                        : QString(),
                 QString::number(expiresIn)));

    /* The one-click form is OFFERED, not opened: launching a browser unasked
     * is the kind of thing that startles people. */
    if (!uriComplete.isEmpty())
        statusBar()->showMessage(tr("one-click link: %1").arg(uriComplete));
}

void MainWindow::onSignedIn(const QString &bearer, const QString &launcherUrl)
{
    bearer_ = bearer;            /* a credential: kept, never logged */
    launcher_url_ = launcherUrl;
    pair_hint_->setText(tr("signed in"));
    pair_code_->clear();
    listMachines();
}

void MainWindow::listMachines()
{
    statusBar()->showMessage(tr("fetching your machines..."));
    const QByteArray base = launcher_url_.toUtf8();
    const QByteArray tok  = bearer_.toUtf8();

    /* `launcher_list_vms` is blocking HTTP, so it does NOT run here. One short
     * call does not justify a fourth worker, so it goes to the thread pool and
     * the result comes back through a queued invocation. */
    (void)QtConcurrent::run([this, base, tok] {
        halyard::ScopedVmPage page;
        long http = 0;
        QStringList ids, labels;
        if (launcher_list_vms(base.constData(), tok.constData(), 0, 50,
                              page.out(), &http)) {
            for (int i = 0; i < page->count; i++) {
                const VmInfo &v = page->items[i];
                ids << str(v.id);
                QString label = str(v.alias);
                if (label.isEmpty()) label = str(v.name);
                if (label.isEmpty()) label = str(v.id);
                const QString st = str(v.state);
                labels << (st.isEmpty() ? label
                                        : QStringLiteral("%1   (%2)").arg(label, st));
            }
        }
        QMetaObject::invokeMethod(this, "onMachinesFetched", Qt::QueuedConnection,
                                  Q_ARG(QStringList, ids),
                                  Q_ARG(QStringList, labels));
    });
}

void MainWindow::onMachinesFetched(const QStringList &ids, const QStringList &labels)
{
    machine_ids_ = ids;
    machines_->clear();
    machines_->addItems(labels);
    statusBar()->showMessage(tr("%1 machine(s)").arg(ids.size()));
    setPage(PageMachines);
    if (!ids.isEmpty()) machines_->setCurrentRow(0);
}

void MainWindow::connectSelected()
{
    const int row = machines_->currentRow();
    if (row < 0 || row >= machine_ids_.size()) return;

    /* Core's contract 1: one session per process. Refused here rather than
     * letting two worker threads into the same module state. */
    if (session_live_) {
        QMessageBox::information(this, tr("Already streaming"),
            tr("Only one session can run at a time: the core's session API is "
               "global. Leave the current stream first."));
        return;
    }
    session_live_ = true;
    connect_->setEnabled(false);

    steps_->reset();
    steps_->setHeadline(tr("connecting to %1").arg(machines_->currentItem()->text()));
    setPage(PageConnecting);

    QMetaObject::invokeMethod(boot_, "start", Qt::QueuedConnection,
                              Q_ARG(QString, launcher_url_),
                              Q_ARG(QString, bearer_),
                              Q_ARG(QString, machine_ids_.at(row)));
}

void MainWindow::onBootstrapReady(const BootstrapWorker::Ready &r)
{
    steps_->setState(6, StepListWidget::State::Done,
                     QStringLiteral("%1:%2").arg(r.vmHost).arg(r.portBase));
    video_->setStatus(tr("opening the stream on :%1").arg(r.portBase + 11));
    setPage(PageStreaming);

    /* FM1 - the file manager becomes reachable. The SFTP channel is granted a
     * moment later, during the control-channel announcements inside the session;
     * the window opened now would find it not-yet-ready and offers Reconnect, so
     * enabling here rather than waiting for a caps signal we do not have is
     * safe. */
    act_files_->setEnabled(true);
    /* IN1 - the video surface takes the keyboard as soon as the stream shows,
     * so the first keystroke is forwarded without a click to focus it first. */
    video_->setFocus(Qt::OtherFocusReason);

    /* The SSE keepalives travel with it, and the session stops them - see
     * SessionWorker::runSession. */
    QMetaObject::invokeMethod(sess_, "runSession", Qt::QueuedConnection,
                              Q_ARG(BootstrapWorker::Ready, r));
}
