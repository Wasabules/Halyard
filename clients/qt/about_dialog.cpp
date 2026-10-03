#include "about_dialog.hpp"

#include <QClipboard>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSysInfo>
#include <QVBoxLayout>
#include <QtGlobal>

#include "theme.hpp"

extern "C" {
#include "core/version.h"
}

namespace halyard {

namespace {

QLabel *valueLabel(const QString &text, bool mono, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    /* Selectable, for the same reason the copy button exists: someone reading
     * a defect report over a call will read these out, and a label you cannot
     * select is a label that gets retyped wrong. */
    l->setTextInteractionFlags(Qt::TextSelectableByMouse |
                               Qt::TextSelectableByKeyboard);
    if (mono) l->setFont(theme::monoFont(parent));
    return l;
}

}  // namespace

AboutDialog::AboutDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("About %1").arg(QString::fromUtf8(SHADOW_APP_NAME)));
    setWindowIcon(theme::appTileIcon());
    setModal(true);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(theme::SpacePage, theme::SpacePage,
                             theme::SpacePage, theme::SpaceGroup);
    root->setSpacing(theme::SpaceGroup);

    /* --- the mark and the name ------------------------------------------- */
    auto *head = new QHBoxLayout;
    head->setSpacing(theme::SpaceGroup);

    auto *mark = new QLabel(this);
    mark->setPixmap(theme::appIcon(palette().color(QPalette::WindowText))
                        .pixmap(64, 64));
    mark->setFixedSize(64, 64);
    head->addWidget(mark, 0, Qt::AlignTop);

    auto *names = new QVBoxLayout;
    names->setSpacing(theme::SpaceTight);

    auto *name = new QLabel(QString::fromUtf8(SHADOW_APP_NAME), this);
    name->setFont(theme::titleFont(this));
    names->addWidget(name);

    auto *desc = new QLabel(QString::fromUtf8(SHADOW_APP_DESC), this);
    desc->setWordWrap(true);
    desc->setStyleSheet(theme::css(theme::muted(this)));
    names->addWidget(desc);

    head->addLayout(names, 1);
    root->addLayout(head);

    /* --- what this program is, stated plainly ---------------------------- */
    auto *disclaimer = new QLabel(
        tr("An unofficial client, not affiliated with or endorsed by the "
           "operator of the Shadow service. The streaming protocol is "
           "reimplemented from observation; nothing here is supported by "
           "anyone but its authors."),
        this);
    disclaimer->setWordWrap(true);
    disclaimer->setStyleSheet(theme::css(theme::muted(this)));
    root->addWidget(disclaimer);

    /* --- the build, atom by atom ----------------------------------------- */
    auto *form = new QFormLayout;
    form->setHorizontalSpacing(theme::SpaceGroup);
    form->setVerticalSpacing(theme::SpaceRow);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    form->addRow(tr("Version"),
                 valueLabel(QString::fromUtf8(SHADOW_VERSION), true, this));
    form->addRow(tr("Built"),
                 valueLabel(QString::fromUtf8(SHADOW_BUILD_DATE), true, this));
    form->addRow(tr("Commit"),
                 valueLabel(QString::fromUtf8(SHADOW_BUILD_HASH), true, this));
    form->addRow(tr("Qt"),
                 valueLabel(QStringLiteral("%1 (built against %2)")
                                .arg(QString::fromUtf8(qVersion()),
                                     QStringLiteral(QT_VERSION_STR)),
                            true, this));
    form->addRow(tr("Platform"),
                 valueLabel(QSysInfo::prettyProductName(), false, this));
    form->addRow(tr("Author"),
                 valueLabel(QString::fromUtf8(SHADOW_APP_AUTHOR), false, this));
    form->addRow(tr("Licence"),
                 valueLabel(QString::fromUtf8(SHADOW_APP_LICENSE), false, this));

    /* The URL as a link. `setOpenExternalLinks` hands it to the desktop's
     * browser; without it the label is a blue decoration that does nothing. */
    auto *url = new QLabel(QStringLiteral("<a href=\"%1\">%1</a>")
                               .arg(QString::fromUtf8(SHADOW_APP_URL)), this);
    url->setOpenExternalLinks(true);
    url->setTextInteractionFlags(Qt::TextBrowserInteraction);
    form->addRow(tr("Source"), url);

    root->addLayout(form);

    /* --- buttons --------------------------------------------------------- */
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *copy = buttons->addButton(tr("Copy build information"),
                                    QDialogButtonBox::ActionRole);
    copy->setToolTip(tr("Everything a defect report needs, as one block."));
    connect(copy, &QPushButton::clicked, this, &AboutDialog::copyBuildInfo);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    setMinimumWidth(480);
}

QString AboutDialog::buildInfo() const
{
    return QStringLiteral(
               "%1 %2\n"
               "built:    %3\n"
               "commit:   %4\n"
               "qt:       %5 (built against %6)\n"
               "platform: %7\n"
               "kernel:   %8 %9\n"
               "arch:     %10")
        .arg(QString::fromUtf8(SHADOW_APP_NAME),
             QString::fromUtf8(SHADOW_VERSION),
             QString::fromUtf8(SHADOW_BUILD_DATE),
             QString::fromUtf8(SHADOW_BUILD_HASH),
             QString::fromUtf8(qVersion()),
             QStringLiteral(QT_VERSION_STR),
             QSysInfo::prettyProductName(),
             QSysInfo::kernelType(),
             QSysInfo::kernelVersion())
        .arg(QSysInfo::currentCpuArchitecture());
}

void AboutDialog::copyBuildInfo()
{
    QGuiApplication::clipboard()->setText(buildInfo());
}

}  // namespace halyard
