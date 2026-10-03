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

void saveVariable(const QString &env, const QString &value)
{
    QSettings st;
    st.beginGroup(kGroup);
    if (value.isEmpty()) st.remove(env);
    else                 st.setValue(env, value);
    st.endGroup();
}

void forgetAll()
{
    QSettings st;
    st.remove(kGroup);
}

}  // namespace halyard::store
