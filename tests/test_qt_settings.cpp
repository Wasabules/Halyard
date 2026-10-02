/* test_qt_settings - the settings table's non-obvious rules, and its shape.
 *
 * The window is generated from `clients/qt/settings_model.hpp`, so a defect in
 * the table is a defect in every row at once. The rules it encodes are each a
 * mistake somebody has already made:
 *
 *   - an ABSENT variable is not 0. `atoi` answers 0 for "", "O" and "off", and
 *     0 is a meaningful setting for most of these keys;
 *   - a value OUT OF RANGE falls back rather than being clamped, because a
 *     clamped value is a setting the user did not choose and cannot see;
 *   - some values must UNSET the variable rather than write it;
 *   - the value is NOT always the index (QT4): `SHADOW_CURSOR_MODE` takes
 *     words and refuses "0", `SHADOW_AUDIO_CODEC` starts at 1;
 *   - what absent MEANS is core's decision (QT5): a default-ON toggle must be
 *     drawn checked, a default choice preselected.
 *
 * Several of those rows were wrong in a draft, in ways no screen would show:
 * the log level offered in reverse order (choosing "Debug" wrote ERROR), and an
 * "off" that wrote "0" to a variable whose mere presence enables it. The
 * assertions below pin the corrected rows by their written bytes, because the
 * written bytes are the only place those defects are visible.
 *
 * It also checks the table's own invariants over every row: a `values` list
 * one entry short, a default out of range, or a duplicated variable produces a
 * control that LOOKS right and writes the wrong thing.
 *
 * Needs no Qt widgets - the model is pure. Compiled with -std=c++20 -Wall
 * -Wextra -Werror -O1 by tests/run_tests.sh (C++20 for the table's designated
 * initializers).
 */
#include <QStringList>

#include <cstdio>

#include "../clients/qt/settings_model.hpp"

using halyard::Setting;
using halyard::settingDefaultChoice;
using halyard::settingDisplayIndex;
using halyard::settingValue;
using halyard::settingWrite;
using halyard::settings;
using halyard::settingsGroups;
using halyard::settingsPages;

static int checks = 0, failures = 0;

static void eq_int(int got, int want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        std::printf("  FAIL %-58s got %d, expected %d\n", what, got, want);
    }
}

static void eq_str(const QString &got, const QString &want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        std::printf("  FAIL %-58s got \"%s\", expected \"%s\"\n", what,
                    got.toUtf8().constData(), want.toUtf8().constData());
    }
}

static void ok(bool cond, const char *what)
{
    checks++;
    if (!cond) { failures++; std::printf("  FAIL %s\n", what); }
}

static const Setting *find(const char *env)
{
    for (const Setting &s : settings())
        if (qstrcmp(s.env, env) == 0) return &s;
    std::printf("  FAIL %s is not in the table\n", env);
    failures++;
    return nullptr;
}

int main()
{
    std::printf("== the Qt settings table (QT3..QT5 2026-10-03) ==\n");

    /* ================================================== RULE 1 and 2 ===== */

    const Setting *clip = find("SHADOW_CLIPBOARD");
    if (clip) {
        eq_int(settingValue(*clip, nullptr), -1, "unset reads as -1, not 0");
        eq_int(settingValue(*clip, ""),      -1, "empty reads as -1, not 0");
        eq_int(settingValue(*clip, "O"),     -1, "the letter O is not a zero");
        eq_int(settingValue(*clip, "off"),   -1, "the word off is not a zero");
        eq_int(settingValue(*clip, "0"),      0, "\"0\" really is 0");
        eq_int(settingValue(*clip, "3"),      3, "a value in range");
        eq_int(settingValue(*clip, "  2"),    2, "leading blanks are accepted");
        eq_int(settingValue(*clip, "4"),     -1, "4 is out of range -> -1, NOT clamped");
        eq_int(settingValue(*clip, "-1"),    -1, "negative likewise");
        /* The clipboard's 0 means OFF and must be WRITTEN. */
        eq_str(settingWrite(*clip, 0), QStringLiteral("0"),
               "the clipboard's 0 means OFF and IS written");
    }

    const Setting *tcp = find("SHADOW_VIDEO_NET_TCP");
    if (tcp) {
        eq_int(settingValue(*tcp, "1"), 1, "toggle 1");
        eq_int(settingValue(*tcp, "2"), -1, "a toggle refuses 2 rather than truthing it");
        eq_str(settingWrite(*tcp, 0), QString(),
               "value 0 UNSETS SHADOW_VIDEO_NET_TCP rather than writing 0");
    }
    /* The asymmetry itself, as one failure rather than two puzzles: give every
     * row the same `unsetValue` - the tidy-looking edit - and this fires. */
    checks++;
    if (tcp && clip && settingWrite(*tcp, 0) == settingWrite(*clip, 0)) {
        failures++;
        std::printf("  FAIL value 0 cannot mean the same thing for "
                    "SHADOW_VIDEO_NET_TCP and SHADOW_CLIPBOARD\n");
    }

    const Setting *stall = find("SHADOW_STALL_MS");
    if (stall) {
        eq_int(settingValue(*stall, "300"), 300, "a number in range");
        eq_int(settingValue(*stall, "10"),   -1, "below the minimum -> -1");
        eq_int(settingValue(*stall, "99999"), -1, "above the maximum -> -1");
    }

    /* ======================================= the corrected rows (QT4) ===== */

    /* journal.h: ERROR=0 WARN=1 INFO=2 DEBUG=3 TRACE=4. The draft offered
     * Debug/Info/Warning/Error as 0..3, so "Debug" wrote ERROR. */
    const Setting *lvl = find("SHADOW_JOURNAL_NIVEAU");
    if (lvl) {
        eq_int((int)lvl->choices.size(), 5, "five log levels, ERROR..TRACE");
        eq_str(lvl->choices.value(0), QStringLiteral("Errors only"),
               "choice 0 is ERROR, as journal.h numbers it");
        eq_str(lvl->choices.value(3), QStringLiteral("Debug"),
               "choice 3 is DEBUG");
        eq_str(settingWrite(*lvl, 3), QStringLiteral("3"),
               "choosing Debug writes 3 (DEBUG), not 0 (ERROR)");
        eq_int(settingDisplayIndex(*lvl, nullptr), 2,
               "absent shows Information, journal.c's initial threshold");
    }

    /* `getenv("SHADOW_CURL_VERBOSE") != NULL`: any value enables it. */
    const Setting *curl = find("SHADOW_CURL_VERBOSE");
    if (curl) {
        eq_str(settingWrite(*curl, 0), QString(),
               "off UNSETS SHADOW_CURL_VERBOSE: writing \"0\" would ENABLE it");
    }

    const Setting *itcp = find("SHADOW_INPUT_TCP");
    if (itcp) {
        eq_int(itcp->def, 1, "SHADOW_INPUT_TCP is ON by default (atoi : 1)");
    }

    const Setting *ec = find("SHADOW_ERR_CONCEAL");
    if (ec) {
        eq_int(settingDisplayIndex(*ec, nullptr), 0,
               "concealment absent shows None - the measured default");
        eq_str(settingWrite(*ec, 1), QStringLiteral("3"),
               "\"Guess lost blocks\" writes 3, the N40 value");
    }

    /* Campaign instruments, not settings: they must not come back. */
    for (const char *gone : { "SHADOW_INPUT_WHEEL", "SHADOW_HID_F3",
                              "SHADOW_INPUT_KEY_HOLD_MS", "SHADOW_PROBE_PORTS" }) {
        checks++;
        for (const Setting &s : settings()) {
            if (qstrcmp(s.env, gone) == 0) {
                failures++;
                std::printf("  FAIL %s is a campaign instrument, not a setting\n",
                            gone);
            }
        }
    }

    /* ======================== RULE 4: the value is not the index ========== */

    const Setting *cur = find("SHADOW_CURSOR_MODE");
    if (cur) {
        /* `cursor_state.c` compares with strcmp against these words; an index
         * would hit the final `else` and select the centred default, which is
         * ALSO index 0 - so only the written string can catch the mistake. */
        eq_str(settingWrite(*cur, 0), QStringLiteral("centered"),
               "choice 0 writes the word, not \"0\"");
        eq_str(settingWrite(*cur, 2), QStringLiteral("delta"), "choice 2 writes \"delta\"");
        eq_int(settingValue(*cur, "delta"), 2, "the word reads back to its index");
        eq_int(settingValue(*cur, "0"), -1,
               "\"0\" is NOT a value for this key and must not read as one");
    }

    const Setting *ac = find("SHADOW_AUDIO_CODEC");
    if (ac) {
        eq_str(settingWrite(*ac, 0), QStringLiteral("1"),
               "the first choice writes 1 (Opus): 0 is not a value");
        eq_str(settingWrite(*ac, 1), QStringLiteral("2"), "the second writes 2 (FLAC)");
        eq_int(settingValue(*ac, "0"), -1, "0 is not a codec");
    }

    const Setting *fps = find("SHADOW_FPS");
    if (fps) {
        eq_str(settingWrite(*fps, 0), QString(), "Automatic frame rate UNSETS");
        eq_str(settingWrite(*fps, 2), QStringLiteral("60"), "the 60 fps preset writes 60");
        eq_int(settingValue(*fps, "60"), 2, "60 reads back as the preset");
        eq_int(settingValue(*fps, "75"), -1,
               "a value with no preset reads as -1 (the window shows it as custom)");
    }

    /* ============================ RULE 5: absent shows core's default ===== */

    const Setting *nack = find("SHADOW_NACK");
    if (nack) {
        eq_int(settingDisplayIndex(*nack, nullptr), 1,
               "a default-ON toggle is drawn CHECKED when absent");
        eq_int(settingDisplayIndex(*nack, "0"), 0, "and unchecked when set to 0");
    }

    const Setting *hw = find("SHADOW_HWACCEL");
    if (hw) {
        eq_str(settingWrite(*hw, 0), QString(), "\"Automatic\" UNSETS the variable");
        eq_str(settingWrite(*hw, 1), QStringLiteral("0"), "\"Off\" writes 0");
        eq_int(settingDisplayIndex(*hw, nullptr), 0, "absent SHOWS as Automatic");
        eq_int(settingDisplayIndex(*hw, "1"), 2, "\"1\" shows as On");
        eq_int(settingValue(*hw, nullptr), -1,
               "settingValue still reports absent as -1 - a different question");
        eq_int(settingDefaultChoice(*hw), -1,
               "\"Automatic\" is not ALSO labelled \"(default)\"");
    }

    if (clip) {
        eq_int(settingDisplayIndex(*clip, nullptr), 1,
               "the clipboard absent shows Both ways (clip_dir_from_env)");
        eq_int(settingDefaultChoice(*clip), 1, "and labels Both ways as the default");
    }

    const Setting *reorder = find("SHADOW_REORDER_MS");
    if (reorder) {
        eq_int(settingDisplayIndex(*reorder, nullptr), 150,
               "a number absent shows its default value (G35's 150), not 0");
    }

    const Setting *vol = find("SHADOW_VOLUME");
    if (vol) {
        /* No fixed default: the control needs an "automatic" position, and
         * that position must unset. */
        eq_int(halyard::numberFloor(*vol), -1, "a number without a default gets a floor");
        eq_int(settingDisplayIndex(*vol, nullptr), -1, "absent shows that floor");
        eq_str(settingWrite(*vol, -1), QString(), "the floor UNSETS");
        eq_str(settingWrite(*vol, 0), QStringLiteral("0"), "0 % is a real value, written");
    }

    /* ======================================================== Flags ===== */

    const Setting *cats = find("SHADOW_JOURNAL_CATEGORIES");
    if (cats) {
        const int all = halyard::flagsAll(*cats);
        eq_int(all, 0x3ff, "ten categories, ten bits");
        eq_int(settingDisplayIndex(*cats, nullptr), all, "absent shows every category");
        eq_int(settingValue(*cats, "0x6"), 0x6, "hex is read the way strtoul base 0 reads it");
        eq_int(settingValue(*cats, "6"), 0x6, "decimal too");
        eq_int(settingValue(*cats, "0"), -1, "0 reads as -1: core ignores it");
        eq_str(settingWrite(*cats, 0x6), QStringLiteral("0x6"), "a subset is written in hex");
        eq_str(settingWrite(*cats, all), QString(), "every bit = core's default = unset");
        eq_str(settingWrite(*cats, 0), QString(),
               "no bit is unset too: core would ignore a 0 mask anyway");
    }

    /* ============================================= TABLE INVARIANTS ===== */

    ok(settings().size() >= 50,
       "the table covers the exposed set, not one session's additions");

    QStringList seen;
    for (const Setting &s : settings()) {
        const char *who = s.env ? s.env : "(null)";
        seen << QString::fromUtf8(who);

        checks++;
        if (!s.env || !*s.env || !s.page || !*s.page || !s.group || !*s.group ||
            !s.label || !*s.label || !s.description || !*s.description) {
            failures++;
            std::printf("  FAIL %s: an empty env/page/group/label/description\n", who);
        }

        /* A description that says what it DOES rather than what it COSTS is
         * the failure mode the table exists to avoid. */
        checks++;
        if (s.description && qstrlen(s.description) < 60) {
            failures++;
            std::printf("  FAIL %s: the description is too short to say what it costs\n",
                        who);
        }

        checks++;
        if (!s.values.isEmpty() && s.values.size() != s.choices.size()) {
            failures++;
            std::printf("  FAIL %s: %d choices but %d values\n", who,
                        (int)s.choices.size(), (int)s.values.size());
        }

        checks++;
        if ((s.kind == Setting::Kind::Choice || s.kind == Setting::Kind::Flags) &&
            s.choices.size() < 2) {
            failures++;
            std::printf("  FAIL %s: %d options\n", who, (int)s.choices.size());
        }

        checks++;
        if (s.kind == Setting::Kind::Number && s.min >= s.max) {
            failures++;
            std::printf("  FAIL %s: number range [%d, %d]\n", who, s.min, s.max);
        }

        /* `def` must be something the control can show. A default of 150 on a
         * row whose maximum is 100 would be clamped by the spin box, which is
         * the "setting nobody chose" rule 2 forbids. */
        checks++;
        bool defOk = true;
        if (s.def >= 0) {
            switch (s.kind) {
            case Setting::Kind::Toggle: defOk = s.def <= 1; break;
            case Setting::Kind::Choice: defOk = s.def < s.choices.size(); break;
            case Setting::Kind::Number: defOk = s.def >= s.min && s.def <= s.max; break;
            case Setting::Kind::Text:
            case Setting::Kind::Flags:  defOk = false; break;
            }
        }
        if (!defOk) {
            failures++;
            std::printf("  FAIL %s: def %d cannot be shown\n", who, s.def);
        }

        /* A toggle must say what it is when absent: an unknown default would
         * have to be drawn as something, and whatever it is drawn as is a
         * guess presented as a fact. */
        checks++;
        if (s.kind == Setting::Kind::Toggle && s.def < 0) {
            failures++;
            std::printf("  FAIL %s: a toggle with no recorded default\n", who);
        }

        /* A plain choice (no `values`) without a default shows nothing when
         * absent - the empty combo box QT5 was reported for. */
        checks++;
        bool unsets = false;
        for (const QString &v : s.values) if (v.isEmpty()) unsets = true;
        if (s.kind == Setting::Kind::Choice && !unsets && s.def < 0) {
            failures++;
            std::printf("  FAIL %s: a choice that shows nothing when absent\n", who);
        }

        checks++;
        int empties = 0;
        for (const QString &v : s.values) if (v.isEmpty()) empties++;
        if (empties > 1) {
            failures++;
            std::printf("  FAIL %s: %d choices unset the variable\n", who, empties);
        }

        checks++;
        if (s.unsetValue >= 0) {
            bool reachable = true;
            switch (s.kind) {
            case Setting::Kind::Toggle: reachable = s.unsetValue <= 1; break;
            case Setting::Kind::Choice: reachable = s.unsetValue < s.choices.size(); break;
            case Setting::Kind::Number:
                reachable = s.unsetValue >= s.min && s.unsetValue <= s.max; break;
            default: break;
            }
            if (!reachable) {
                failures++;
                std::printf("  FAIL %s: unsetValue %d is unreachable\n", who,
                            s.unsetValue);
            }
        }
    }

    {
        QStringList uniq = seen;
        uniq.removeDuplicates();
        eq_int(uniq.size(), seen.size(), "no variable appears twice");
    }

    /* Every page has a structure (QT5's "category within the category"), and
     * every group is worth a header. A group of one row on a page that has
     * others is allowed; a page with one row is not. */
    for (const QString &page : settingsPages()) {
        int n = 0;
        for (const Setting &s : settings())
            if (QString::fromUtf8(s.page) == page) n++;
        checks++;
        if (n < 2) {
            failures++;
            std::printf("  FAIL page \"%s\" has %d row(s)\n", qPrintable(page), n);
        }
        ok(!settingsGroups(page).isEmpty(), "every page has at least one group");
    }
    ok(settingsGroups(QStringLiteral("Video")).size() >= 3,
       "Video is split into sections, not one list");

    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
