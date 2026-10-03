/* The three painted pieces of the sign-in screen.
 *
 * === UI4 2026-10-03 — WHY THREE WIDGETS AND NOT THREE LABELS ===============
 *
 * The sign-in screen asks someone to carry eight characters from this window
 * to a browser. Everything here exists to make that transfer less likely to go
 * wrong, and each piece is painted because a QLabel cannot do the job:
 *
 *  - `QrView` - the one that removes the transfer entirely: scan it with a
 *    phone and the code is already in the URL. Borealis has had this since its
 *    first login screen (`boot_activity.cpp`); this client did not, so a PC
 *    user retyped what a console user scanned. Painted from the module grid
 *    (`qr_matrix`, QR1) rather than from the BMP writer the console needs,
 *    because routing a few hundred bits through a temporary file to draw them
 *    is a disk write, a path to invent and a file to delete on every refresh.
 *
 *  - `CodeCells` - one box per character. The code was one string with eight
 *    pixels of letter spacing, which separates the glyphs but does nothing
 *    about the actual failure: `O` against `0` and `I` against `1`. A box per
 *    character makes the count unambiguous and the grouping explicit, and
 *    being painted is what allows the zero to be slashed - a font cannot be
 *    relied on to distinguish it.
 *
 *  - `ValidityBar` - the 600 seconds, as something that visibly drains. The
 *    countdown was text, and text is read once; a bar is read every time the
 *    eye passes over it.
 */
#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

namespace halyard {

class QrView : public QWidget
{
    Q_OBJECT

public:
    explicit QrView(QWidget *parent = nullptr);

    /* Encodes `text`. Returns false when there is no QR to show - either
     * libqrencode is absent or the text would not encode - and the caller then
     * shows the URL as prose, exactly as the console client's fallback does. */
    bool setText(const QString &text);

    bool hasCode() const { return width_ > 0; }
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    QVector<unsigned char> modules_;   /* width_ * width_, 1 = dark */
    int width_ = 0;
};

class CodeCells : public QWidget
{
    Q_OBJECT

public:
    explicit CodeCells(QWidget *parent = nullptr);

    void setCode(const QString &code);
    QString code() const { return code_; }
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    QString code_;
};

class ValidityBar : public QWidget
{
    Q_OBJECT

public:
    explicit ValidityBar(QWidget *parent = nullptr);

    /* `total` is what the server granted, `left` what remains. */
    void setRemaining(int left, int total);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    int left_ = 0, total_ = 0;
};

}  // namespace halyard
