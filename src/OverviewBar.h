#pragma once

#include <QColor>
#include <QVector>
#include <QWidget>

class QWheelEvent;

// The strip beside the diff showing where every change is in the whole file, with the
// visible area outlined. It is the diff's scrollbar too (the views have none): click to
// jump somewhere, drag the outlined area like a scrollbar's thumb, or turn the wheel.
class OverviewBar : public QWidget
{
    Q_OBJECT
public:
    // What one row looks like in the strip, for each half (left/old and right/new).
    enum class Mark : unsigned char { None, Added, Removed, Moved, Filler };

    explicit OverviewBar(QWidget *parent = nullptr);

    // `left` and `right` have one entry per row; pass the same vector twice for inline.
    void setRows(const QVector<Mark> &left, const QVector<Mark> &right);
    void setColors(const QColor &added, const QColor &removed, const QColor &moved, const QColor &match);
    void setVisibleRange(int firstRow, int rowCount);
    void setCurrentRange(int first, int last);
    void setMatchRows(const QVector<int> &rows);

    static constexpr int kWidth = 42;
    QSize sizeHint() const override { return {kWidth, 100}; }

signals:
    void rowRequested(int row);    // scroll so this row is in view (centred)
    void topRowRequested(int row); // scroll so this row is at the top (dragging the visible area)
    void wheelTurned(QWheelEvent *event);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    int rowAtY(int y) const;
    QColor colorFor(Mark m) const;

    QVector<Mark> m_left, m_right;
    QColor m_added, m_removed, m_moved, m_match;
    int m_visibleFirst = 0;
    int m_visibleCount = 0;
    int m_currentFirst = -1;
    int m_currentLast = -1;
    QVector<int> m_matches;
    int m_dragOffset = -1; // rows between the grab point and the visible area's top; -1 when not dragging it
};
