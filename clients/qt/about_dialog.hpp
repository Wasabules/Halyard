/* about_dialog - the application's identity, in one place that can be read.
 *
 * === QT4 2026-10-03 — WHY A DIALOG AND NOT A MESSAGE BOX ===================
 *
 * `QMessageBox::about()` would be two lines of code. It is not enough, for one
 * reason that is not cosmetic: THE BUILD IDENTITY IS DIAGNOSTIC DATA. Every
 * report of a defect needs the version, the build date and the git hash, and
 * every time they are hard to copy, a report arrives without them and costs a
 * round trip to establish which binary was running.
 *
 * So the three atoms are shown separately (which is why `version.h` emits them
 * separately - see its header) and there is a button that puts all of it on the
 * clipboard in one go.
 *
 * It also states what this program is. An unofficial client for a commercial
 * service must say so where someone looks for it, not only in a README: the
 * name "Shadow" belongs to its owner, we reimplement a protocol we reverse
 * engineered, and nobody should be able to run this believing it is supported.
 */
#pragma once

#include <QDialog>

namespace halyard {

class AboutDialog : public QDialog {
    Q_OBJECT
public:
    explicit AboutDialog(QWidget *parent = nullptr);

private slots:
    void copyBuildInfo();

private:
    /* The same text the button copies, kept here so the label and the
     * clipboard cannot drift apart. */
    QString buildInfo() const;
};

}  // namespace halyard
