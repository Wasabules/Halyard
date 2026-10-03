/* halyard-qt - the desktop client.
 *
 * Four states in a straight line: pairing, machines, connecting, streaming.
 * `main_window.hpp` explains the structure; `clients/qt/README.md` explains the
 * choice of Qt and the three contracts `halyard-core` imposes.
 *
 * This file does one thing: hand the environment to core before anything reads
 * it. That ordering is contract 3 and it is the only thing that cannot be done
 * later — the ~260 `SHADOW_*` toggles are read with `getenv` at FIRST USE and
 * cached in statics, so a setting applied after the first session does nothing
 * until the next one.
 */
#include <QApplication>
#include <QStyleFactory>
#include <QSettings>

#include "i18n.hpp"
#include "main_window.hpp"
#include "probe.hpp"
#include "settings_store.hpp"
#include "theme.hpp"

extern "C" {
#include "core/services/env_override.h"
#include "core/version.h"
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    /* === QT4 — THE IDENTITY, FROM THE ONE DECLARATION =====================
     *
     * Every string here comes from `build_id.h`, which CMake generates from the
     * single app-identity block near the top of `CMakeLists.txt` - the same
     * source the NACP, `param.sfo` and the LiveArea are derived from. Nothing
     * is retyped, because the version being retyped in two places is the exact
     * defect `core/version.h` was written to end (S85).
     *
     * What each one is actually for, since they are not interchangeable:
     *
     *  - `applicationName` is the DISPLAY name. It is what a desktop shows and
     *    what several Qt dialogs fall back to; "halyard-qt" was a build
     *    target's name leaking into the UI.
     *  - `applicationDisplayName` is deliberately NOT set. Qt APPENDS it to
     *    every window title that does not already contain it, so with it set
     *    the settings window read "Halyard settings - Halyard" and the About
     *    box "About Halyard - Halyard". Each window titles itself.
     *  - `organizationName`/`organizationDomain` decide where QSettings puts
     *    its file. Changing them later ORPHANS whatever was stored, so they are
     *    set now, before anything stores anything - which is the real reason
     *    this is worth doing before the first feature that needs it.
     *  - `desktopFileName` is how Wayland ties a window to its .desktop entry;
     *    without it the taskbar shows a generic icon regardless of what
     *    `setWindowIcon` says. */
    QApplication::setApplicationName(QString::fromUtf8(SHADOW_APP_NAME));
    QApplication::setApplicationVersion(QString::fromUtf8(SHADOW_VERSION));
    QApplication::setOrganizationName(QString::fromUtf8(SHADOW_APP_AUTHOR));
    QApplication::setOrganizationDomain(QStringLiteral("halyard.invalid"));
    QApplication::setDesktopFileName(QStringLiteral("halyard"));

    /* Set on the application and not only on each window: a dialog created
     * without an explicit icon inherits this one, so the About box and every
     * future window are right without remembering to ask. */
    QApplication::setWindowIcon(halyard::theme::appTileIcon());

    /* === UI1 — ONE STYLE, ONE SHEET, BEFORE THE FIRST WIDGET ==============
     *
     * Fusion and not the platform style, measured on Windows 11 2026-10-03:
     * the `windows11` style draws its buttons, tabs and line edits through the
     * native theme renderer, which IGNORES `border-radius`, `border` and
     * `padding` from a stylesheet - and worse, a sheet that touches a widget
     * makes that style fall back to an unstyled box, so half the controls came
     * out rounded and the other half square. Fusion honours the sheet, and it
     * is the same renderer the Linux build gets, so the design is verifiable on
     * one platform and true on both.
     *
     * `SHADOW_QT_STYLE` reverts it: set it to `windows11`, `windowsvista` or
     * any key `QStyleFactory` knows to get the platform look back (an empty or
     * unknown key leaves Qt's own default alone). */
    {
        const QByteArray want = qgetenv("SHADOW_QT_STYLE");
        if (!want.isEmpty()) QApplication::setStyle(QString::fromUtf8(want));
        else                 QApplication::setStyle(QStringLiteral("Fusion"));
    }

    /* D2 - the theme choice, BEFORE the sheet: the sheet's colours are derived
     * from the palette, so a palette set afterwards would leave every card,
     * pill and border painted for the other theme. */
    {
        const int m = QSettings().value(QStringLiteral("ui/theme"), 0).toInt();
        halyard::theme::applyThemeMode(static_cast<halyard::theme::ThemeMode>(m));
    }

    /* The sheet on the application, not per window: a dialog opened later - the
     * settings window, the file manager, the overlay - inherits it without
     * having to remember. The colours inside it are derived from the palette,
     * so this follows a dark or light system theme. */
    if (qgetenv("SHADOW_QT_PLAIN") != "1")
        qApp->setStyleSheet(halyard::theme::appStyleSheet(nullptr));

    /* === CONTRACT 3, AND IT MUST BE FIRST ==================================
     *
     * `env_override_snapshot()` records which `SHADOW_*` variables came from
     * OUTSIDE the application, so a settings screen can later show which of
     * its rows the session is ignoring. It has to run BEFORE the client sets
     * any of its own, or the client's writes read as the user's.
     *
     * The Borealis client does the same thing in the same order
     * (`main.cpp`: env.txt -> env_override_snapshot -> applyToggles), and
     * getting it backwards is the defect that order exists to prevent. */
    env_override_snapshot();

    /* QT5 - the settings window's saved choices, AFTER the snapshot (so they
     * are not mistaken for the user's environment and do not lock their own
     * rows) and BEFORE anything core reads. INI rather than the registry: a
     * file that can be read, diffed, deleted and attached to a report. */
    QSettings::setDefaultFormat(QSettings::IniFormat);
    halyard::store::loadIntoEnvironment();

    /* The language before the first widget, so nothing is built in one
     * language and immediately rebuilt in another. */
    {
        /* `--lang <code>` for this run only (see i18n::init). */
        const QStringList a = QApplication::arguments();
        const int at = a.indexOf(QStringLiteral("--lang"));
        halyard::i18n::init(at >= 0 && at + 1 < a.size() ? a.at(at + 1) : QString());
    }

    /* === PROBE1 — headless, before any widget ============================
     *
     * `--probe <name> [--vm <id>] [--out <path>]` answers a question about
     * the account and exits. No window is created, so it runs from a script
     * and on a machine with no display; `QApplication` is already up, which
     * is all the probe needs (Qt's networking and JSON, not a GUI).
     *
     * Placed AFTER the environment and settings are loaded, so a probe sees
     * the same `SHADOW_*` the real client would - and before the window, so
     * nothing is drawn on the way. */
    {
        const QStringList a = QApplication::arguments();
        const int at = a.indexOf(QStringLiteral("--probe"));
        if (at >= 0) {
            const auto argAfter = [&a](const QString &flag) {
                const int i = a.indexOf(flag);
                return (i >= 0 && i + 1 < a.size()) ? a.at(i + 1) : QString();
            };
            return halyard::probe::run(
                at + 1 < a.size() ? a.at(at + 1) : QStringLiteral("caps"),
                argAfter(QStringLiteral("--vm")),
                argAfter(QStringLiteral("--out")));
        }
    }

    MainWindow win;
    win.show();

    /* QT3 - `--metrics` and `--settings` open those windows at startup.
     *
     * Not a convenience: a window reached only through a menu can only be
     * tested by driving the menu, and driving a menu from a script is how a
     * verification turns into a guess about focus. A flag makes "does it open"
     * answerable. */
    const QStringList args = QApplication::arguments();
    if (args.contains(QStringLiteral("--settings"))) win.openSettings();
    if (args.contains(QStringLiteral("--metrics")))  win.openMetrics();
    /* `--files` forces the file manager open with no session, which is the only
     * way to check its layout and its "not connected" path without a VM. In
     * normal use the menu entry is disabled until a session grants the channel. */
    if (args.contains(QStringLiteral("--files")))    win.openFileManagerForced();

    return app.exec();
}
