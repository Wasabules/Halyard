/* test_qt_shortcuts - the keyboard shortcut table (KEY1).
 *
 * What this pins is what went wrong before the table existed: a key written
 * at two sites and drifting apart, and two commands given the same key with
 * nothing to notice. The defaults are asserted BY VALUE, so changing one is a
 * deliberate edit here and not a silent change of behaviour.
 */
#include <cstdio>

#include "../clients/qt/shortcuts.hpp"

using namespace halyard::keys;

static int checks = 0, failures = 0;

static void ok(bool cond, const char *what)
{
    checks++;
    if (!cond) { failures++; std::printf("  FAIL %s\n", what); }
}

static void eq(const QString &got, const QString &want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        std::printf("  FAIL %-46s got \"%s\", expected \"%s\"\n", what,
                    got.toUtf8().constData(), want.toUtf8().constData());
    }
}

int main()
{
    std::printf("== Qt keyboard shortcuts (KEY1 2026-10-03) ==\n");

    /* --- the table itself ------------------------------------------------ */
    ok(ActCount == 7, "seven commands");
    eq(QString::number(defaults().size()), QStringLiteral("7"),
       "one default per command");

    for (int i = 0; i < ActCount; i++) {
        const ActionDef &a = action(i);
        ok(a.id && *a.id, "every entry has an id");
        ok(a.settingsKey && *a.settingsKey, "every entry has a settings key");
        ok(a.def && *a.def, "every entry has a default");
        ok(a.label && *a.label, "every entry has a label");
    }

    /* Ids and settings keys are both identifiers: a duplicate would make one
     * command silently read and write the other's stored value. */
    for (int i = 0; i < ActCount; i++)
        for (int j = i + 1; j < ActCount; j++) {
            ok(QString::fromUtf8(action(i).id) != QString::fromUtf8(action(j).id),
               "ids are distinct");
            ok(QString::fromUtf8(action(i).settingsKey)
                   != QString::fromUtf8(action(j).settingsKey),
               "settings keys are distinct");
        }

    /* --- the defaults, by value ------------------------------------------ */
    eq(QString::fromUtf8(action(ActFullscreen).def), QStringLiteral("F11"),
       "fullscreen defaults to F11");
    eq(QString::fromUtf8(action(ActOverlay).def), QStringLiteral("F8"),
       "the overlay defaults to F8");
    eq(QString::fromUtf8(action(ActMetrics).def), QStringLiteral("Ctrl+M"),
       "metrics default to Ctrl+M");
    eq(QString::fromUtf8(action(ActFiles).def), QStringLiteral("Ctrl+T"),
       "file transfer defaults to Ctrl+T");
    eq(QString::fromUtf8(action(ActSettings).def), QStringLiteral("Ctrl+,"),
       "settings default to Ctrl+,");

    /* The shipped set must not collide with itself - which is the one thing
     * nobody checked when the keys lived at seven separate call sites. */
    ok(conflicting(defaults()).isEmpty(), "the defaults do not collide");

    /* The overlay keeps its ORIGINAL settings key: renaming it would orphan
     * the choice of anyone who had already changed it. */
    eq(QString::fromUtf8(action(ActOverlay).settingsKey),
       QStringLiteral("ui/overlay_hotkey"), "the overlay key is not renamed");

    /* --- conflict detection ---------------------------------------------- */
    {
        const QStringList s{ QStringLiteral("F11"), QStringLiteral("F8"),
                             QStringLiteral("F11") };
        const QVector<int> c = conflicting(s);
        ok(c.size() == 2 && c.at(0) == 0 && c.at(1) == 2,
           "both sides of a collision are reported, in order");
    }
    {
        /* Three the same: all three are at fault, not two of them. */
        const QStringList s{ QStringLiteral("F5"), QStringLiteral("F5"),
                             QStringLiteral("F5") };
        ok(conflicting(s).size() == 3, "a three-way collision reports all three");
    }
    {
        /* Unbound is not a collision, however many are unbound - otherwise
         * clearing two keys would light up as an error. */
        const QStringList s{ QString(), QString(), QStringLiteral("F8") };
        ok(conflicting(s).isEmpty(), "empty sequences never collide");
    }
    {
        const QStringList s{ QStringLiteral("Ctrl+M"), QStringLiteral("Ctrl+N") };
        ok(conflicting(s).isEmpty(), "distinct sequences do not collide");
    }
    {
        /* Normalisation is the caller's job, and this function must NOT fold
         * case itself: `QKeySequence::toString` has already settled it, and a
         * second opinion here would disagree with Qt's own dispatch. */
        const QStringList s{ QStringLiteral("Ctrl+M"), QStringLiteral("ctrl+m") };
        ok(conflicting(s).isEmpty(),
           "comparison is exact on already-normalised text");
    }
    ok(conflicting(QStringList()).isEmpty(), "an empty list is not a collision");

    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
