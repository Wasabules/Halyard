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

int loadIntoEnvironment()
{
    QSettings st;
    st.beginGroup(kGroup);
    int applied = 0;
    for (const Setting &s : settings()) {
        const QString key = QString::fromUtf8(s.env);
        if (!st.contains(key)) continue;
        if (env_override_active(s.env) != 0) continue;   /* outside wins */
        const QByteArray v = st.value(key).toString().toUtf8();
        if (v.isEmpty()) continue;
        setenv(s.env, v.constData(), 1);
        applied++;
    }
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
