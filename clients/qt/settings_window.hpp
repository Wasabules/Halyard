/* SettingsWindow - a real, independent window, generated from the table.
 *
 * === QT3 2026-10-03 — A WINDOW, NOT A PAGE =================================
 *
 * A `QWidget` with `Qt::Window`, not a page of the main stack and not a modal
 * dialog: it stays open BESIDE a running stream (change one, watch, change the
 * next), it can move to another monitor, and the stream keeps its full-screen
 * state. Left rail of pages, right panel of rows - the shape the Borealis
 * settings screen arrived at after S79, when thirty-four rows in one column
 * could no longer be scanned.
 *
 * Per row it shows what a settings screen usually hides: whether the value is
 * **taken over** by the environment (the control is disabled and says so,
 * rather than accepting a click that will be ignored), and whether it applies
 * **live** or **on the next session** - most of these are read once and
 * cached, and a window that implies otherwise is lying.
 *
 * === QT4 — TWO REPORTED DEFECTS ============================================
 *
 *   1. The row's label was "<label>\n<env>", so the screen showed
 *      `SHADOW_JOURNAL_NIVEAU` as the setting's name. The variable stays
 *      visible - users of this client read logs, and a row nobody can tie to a
 *      `SHADOW_*` log line is a row nobody can reason about - but as a
 *      secondary mono tag under a clean label.
 *   2. Descriptions were `color: palette(mid)`: dark grey on dark. Every colour
 *      now comes from theme.hpp, derived from the palette.
 *
 * === QT5 — "MAKE THE SETTINGS CLEAR" =======================================
 *
 * Reported: no defaults shown, raw numbers to type, nothing preselected, no
 * structure inside a page. So, per row:
 *
 *   - the control shows what core USES when nothing is set (`def`, read out of
 *     core), and the default choice is labelled "(default)";
 *   - a "Reset" button appears only when the row is actually set, and puts it
 *     back to core's default by REMOVING the variable - not by writing the
 *     default, which would pin today's default against tomorrow's;
 *   - a value set outside the window that matches no preset is shown as
 *     "Custom: <value>" rather than as an empty box;
 *   - units sit in the control, presets replace free numbers, the category
 *     bitmask is checkboxes, a file path has a Browse button;
 *   - each page is split into the table's groups, with a heading each.
 *
 * And the window is translated (i18n.hpp): a "General" page holds the
 * language selector, and the whole content is rebuilt on
 * `QEvent::LanguageChange`, so switching language needs no restart.
 */
#pragma once

#include <QVector>
#include <QKeySequence>
#include <QWidget>

#include <functional>

class QLabel;
class QKeySequenceEdit;
class QLineEdit;
class QListWidget;
class QStackedWidget;
class QVBoxLayout;

class SettingsWindow : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsWindow(QWidget *parent = nullptr);

signals:
    /* Emitted when a value changed, so the main window can say "takes effect
     * on the next session" where that is true. */
    void settingChanged(const QString &env, const QString &value, bool live);
    /* OV1 - the overlay hotkey was changed; the main window rebuilds its
     * shortcut. A client preference (QSettings), not a core env var. */
    /* KEY1 - a binding changed; the main window re-reads the whole table.
     * The name is kept from when the overlay was the only configurable key,
     * because renaming a signal is churn with no reader. */
    void overlayHotkeyChanged();

protected:
    void changeEvent(QEvent *e) override;

private:
    /* KEY1 - one editor per command, indexed by halyard::keys::ActionId. */
    QKeySequenceEdit *key_edits_[7] = {};
    QLabel           *key_conflict_ = nullptr;
    QKeySequence      storedShortcut(int i) const;
    void              refreshShortcutConflicts();

    void rebuild();
    QWidget *buildContent();
    QWidget *buildGeneralPage(QWidget *parent);

    void write(const QString &env, const QString &value);
    void writeRaw(const QString &env, const QString &value);
    void applyFilter(const QString &needle);
    void restoreDefaults();

    /* One row's widgets. The filter hides the whole row (a QFormLayout row is
     * two widgets, and hiding one leaves the other floating), and `refresh`
     * re-reads the environment after a reset - with signals blocked, so
     * showing a value never writes it. */
    struct Row {
        QString  env;
        QWidget *left  = nullptr;
        QWidget *right = nullptr;
        QString  haystack;     /* translated label + env + description, folded */
        int      page  = 0;
        std::function<void()> refresh;
    };

    QVBoxLayout    *root_    = nullptr;
    QWidget        *content_ = nullptr;
    QListWidget    *rail_    = nullptr;
    QStackedWidget *pages_   = nullptr;
    QLineEdit      *filter_  = nullptr;
    QVector<Row>    rows_;

    /* Survive a rebuild, so a language switch does not throw the user back
     * to the first page with an empty filter. */
    int     keepRow_ = 0;
    QString keepFilter_;
    bool    rebuildPending_ = false;
};
