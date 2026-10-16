#pragma once

#include "DiffModel.h"

#include <QBrush>
#include <QPlainTextEdit>

// One displayed row in a CodeView. Filler rows pad side-by-side views so that
// both sides stay aligned.
struct Row {
    LineKind kind = LineKind::Equal;
    bool filler = false;
    int oldNo = 0;
    int newNo = 0;
    int lineIndex = -1;     // index into FileDiff::lines, -1 for filler
    bool moved = false;
    bool moveStart = false; // first row of a moved block
    bool moveEnd = false;   // last row of a moved block
    QString moveLabel;      // short annotation drawn on the first row of a moved block
    QString tooltip;
    QString text;
    QVector<Span> highlights;
};

struct ViewPalette {
    QColor added, removed, moved;
    QColor addedEmph, removedEmph, movedEmph;
    QBrush filler;
    QColor gutterBg, gutterText, current;
};

// Read-only text view with a line-number gutter, line backgrounds and
// moved-block annotations.
class CodeView : public QPlainTextEdit
{
    Q_OBJECT
public:
    enum class Numbers { Old, New, Both };

    explicit CodeView(Numbers numbers, QWidget *parent = nullptr);

    void setRows(const QVector<Row> &rows, const ViewPalette &palette, const QFont &font, int tabWidth);
    const QVector<Row> &rows() const { return m_rows; }

    void setCurrentRange(int first, int last);
    void scrollToRow(int row);
    void selectRow(int row);
    int rowAt(const QPoint &viewportPos) const;

    int gutterWidth() const;
    void paintGutter(QPaintEvent *event);
    bool gutterEvent(QEvent *event);

signals:
    void moveJumpRequested(int row);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    bool viewportEvent(QEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    QColor rowColor(const Row &r) const;
    void updateGutterWidth();
    int maxLineNumber() const;

    class Gutter;
    Gutter *m_gutter = nullptr;
    Numbers m_numbers;
    QVector<Row> m_rows;
    ViewPalette m_pal;
    int m_currentFirst = -1;
    int m_currentLast = -1;
};
