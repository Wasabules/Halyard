/* FileManagerWindow - see the header for the shape and the threading. */
#include "file_manager_window.hpp"

#include "theme.hpp"

#include <QCheckBox>
#include <QDir>
#include <QEvent>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabWidget>
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
    remoteView_->setContextMenuPolicy(Qt::CustomContextMenu);
    localView_->setContextMenuPolicy(Qt::CustomContextMenu);
    wholeFs_ = new QCheckBox(this);   /* FM2 - browse the whole VM filesystem */

    /* ---- the buttons between the panes ------------------------------- */
    sendBtn_ = new QPushButton(this);
    recvBtn_ = new QPushButton(this);
    refreshBtn_ = new QToolButton(this);
    newFolderBtn_ = new QToolButton(this);
    deleteBtn_ = new QToolButton(this);

    /* ---- the transfer panel (FM3) ----------------------------------- */
    cancelBtn_ = new QPushButton(this);
    cancelBtn_->setVisible(false);
    reconnectBtn_ = new QPushButton(this);
    reconnectBtn_->setVisible(false);
    status_ = new QLabel(this);
    status_->setStyleSheet(theme::css(theme::muted(this)));

    auto makeTxTab = [this] {
        auto *t = new QTreeWidget(this);
        t->setColumnCount(4);
        t->setRootIsDecorated(false);
        t->setUniformRowHeights(true);
        t->setSelectionMode(QAbstractItemView::NoSelection);
        t->setFocusPolicy(Qt::NoFocus);
        return t;
    };
    txActive_ = makeTxTab();
    txFailed_ = makeTxTab();
    txDone_   = makeTxTab();
    transfers_ = new QTabWidget(this);
    transfers_->addTab(txActive_, QString());
    transfers_->addTab(txFailed_, QString());
    transfers_->addTab(txDone_, QString());
    transfers_->setMinimumHeight(150);

    localHeading_ = new QLabel(this);
    localHeading_->setFont(theme::headingFont(this));
    remoteHeading_ = new QLabel(this);
    remoteHeading_->setFont(theme::headingFont(this));

    /* ---- layout ------------------------------------------------------ */
    auto paneColumn = [&](QLabel *head, QLineEdit *path, QToolButton *up,
                          QWidget *view, QWidget *extra) {
        auto *col = new QVBoxLayout;
        col->setSpacing(theme::SpaceTight);
        col->addWidget(head);
        auto *bar = new QHBoxLayout;
        bar->addWidget(path, 1);
        bar->addWidget(up);
        col->addLayout(bar);
        col->addWidget(view, 1);
        if (extra) col->addWidget(extra);
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
    panes->addLayout(paneColumn(localHeading_, localPath_, localUp_, localView_, nullptr), 5);
    panes->addLayout(mid, 0);
    panes->addLayout(paneColumn(remoteHeading_, remotePath_, remoteUp_, remoteView_, wholeFs_), 5);

    auto *statusRow = new QHBoxLayout;
    statusRow->addWidget(status_, 1);
    statusRow->addWidget(reconnectBtn_);
    statusRow->addWidget(cancelBtn_);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(theme::SpaceGroup, theme::SpaceGroup,
                             theme::SpaceGroup, theme::SpaceRow);
    root->setSpacing(theme::SpaceRow);
    root->addLayout(panes, 3);
    root->addLayout(statusRow);
    root->addWidget(transfers_, 1);

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
    connect(remoteView_, &QWidget::customContextMenuRequested, this,
            &FileManagerWindow::remoteContextMenu);
    connect(localView_, &QWidget::customContextMenuRequested, this,
            &FileManagerWindow::localContextMenu);
    connect(wholeFs_, &QCheckBox::toggled, this, &FileManagerWindow::toggleWholeFs);
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
    wholeFs_->setText(tr("Browse the whole VM filesystem"));
    wholeFs_->setToolTip(tr("Off, the VM side is confined to its Downloads "
                            "folder. On, the entire filesystem - the credential "
                            "already grants full read/write access to it."));

    const QStringList txCols = { tr("Name"), tr("Direction"), tr("Progress"),
                                 tr("Status") };
    txActive_->setHeaderLabels(txCols);
    txFailed_->setHeaderLabels(txCols);
    txDone_->setHeaderLabels(txCols);
    transfers_->setTabText(0, tr("In progress"));
    transfers_->setTabText(1, tr("Failed"));
    transfers_->setTabText(2, tr("Done"));
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
    if (remoteDir_ == remoteFloor()) return;      /* already at the top */
    const int cut = remoteDir_.lastIndexOf(QLatin1Char('/'));
    remoteDir_ = cut > 0 ? remoteDir_.left(cut) : remoteFloor();
    refreshRemote();
}

void FileManagerWindow::toggleWholeFs(bool on)
{
    if (!remoteReady_) return;
    absolute_ = on;
    /* Jump to the right root for the new mode: "/" for the whole FS, the
     * Downloads root ("") when confined again. The worker sets the mode in core
     * and re-lists in one call, so the list and the confinement never disagree. */
    const QString target = on ? QStringLiteral("/") : QString();
    QMetaObject::invokeMethod(worker_, "setAllowAbsolute", Qt::QueuedConnection,
                              Q_ARG(bool, on), Q_ARG(QString, target));
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

int FileManagerWindow::enqueueRow(const QString &name, bool upload)
{
    const int id = nextId_++;
    auto *it = new QTreeWidgetItem(txActive_);
    it->setText(0, name);
    it->setText(1, upload ? tr("→ VM") : tr("← PC"));
    it->setText(3, tr("Queued"));
    it->setData(0, Qt::UserRole, id);
    /* FM6 - a real bar in the Progress column, as a cell widget. It is owned by
     * the item, so removing the row removes the bar with it. */
    auto *bar = new QProgressBar(txActive_);
    bar->setRange(0, 100);
    bar->setValue(0);
    bar->setTextVisible(true);
    bar->setFormat(QStringLiteral("%p %"));
    bar->setMaximumHeight(16);
    txActive_->setItemWidget(it, 2, bar);
    txItems_.insert(id, it);
    transfers_->setCurrentWidget(txActive_);
    return id;
}

void FileManagerWindow::sendToVm()
{
    if (!remoteReady_) return;
    const QModelIndexList sel = localView_->selectionModel()->selectedRows(0);
    QStringList files;
    for (const QModelIndex &i : sel)
        if (!localModel_->isDir(i)) files << localModel_->filePath(i);
    if (files.isEmpty()) {
        status_->setText(tr("Pick one or more files on the left. Folders are not "
                            "sent yet - only files."));
        return;
    }
    /* One queued call per file, each with its own id so the panel can follow it.
     * The worker runs them in order and re-lists the remote dir after each. */
    for (const QString &f : files) {
        const int id = enqueueRow(QFileInfo(f).fileName(), true);
        QMetaObject::invokeMethod(worker_, "upload", Qt::QueuedConnection,
                                  Q_ARG(int, id), Q_ARG(QString, f),
                                  Q_ARG(QString, remoteDir_));
    }
}

void FileManagerWindow::receiveFromVm()
{
    if (!remoteReady_) return;
    QList<QTreeWidgetItem *> files;
    for (QTreeWidgetItem *it : remoteView_->selectedItems())
        if (!it->data(0, RoleIsDir).toBool()) files << it;

    if (files.isEmpty()) {
        status_->setText(tr("Pick one or more files on the right. To enter a "
                            "folder, double-click it."));
        return;
    }
    const QString dir = localDir();
    for (QTreeWidgetItem *it : files) {
        const int id = enqueueRow(it->text(0), false);
        QMetaObject::invokeMethod(worker_, "download", Qt::QueuedConnection,
                                  Q_ARG(int, id),
                                  Q_ARG(QString, remoteJoin(it->text(0))),
                                  Q_ARG(QString, dir),
                                  Q_ARG(quint64, it->data(0, RoleSize).toULongLong()));
    }
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
    const QList<QTreeWidgetItem *> rows = remoteView_->selectedItems();
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

void FileManagerWindow::renameRemote()
{
    if (!remoteReady_) return;
    QTreeWidgetItem *it = remoteView_->currentItem();
    if (!it) return;
    bool ok = false;
    const QString to = QInputDialog::getText(this, tr("Rename"),
        tr("New name:"), QLineEdit::Normal, it->text(0), &ok);
    if (!ok || to.trimmed().isEmpty() || to == it->text(0)) return;
    QString dst = remoteDir_;
    if (!dst.isEmpty() && !dst.endsWith(QLatin1Char('/'))) dst += QLatin1Char('/');
    dst += to.trimmed();
    QMetaObject::invokeMethod(worker_, "rename", Qt::QueuedConnection,
                              Q_ARG(QString, remoteJoin(it->text(0))),
                              Q_ARG(QString, dst), Q_ARG(QString, remoteDir_));
}

/* === FM2 - the right-click menus ========================================= */

void FileManagerWindow::remoteContextMenu(const QPoint &pos)
{
    if (!remoteReady_) return;
    QTreeWidgetItem *it = remoteView_->itemAt(pos);
    QMenu menu(this);
    if (it) {
        const bool isDir = it->data(0, RoleIsDir).toBool();
        if (isDir)
            menu.addAction(tr("Open"), this, [this, it] { remoteActivated(it, 0); });
        else
            menu.addAction(tr("Receive"), this, &FileManagerWindow::receiveFromVm);
        menu.addAction(tr("Rename..."), this, &FileManagerWindow::renameRemote);
        menu.addAction(tr("Delete"), this, &FileManagerWindow::deleteRemote);
        menu.addSeparator();
    }
    menu.addAction(tr("New folder..."), this, &FileManagerWindow::newRemoteFolder);
    menu.addAction(tr("Refresh"), this, &FileManagerWindow::refreshRemote);
    if (!busy_) menu.exec(remoteView_->viewport()->mapToGlobal(pos));
}

void FileManagerWindow::localContextMenu(const QPoint &pos)
{
    const QModelIndex idx = localView_->indexAt(pos);
    QMenu menu(this);
    if (idx.isValid() && !localModel_->isDir(idx))
        menu.addAction(tr("Send to the VM"), this, &FileManagerWindow::sendToVm);
    if (idx.isValid() && localModel_->isDir(idx))
        menu.addAction(tr("Open"), this,
                       [this, idx] { setLocalDir(localModel_->filePath(idx)); });
    menu.addSeparator();
    menu.addAction(tr("Refresh"), this, [this] { setLocalDir(localDir()); });
    menu.exec(localView_->viewport()->mapToGlobal(pos));
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
    wholeFs_->setEnabled(ok);
    if (ok) {
        /* A reconnect starts confined again; the checkbox and core's mode must
         * agree, so reset both rather than trust a stale tick. */
        absolute_ = false;
        const QSignalBlocker b(wholeFs_);
        wholeFs_->setChecked(false);
        remoteDir_.clear();
        refreshRemote();
    }
}

void FileManagerWindow::onListed(const QString &dir,
                                 const QList<halyard::FtEntry> &entries,
                                 bool truncated)
{
    remoteDir_ = dir;
    /* Confined: "" is the Downloads root. Whole-FS: paths are already absolute
     * (they start with "/"), so show them as-is and never double the slash. */
    QString shown;
    if (absolute_) shown = dir.isEmpty() ? QStringLiteral("/") : dir;
    else shown = dir.isEmpty() ? tr("/ (Downloads)") : QLatin1Char('/') + dir;
    remotePath_->setText(shown);
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

void FileManagerWindow::onTransferStarted(int id, const QString &name, bool upload)
{
    setBusy(true);
    QTreeWidgetItem *it = txItems_.value(id, nullptr);
    if (!it) { id = enqueueRow(name, upload); it = txItems_.value(id); }
    if (it) {
        it->setText(3, tr("In progress"));
        if (auto *bar = qobject_cast<QProgressBar *>(txActive_->itemWidget(it, 2)))
            bar->setValue(0);
    }
    status_->setText(upload ? tr("Sending %1").arg(name)
                            : tr("Receiving %1").arg(name));
}

void FileManagerWindow::onProgress(int id, qint64 done, qint64 total)
{
    QTreeWidgetItem *it = txItems_.value(id, nullptr);
    if (!it) return;
    auto *bar = qobject_cast<QProgressBar *>(txActive_->itemWidget(it, 2));
    if (!bar) return;
    if (total > 0) {
        bar->setRange(0, 100);
        bar->setValue((int)(done * 100 / total));
        bar->setFormat(QStringLiteral("%1 / %2  (%p %)")
                           .arg(humanSize((quint64)done), humanSize((quint64)total)));
    } else {
        /* Size unknown: a busy bar rather than a figure that would be a guess. */
        bar->setRange(0, 0);
        bar->setFormat(humanSize((quint64)done));
    }
}

void FileManagerWindow::onTransferDone(int id, bool ok, const QString &message)
{
    QTreeWidgetItem *it = txItems_.take(id);
    /* Move the row to its outcome tab, with the detail on it - this IS the error
     * report the user asked for, kept in place instead of a one-shot dialog. */
    if (it) {
        /* Capture the texts BEFORE removing the row: takeTopLevelItem returns
         * `it` itself, and deleting it then reading it->text() is a
         * use-after-free - which crashed the client on the first failed put. */
        const QString name = it->text(0);
        const QString dir  = it->text(1);
        QString prog;
        if (auto *bar = qobject_cast<QProgressBar *>(txActive_->itemWidget(it, 2)))
            prog = bar->maximum() > 0 ? QStringLiteral("%1 %").arg(bar->value())
                                      : QString();
        const int idx = txActive_->indexOfTopLevelItem(it);
        if (idx >= 0) delete txActive_->takeTopLevelItem(idx);

        auto *dst = ok ? txDone_ : txFailed_;
        auto *row = new QTreeWidgetItem(dst);
        row->setText(0, name);
        row->setText(1, dir);
        row->setText(2, ok ? QStringLiteral("100 %") : prog);
        row->setText(3, message);
        transfers_->setCurrentWidget(ok ? txDone_ : txFailed_);
    }
    /* busy only while something is still active. */
    setBusy(txActive_->topLevelItemCount() > 0);
    status_->setText(ok ? message
                        : tr("Transfer failed — see the Failed tab for why."));
}

void FileManagerWindow::onActionDone(bool ok, const QString &message)
{
    status_->setText(message);
}

void FileManagerWindow::onFailed(const QString &message)
{
    status_->setText(message);
}

/* ============================================================ state ===== */

void FileManagerWindow::setBusy(bool busy)
{
    busy_ = busy;
    /* Cancel acts on the transfer in flight; the rest stays live, because more
     * transfers just queue behind it and the panel follows each one. */
    cancelBtn_->setVisible(busy);
    cancelBtn_->setEnabled(busy);
}
