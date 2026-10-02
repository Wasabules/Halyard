#include "i18n.hpp"

#include "i18n_match.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QTranslator>

#include <algorithm>

namespace halyard::i18n {

namespace {

const QString kKey = QStringLiteral("ui/language");
/* Where qt_add_translations puts the compiled catalogues (see CMakeLists.txt,
 * RESOURCE_PREFIX). Changing one without the other makes every catalogue
 * invisible, and the selector then offers English only - a silent failure, so
 * the two are named together in both places. */
const QString kResourceDir = QStringLiteral(":/i18n");
const QString kPrefix = QStringLiteral("halyard_");

QTranslator *g_app = nullptr;   /* our strings */
QTranslator *g_qt  = nullptr;   /* Qt's own strings */
QString      g_current = sourceLanguage();

QStringList embeddedCodes()
{
    QStringList out;
    const QStringList files = QDir(kResourceDir).entryList(
        { kPrefix + QStringLiteral("*.qm") }, QDir::Files, QDir::Name);
    for (QString f : files) {
        f.chop(3);                       /* ".qm" */
        out << f.mid(kPrefix.size());    /* "halyard_fr" -> "fr" */
    }
    return out;
}

QString nativeNameOf(const QString &code)
{
    /* QLocale("en") answers "American English"; the source language is just
     * English, and saying otherwise would invent a dialect choice we never
     * made. */
    if (code == sourceLanguage()) return QStringLiteral("English");
    QString n = QLocale(code).nativeLanguageName();
    if (n.isEmpty()) return code;
    /* "français" -> "Français": a list of languages is a list of names. */
    n[0] = n.at(0).toUpper();
    return n;
}

/* Qt's own catalogue, wherever this build can find one. On MSYS2 the package
 * that ships them (qt6-translations) is not installed by default; without it
 * Qt's few built-in strings stay English and nothing else changes, so it is
 * looked for, never required. */
bool loadQtBase(QTranslator *t, const QLocale &loc)
{
    const QStringList dirs = {
        QCoreApplication::applicationDirPath() + QStringLiteral("/translations"),
        QLibraryInfo::path(QLibraryInfo::TranslationsPath),
    };
    for (const QString &d : dirs)
        if (t->load(loc, QStringLiteral("qtbase"), QStringLiteral("_"), d))
            return true;
    return false;
}

void apply(const QString &code)
{
    auto *app = QCoreApplication::instance();
    if (!app) return;

    if (g_app) { QCoreApplication::removeTranslator(g_app); delete g_app; g_app = nullptr; }
    if (g_qt)  { QCoreApplication::removeTranslator(g_qt);  delete g_qt;  g_qt  = nullptr; }

    g_current = code;
    const QLocale loc(code);
    /* Number and date formatting follow the interface language, not the OS
     * region: a French interface printing "1,234.5" next to French words is
     * the half-translated look this engine exists to avoid. */
    QLocale::setDefault(loc);

    if (code == sourceLanguage()) {
        /* Removing the translators is enough: the strings in the binary ARE
         * English. Qt still sends LanguageChange for the removal. */
        return;
    }

    g_app = new QTranslator(app);
    if (g_app->load(kPrefix + code, kResourceDir)) {
        QCoreApplication::installTranslator(g_app);
    } else {
        /* Listed by available() but not loadable means a corrupt resource,
         * which is a build defect - said, and English kept. */
        qWarning("i18n: catalogue %s listed but not loadable", qPrintable(code));
        delete g_app; g_app = nullptr;
        g_current = sourceLanguage();
        QLocale::setDefault(QLocale(sourceLanguage()));
        return;
    }

    g_qt = new QTranslator(app);
    if (loadQtBase(g_qt, loc)) QCoreApplication::installTranslator(g_qt);
    else { delete g_qt; g_qt = nullptr; }
}

}  // namespace

QList<Language> available()
{
    QList<Language> out;
    for (const QString &c : embeddedCodes()) out << Language{ c, nativeNameOf(c) };
    std::sort(out.begin(), out.end(), [](const Language &a, const Language &b) {
        return a.nativeName.localeAwareCompare(b.nativeName) < 0;
    });
    out.prepend(Language{ sourceLanguage(), nativeNameOf(sourceLanguage()) });
    return out;
}

QString requested()
{
    return QSettings().value(kKey).toString();
}

QString current() { return g_current; }

void init(const QString &forRun)
{
    const QStringList sys = QLocale::system().uiLanguages();
    const QStringList cats = embeddedCodes();
    apply(pickLanguage(forRun.isEmpty() ? requested() : forRun, sys, cats));
    /* One line, always: "the interface is in the wrong language" is otherwise
     * unanswerable from a report - was the catalogue missing, the preference
     * stale, or the system list not what we assumed? */
    qInfo("i18n: requested '%s'%s, system [%s], catalogues [%s] -> %s",
          qPrintable(forRun.isEmpty() ? requested() : forRun),
          forRun.isEmpty() ? "" : " (--lang, not stored)",
          qPrintable(sys.join(QLatin1Char(','))),
          qPrintable(cats.join(QLatin1Char(','))), qPrintable(g_current));
}

void setRequested(const QString &code)
{
    QSettings st;
    if (code.isEmpty()) st.remove(kKey);
    else                st.setValue(kKey, code);
    apply(pickLanguage(code, QLocale::system().uiLanguages(), embeddedCodes()));
}

}  // namespace halyard::i18n
