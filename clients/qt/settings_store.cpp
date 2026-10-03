#include "settings_store.hpp"

#include "settings_model.hpp"

#include <QSettings>

extern "C" {
#include "core/services/env_override.h"
#include "core/services/win_compat.h"   /* setenv/unsetenv on Windows */
}

namespace halyard::store {

namespace {
const QString kGroup = QStringLiteral("env");
}

/* Keys the window persists that are NOT rows of the settings table because
 * their choices are discovered at runtime (the audio output device, AUD-DEV1).
 * An allowlist and not "load everything under [env]" on purpose: a stale file
 * from another build must not be able to switch on a campaign instrument the
 * window never shows. */
static const char *const kExtraKeys[] = {
    "SHADOW_WIN_AUDIO_DEVICE",
};

static int loadKey(QSettings &st, const char *env, int *applied)
{
    const QString key = QString::fromUtf8(env);
    if (!st.contains(key)) return 0;
    if (env_override_active(env) != 0) return 0;         /* outside wins */
    const QByteArray v = st.value(key).toString().toUtf8();
    if (v.isEmpty()) return 0;
    setenv(env, v.constData(), 1);
    (*applied)++;
    return 1;
}

int loadIntoEnvironment()
{
    QSettings st;
    st.beginGroup(kGroup);
    int applied = 0;
    for (const Setting &s : settings()) loadKey(st, s.env, &applied);
    for (const char *k : kExtraKeys)    loadKey(st, k, &applied);
    st.endGroup();
    return applied;
}

/* === QT5/SET2 2026-10-03 — WRITE THE FILE *AND* THIS PROCESS =============
 *
 * This used to write QSettings and stop there, so a setting took effect at the
 * next LAUNCH, not at the next session - while every description in the
 * settings window promises "applies on the next session". Found through
 * `SHADOW_FT_SELFTEST`: ticking it and reconnecting ran nothing, because core
 * read `getenv` on a variable the application had never set.
 *
 * The variable is therefore applied to this process too. The ordering rule
 * from `loadIntoEnvironment` holds: a variable that came from OUTSIDE (the
 * real environment, env.txt) wins, and the settings window must not overwrite
 * it - `env_override_active` is what records which those are, snapshotted
 * before the client set anything.
 *
 * This does NOT make a setting live: core reads most toggles once, at first
 * use, and caches them in a static. It makes the promise true - the next
 * session sees it - and that is what the descriptions claim. */
void saveVariable(const QString &env, const QString &value)
{
    QSettings st;
    st.beginGroup(kGroup);
    if (value.isEmpty()) st.remove(env);
    else                 st.setValue(env, value);
    st.endGroup();

    const QByteArray k = env.toUtf8();
    if (env_override_active(k.constData()) != 0) return;   /* outside wins */
    if (value.isEmpty()) unsetenv(k.constData());
    else                 setenv(k.constData(), value.toUtf8().constData(), 1);
}

void forgetAll()
{
    /* SET2 - unset in this process as well, and for the same reason: leaving
     * them set would mean "restore defaults" had no effect until a restart,
     * which is precisely the surprise this change exists to remove. The list
     * is read BEFORE the group is removed, because afterwards there is nothing
     * left to say which variables were ours. */
    QSettings st;
    st.beginGroup(kGroup);
    const QStringList ours = st.allKeys();
    st.endGroup();
    st.remove(kGroup);

    for (const QString &k : ours) {
        const QByteArray b = k.toUtf8();
        if (env_override_active(b.constData()) == 0) unsetenv(b.constData());
    }
}

}  // namespace halyard::store
