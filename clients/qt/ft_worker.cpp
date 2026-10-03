/* FtWorker - see ft_worker.hpp for the threading and credential contracts. */
#include "ft_worker.hpp"

#include <QFileInfo>
#include <QDir>

#include <cstring>
#include <vector>

extern "C" {
#include "core/protocol/filetransfer.h"
#include "core/protocol/session_caps.h"
}

namespace halyard {

FtWorker::FtWorker(QObject *parent) : QObject(parent) {}

FtWorker::~FtWorker()
{
    if (ft_) shadow_ft_close(ft_);
}

bool FtWorker::progressTrampoline(quint64 done, quint64 total, void *user)
{
    return static_cast<FtWorker *>(user)->onProgress(done, total);
}

bool FtWorker::onProgress(quint64 done, quint64 total)
{
    emit progress((qint64)done, (qint64)total);
    /* false aborts the transfer; core then returns SHADOW_FT_CANCELLED and
     * removes the partial file at both ends. */
    return !cancel_.load(std::memory_order_relaxed);
}

void FtWorker::connectToVm()
{
    if (ft_) { shadow_ft_close(ft_); ft_ = nullptr; }

    char host[256];
    uint16_t port = 0;
    if (!ctrl_session_file_transfer_endpoint(host, sizeof host, &port)) {
        emit connected(false,
            tr("No file-transfer channel on this session. Connect to a machine "
               "first - the channel is granted during the stream's bootstrap."));
        return;
    }

    /* The secret on the stack for exactly this call, then wiped. */
    char secret[1024];
    size_t sn = 0;
    if (!ctrl_session_file_transfer_secret(secret, sizeof secret, &sn) || sn == 0) {
        emit connected(false, tr("The session granted no file-transfer credential."));
        return;
    }

    const shadow_ft_err e = shadow_ft_open(&ft_, host, port, secret, sn);
    for (size_t z = 0; z < sizeof secret; z++)
        ((volatile char *)secret)[z] = 0;

    if (e != SHADOW_FT_OK) {
        ft_ = nullptr;
        if (e == SHADOW_FT_UNSUPPORTED)
            emit connected(false,
                tr("This build has no file-transfer support. Rebuild with "
                   "-DSHADOW_FILETRANSFER=ON (it needs libssh)."));
        else
            emit connected(false, tr("Could not open the channel: %1")
                                      .arg(QString::fromUtf8(shadow_ft_strerror(e))));
        return;
    }
    emit connected(true, tr("Connected to the VM over SFTP on port %1.").arg(port));
}

void FtWorker::disconnectFromVm()
{
    if (ft_) { shadow_ft_close(ft_); ft_ = nullptr; }
}

void FtWorker::listRemote(const QString &dir)
{
    if (!ft_) { emit failed(tr("Not connected.")); return; }

    /* One page of a generous size; the server's own listings are small, and
     * `n_total` tells us if we clipped it rather than guessing. */
    std::vector<shadow_ft_entry> buf(4096);
    size_t total = 0;
    const QByteArray d = dir.toUtf8();
    const shadow_ft_err e = shadow_ft_list(ft_, d.constData(), buf.data(),
                                           buf.size(), &total);
    if (e != SHADOW_FT_OK) {
        emit failed(tr("Could not list %1: %2").arg(
            dir.isEmpty() ? QStringLiteral(".") : dir,
            QString::fromUtf8(shadow_ft_strerror(e))));
        return;
    }
    const size_t shown = qMin(total, buf.size());
    QList<FtEntry> out;
    out.reserve((int)shown);
    for (size_t i = 0; i < shown; i++) {
        const char *nm = buf[i].name;
        if (std::strcmp(nm, ".") == 0 || std::strcmp(nm, "..") == 0) continue;
        out.append(FtEntry{ QString::fromUtf8(nm), buf[i].size, buf[i].is_dir });
    }
    emit listed(dir, out, total > buf.size());
}

void FtWorker::upload(const QString &localPath, const QString &remoteDir)
{
    if (!ft_) { emit transferDone(false, tr("Not connected.")); return; }
    cancel_.store(false, std::memory_order_relaxed);

    const QString base = QFileInfo(localPath).fileName();
    QString remote = remoteDir;
    if (!remote.isEmpty() && !remote.endsWith(QLatin1Char('/')))
        remote += QLatin1Char('/');
    remote += base;

    emit transferStarted(tr("Sending %1").arg(base));
    const QByteArray lp = localPath.toUtf8();
    const QByteArray rp = remote.toUtf8();
    const shadow_ft_err e = shadow_ft_put(ft_, lp.constData(), rp.constData(),
                                          progressTrampoline, this);
    if (e == SHADOW_FT_OK)        emit transferDone(true, tr("Sent %1").arg(base));
    else if (e == SHADOW_FT_CANCELLED) emit transferDone(false, tr("Cancelled."));
    else emit transferDone(false, tr("Send failed: %1")
                                      .arg(QString::fromUtf8(shadow_ft_strerror(e))));
    listRemote(remoteDir);
}

void FtWorker::download(const QString &remotePath, const QString &localDir,
                        quint64 knownSize)
{
    (void)knownSize;
    if (!ft_) { emit transferDone(false, tr("Not connected.")); return; }
    cancel_.store(false, std::memory_order_relaxed);

    const QString base = remotePath.section(QLatin1Char('/'), -1);
    const QString local = QDir(localDir).filePath(base);

    emit transferStarted(tr("Receiving %1").arg(base));
    const QByteArray rp = remotePath.toUtf8();
    const QByteArray lp = local.toUtf8();
    const shadow_ft_err e = shadow_ft_get(ft_, rp.constData(), lp.constData(),
                                          progressTrampoline, this);
    if (e == SHADOW_FT_OK)        emit transferDone(true, tr("Received %1").arg(base));
    else if (e == SHADOW_FT_CANCELLED) emit transferDone(false, tr("Cancelled."));
    else emit transferDone(false, tr("Receive failed: %1")
                                      .arg(QString::fromUtf8(shadow_ft_strerror(e))));
}

void FtWorker::makeDir(const QString &remoteParent, const QString &name)
{
    if (!ft_) { emit actionDone(false, tr("Not connected.")); return; }
    QString path = remoteParent;
    if (!path.isEmpty() && !path.endsWith(QLatin1Char('/'))) path += QLatin1Char('/');
    path += name;
    const QByteArray p = path.toUtf8();
    const shadow_ft_err e = shadow_ft_mkdir(ft_, p.constData());
    emit actionDone(e == SHADOW_FT_OK,
                    e == SHADOW_FT_OK ? tr("Created %1").arg(name)
                                      : tr("Could not create the folder: %1")
                                            .arg(QString::fromUtf8(shadow_ft_strerror(e))));
    listRemote(remoteParent);
}

void FtWorker::removeEntry(const QString &remotePath, bool isDir,
                           const QString &listAfter)
{
    if (!ft_) { emit actionDone(false, tr("Not connected.")); return; }
    const QByteArray p = remotePath.toUtf8();
    const shadow_ft_err e = isDir ? shadow_ft_rmdir(ft_, p.constData())
                                  : shadow_ft_remove(ft_, p.constData());
    emit actionDone(e == SHADOW_FT_OK,
                    e == SHADOW_FT_OK ? tr("Deleted.")
                                      : tr("Could not delete: %1")
                                            .arg(QString::fromUtf8(shadow_ft_strerror(e))));
    listRemote(listAfter);
}

void FtWorker::rename(const QString &fromPath, const QString &toPath,
                      const QString &listAfter)
{
    if (!ft_) { emit actionDone(false, tr("Not connected.")); return; }
    const QByteArray f = fromPath.toUtf8(), t = toPath.toUtf8();
    const shadow_ft_err e = shadow_ft_rename(ft_, f.constData(), t.constData());
    emit actionDone(e == SHADOW_FT_OK,
                    e == SHADOW_FT_OK ? tr("Renamed.")
                                      : tr("Could not rename: %1")
                                            .arg(QString::fromUtf8(shadow_ft_strerror(e))));
    listRemote(listAfter);
}

}  // namespace halyard
