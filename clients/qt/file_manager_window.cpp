/* FileManagerWindow - see the header for the shape and the threading. */
#include "file_manager_window.hpp"

#include "theme.hpp"

#include <QDir>
#include <QEvent>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QThread>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace theme = halyard::theme;

namespace {

QString humanSize(quint64 n)
{
    const char *u[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)n;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    return i == 0 ? QStringLiteral("%1 B").arg(n)
                  : QStringLiteral("%1 %2").arg(v, 0, 'f', 1).arg(QString::fromUtf8(u[i]));
}

enum { RoleIsDir = Qt::UserRole + 1, RoleSize = Qt::UserRole + 2 };

}  // namespace

FileManagerWindow::FileManagerWindow(QWidget *parent)
    : QWidget(parent, Qt::Window)
{
    setWindowIcon(theme::appIcon());
    resize(1080, 680);

    qRegisterMetaType<QList<halyard::FtEntry>>();

    /* ---- local pane -------------------------------------------------- */
    localModel_ = new QFileSystemModel(this);
    localModel_->setRootPath(QString());
    localView_ = new QTreeView(this);
    localView_->setModel(localModel_);
    localView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    localView_->setRootIsDecorated(false);
    localView_->setSortingEnabled(true);
    localView_->sortByColumn(0, Qt::AscendingOrder);
    localView_->setColumnWidth(0, 260);
    localPath_ = new QLineEdit(this);
    localUp_ = new QToolButton(this);
    localUp_->setText(QStringLiteral("↑"));

    /* ---- remote pane ------------------------------------------------- */
    remoteView_ = new QTreeWidget(this);
    remoteView_->setColumnCount(3);
    remoteView_->setRootIsDecorated(false);
    remoteView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    remoteView_->setSortingEnabled(true);
    remoteView_->sortByColumn(0, Qt::AscendingOrder);
    remoteView_->setColumnWidth(0, 300);
    remotePath_ = new QLineEdit(this);
    remotePath_->setReadOnly(true);
    remoteUp_ = new QToolButton(this);
    remoteUp_->setText(QStringLiteral("↑"));

    /* ---- the buttons between the panes ------------------------------- */
    sendBtn_ = new QPushButton(this);
    recvBtn_ = new QPushButton(this);
    refreshBtn_ = new QToolButton(this);
    newFolderBtn_ = new QToolButton(this);
    deleteBtn_ = new QToolButton(this);

    /* ---- the progress row ------------------------------------------- */
    progress_ = new QProgressBar(this);
    progress_->setVisible(false);
    cancelBtn_ = new QPushButton(this);
    cancelBtn_->setVisible(false);
    reconnectBtn_ = new QPushButton(this);
    reconnectBtn_->setVisible(false);
    status_ = new QLabel(this);
    status_->setStyleSheet(theme::css(theme::muted(this)));

    localHeading_ = new QLabel(this);
    localHeading_->setFont(theme::headingFont(this));
    remoteHeading_ = new QLabel(this);
    remoteHeading_->setFont(theme::headingFont(this));

    /* ---- layout ------------------------------------------------------ */
    auto paneColumn = [&](QLabel *head, QLineEdit *path, QToolButton *up,
                          QWidget *view) {
        auto *col = new QVBoxLayout;
        col->setSpacing(theme::SpaceTight);
        col->addWidget(head);
        auto *bar = new QHBoxLayout;
        bar->addWidget(path, 1);
        bar->addWidget(up);
        col->addLayout(bar);
        col->addWidget(view, 1);
        return col;
    };

    auto *mid = new QVBoxLayout;
    mid->addStretch(1);
    mid->addWidget(sendBtn_);
    mid->addWidget(recvBtn_);
    mid->addSpacing(theme::SpaceGroup);
    mid->addWidget(refreshBtn_);
    mid->addWidget(newFolderBtn_);
    mid->addWidget(deleteBtn_);
    mid->addStretch(1);

    auto *panes = new QHBoxLayout;
    panes->setSpacing(theme::SpaceGroup);
    panes->addLayout(paneColumn(localHeading_, localPath_, localUp_, localView_), 5);
    panes->addLayout(mid, 0);
    panes->addLayout(paneColumn(remoteHeading_, remotePath_, remoteUp_, remoteView_), 5);

    auto *progressRow = new QHBoxLayout;
    progressRow->addWidget(status_, 1);
    progressRow->addWidget(reconnectBtn_);
    progressRow->addWidget(progress_, 1);
    progressRow->addWidget(cancelBtn_);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(theme::SpaceGroup, theme::SpaceGroup,
                             theme::SpaceGroup, theme::SpaceRow);
    root->setSpacing(theme::SpaceRow);
    root->addLayout(panes, 1);
    root->addLayout(progressRow);

    /* ---- the worker on its own thread ------------------------------- */
    thread_ = new QThread(this);
    worker_ = new halyard::FtWorker;
    worker_->moveToThread(thread_);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);

    using W = halyard::FtWorker;
    connect(worker_, &W::connected, this, &FileManagerWindow::onConnected, Qt::QueuedConnection);
    connect(worker_, &W::listed, this, &FileManagerWindow::onListed, Qt::QueuedConnection);
    connect(worker_, &W::transferStarted, this, &FileManagerWindow::onTransferStarted, Qt::QueuedConnection);
    connect(worker_, &W::progress, this, &FileManagerWindow::onProgress, Qt::QueuedConnection);
    connect(worker_, &W::transferDone, this, &FileManagerWindow::onTransferDone, Qt::QueuedConnection);
    connect(worker_, &W::actionDone, this, &FileManagerWindow::onActionDone, Qt::QueuedConnection);
    connect(worker_, &W::failed, this, &FileManagerWindow::onFailed, Qt::QueuedConnection);

    connect(sendBtn_, &QPushButton::clicked, this, &FileManagerWindow::sendToVm);
    connect(recvBtn_, &QPushButton::clicked, this, &FileManagerWindow::receiveFromVm);
    connect(refreshBtn_, &QToolButton::clicked, this, &FileManagerWindow::refreshRemote);
    connect(newFolderBtn_, &QToolButton::clicked, this, &FileManagerWindow::newRemoteFolder);
    connect(deleteBtn_, &QToolButton::clicked, this, &FileManagerWindow::deleteRemote);
    connect(remoteUp_, &QToolButton::clicked, this, &FileManagerWindow::remoteUp);
    connect(localUp_, &QToolButton::clicked, this, &FileManagerWindow::localUp);
    connect(remoteView_, &QTreeWidget::itemActivated, this, &FileManagerWindow::remoteActivated);
    connect(localView_, &QTreeView::activated, this, &FileManagerWindow::localActivated);
    connect(localPath_, &QLineEdit::returnPressed, this,
            [this] { setLocalDir(localPath_->text()); });
    connect(cancelBtn_, &QPushButton::clicked, this,
            [this] { worker_->requestCancel(); cancelBtn_->setEnabled(false); });
    connect(reconnectBtn_, &QPushButton::clicked, this, [this] {
        reconnectBtn_->setVisible(false);
        status_->setText(tr("Connecting to the VM..."));
        QMetaObject::invokeMethod(worker_, "connectToVm", Qt::QueuedConnection);
    });

    thread_->start();

    /* Downloads to start, mirroring where the VM's SFTP root (its own Downloads)
     * is - the two then line up for the common case of moving a file across. */
    setLocalDir(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    retranslate();
    setBusy(false);
    remoteReady_ = false;
    status_->setText(tr("Connecting to the VM..."));
    QMetaObject::invokeMethod(worker_, "connectToVm", Qt::QueuedConnection);
}

FileManagerWindow::~FileManagerWindow()
{
    worker_->requestCancel();
    QMetaObject::invokeMethod(worker_, "disconnectFromVm", Qt::QueuedConnection);
    thread_->quit();
    thread_->wait(5000);
}

void FileManagerWindow::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::LanguageChange) retranslate();
    QWidget::changeEvent(e);
}

void FileManagerWindow::retranslate()
{
    setWindowTitle(tr("Halyard file transfer"));
    localHeading_->setText(tr("This PC"));
    remoteHeading_->setText(tr("The VM"));
    sendBtn_->setText(tr("Send  →"));
    reconnectBtn_->setText(tr("Reconnect"));
    recvBtn_->setText(tr("←  Receive"));
    refreshBtn_->setText(tr("Refresh"));
    newFolderBtn_->setText(tr("New folder"));
    deleteBtn_->setText(tr("Delete"));
    cancelBtn_->setText(tr("Cancel"));
    remoteView_->setHeaderLabels({ tr("Name"), tr("Size"), tr("Type") });
    localUp_->setToolTip(tr("Parent folder"));
    remoteUp_->setToolTip(tr("Parent folder"));
}

/* ====================================================== local pane ===== */

QString FileManagerWindow::localDir() const
{
    return localModel_->filePath(localView_->rootIndex());
}

void FileManagerWindow::setLocalDir(const QString &dir)
{
    const QModelIndex idx = localModel_->index(dir);
    if (!idx.isValid()) return;
    localView_->setRootIndex(idx);
    localPath_->setText(QDir::toNativeSeparators(dir));
}

void FileManagerWindow::localUp()
{
    QDir d(localDir());
    if (d.cdUp()) setLocalDir(d.absolutePath());
}

void FileManagerWindow::localActivated(const QModelIndex &index)
{
    if (localModel_->isDir(index)) setLocalDir(localModel_->filePath(index));
}

/* ===================================================== remote pane ===== */

QString FileManagerWindow::remoteJoin(const QString &name) const
{
    if (remoteDir_.isEmpty()) return name;
    if (remoteDir_.endsWith(QLatin1Char('/'))) return remoteDir_ + name;
    return remoteDir_ + QLatin1Char('/') + name;
}

void FileManagerWindow::refreshRemote()
{
    if (!remoteReady_) return;
    QMetaObject::invokeMethod(worker_, "listRemote", Qt::QueuedConnection,
                              Q_ARG(QString, remoteDir_));
}

void FileManagerWindow::remoteUp()
{
    if (remoteDir_.isEmpty()) return;
    const int cut = remoteDir_.lastIndexOf(QLatin1Char('/'));
    remoteDir_ = cut > 0 ? remoteDir_.left(cut) : QString();
    refreshRemote();
}

void FileManagerWindow::remoteActivated(QTreeWidgetItem *item, int)
{
    if (!item) return;
    if (item->data(0, RoleIsDir).toBool()) {
        remoteDir_ = remoteJoin(item->text(0));
        refreshRemote();
    }
}

/* ======================================================= transfers ===== */

void FileManagerWindow::sendToVm()
{
    if (busy_ || !remoteReady_) return;
    const QModelIndexList sel = localView_->selectionModel()->selectedRows(0);
    QStringList files;
    for (const QModelIndex &i : sel)
        if (!localModel_->isDir(i)) files << localModel_->filePath(i);
    if (files.isEmpty()) {
        status_->setText(tr("Pick one or more files on the left. Folders are not "
                            "sent yet - only files."));
        return;
    }
    /* One queued call per file; the worker runs them in order on its thread and
     * re-lists the remote dir after each, so the view fills as they land. */
    for (const QString &f : files)
        QMetaObject::invokeMethod(worker_, "upload", Qt::QueuedConnection,
                                  Q_ARG(QString, f), Q_ARG(QString, remoteDir_));
}

void FileManagerWindow::receiveFromVm()
{
    if (busy_ || !remoteReady_) return;
    /* selectedItems() already returns one entry per selected ROW for a tree
     * widget, so no dedupe is needed; keep only the files. */
    QList<QTreeWidgetItem *> files;
    for (QTreeWidgetItem *it : remoteView_->selectedItems())
        if (!it->data(0, RoleIsDir).toBool()) files << it;

    if (files.isEmpty()) {
        status_->setText(tr("Pick one or more files on the right. To enter a "
                            "folder, double-click it."));
        return;
    }
    const QString dir = localDir();
    for (QTreeWidgetItem *it : files)
        QMetaObject::invokeMethod(worker_, "download", Qt::QueuedConnection,
                                  Q_ARG(QString, remoteJoin(it->text(0))),
                                  Q_ARG(QString, dir),
                                  Q_ARG(quint64, it->data(0, RoleSize).toULongLong()));
}

void FileManagerWindow::newRemoteFolder()
{
    if (!remoteReady_) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New folder"),
        tr("Folder name:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    QMetaObject::invokeMethod(worker_, "makeDir", Qt::QueuedConnection,
                              Q_ARG(QString, remoteDir_), Q_ARG(QString, name.trimmed()));
}

void FileManagerWindow::deleteRemote()
{
    if (!remoteReady_) return;
    QList<QTreeWidgetItem *> rows;
    for (QTreeWidgetItem *it : remoteView_->selectedItems())
        if (!rows.contains(it)) rows << it;
    if (rows.isEmpty()) return;

    const QString what = rows.size() == 1 ? rows.first()->text(0)
                                          : tr("%1 items").arg(rows.size());
    if (QMessageBox::question(this, tr("Delete on the VM"),
            tr("Delete %1 from the VM? This cannot be undone.").arg(what))
        != QMessageBox::Yes) return;

    for (QTreeWidgetItem *it : rows)
        QMetaObject::invokeMethod(worker_, "removeEntry", Qt::QueuedConnection,
                                  Q_ARG(QString, remoteJoin(it->text(0))),
                                  Q_ARG(bool, it->data(0, RoleIsDir).toBool()),
                                  Q_ARG(QString, remoteDir_));
}

/* ========================================================= signals ===== */

void FileManagerWindow::onConnected(bool ok, const QString &message)
{
    remoteReady_ = ok;
    status_->setText(message);
    /* Offer a retry when the channel is not up yet (opened during the
     * bootstrap, before the announcements) or dropped mid-session. */
    reconnectBtn_->setVisible(!ok);
    sendBtn_->setEnabled(ok);
    recvBtn_->setEnabled(ok);
    refreshBtn_->setEnabled(ok);
    newFolderBtn_->setEnabled(ok);
    deleteBtn_->setEnabled(ok);
    remoteUp_->setEnabled(ok);
    if (ok) {
        remoteDir_.clear();
        refreshRemote();
    }
}

void FileManagerWindow::onListed(const QString &dir,
                                 const QList<halyard::FtEntry> &entries,
                                 bool truncated)
{
    remoteDir_ = dir;
    remotePath_->setText(dir.isEmpty() ? QStringLiteral("/ (Downloads)")
                                       : QStringLiteral("/") + dir);
    remoteView_->setSortingEnabled(false);
    remoteView_->clear();
    for (const halyard::FtEntry &e : entries) {
        auto *it = new QTreeWidgetItem(remoteView_);
        it->setText(0, e.name);
        it->setText(1, e.isDir ? QString() : humanSize(e.size));
        it->setText(2, e.isDir ? tr("Folder") : tr("File"));
        it->setData(0, RoleIsDir, e.isDir);
        if (!e.isDir) it->setData(0, RoleSize, e.size);
        /* Folders sort before files: column-1 size sort alone would scatter
         * them, so a leading key keeps dirs grouped at the top. */
        it->setData(0, Qt::InitialSortOrderRole, e.isDir ? 0 : 1);
    }
    remoteView_->setSortingEnabled(true);
    QString s = tr("%n item(s).", "", entries.size());
    if (truncated) s += QLatin1Char(' ') +
        tr("Listing clipped - the folder has more than shown.");
    status_->setText(s);
}

void FileManagerWindow::onTransferStarted(const QString &what)
{
    setBusy(true);
    progress_->setRange(0, 0);     /* indeterminate until the first progress */
    progress_->setFormat(what + QStringLiteral("  %p%"));
    status_->setText(what);
}

void FileManagerWindow::onProgress(qint64 done, qint64 total)
{
    if (total > 0) {
        progress_->setRange(0, 100);
        progress_->setValue((int)(done * 100 / total));
    } else {
        progress_->setRange(0, 0);
    }
}

void FileManagerWindow::onTransferDone(bool ok, const QString &message)
{
    setBusy(false);
    status_->setText(message);
    Q_UNUSED(ok);
}

void FileManagerWindow::onActionDone(bool ok, const QString &message)
{
    status_->setText(message);
    Q_UNUSED(ok);
}

void FileManagerWindow::onFailed(const QString &message)
{
    status_->setText(message);
}

/* ============================================================ state ===== */

void FileManagerWindow::setBusy(bool busy)
{
    busy_ = busy;
    progress_->setVisible(busy);
    cancelBtn_->setVisible(busy);
    cancelBtn_->setEnabled(busy);
    /* A second transfer while one runs would be queued behind it on the worker,
     * which is fine, but the two would share one progress bar and read as one -
     * so the buttons wait. Navigation stays live. */
    sendBtn_->setEnabled(!busy && remoteReady_);
    recvBtn_->setEnabled(!busy && remoteReady_);
    deleteBtn_->setEnabled(!busy && remoteReady_);
    newFolderBtn_->setEnabled(!busy && remoteReady_);
}
