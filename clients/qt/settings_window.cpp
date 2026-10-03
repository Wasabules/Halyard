/* SettingsWindow - see settings_window.hpp for the shape and for the reported
 * defects this file was reshaped around. */
#include "settings_window.hpp"

#include "shortcuts.hpp"

#include <QApplication>

/* === KEY1 — THE SHORTCUT LABELS, FOR lupdate ==============================
 *
 * The rows are built from `halyard::keys::actions()`, so the call is
 * `tr(a.label)` with a pointer resolved at RUNTIME - which lupdate cannot
 * see, and an unseen string renders in English however complete the French
 * catalogue is. Declaring them here puts the seven in the catalogue under
 * exactly the context `tr()` looks them up in.
 *
 * They are not read: the array exists so the scanner has something to find.
 * `test_qt_shortcuts` asserts the table's size, which is what makes a label
 * added there and forgotten here a test failure rather than an English word
 * in a French window. */
[[maybe_unused]] static const char *const kShortcutLabelsForLupdate[] = {
    QT_TRANSLATE_NOOP("SettingsWindow", "Fullscreen"),
    QT_TRANSLATE_NOOP("SettingsWindow", "Open the overlay"),
    QT_TRANSLATE_NOOP("SettingsWindow", "Metrics window"),
    QT_TRANSLATE_NOOP("SettingsWindow", "File transfer"),
    QT_TRANSLATE_NOOP("SettingsWindow", "Settings"),
    QT_TRANSLATE_NOOP("SettingsWindow", "Take a screenshot"),
    QT_TRANSLATE_NOOP("SettingsWindow", "Disconnect"),
};
static_assert(sizeof kShortcutLabelsForLupdate / sizeof *kShortcutLabelsForLupdate
                  == halyard::keys::ActCount,
              "a shortcut was added to the table without a translatable label");

#include "i18n.hpp"
#include "settings_model.hpp"
#include "settings_store.hpp"
#include "theme.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QListWidget>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cstdlib>
#include <memory>

extern "C" {
#include "core/services/env_override.h"
#include "core/services/win_compat.h"   /* setenv/unsetenv on Windows */
}

using halyard::Setting;
namespace theme = halyard::theme;

namespace {

/* The table's strings are marked QT_TRANSLATE_NOOP("Settings", ...); this is
 * the other half of that contract. */
QString T(const char *s) { return QCoreApplication::translate("Settings", s); }
QString T(const QString &s) { return T(s.toUtf8().constData()); }

const char *envRaw(const Setting &s) { return std::getenv(s.env); }

QLabel *mutedLabel(const QString &text, QWidget *parent, bool italic = false)
{
    auto *l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setStyleSheet(theme::css(theme::muted(parent)) +
                     (italic ? QStringLiteral(" font-style: italic;") : QString()));
    return l;
}

/* The variable's name as a secondary tag (QT4 defect 1): mono because it is an
 * identifier, muted because it is not the row's name, selectable because it
 * gets copied into env.txt and into issues. */
QLabel *envTag(const QString &env, QWidget *parent)
{
    auto *tag = new QLabel(env, parent);
    tag->setFont(theme::monoFont(parent));
    tag->setStyleSheet(theme::css(theme::muted(parent)));
    tag->setTextInteractionFlags(Qt::TextSelectableByMouse);
    tag->setToolTip(SettingsWindow::tr(
        "The environment variable this row writes - what the log and env.txt "
        "call it."));
    return tag;
}

/* What core does when nothing is set, in words, or empty when that is not a
 * single value (an "Automatic" entry already says it). */
QString defaultText(const Setting &s)
{
    switch (s.kind) {
    case Setting::Kind::Toggle:
        if (s.def < 0) return QString();
        return s.def ? SettingsWindow::tr("On") : SettingsWindow::tr("Off");
    case Setting::Kind::Choice: {
        const int d = halyard::settingDefaultChoice(s);
        return d >= 0 ? T(s.choices.at(d)) : QString();
    }
    case Setting::Kind::Number:
        if (s.def < 0) return QString();
        return s.unit ? QStringLiteral("%1 %2").arg(s.def).arg(T(s.unit))
                      : QString::number(s.def);
    case Setting::Kind::Flags:
        return SettingsWindow::tr("All");
    case Setting::Kind::Text:
        return QString();
    }
    return QString();
}

}  // namespace

SettingsWindow::SettingsWindow(QWidget *parent)
    : QWidget(parent, Qt::Window)
{
    setWindowIcon(theme::appTileIcon());
    resize(1000, 720);
    root_ = new QVBoxLayout(this);
    root_->setContentsMargins(0, 0, 0, 0);
    rebuild();
}

/* ============================================================ rebuilding */

void SettingsWindow::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::LanguageChange && content_) {
        /* Deferred and coalesced. The language combo box that caused this
         * event is part of the content being rebuilt, and deleting a widget
         * from inside its own signal is undefined; installing two translators
         * (ours and Qt's) also sends the event twice. */
        if (!rebuildPending_) {
            rebuildPending_ = true;
            QTimer::singleShot(0, this, [this] {
                rebuildPending_ = false;
                rebuild();
            });
        }
    }
    QWidget::changeEvent(e);
}

void SettingsWindow::rebuild()
{
    if (rail_)   keepRow_ = qMax(0, rail_->currentRow());
    if (filter_) keepFilter_ = filter_->text();

    setWindowTitle(tr("Halyard settings"));
    rows_.clear();
    if (content_) {
        root_->removeWidget(content_);
        content_->deleteLater();
    }
    content_ = buildContent();
    root_->addWidget(content_);

    rail_->setCurrentRow(qBound(0, keepRow_, rail_->count() - 1));
    if (!keepFilter_.isEmpty()) filter_->setText(keepFilter_);
}

QWidget *SettingsWindow::buildContent()
{
    auto *content = new QWidget(this);

    rail_ = new QListWidget(content);
    rail_->setFixedWidth(180);
    pages_ = new QStackedWidget(content);

    filter_ = new QLineEdit(content);
    filter_->setClearButtonEnabled(true);
    filter_->setPlaceholderText(
        tr("Search - a setting, a variable name, or a word from a description"));
    connect(filter_, &QLineEdit::textChanged, this, &SettingsWindow::applyFilter);

    /* --- General: what is the client's, not core's --------------------- */
    rail_->addItem(tr("General"));
    {
        auto *scroll = new QScrollArea(pages_);
        scroll->setWidget(buildGeneralPage(scroll));
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        pages_->addWidget(scroll);
    }

    /* --- one page per table page, one section per group ---------------- */
    const QStringList pageNames = halyard::settingsPages();
    for (int p = 0; p < pageNames.size(); p++) {
        const QString page = pageNames.at(p);
        const int pageIndex = p + 1;            /* General is 0 */
        rail_->addItem(T(page));

        auto *holder = new QWidget(pages_);
        auto *col = new QVBoxLayout(holder);
        col->setContentsMargins(theme::SpaceRow, theme::SpaceRow,
                                theme::SpaceGroup, theme::SpaceGroup);
        col->setSpacing(theme::SpaceRow);

        for (const QString &group : halyard::settingsGroups(page)) {
            /* The section heading: QT5's "category within the category". */
            auto *head = new QLabel(T(group), holder);
            head->setFont(theme::headingFont(holder));
            auto *rule = new QFrame(holder);
            rule->setFrameShape(QFrame::HLine);
            rule->setFrameShadow(QFrame::Plain);
            rule->setStyleSheet(theme::css(theme::muted(holder)));
            col->addSpacing(theme::SpaceTight);
            col->addWidget(head);
            col->addWidget(rule);

            auto *form = new QFormLayout;
            form->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
            form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
            form->setHorizontalSpacing(theme::SpaceGroup);
            form->setVerticalSpacing(theme::SpaceGroup);
            form->setContentsMargins(0, theme::SpaceTight, 0, theme::SpaceRow);

            QVector<Row *> groupRows;
            for (const Setting &s : halyard::settings()) {
                if (QString::fromUtf8(s.page) != page ||
                    QString::fromUtf8(s.group) != group) continue;

                const QString env = QString::fromUtf8(s.env);
                const bool forced = env_override_active(s.env) != 0;

                /* --- left: the clean name, then the variable ----------- */
                auto *left = new QWidget(holder);
                auto *ll = new QVBoxLayout(left);
                ll->setContentsMargins(0, 0, 0, 0);
                ll->setSpacing(theme::SpaceTight);
                auto *name = new QLabel(T(s.label), left);
                name->setWordWrap(true);
                ll->addWidget(name);
                ll->addWidget(envTag(env, left));
                ll->addStretch(1);
                left->setFixedWidth(240);

                /* --- right: control line, description, status ---------- */
                auto *right = new QWidget(holder);
                auto *rl = new QVBoxLayout(right);
                rl->setContentsMargins(0, 0, 0, 0);
                rl->setSpacing(theme::SpaceTight);

                auto *line = new QHBoxLayout;
                line->setSpacing(theme::SpaceRow);
                rl->addLayout(line);

                auto *reset = new QToolButton(right);
                reset->setText(tr("Reset"));
                reset->setToolTip(tr("Remove the variable, so core uses its "
                                     "own default again"));
                reset->setAutoRaise(true);

                auto *status = new QLabel(right);
                status->setWordWrap(true);

                rl->addWidget(mutedLabel(T(s.description), right));
                rl->addWidget(status);

                /* The status line and the reset button both depend on the
                 * CURRENT state, so they are recomputed by `refresh`. */
                auto refreshChrome = [this, &s, reset, status, forced, right] {
                    const bool set = envRaw(s) && *envRaw(s);
                    reset->setVisible(set && !forced);
                    QString text;
                    if (forced) {
                        text = tr("Set outside the application (environment or "
                                  "env.txt) - this window cannot change it.");
                        status->setStyleSheet(theme::css(theme::overridden(right)) +
                                              QStringLiteral(" font-style: italic;"));
                    } else {
                        QStringList parts;
                        const QString d = defaultText(s);
                        if (!d.isEmpty()) parts << tr("Default: %1").arg(d);
                        parts << (s.appliesLive ? tr("applies immediately")
                                                : tr("applies on the next session"));
                        text = parts.join(QStringLiteral("  ·  "));
                        status->setStyleSheet(theme::css(theme::muted(right)) +
                                              QStringLiteral(" font-style: italic;"));
                    }
                    status->setText(text);
                };

                std::function<void()> refreshControl;
                QWidget *control = nullptr;

                switch (s.kind) {
                case Setting::Kind::Toggle: {
                    auto *cb = new QCheckBox(right);
                    connect(cb, &QCheckBox::toggled, this, [this, &s](bool on) {
                        write(QString::fromUtf8(s.env),
                              halyard::settingWrite(s, on ? 1 : 0));
                    });
                    refreshControl = [cb, &s] {
                        const QSignalBlocker b(cb);
                        cb->setChecked(halyard::settingDisplayIndex(s, envRaw(s)) == 1);
                    };
                    control = cb;
                    break;
                }
                case Setting::Kind::Choice: {
                    auto *box = new QComboBox(right);
                    box->setSizeAdjustPolicy(QComboBox::AdjustToContents);
                    const int defChoice = halyard::settingDefaultChoice(s);
                    for (int i = 0; i < s.choices.size(); i++) {
                        const QString label = T(s.choices.at(i));
                        box->addItem(i == defChoice ? tr("%1 (default)").arg(label)
                                                    : label, i);
                    }
                    connect(box, &QComboBox::currentIndexChanged, this,
                            [this, &s, box](int i) {
                        const QVariant d = box->itemData(i);
                        if (!d.isValid() || d.toInt() < 0) return;  /* "Custom" */
                        write(QString::fromUtf8(s.env),
                              halyard::settingWrite(s, d.toInt()));
                    });
                    refreshControl = [box, &s] {
                        const QSignalBlocker b(box);
                        /* Drop a previous "Custom" entry before deciding. */
                        const int last = box->count() - 1;
                        if (last >= 0 && box->itemData(last).toInt() < 0)
                            box->removeItem(last);
                        const char *raw = envRaw(s);
                        const int idx = halyard::settingDisplayIndex(s, raw);
                        if (idx >= 0) {
                            box->setCurrentIndex(box->findData(idx));
                        } else if (raw && *raw) {
                            /* Set elsewhere to a value with no preset: shown as
                             * it is, not as an empty box, and not writable. */
                            box->addItem(tr("Custom: %1").arg(QString::fromUtf8(raw)), -1);
                            box->setCurrentIndex(box->count() - 1);
                        } else {
                            box->setCurrentIndex(-1);
                        }
                    };
                    control = box;
                    break;
                }
                case Setting::Kind::Number: {
                    auto *sp = new QSpinBox(right);
                    sp->setRange(halyard::numberFloor(s), s.max);
                    if (s.unit) sp->setSuffix(QStringLiteral(" ") + T(s.unit));
                    if (halyard::numberFloor(s) < s.min)
                        sp->setSpecialValueText(tr("Automatic"));
                    sp->setMinimumWidth(110);
                    connect(sp, &QSpinBox::valueChanged, this, [this, &s](int v) {
                        write(QString::fromUtf8(s.env), halyard::settingWrite(s, v));
                    });
                    refreshControl = [sp, &s] {
                        const QSignalBlocker b(sp);
                        const int idx = halyard::settingDisplayIndex(s, envRaw(s));
                        sp->setValue(idx >= halyard::numberFloor(s) ? idx
                                                                    : halyard::numberFloor(s));
                    };
                    control = sp;
                    break;
                }
                case Setting::Kind::Flags: {
                    auto *box = new QWidget(right);
                    auto *grid = new QGridLayout(box);
                    grid->setContentsMargins(0, 0, 0, 0);
                    grid->setHorizontalSpacing(theme::SpaceGroup);
                    auto checks = std::make_shared<QVector<QCheckBox *>>();
                    for (int i = 0; i < s.choices.size(); i++) {
                        auto *cb = new QCheckBox(T(s.choices.at(i)), box);
                        grid->addWidget(cb, i / 4, i % 4);
                        checks->append(cb);
                        connect(cb, &QCheckBox::toggled, this, [this, &s, checks] {
                            int mask = 0;
                            for (int b = 0; b < checks->size(); b++)
                                if (checks->at(b)->isChecked()) mask |= 1 << b;
                            write(QString::fromUtf8(s.env), halyard::settingWrite(s, mask));
                        });
                    }
                    refreshControl = [checks, &s] {
                        int mask = halyard::settingDisplayIndex(s, envRaw(s));
                        if (mask < 0) mask = halyard::flagsAll(s);
                        for (int b = 0; b < checks->size(); b++) {
                            const QSignalBlocker blk(checks->at(b));
                            checks->at(b)->setChecked(mask & (1 << b));
                        }
                    };
                    control = box;
                    break;
                }
                case Setting::Kind::Text: {
                    auto *le = new QLineEdit(right);
                    le->setClearButtonEnabled(true);
                    le->setFont(theme::monoFont(right));
                    le->setPlaceholderText(tr("Not set"));
                    /* editingFinished, not textChanged: setenv on every keystroke
                     * lets a session that starts mid-edit read half a path. */
                    connect(le, &QLineEdit::editingFinished, this, [this, &s, le] {
                        write(QString::fromUtf8(s.env), le->text().trimmed());
                    });
                    if (s.isPath) {
                        auto *browse = new QPushButton(tr("Browse..."), right);
                        connect(browse, &QPushButton::clicked, this, [this, le, &s] {
                            const QString f = QFileDialog::getOpenFileName(
                                this, T(s.label), le->text(),
                                tr("Certificates (*.pem *.crt);;All files (*)"));
                            if (f.isEmpty()) return;
                            le->setText(QDir::toNativeSeparators(f));
                            write(QString::fromUtf8(s.env), le->text());
                        });
                        browse->setEnabled(!forced);
                        line->addWidget(le, 1);
                        line->addWidget(browse);
                    }
                    refreshControl = [le, &s] {
                        const QSignalBlocker b(le);
                        const char *raw = envRaw(s);
                        le->setText(raw ? QString::fromUtf8(raw) : QString());
                    };
                    control = le;
                    break;
                }
                }

                if (!(s.kind == Setting::Kind::Text && s.isPath)) {
                    line->addWidget(control);
                    line->addStretch(1);
                }
                line->addWidget(reset);

                if (forced) {
                    control->setEnabled(false);
                    control->setToolTip(tr("Set outside the application - the "
                                           "environment has priority over this "
                                           "window."));
                }

                Row row;
                row.env = env;
                row.left = left;
                row.right = right;
                row.page = pageIndex;
                row.haystack = (T(s.label) + QLatin1Char(' ') + env + QLatin1Char(' ') +
                                T(s.description) + QLatin1Char(' ') + T(group))
                                   .toCaseFolded();
                row.refresh = [refreshControl, refreshChrome] {
                    refreshControl();
                    refreshChrome();
                };
                row.refresh();

                connect(reset, &QToolButton::clicked, this, [this, &s] {
                    write(QString::fromUtf8(s.env), QString());
                });

                form->addRow(left, right);
                rows_.append(row);
            }
            col->addLayout(form);
        }
        col->addStretch(1);

        auto *scroll = new QScrollArea(pages_);
        scroll->setWidget(holder);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        pages_->addWidget(scroll);
    }

    connect(rail_, &QListWidget::currentRowChanged,
            pages_, &QStackedWidget::setCurrentIndex);

    auto *footer = mutedLabel(
        tr("Settings are saved and restored at the next launch. Most are read "
           "when a session starts, so they take effect the next time you "
           "connect; each row says which. A variable set outside the "
           "application always wins over this window."), content);

    auto *split = new QHBoxLayout;
    split->setSpacing(theme::SpaceGroup);
    split->addWidget(rail_);
    split->addWidget(pages_, 1);

    auto *lay = new QVBoxLayout(content);
    lay->setContentsMargins(theme::SpaceGroup, theme::SpaceGroup,
                            theme::SpaceGroup, theme::SpaceRow);
    lay->setSpacing(theme::SpaceRow);
    lay->addWidget(filter_);
    lay->addLayout(split, 1);
    lay->addWidget(footer);
    return content;
}

/* ========================================================== General page */

QWidget *SettingsWindow::buildGeneralPage(QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *col = new QVBoxLayout(page);
    col->setContentsMargins(theme::SpaceRow, theme::SpaceRow,
                            theme::SpaceGroup, theme::SpaceGroup);
    col->setSpacing(theme::SpaceRow);

    auto section = [&](const QString &title) {
        auto *head = new QLabel(title, page);
        head->setFont(theme::headingFont(page));
        auto *rule = new QFrame(page);
        rule->setFrameShape(QFrame::HLine);
        rule->setFrameShadow(QFrame::Plain);
        rule->setStyleSheet(theme::css(theme::muted(page)));
        col->addSpacing(theme::SpaceTight);
        col->addWidget(head);
        col->addWidget(rule);
    };

    /* --- Language ------------------------------------------------------- */
    section(tr("Language"));
    {
        auto *form = new QFormLayout;
        form->setHorizontalSpacing(theme::SpaceGroup);
        auto *box = new QComboBox(page);
        box->setSizeAdjustPolicy(QComboBox::AdjustToContents);

        /* "Follow the system" first, and it SAYS what the system resolves to:
         * otherwise choosing it looks like doing nothing. */
        QString resolvedName = halyard::i18n::current();
        const auto langs = halyard::i18n::available();
        for (const auto &l : langs)
            if (l.code == halyard::i18n::current()) resolvedName = l.nativeName;
        box->addItem(tr("Follow the system (%1)").arg(resolvedName), QString());
        for (const auto &l : langs) box->addItem(l.nativeName, l.code);

        const int sel = box->findData(halyard::i18n::requested());
        box->setCurrentIndex(sel >= 0 ? sel : 0);
        connect(box, &QComboBox::currentIndexChanged, this, [box](int i) {
            halyard::i18n::setRequested(box->itemData(i).toString());
        });
        form->addRow(tr("Interface language"), box);
        col->addLayout(form);
        col->addWidget(mutedLabel(
            tr("Applies immediately to every window. Translations cover the "
               "interface and the settings; the session log stays in English, "
               "because it is what gets attached to a report."), page));
    }

    /* --- Audio output (Windows) ---------------------------------------- *
     *
     * Not a row of the settings table, because the choices are discovered at
     * runtime: the table is static data, and the list of output devices is
     * whatever is plugged in right now. It writes the same variable core reads
     * (SHADOW_WIN_AUDIO_DEVICE, AUD-DEV1), through the same store, so it
     * persists and is overridden by env.txt exactly like every other setting.
     *
     * Windows only: the variable only steers the WASAPI path. QMediaDevices'
     * id() on Windows is the WASAPI endpoint ID, which is the string core hands
     * to IMMDeviceEnumerator::GetDevice - so the two line up without a mapping. */
#ifdef Q_OS_WIN
    section(tr("Audio output"));
    {
        auto *form = new QFormLayout;
        form->setHorizontalSpacing(theme::SpaceGroup);
        auto *box = new QComboBox(page);
        box->setSizeAdjustPolicy(QComboBox::AdjustToContents);

        const bool forced = env_override_active("SHADOW_WIN_AUDIO_DEVICE") != 0;
        box->addItem(tr("System default"), QString());
        for (const QAudioDevice &d : QMediaDevices::audioOutputs())
            box->addItem(d.description(), QString::fromUtf8(d.id()));

        const char *cur = std::getenv("SHADOW_WIN_AUDIO_DEVICE");
        const int sel = cur ? box->findData(QString::fromUtf8(cur)) : 0;
        box->setCurrentIndex(sel >= 0 ? sel : 0);
        box->setEnabled(!forced);
        connect(box, &QComboBox::currentIndexChanged, this, [this, box](int i) {
            writeRaw(QStringLiteral("SHADOW_WIN_AUDIO_DEVICE"),
                     box->itemData(i).toString());
        });
        form->addRow(tr("Play sound through"), box);
        col->addLayout(form);
        col->addWidget(mutedLabel(
            tr("Applies on the next session - the output is opened when a stream "
               "starts. A device that is unplugged later falls back to the system "
               "default rather than going silent."), page));
    }
#endif

    /* === D2 — THE THEME ================================================= *
     *
     * Three choices and no more. "Auto" leaves the palette Qt resolved from
     * the desktop alone, which is right almost always; the two overrides exist
     * for the case the desktop gets it wrong, which on Linux it regularly does
     * (a GTK dark theme that Qt reads as light).
     *
     * Applied at once AND stored: the palette is set, then the stylesheet is
     * rebuilt from it, because every colour in the sheet is derived from the
     * palette and a sheet left in place would still be painted for the other
     * theme. A restart is still mentioned because some already-built widgets
     * cache a colour they took at construction. */
    section(tr("Appearance"));
    {
        auto *form = new QFormLayout;
        form->setHorizontalSpacing(theme::SpaceGroup);
        auto *box = new QComboBox(page);
        box->addItem(tr("Follow the system"), 0);
        box->addItem(tr("Light"), 1);
        box->addItem(tr("Dark"), 2);
        QSettings st;
        const int cur = st.value(QStringLiteral("ui/theme"), 0).toInt();
        box->setCurrentIndex(box->findData(cur) >= 0 ? box->findData(cur) : 0);
        connect(box, &QComboBox::currentIndexChanged, this, [this, box](int i) {
            const int m = box->itemData(i).toInt();
            QSettings().setValue(QStringLiteral("ui/theme"), m);
            theme::applyThemeMode(static_cast<theme::ThemeMode>(m));
            if (qApp) qApp->setStyleSheet(theme::appStyleSheet(nullptr));
        });
        form->addRow(tr("Theme"), box);
        col->addLayout(form);
        col->addWidget(mutedLabel(
            tr("Applies at once. A few already-open windows keep a colour they "
               "took when they were built - restart if one looks wrong."), page));
    }

    /* === KEY1 — the keyboard shortcuts, all seven ======================= *
     *
     * One row per command, from `shortcuts.hpp`. What this section exists to
     * make possible, beyond changing a key: SEEING a collision. The keys used
     * to be set at seven separate call sites, so nothing had the whole set in
     * front of it and two commands could quietly share a chord - at which
     * point Qt fires neither and the pair simply stops working. */
    section(tr("Keyboard shortcuts"));
    {
        using namespace halyard::keys;
        auto *form = new QFormLayout;
        form->setHorizontalSpacing(theme::SpaceGroup);

        for (int i = 0; i < ActCount; i++) {
            const ActionDef &a = action(i);
            auto *edit = new QKeySequenceEdit(page);
            edit->setMaximumSequenceLength(1);   /* a single chord, like a game key */
            edit->setKeySequence(storedShortcut(i));
            key_edits_[i] = edit;

            connect(edit, &QKeySequenceEdit::editingFinished, this, [this, i, edit] {
                const QString seq =
                    edit->keySequence().toString(QKeySequence::PortableText);
                /* "-" and not "" for a deliberately cleared key: an ABSENT
                 * QSettings value means "never set" and must take the
                 * default, while a user who unbound a command must get
                 * nothing. The empty string cannot mean both. */
                QSettings().setValue(QString::fromUtf8(action(i).settingsKey),
                                     seq.isEmpty() ? QStringLiteral("-") : seq);
                refreshShortcutConflicts();
                emit overlayHotkeyChanged();   /* "a binding changed" - see the hpp */
            });

            /* The label says when a command is only meaningful in a stream,
             * rather than leaving someone to wonder why F12 did nothing on the
             * machine list. */
            const QString label = a.streamOnly
                ? tr("%1 (while streaming)").arg(tr(a.label))
                : tr(a.label);
            form->addRow(label, edit);
        }
        col->addLayout(form);

        key_conflict_ = mutedLabel(QString(), page);
        key_conflict_->setVisible(false);
        col->addWidget(key_conflict_);

        auto *reset = new QPushButton(tr("Restore the default shortcuts"), page);
        connect(reset, &QPushButton::clicked, this, [this] {
            using namespace halyard::keys;
            QSettings st;
            for (int i = 0; i < ActCount; i++) {
                st.remove(QString::fromUtf8(action(i).settingsKey));
                key_edits_[i]->setKeySequence(
                    QKeySequence(QString::fromUtf8(action(i).def)));
            }
            refreshShortcutConflicts();
            emit overlayHotkeyChanged();
        });
        auto *line = new QHBoxLayout;
        line->addWidget(reset);
        line->addStretch(1);
        col->addLayout(line);

        col->addWidget(mutedLabel(
            tr("Clearing a field unbinds the command. Changes take effect at "
               "once - no restart. The overlay is a frameless menu over the "
               "stream (volume, equaliser, which metrics show in the corner); "
               "it takes the keyboard while open, so its keys do not reach the "
               "VM."), page));

        refreshShortcutConflicts();
    }

    /* --- Defaults ------------------------------------------------------- */
    section(tr("Defaults"));
    {
        auto *btn = new QPushButton(tr("Restore all defaults..."), page);
        connect(btn, &QPushButton::clicked, this, &SettingsWindow::restoreDefaults);
        auto *line = new QHBoxLayout;
        line->addWidget(btn);
        line->addStretch(1);
        col->addLayout(line);
        col->addWidget(mutedLabel(
            tr("Removes every variable this window has set, so core uses its own "
               "defaults again. Variables set outside the application are not "
               "touched."), page));
    }

    col->addStretch(1);
    return page;
}

/* A runtime setting that is NOT a row of the table (the audio device, whose
 * choices are discovered live). Same store, same env.txt precedence, so it
 * persists and is overridden exactly like a table row - it just has no `Setting`
 * to look up. */
void SettingsWindow::writeRaw(const QString &env, const QString &value)
{
    const QByteArray key = env.toUtf8();
    if (env_override_active(key.constData()) != 0) return;
    if (value.isEmpty()) unsetenv(key.constData());
    else { const QByteArray v = value.toUtf8(); setenv(key.constData(), v.constData(), 1); }
    halyard::store::saveVariable(env, value);
    emit settingChanged(env, value, false);
}

void SettingsWindow::restoreDefaults()
{
    const auto answer = QMessageBox::question(
        this, tr("Restore all defaults"),
        tr("Remove every setting made in this window? Variables set outside the "
           "application are kept."));
    if (answer != QMessageBox::Yes) return;

    for (const Setting &s : halyard::settings()) {
        if (env_override_active(s.env) != 0) continue;
        unsetenv(s.env);
    }
    halyard::store::forgetAll();
    for (const Row &r : rows_) if (r.refresh) r.refresh();
}

/* =============================================================== writing */

void SettingsWindow::write(const QString &env, const QString &value)
{
    const QByteArray key = env.toUtf8();
    /* The environment has priority in core itself, so writing here would be
     * misleading even though it would not take effect. Refused. */
    if (env_override_active(key.constData()) != 0) return;

    const Setting *s = nullptr;
    for (const Setting &row : halyard::settings())
        if (env == QString::fromUtf8(row.env)) { s = &row; break; }
    if (!s) return;

    if (value.isEmpty()) {
        /* Fact 3: for these keys, absent means something different from 0. */
        unsetenv(key.constData());
    } else {
        const QByteArray val = value.toUtf8();
        setenv(key.constData(), val.constData(), 1);
    }
    halyard::store::saveVariable(env, value);

    /* Show the result of the write, not the click: a reset must put the
     * control back on the default, and a "Custom" entry must disappear once
     * a preset is chosen. */
    for (const Row &r : rows_) {
        if (r.env == env && r.refresh) r.refresh();
    }
    emit settingChanged(env, value, s->appliesLive);
}

void SettingsWindow::applyFilter(const QString &needle)
{
    const QString n = needle.trimmed().toCaseFolded();
    QVector<int> hits(rail_->count(), 0);
    hits[0] = n.isEmpty() ? 1 : 0;   /* General has no table rows */

    for (const Row &r : rows_) {
        const bool show = n.isEmpty() || r.haystack.contains(n);
        if (r.left)  r.left->setVisible(show);
        if (r.right) r.right->setVisible(show);
        if (show && r.page < hits.size()) hits[r.page]++;
    }

    /* Grey the empty pages rather than hide them: hiding would renumber the
     * rail and break the row -> page mapping. */
    for (int i = 0; i < rail_->count(); i++) {
        QListWidgetItem *it = rail_->item(i);
        it->setForeground(hits.value(i) > 0 || n.isEmpty()
                              ? QBrush(palette().color(QPalette::WindowText))
                              : QBrush(theme::muted(this)));
    }
    if (!n.isEmpty() && hits.value(rail_->currentRow()) == 0) {
        for (int i = 0; i < hits.size(); i++)
            if (hits[i] > 0) { rail_->setCurrentRow(i); break; }
    }
}

/* KEY1 - what is stored for `i`, or its default. Mirrors
 * `MainWindow::keySequenceFor`; the two are kept together deliberately rather
 * than shared, because this one must not depend on the main window and that
 * one must not depend on a window that may never be opened. */
QKeySequence SettingsWindow::storedShortcut(int i) const
{
    const auto &a = halyard::keys::action(i);
    const QString v = QSettings().value(QString::fromUtf8(a.settingsKey)).toString();
    if (v == QStringLiteral("-")) return QKeySequence();
    if (v.isEmpty()) return QKeySequence(QString::fromUtf8(a.def));
    return QKeySequence(v);
}

/* KEY1 - mark the rows that share a chord.
 *
 * The comparison is done on the text `QKeySequence::toString` produces, which
 * is the same normalisation Qt dispatches on - so the check agrees with what
 * actually happens rather than with a second opinion about what "Ctrl+M"
 * means. `halyard::keys::conflicting` is pure and tested. */
void SettingsWindow::refreshShortcutConflicts()
{
    using namespace halyard::keys;

    QStringList seqs;
    for (int i = 0; i < ActCount; i++)
        seqs << key_edits_[i]->keySequence().toString(QKeySequence::PortableText);

    const QVector<int> bad = conflicting(seqs);
    for (int i = 0; i < ActCount; i++)
        key_edits_[i]->setStyleSheet(
            bad.contains(i) ? QStringLiteral("border: 1px solid %1;")
                                  .arg(theme::bad(this).name())
                            : QString());

    if (bad.isEmpty()) {
        key_conflict_->setVisible(false);
        return;
    }
    QStringList names;
    for (int i : bad) names << tr(action(i).label);
    key_conflict_->setText(
        tr("Same key for: %1. Qt fires neither when two commands share a "
           "chord, so both stop working until one is changed.")
            .arg(names.join(QStringLiteral(", "))));
    key_conflict_->setStyleSheet(theme::css(theme::bad(this)));
    key_conflict_->setVisible(true);
}
