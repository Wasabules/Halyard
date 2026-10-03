/* FileManagerWindow - two panes, send and receive. A FileZilla for the VM.
 *
 * === FM1 2026-10-03 — WHAT IT IS ===========================================
 *
 * Left pane: this PC. Right pane: the VM, over the SFTP channel on `:base+15`
 * that core already speaks (`filetransfer.h`). A file picked on one side and the
 * button between them moves it to the other. The whole point of the channel was
 * that no third-party client can use the credential (it is a 395-byte PEM used
 * as a password, which no GUI password box accepts - FT5); this is the client
 * that can, because it calls libssh directly through core rather than pasting a
 * credential into someone else's box.
 *
 * === WHY EACH PANE IS ONE DIRECTORY, NOT A TREE =============================
 *
 * FileZilla shows a tree and a list per side; this shows one directory per side
 * with a path bar and an "up". The reason is the remote half: the server answers
 * READDIR but not REALPATH or LSTAT (KB §3.50), so a lazy-expanding tree would
 * have to stat each node to know if it can expand, and it cannot. One directory
 * at a time is the shape the protocol actually supports, so both panes use it
 * and they match.
 *
 * === THREADING ==============================================================
 *
 * The SFTP handle is single-threaded and its transfers block, so it lives on a
 * worker (`FtWorker`) on its own QThread; this window only sends queued requests
 * and paints the signals. Cancel is the one exception - an atomic the worker
 * reads mid-transfer (see ft_worker.hpp). The local side is plain
 * `QFileSystemModel`, which is already threaded inside Qt.
 */
#pragma once

#include <QWidget>

#include <QHash>

#include "ft_worker.hpp"

class QFileSystemModel;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QThread;
class QToolButton;
class QTreeView;
class QTreeWidget;
class QTreeWidgetItem;

class FileManagerWindow : public QWidget {
    Q_OBJECT
public:
    explicit FileManagerWindow(QWidget *parent = nullptr);
    ~FileManagerWindow() override;

protected:
    void changeEvent(QEvent *e) override;

private slots:
    void onConnected(bool ok, const QString &message);
    void onListed(const QString &dir, const QList<halyard::FtEntry> &entries,
                  bool truncated);
    void onTransferStarted(int id, const QString &name, bool upload);
    void onProgress(int id, qint64 done, qint64 total);
    void onTransferDone(int id, bool ok, const QString &message);
    void onActionDone(bool ok, const QString &message);
    void onFailed(const QString &message);

    void sendToVm();
    void receiveFromVm();
    void remoteActivated(QTreeWidgetItem *item, int column);
    void remoteUp();
    void localUp();
    void localActivated(const QModelIndex &index);
    void newRemoteFolder();
    void deleteRemote();
    void refreshRemote();
    void renameRemote();
    void remoteContextMenu(const QPoint &pos);
    void localContextMenu(const QPoint &pos);
    void toggleWholeFs(bool on);

private:
    void retranslate();
    void setBusy(bool busy);
    void setLocalDir(const QString &dir);
    QString localDir() const;
    QString remoteJoin(const QString &name) const;
    QString remoteFloor() const { return absolute_ ? QStringLiteral("/") : QString(); }
    int  enqueueRow(const QString &name, bool upload);   /* adds to the Active tab */

    QThread          *thread_ = nullptr;
    halyard::FtWorker *worker_ = nullptr;

    QFileSystemModel *localModel_ = nullptr;
    QTreeView        *localView_  = nullptr;
    QLineEdit        *localPath_  = nullptr;
    QToolButton      *localUp_    = nullptr;

    QTreeWidget      *remoteView_ = nullptr;
    QLineEdit        *remotePath_ = nullptr;
    QToolButton      *remoteUp_   = nullptr;
    class QCheckBox  *wholeFs_    = nullptr;
    QString           remoteDir_;
    bool              remoteReady_ = false;
    bool              absolute_   = false;   /* FM2 - whole-FS browsing on */

    QPushButton      *sendBtn_    = nullptr;
    QPushButton      *recvBtn_    = nullptr;
    QToolButton      *refreshBtn_ = nullptr;
    QToolButton      *newFolderBtn_ = nullptr;
    QToolButton      *deleteBtn_  = nullptr;

    QPushButton      *cancelBtn_ = nullptr;
    QPushButton      *reconnectBtn_ = nullptr;
    QLabel           *status_ = nullptr;

    /* FM3 - the transfer panel: three tabs (active / failed / done), one row per
     * transfer, keyed by the worker's id so progress and the result land on the
     * right line. This is what replaces the single bar and the vague dialog. */
    class QTabWidget *transfers_ = nullptr;
    QTreeWidget      *txActive_ = nullptr;
    QTreeWidget      *txFailed_ = nullptr;
    QTreeWidget      *txDone_   = nullptr;
    QHash<int, QTreeWidgetItem *> txItems_;
    int               nextId_ = 1;

    bool              busy_ = false;

    QLabel           *localHeading_ = nullptr;
    QLabel           *remoteHeading_ = nullptr;
};
