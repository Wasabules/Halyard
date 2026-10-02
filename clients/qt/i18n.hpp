/* i18n - the Qt client's translation engine and its language preference.
 *
 * === QT5 2026-10-03 — THE STANDARD MACHINERY, AND ONLY THAT =================
 *
 * Qt Linguist, not a home-made catalogue. The Borealis client has its own JSON
 * catalogues because Borealis has its own i18n; this client is Qt, and Qt's
 * tooling already does the parts that are easy to get wrong:
 *
 *   - `lupdate` EXTRACTS every `tr()` / `QT_TRANSLATE_NOOP` from the sources
 *     into `clients/qt/i18n/halyard_<lang>.ts`, keeps translations of strings
 *     that did not change, and marks the changed ones unfinished instead of
 *     silently keeping a stale sentence;
 *   - `lrelease` compiles them, and CMake (`qt_add_translations`) embeds the
 *     result in the binary, so there is no file to forget next to the .exe -
 *     the CA bundle already showed what that costs;
 *   - Qt's own strings (the "Close" of a dialog box, a line edit's context
 *     menu) come from Qt's `qtbase_<lang>.qm`, loaded alongside when present.
 *
 * Adding a language is adding one .ts file to the CMake list. No code here
 * names a language: the selector lists whatever catalogues are embedded.
 *
 * === LIVE SWITCHING ========================================================
 *
 * `setRequested()` installs the new translator, and Qt then sends
 * `QEvent::LanguageChange` to every widget. Each window rebuilds its texts on
 * that event, so the language changes without a restart. Text a worker has
 * already produced (a status line) stays in the language it was produced in
 * until the next update - which is honest: it was said then.
 *
 * The preference is stored as the user's CHOICE ("fr", or empty for "follow
 * the system"), not as the language resolved from it. Storing the resolution
 * would freeze the system language at the day of first launch.
 */
#pragma once

#include <QList>
#include <QString>

namespace halyard::i18n {

struct Language {
    QString code;        /* catalogue code: "en", "fr", "zh-Hans" */
    QString nativeName;  /* in its own language: "Français" */
};

/* Every language that can be chosen: the source language, then each embedded
 * catalogue, by native name. */
QList<Language> available();

/* The stored choice; empty = follow the system. */
QString requested();

/* The language actually loaded. */
QString current();

/* Load the stored choice - or `forRun`, when given, for this run only and
 * without storing it (`--lang fr`: a screenshot or a test in another language
 * must not change the user's preference). Called once from main(), after the
 * QApplication exists and before the first window, so no widget is ever built
 * in the wrong language and then rebuilt. */
void init(const QString &forRun = QString());

/* Store a new choice and apply it immediately (empty = follow the system). */
void setRequested(const QString &code);

}  // namespace halyard::i18n
