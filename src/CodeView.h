#pragma once

#include "DiffModel.h"

#include <QBrush>
#include <QPlainTextEdit>

// One displayed row in a CodeView. Filler rows pad side-by-side views so that
// both sides stay aligned; fold rows stand in for a collapsed run of unchanged lines.
struct Row {
    LineKind kind = LineKind::Equal;
    bool filler = false;
    bool fold = false;
    bool comment = false;   // a review-comment thread shown under the line it belongs to
    qint64 threadId = 0;    // comment rows: id of the thread's first comment
    int foldFirstLine = -1; // fold rows: first hidden FileDiff line
    int foldCount = 0;      // fold rows: number of hidden lines
    int oldNo = 0;
    int newNo = 0;
    int lineIndex = -1;     // index into FileDiff::lines, -1 for filler and fold rows
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
    QColor fold;
    QColor gutterBg, gutterText, current;
    QColor match, currentMatch;
    QColor comment;
};

// A search hit to highlight: row in this view, character range in that row.
struct ViewHit {
    int row = 0;
    int start = 0;
    int length = 0;
};

// Read-only (or, for the working-tree side, editable) text view with a line-number
// gutter, line backgrounds, moved-block annotations, folds and search highlights.
class CodeView : public QPlainTextEdit
{
    Q_OBJECT
public:
    enum class Numbers { Old, New, Both };

    explicit CodeView(Numbers numbers, QWidget *parent = nullptr);

    void setRows(const QVector<Row> &rows, const ViewPalette &palette, const QFont &font, int tabWidth);
    const QVector<Row> &rows() const { return m_rows; }

    void setSyntax(const QString &language, bool darkTheme);
    void setShowWhitespace(bool on);
    void setWordWrap(bool on);
    bool wordWrap() const { return m_wrap; }

    // Word wrap in side-by-side: visual lines a row needs at the current width
    // (ignoring padding), and extra blank visual lines to append so both sides match.
    int naturalLineCount(int row) const;
    void setWrapPadding(const QVector<int> &extraLines);

    void setSearchHits(const QVector<ViewHit> &hits, int current);

    // Editing (working-tree side only). Lines added by the user are shown as additions.
    void setEditing(bool on);
    bool isEditing() const { return m_editing; }
    bool isModified() const;
    QStringList editedLines() const;

    void setCurrentRange(int first, int last);
    void scrollToRow(int row);
    void selectRow(int row);
    int rowAt(const QPoint &viewportPos) const;
    int topRow() const;

    int gutterWidth() const;
    void paintGutter(QPaintEvent *event);
    bool gutterEvent(QEvent *event);

signals:
    void moveJumpRequested(int row);
    void foldClicked(int row);
    void contextMenuRequested(int row, const QPoint &globalPos);
    void modifiedChanged(bool modified);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    bool viewportEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    QColor rowColor(const Row &r) const;
    void updateGutterWidth();
    int maxLineNumber() const;
    void applyTextOptions();
    void rebuildRowsAfterEdit();
    static QString stripPadding(const QString &s);

    class Gutter;
    Gutter *m_gutter = nullptr;
    Numbers m_numbers;
    QVector<Row> m_rows;
    QVector<int> m_padding;
    ViewPalette m_pal;
    QObject *m_highlighter = nullptr;
    QString m_language;
    bool m_dark = false;
    bool m_showWhitespace = false;
    bool m_wrap = false;
    bool m_editing = false;
    bool m_rebuildingAfterEdit = false;
    int m_currentFirst = -1;
    int m_currentLast = -1;
};
