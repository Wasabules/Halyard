/* halyard-qt - the desktop client's skeleton.
 *
 * It does ONE thing on purpose: start a session on a worker thread, and put
 * whatever pictures come out of it on screen. No OAuth, no machine list, no
 * settings. Those are forms; this is the part the choice of framework hangs on.
 *
 * Run it with a VM address and port base to exercise the real path:
 *     halyard-qt <vm-host> <port-base>
 * Without arguments it still starts, shows the window, and reports that the
 * session stops at the control channel - which is correct, since the REST
 * bootstrap that yields the tokens is not written yet.
 *
 * See clients/qt/README.md for why Qt, which version, and the three contracts.
 */
#include <QApplication>
#include <QMainWindow>
#include <QThread>
#include <QStatusBar>
#include <QTimer>

#include "session_worker.hpp"
#include "video_widget.hpp"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("halyard-qt"));

    QMainWindow win;
    win.setWindowTitle(QStringLiteral("Halyard (Qt skeleton)"));

    auto *video = new VideoWidget(&win);
    win.setCentralWidget(video);
    win.resize(1280, 720);

    /* The worker lives on its own thread for the whole run. `deleteLater` on
     * the thread's `finished` is what keeps the destruction order right: the
     * worker unregisters the frame sink in its destructor, and that must happen
     * on the thread that ran the session. */
    auto *thread = new QThread(&win);
    auto *worker = new SessionWorker;
    worker->moveToThread(thread);
    QObject::connect(thread, &QThread::finished, worker, &QObject::deleteLater);

    /* QUEUED, both of them: the frame signal is emitted from the DECODE thread
     * and the progress signal from the session thread. A direct connection
     * would run Qt widget code off the GUI thread, which is the one thing Qt
     * does not forgive. */
    QObject::connect(worker, &SessionWorker::frameReady,
                     video, &VideoWidget::presentFrame, Qt::QueuedConnection);
    QObject::connect(worker, &SessionWorker::progress, video,
                     [video](const QString &step, const QString &detail) {
                         video->setStatus(step + QStringLiteral(" - ") + detail);
                     }, Qt::QueuedConnection);
    QObject::connect(worker, &SessionWorker::finished, video,
                     [video](bool ok, const QString &why) {
                         video->setStatus((ok ? QObject::tr("ended: ")
                                              : QObject::tr("stopped: ")) + why);
                     }, Qt::QueuedConnection);

    /* Closing the window raises the abort flag and waits for the session to
     * leave. Core polls it within ~100 ms - the bound the consoles impose, and
     * which this client inherits without doing anything. */
    QObject::connect(&app, &QApplication::aboutToQuit, [worker, thread] {
        worker->requestStop();
        thread->quit();
        thread->wait(3000);
    });

    thread->start();

    const QStringList args = QApplication::arguments();
    const QString host = args.size() > 1 ? args.at(1) : QString();
    const int portBase = args.size() > 2 ? args.at(2).toInt() : 0;

    if (host.isEmpty()) {
        video->setStatus(QObject::tr(
            "no VM given - run: halyard-qt <vm-host> <port-base>. "
            "The window, the thread and the frame sink are wired."));
    } else {
        /* Invoked on the worker's thread, never called directly. */
        QMetaObject::invokeMethod(worker, "run", Qt::QueuedConnection,
                                  Q_ARG(QString, host), Q_ARG(int, portBase));
    }

    win.show();
    return app.exec();
}
