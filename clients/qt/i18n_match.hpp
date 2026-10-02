/* i18n_match - which catalogue to load, decided without touching Qt's runtime.
 *
 * === QT5 2026-10-03 — WHY THIS IS ITS OWN HEADER ===========================
 *
 * Picking a language looks like one line - `QLocale::system().name()` - and
 * that line is wrong three ways, each of which ships:
 *
 *   1. It takes ONE locale. The system gives an ordered LIST of preferences
 *      (`QLocale::uiLanguages()`: "fr-CA", "fr", "en-US"...). A Canadian user
 *      with French first must get the French catalogue we ship as plain "fr",
 *      which a single exact match never finds.
 *   2. It skips the source language. If the user's FIRST preference is
 *      English and their second is French, they want English - the strings
 *      compiled into the binary - not the first catalogue that happens to
 *      exist. Walking the list and taking the first catalogue found gives
 *      French to someone who asked for English.
 *   3. Scripts. Chinese is "zh-Hans-CN" in the preference list and the
 *      catalogue is "zh-Hans" (that is how Borealis names it); "zh_CN" is a
 *      region, not a script, and a match on the first two letters would also
 *      hand Simplified Chinese to a Traditional reader.
 *
 * So the rule is: each preference is tried from most to least specific
 * ("zh-Hans-CN" -> "zh-Hans" -> "zh"), the source language counts as available,
 * and the first preference that matches anything wins. Case and the `_`/`-`
 * spelling are not significant.
 *
 * Pure: QString only, no QLocale, no files. Tested by tests/test_qt_i18n.cpp.
 */
#pragma once

#include <QString>
#include <QStringList>

namespace halyard::i18n {

/* The language the strings are written in. It needs no catalogue. */
inline QString sourceLanguage() { return QStringLiteral("en"); }

/* "fr_FR" and "FR-fr" both become "fr-fr" - comparison form only. */
inline QString normalise(const QString &tag)
{
    return tag.trimmed().replace(QLatin1Char('_'), QLatin1Char('-')).toLower();
}

/* "zh-Hans-CN" -> {"zh-hans-cn", "zh-hans", "zh"}: most specific first. */
inline QStringList fallbacks(const QString &tag)
{
    QStringList out;
    QString t = normalise(tag);
    while (!t.isEmpty()) {
        out << t;
        const int cut = t.lastIndexOf(QLatin1Char('-'));
        if (cut <= 0) break;
        t.truncate(cut);
    }
    return out;
}

/* `available` as given (catalogue codes, e.g. "fr", "zh-Hans"), matched
 * against `tag` from most to least specific. Returns the catalogue's own
 * spelling, or an empty string. The source language always matches. */
inline QString matchOne(const QString &tag, const QStringList &available)
{
    for (const QString &candidate : fallbacks(tag)) {
        if (candidate == normalise(sourceLanguage())) return sourceLanguage();
        for (const QString &a : available)
            if (normalise(a) == candidate) return a;
    }
    return QString();
}

/* The catalogue to load.
 *
 *   requested   the user's explicit choice, or empty for "follow the system";
 *   system      the system's preferences, in order (QLocale::uiLanguages());
 *   available   the catalogues that exist, not counting the source language.
 *
 * An explicit choice that no longer exists (a catalogue removed, a settings
 * file copied from a newer build) falls through to the system rather than
 * failing: the user still gets the best language there is, and the stored
 * choice is left alone in case the catalogue comes back. */
inline QString pickLanguage(const QString &requested, const QStringList &system,
                            const QStringList &available)
{
    if (!requested.trimmed().isEmpty()) {
        const QString m = matchOne(requested, available);
        if (!m.isEmpty()) return m;
    }
    for (const QString &pref : system) {
        const QString m = matchOne(pref, available);
        if (!m.isEmpty()) return m;
    }
    return sourceLanguage();
}

}  // namespace halyard::i18n
