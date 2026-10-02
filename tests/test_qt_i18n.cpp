/* test_qt_i18n - which catalogue the Qt client loads.
 *
 * `clients/qt/i18n_match.hpp` explains the three ways the one-line version
 * (`QLocale::system().name()`) goes wrong. Each has a case here, plus the
 * mutation that would reintroduce it. Pure: Qt6Core only, no QLocale data, no
 * files, so it runs the same on every machine.
 */
#include <QStringList>

#include <cstdio>

#include "../clients/qt/i18n_match.hpp"

using halyard::i18n::fallbacks;
using halyard::i18n::pickLanguage;

static int checks = 0, failures = 0;

static void eq(const QString &got, const QString &want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        std::printf("  FAIL %-60s got \"%s\", expected \"%s\"\n", what,
                    qPrintable(got), qPrintable(want));
    }
}

int main()
{
    std::printf("== Qt client language choice (QT5 2026-10-03) ==\n");

    const QStringList shipped = { QStringLiteral("fr"), QStringLiteral("ru"),
                                  QStringLiteral("zh-Hans") };

    /* --- following the system ------------------------------------------- */
    eq(pickLanguage({}, { "fr-FR", "en-US" }, shipped), "fr",
       "fr-FR first -> the plain fr catalogue");
    eq(pickLanguage({}, { "fr_CA" }, shipped), "fr",
       "underscore spelling and a region we do not ship");

    /* Trap 2: the source language is a real answer. */
    eq(pickLanguage({}, { "en-GB", "fr-FR" }, shipped), "en",
       "English FIRST stays English, even though fr exists");

    eq(pickLanguage({}, { "de-DE", "it-IT" }, shipped), "en",
       "nothing matches -> the source language");
    eq(pickLanguage({}, { "de-DE", "ru-RU" }, shipped), "ru",
       "the first preference that MATCHES wins, not the first one");
    eq(pickLanguage({}, {}, shipped), "en", "no preferences at all");

    /* Trap 3: scripts. */
    eq(pickLanguage({}, { "zh-Hans-CN" }, shipped), "zh-Hans",
       "zh-Hans-CN -> zh-Hans");
    eq(pickLanguage({}, { "zh-Hant-TW" }, shipped), "en",
       "Traditional is NOT handed the Simplified catalogue");

    /* --- an explicit choice ---------------------------------------------- */
    eq(pickLanguage("ru", { "fr-FR" }, shipped), "ru", "an explicit choice wins");
    eq(pickLanguage("en", { "fr-FR" }, shipped), "en",
       "explicit English wins over a French system");
    eq(pickLanguage("FR", {}, shipped), "fr", "case does not matter");
    eq(pickLanguage("pt", { "fr-FR" }, shipped), "fr",
       "a choice that no longer exists falls through to the system");
    eq(pickLanguage("   ", { "ru" }, shipped), "ru", "blank = follow the system");

    /* The catalogue's own spelling is returned - it is a file name. */
    eq(pickLanguage({}, { "ZH-HANS" }, shipped), "zh-Hans",
       "the returned code keeps the catalogue's spelling");

    /* --- the fallback chain ---------------------------------------------- */
    {
        const QStringList f = fallbacks("zh-Hans-CN");
        checks++;
        if (f != QStringList({ "zh-hans-cn", "zh-hans", "zh" })) {
            failures++;
            std::printf("  FAIL fallbacks(zh-Hans-CN) = %s\n",
                        qPrintable(f.join(',')));
        }
    }

    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
