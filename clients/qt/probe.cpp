/* probe.cpp - see probe.hpp for why a GUI client has a headless mode. */
#include "probe.hpp"

#include "core_scope.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QTextStream>

extern "C" {
#include "core/services/config.h"
}

using halyard::str;

namespace halyard::probe {

namespace {

/* The sign-in, silent. Only the refresh-token path: a device grant wants a
 * browser and a person, which is the opposite of what a probe is for. Fails
 * plainly when there is no saved session rather than hanging on a poll. */
bool signInSilently(ScopedDiscovery &disc, ScopedAuthState &auth,
                    QString &launcherUrl, QString &why)
{
    long http = 0;

    ScopedGapInfo gap;
    if (!tinag_get_datacenter("test@example.com", gap.out(), &http)) {
        why = QStringLiteral("the data centre could not be resolved (HTTP %1)")
                  .arg(http);
        return false;
    }
    launcherUrl = str(gap->launcher_api_url);
    if (launcherUrl.isEmpty()) {
        why = QStringLiteral("the data centre gave no launcher URL");
        return false;
    }

    if (!oauth_discover(disc.out(), &http)) {
        why = QStringLiteral("OIDC discovery failed (HTTP %1)").arg(http);
        return false;
    }

    char *saved = nullptr;
    if (!oauth_load_refresh(&saved) || !saved || !*saved) {
        free(saved);
        why = QStringLiteral("no saved session - sign in with the window once "
                             "first");
        return false;
    }
    auth.get().refresh_token = saved;   /* oauth_state_free owns it now */
    if (!oauth_refresh(disc.out(), SHADOW_OAUTH_CLIENT_ID, auth.out())
        || !auth->access_token) {
        why = QStringLiteral("the saved session is no longer valid");
        return false;
    }

    /* === PROBE1 2026-10-03 - SAVE IT BACK, OR THE PROBE LOGS YOU OUT =======
     *
     * The refresh token ROTATES: `oauth_refresh` comes back with a new one
     * and the old one is dead the moment the server issues it. The first
     * version of this function did not persist it, so running the probe once
     * worked, consumed the stored token and left nothing in its place - and
     * the very next run, and the application itself, reported "the saved
     * session is no longer valid" and demanded a browser again.
     *
     * Found by running the probe twice. `AuthWorker::signIn` has always done
     * this; a second sign-in path had to learn the same thing, which is the
     * argument for the two eventually sharing one. */
    (void)oauth_save_refresh(auth.out());
    return true;
}

/* A body as JSON when it parses, as a string when it does not. Keeping the
 * unparseable form rather than dropping it: a body that is not JSON is itself
 * the finding, and a report that silently showed `null` would hide it. */
QJsonValue bodyValue(const char *raw)
{
    if (!raw || !*raw) return QJsonValue::Null;
    QJsonParseError err{};
    const QJsonDocument d = QJsonDocument::fromJson(QByteArray(raw), &err);
    if (err.error != QJsonParseError::NoError)
        return QJsonValue(QString::fromUtf8(raw));
    return d.isArray() ? QJsonValue(d.array()) : QJsonValue(d.object());
}

int write(const QJsonObject &report, const QString &outPath)
{
    const QByteArray text = QJsonDocument(report).toJson(QJsonDocument::Indented);
    if (outPath.isEmpty() || outPath == QStringLiteral("-")) {
        QTextStream(stdout) << QString::fromUtf8(text);
        return 0;
    }
    QFile f(outPath);
    if (!f.open(QIODevice::WriteOnly)) {
        QTextStream(stderr) << QStringLiteral("cannot write %1: %2\n")
                                   .arg(outPath, f.errorString());
        return 3;
    }
    f.write(text);
    f.close();
    QTextStream(stderr) << QStringLiteral("probe written to %1\n").arg(outPath);
    return 0;
}

}  // namespace

int run(const QString &what, const QString &vmId, const QString &outPath)
{
    QJsonObject report;
    report[QStringLiteral("probe")] = what;

    ScopedDiscovery disc;
    ScopedAuthState auth;
    QString launcherUrl, why;
    if (!signInSilently(disc, auth, launcherUrl, why)) {
        report[QStringLiteral("ok")] = false;
        report[QStringLiteral("error")] = why;
        write(report, outPath);
        return 1;
    }
    report[QStringLiteral("launcher")] = launcherUrl;

    const QByteArray base = launcherUrl.toUtf8();
    const QByteArray tok = QByteArray(auth->access_token);
    long http = 0;

    /* The machine list, always: `caps` needs a machine id and a report that
     * named one without showing where it came from would be hard to check. */
    ScopedVmPage page;
    QJsonArray machines;
    QString pick = vmId;
    if (launcher_list_vms(base.constData(), tok.constData(), 0, 50, page.out(),
                          &http)) {
        for (int i = 0; i < page->count; i++) {
            const VmInfo &v = page->items[i];
            QJsonObject m;
            m[QStringLiteral("id")] = str(v.id);
            m[QStringLiteral("alias")] = str(v.alias);
            m[QStringLiteral("name")] = str(v.name);
            m[QStringLiteral("state")] = str(v.state);
            /* The raw body per machine is what the KB calls the one place a
             * hidden field could still be hiding - it is kept and then
             * discarded during conversion, and nobody has printed it. */
            m[QStringLiteral("raw")] = bodyValue(v.raw_json);
            machines.append(m);
            if (pick.isEmpty()) pick = str(v.id);
        }
    }
    report[QStringLiteral("machines_http")] = double(http);
    report[QStringLiteral("machines")] = machines;

    if (what == QStringLiteral("vms")) {
        report[QStringLiteral("ok")] = !machines.isEmpty();
        return write(report, outPath) ? 3 : (machines.isEmpty() ? 2 : 0);
    }

    if (pick.isEmpty()) {
        report[QStringLiteral("ok")] = false;
        report[QStringLiteral("error")] = QStringLiteral("no machine on this account");
        write(report, outPath);
        return 2;
    }
    report[QStringLiteral("vm_id")] = pick;

    /* === DISP1 — the point of the exercise ================================
     *
     * No VM is started. `/capabilities` answers for a machine that is asleep,
     * and starting one would spend the account's hours to learn nothing more.
     * The four parsed values are printed beside the raw body so a reader can
     * see at a glance which keys we already consume and which we ignore. */
    ScopedVmCaps caps;
    const QByteArray id = pick.toUtf8();
    http = 0;
    const bool got = launcher_get_capabilities(base.constData(), tok.constData(),
                                               id.constData(), caps.out(), &http);
    QJsonObject c;
    c[QStringLiteral("http")] = double(http);
    c[QStringLiteral("ok")] = got;
    QJsonObject parsed;
    parsed[QStringLiteral("video_allowed")] = caps->video_allowed;
    parsed[QStringLiteral("max_frame_rate")] = caps->max_frame_rate;
    parsed[QStringLiteral("max_width")] = caps->max_width;
    parsed[QStringLiteral("max_height")] = caps->max_height;
    parsed[QStringLiteral("video_codecs")] = str(caps->video_codecs);
    parsed[QStringLiteral("audio_allowed")] = caps->audio_allowed;
    parsed[QStringLiteral("audio_codecs")] = str(caps->audio_codecs);
    /* CAPS2 - the keys the parser gained once the body was printed. Shown
     * beside the raw so a regression in the parser is visible by comparison
     * rather than by trusting it. */
    parsed[QStringLiteral("max_monitor_count")] = caps->max_monitor_count;
    parsed[QStringLiteral("video_chroma")] = str(caps->video_chroma);
    parsed[QStringLiteral("micro_allowed")] = caps->micro_allowed;
    parsed[QStringLiteral("clipboard_allowed")] = caps->clipboard_allowed;
    parsed[QStringLiteral("filetransfer_allowed")] = caps->filetransfer_allowed;
    parsed[QStringLiteral("gamepad_allowed")] = caps->gamepad_allowed;
    QJsonObject use;
    use[QStringLiteral("max_session_length")] = caps->usage.max_session_length;
    use[QStringLiteral("max_duration")] = caps->usage.max_duration;
    use[QStringLiteral("fair_use_usage")] = caps->usage.fair_use_usage;
    use[QStringLiteral("fair_use_alert_threshold")] = caps->usage.fair_use_alert_threshold;
    use[QStringLiteral("fair_use_renew_date")] = str(caps->usage.fair_use_renew_date);
    use[QStringLiteral("end_of_streaming_session")] = str(caps->usage.end_of_streaming_session);
    use[QStringLiteral("time_slots_enabled")] = caps->usage.time_slots_enabled;
    parsed[QStringLiteral("usage")] = use;
    c[QStringLiteral("parsed")] = parsed;
    c[QStringLiteral("raw")] = bodyValue(caps->raw_json);
    report[QStringLiteral("capabilities")] = c;

    report[QStringLiteral("ok")] = got;
    const int w = write(report, outPath);
    return w ? w : (got ? 0 : 1);
}

}  // namespace halyard::probe
