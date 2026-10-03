/* FtWorker - the SFTP channel, driven from ONE thread.
 *
 * === FM1 2026-10-03 — WHY A WORKER AND NOT CALLS FROM THE WINDOW ============
 *
 * `shadow_ft` owns a libssh session and is NOT thread-safe: core's own header
 * says so, in capitals, and spells out that two threads in one handle corrupt
 * the SSH stream. A transfer also BLOCKS - a 2 GB download parks its thread for
 * minutes. Both facts point the same way: the handle lives on a dedicated
 * QThread, every call to it is a queued slot, and the GUI thread only ever
 * sends requests and receives signals.
 *
 * The one thing that does NOT go through the queue is cancellation. While a
 * transfer runs, the worker thread is inside `shadow_ft_get` and will not
 * process another slot until it returns - so a queued `cancel()` would arrive
 * only once the thing it meant to stop had finished. Cancel is therefore an
 * atomic flag the GUI thread sets directly and the progress callback (which
 * runs ON the worker thread, inside the transfer) reads. That is the single
 * piece of shared state between the two threads, and it is atomic.
 *
 * === THE CREDENTIAL NEVER LANDS HERE ========================================
 *
 * `connectToVm()` fetches the secret from core into a local buffer, hands it to
 * `shadow_ft_open`, and zeroes it before the slot returns. This object never
 * stores it, never logs it, and has no getter for it. It grants read AND write
 * to the whole VM filesystem (KB §3.50); it stays on the stack for the length
 * of one call and nowhere else.
 */
#pragma once

#include <QList>
#include <QObject>
#include <QString>

#include <atomic>

struct shadow_ft;   /* opaque, from core/protocol/filetransfer.h */

namespace halyard {

/* One remote directory entry, copied out of core's shadow_ft_entry so nothing
 * in the GUI holds a pointer into the worker. */
struct FtEntry {
    QString  name;
    quint64  size = 0;
    bool     isDir = false;
};

class FtWorker : public QObject {
    Q_OBJECT
public:
    explicit FtWorker(QObject *parent = nullptr);
    ~FtWorker() override;

    /* Set from the GUI thread, read by the transfer in flight. Asking to cancel
     * is the only thing that may touch the worker while it is busy. */
    void requestCancel() { cancel_.store(true, std::memory_order_relaxed); }

public slots:
    /* Opens the channel from core's live grant (host, port, secret). Emits
     * connected(). Safe to call again to reconnect. */
    void connectToVm();
    void disconnectFromVm();

    /* Lists a remote directory and emits listed(). "" or "." is the Downloads
     * root. */
    void listRemote(const QString &dir);

    /* One file each, carrying a caller-assigned `id` so the transfer panel can
     * follow exactly this row through started/progress/done - names are not
     * unique (the same file sent twice) and the queue runs several. `remoteDir`
     * is where an upload lands; the basename is taken from the local path. Emits
     * transferStarted/progress/transferDone, then re-lists `remoteDir`. */
    void upload(int id, const QString &localPath, const QString &remoteDir);
    void download(int id, const QString &remotePath, const QString &localDir,
                  quint64 knownSize);

    void makeDir(const QString &remoteParent, const QString &name);
    void removeEntry(const QString &remotePath, bool isDir, const QString &listAfter);
    void rename(const QString &fromPath, const QString &toPath, const QString &listAfter);

    /* FM2 - lift the Downloads-root confinement for this session, so the whole
     * VM filesystem can be browsed. The credential already grants it (KB §3.50);
     * ft_path's confinement is a seatbelt, and this unbuckles it deliberately.
     * `then` re-lists that directory once the mode is set. */
    void setAllowAbsolute(bool on, const QString &then);

signals:
    void connected(bool ok, const QString &message);
    void listed(const QString &dir, const QList<halyard::FtEntry> &entries,
                bool truncated);
    void transferStarted(int id, const QString &name, bool upload);
    void progress(int id, qint64 done, qint64 total);
    void transferDone(int id, bool ok, const QString &message);
    void actionDone(bool ok, const QString &message);
    void failed(const QString &message);         /* list/connect errors */

private:
    /* The C progress callback trampolines here; returning false cancels. */
    bool onProgress(quint64 done, quint64 total);
    static bool progressTrampoline(quint64 done, quint64 total, void *user);

    shadow_ft        *ft_ = nullptr;
    std::atomic<bool> cancel_{false};
    int               current_id_ = -1;   /* the transfer onProgress reports on */
};

}  // namespace halyard

Q_DECLARE_METATYPE(QList<halyard::FtEntry>)
