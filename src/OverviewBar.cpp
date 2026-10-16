#include "OverviewBar.h"

#include <QMouseEvent>
#include <QPainter>

OverviewBar::OverviewBar(QWidget *parent) : QWidget(parent)
{
    setFixedWidth(14);
    setCursor(Qt::PointingHandCursor);
    setToolTip(tr("Overview of all changes. Click to jump."));
}

void OverviewBar::setRows(const QVector<Mark> &left, const QVector<Mark> &right)
{
    m_left = left;
    m_right = right;
    m_matches.clear();
    m_currentFirst = m_currentLast = -1;
    update();
}

void OverviewBar::setColors(const QColor &added, const QColor &removed, const QColor &moved, const QColor &match)
{
    m_added = added;
    m_removed = removed;
    m_moved = moved;
    m_match = match;
    update();
}

void OverviewBar::setVisibleRange(int firstRow, int rowCount)
{
    if (firstRow == m_visibleFirst && rowCount == m_visibleCount)
        return;
    m_visibleFirst = firstRow;
    m_visibleCount = rowCount;
    update();
}

void OverviewBar::setCurrentRange(int first, int last)
{
    m_currentFirst = first;
    m_currentLast = last;
    update();
}

void OverviewBar::setMatchRows(const QVector<int> &rows)
{
    m_matches = rows;
    update();
}

QColor OverviewBar::colorFor(Mark m) const
{
    switch (m) {
    case Mark::Added: return m_added;
    case Mark::Removed: return m_removed;
    case Mark::Moved: return m_moved;
    case Mark::Filler:
    case Mark::None: break;
    }
    return {};
}

void OverviewBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), palette().color(QPalette::Base).darker(104));
    const int n = int(std::max(m_left.size(), m_right.size()));
    if (n == 0)
        return;
    const double h = height();
    const double rowH = h / n;
    const int w = width();
    const int half = w / 2;

    // Draw each row's mark; tiny rows are merged by pixel so long files stay fast.
    auto drawHalf = [&](const QVector<Mark> &marks, int x, int width) {
        for (int i = 0; i < marks.size(); ++i) {
            const QColor c = colorFor(marks[i]);
            if (!c.isValid())
                continue;
            int j = i;
            while (j + 1 < marks.size() && marks[j + 1] == marks[i])
                ++j;
            const int y0 = int(i * rowH);
            const int y1 = std::max(y0 + 2, int((j + 1) * rowH));
            p.fillRect(QRect(x, y0, width, y1 - y0), c);
            i = j;
        }
    };
    if (m_left.constData() == m_right.constData() || m_left == m_right) {
        drawHalf(m_left, 1, w - 2);
    } else {
        drawHalf(m_left, 1, half - 1);
        drawHalf(m_right, half, w - half - 1);
    }

    for (int r : m_matches) {
        const int y = int(r * rowH);
        p.fillRect(QRect(0, y, w, 2), m_match);
    }

    if (m_currentFirst >= 0) {
        const int y0 = int(m_currentFirst * rowH);
        const int y1 = std::max(y0 + 2, int((m_currentLast + 1) * rowH));
        p.fillRect(QRect(0, y0, 2, y1 - y0), palette().color(QPalette::Highlight));
    }

    // Visible area.
    if (m_visibleCount > 0 && m_visibleCount < n) {
        const int y0 = int(m_visibleFirst * rowH);
        const int y1 = std::min(height() - 1, int((m_visibleFirst + m_visibleCount) * rowH));
        QColor frame = palette().color(QPalette::Text);
        frame.setAlpha(110);
        QColor fill = frame;
        fill.setAlpha(28);
        p.fillRect(QRect(0, y0, w, y1 - y0), fill);
        p.setPen(frame);
        p.drawRect(QRect(0, y0, w - 1, std::max(2, y1 - y0)));
    }
}

int OverviewBar::rowAtY(int y) const
{
    const int n = int(std::max(m_left.size(), m_right.size()));
    if (n == 0 || height() <= 0)
        return -1;
    return std::clamp(int(double(y) / height() * n), 0, n - 1);
}

void OverviewBar::mousePressEvent(QMouseEvent *event)
{
    const int row = rowAtY(int(event->position().y()));
    if (row >= 0)
        emit rowRequested(row);
}

void OverviewBar::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton)
        mousePressEvent(event);
}
