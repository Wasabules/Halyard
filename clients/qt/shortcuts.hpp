/* The keyboard shortcut table - one declaration, read by everyone.
 *
 * === KEY1 2026-10-03 — WHY A TABLE AND NOT setShortcut() AT EACH SITE ======
 *
 * Seven commands had their keys written where they happened to be built:
 * `Ctrl+,`, `Ctrl+M`, `Ctrl+T` and `F11` in the menu construction, `F8` read
 * from QSettings in one place, and `Qt::Key_F11` AGAIN inside the video
 * widget's key handler. Three consequences, all of them real:
 *
 *   - only one of the seven could be changed by the user;
 *   - F11 was declared twice, so changing it in the menu left the stream's own
 *     handler still swallowing the old key - the two drift apart silently;
 *   - nothing could tell that two commands had been given the same key,
 *     because nothing had the whole set in front of it.
 *
 * This header holds the set. It is PURE - no Qt widgets, no QSettings, no
 * QKeySequence (that lives in QtGui) - so `tests/test_qt_shortcuts.cpp` can
 * exercise the parts that have historically been got wrong: the defaults, and
 * which entries collide.
 *
 * Normalisation is the CALLER's job: the settings window turns what the user
 * typed into portable text with `QKeySequence::toString(PortableText)` before
 * storing or comparing it. Keeping QKeySequence out of here is what keeps the
 * test in the offline suite, which links Qt6Core only.
 *
 * === WHERE THEY ARE STORED ================================================
 *
 * `QSettings` key `settingsKey`, which for the overlay is the pre-existing
 * `ui/overlay_hotkey` and NOT `ui/shortcut/overlay`. Renaming it would orphan
 * the choice of anyone who had already set it - the same reasoning that keeps
 * the French `SHADOW_*` names in core.
 */
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace halyard::keys {

/* The order is the order the settings window lists them in. */
enum ActionId {
    ActFullscreen = 0,
    ActOverlay,
    ActMetrics,
    ActFiles,
    ActSettings,
    ActScreenshot,
    ActDisconnect,
    ActCount
};

struct ActionDef {
    const char *id;            /* stable, never shown */
    const char *settingsKey;   /* QSettings key - see the header note */
    const char *def;           /* the default, in portable text */
    const char *label;         /* QT_TR_NOOP source, translated at display */
    bool        streamOnly;    /* meaningless without a running session */
};

inline const ActionDef *actions()
{
    /* `QT_TR_NOOP` is deliberately NOT used here: this header is compiled into
     * the offline test, which does not link QtCore's translation machinery.
     * The settings window wraps each label in `tr()` with the same context, so
     * lupdate still collects them there. */
    static const ActionDef kTable[ActCount] = {
        { "fullscreen", "ui/shortcut/fullscreen", "F11",    "Fullscreen",            false },
        { "overlay",    "ui/overlay_hotkey",      "F8",     "Open the overlay",      true  },
        { "metrics",    "ui/shortcut/metrics",    "Ctrl+M", "Metrics window",        false },
        { "files",      "ui/shortcut/files",      "Ctrl+T", "File transfer",         false },
        { "settings",   "ui/shortcut/settings",   "Ctrl+,", "Settings",              false },
        { "screenshot", "ui/shortcut/screenshot", "F12",    "Take a screenshot",     true  },
        { "disconnect", "ui/shortcut/disconnect", "Ctrl+D", "Disconnect",            true  },
    };
    return kTable;
}

inline const ActionDef &action(int i) { return actions()[i]; }

inline QStringList defaults()
{
    QStringList out;
    for (int i = 0; i < ActCount; i++) out << QString::fromUtf8(actions()[i].def);
    return out;
}

/* Which entries share their sequence with another.
 *
 * An EMPTY sequence means unbound and never collides, however many of them
 * there are - that is the whole point of allowing a command to have no key.
 * Comparison is exact on the normalised text the caller passes in; case is
 * already settled by `QKeySequence::toString`, so folding it here would only
 * invent a difference between this function and Qt's own dispatch.
 *
 * Returns the indices, ascending, so the caller can mark exactly those rows. */
inline QVector<int> conflicting(const QStringList &seqs)
{
    QVector<int> out;
    for (int i = 0; i < seqs.size(); i++) {
        if (seqs.at(i).isEmpty()) continue;
        for (int j = 0; j < seqs.size(); j++) {
            if (i == j) continue;
            if (seqs.at(i) == seqs.at(j)) { out.append(i); break; }
        }
    }
    return out;
}

}  // namespace halyard::keys
