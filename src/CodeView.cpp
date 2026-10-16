#include "CodeView.h"

#include <QHelpEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QToolTip>

class CodeView::Gutter : public QWidget
{
public:
    explicit Gutter(CodeView *view) : QWidget(view), m_view(view) {}
    QSize sizeHint() const override { return {m_view->gutterWidth(), 0}; }

protected:
    void paintEvent(QPaintEvent *e) override { m_view->paintGutter(e); }
    bool event(QEvent *e) override { return m_view->gutterEvent(e) || QWidget::event(e); }
    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        const int row = m_view->rowAt(QPoint(0, int(e->position().y())));
        if (row >= 0 && row < m_view->rows().size() && m_view->rows()[row].moved)
            emit m_view->moveJumpRequested(row);
    }

private:
    CodeView *m_view;
};

namespace {
constexpr int kPad = 6;
constexpr int kMarkerWidth = 16;
} // namespace

CodeView::CodeView(Numbers numbers, QWidget *parent) : QPlainTextEdit(parent), m_numbers(numbers)
{
    m_gutter = new Gutter(this);
    setReadOnly(true);
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setWordWrapMode(QTextOption::NoWrap);
    setUndoRedoEnabled(false);
    setFrameShape(QFrame::NoFrame);
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect &rect, int dy) {
        if (dy)
            m_gutter->scroll(0, dy);
        else
            m_gutter->update(0, rect.y(), m_gutter->width(), rect.height());
    });
    updateGutterWidth();
}

int CodeView::maxLineNumber() const
{
    int m = 0;
    for (const Row &r : m_rows)
        m = std::max({m, r.oldNo, r.newNo});
    return m;
}

int CodeView::gutterWidth() const
{
    const int digits = std::max(3, int(QString::number(maxLineNumber()).size()));
    const int numberWidth = fontMetrics().horizontalAdvance(u'9') * digits;
    const int columns = m_numbers == Numbers::Both ? 2 : 1;
    return kPad + columns * (numberWidth + kPad) + kMarkerWidth + 2;
}

void CodeView::updateGutterWidth()
{
    setViewportMargins(gutterWidth(), 0, 0, 0);
    const QRect cr = contentsRect();
    m_gutter->setGeometry(QRect(cr.left(), cr.top(), gutterWidth(), cr.height()));
    m_gutter->update();
}

void CodeView::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    const QRect cr = contentsRect();
    m_gutter->setGeometry(QRect(cr.left(), cr.top(), gutterWidth(), cr.height()));
}

QColor CodeView::rowColor(const Row &r) const
{
    if (r.filler)
        return {};
    if (r.moved)
        return m_pal.moved;
    switch (r.kind) {
    case LineKind::Added: return m_pal.added;
    case LineKind::Removed: return m_pal.removed;
    case LineKind::Equal: break;
    }
    return {};
}

void CodeView::setRows(const QVector<Row> &rows, const ViewPalette &palette, const QFont &font, int tabWidth)
{
    m_rows = rows;
    m_pal = palette;
    m_currentFirst = m_currentLast = -1;

    setFont(font);
    m_gutter->setFont(font);
    setTabStopDistance(fontMetrics().horizontalAdvance(u' ') * tabWidth);

    QStringList texts;
    texts.reserve(rows.size());
    for (const Row &r : rows)
        texts << r.text;
    setPlainText(texts.join(u'\n'));

    QTextCursor c(document());
    c.beginEditBlock();
    QTextBlock b = document()->firstBlock();
    for (int i = 0; i < m_rows.size() && b.isValid(); ++i, b = b.next()) {
        const Row &r = m_rows[i];
        QBrush bg;
        if (r.filler)
            bg = m_pal.filler;
        else if (const QColor col = rowColor(r); col.isValid())
            bg = col;
        if (bg.style() != Qt::NoBrush) {
            c.setPosition(b.position());
            QTextBlockFormat f;
            f.setBackground(bg);
            c.setBlockFormat(f);
        }
        if (!r.highlights.isEmpty()) {
            QTextCharFormat cf;
            cf.setBackground(r.kind == LineKind::Added ? m_pal.addedEmph : m_pal.removedEmph);
            for (const Span &s : r.highlights) {
                c.setPosition(b.position() + s.start);
                c.setPosition(b.position() + s.start + s.length, QTextCursor::KeepAnchor);
                c.mergeCharFormat(cf);
            }
        }
    }
    c.endEditBlock();

    QPalette p = this->palette();
    m_gutter->setPalette(p);
    updateGutterWidth();
    verticalScrollBar()->setValue(0);
    horizontalScrollBar()->setValue(0);
}

void CodeView::setCurrentRange(int first, int last)
{
    m_currentFirst = first;
    m_currentLast = last;
    m_gutter->update();
}

void CodeView::scrollToRow(int row)
{
    const QTextBlock b = document()->findBlockByNumber(std::max(0, row));
    if (!b.isValid())
        return;
    QTextCursor c(b);
    setTextCursor(c);
    const int lineHeight = std::max(1, fontMetrics().lineSpacing());
    const int visible = viewport()->height() / lineHeight;
    verticalScrollBar()->setValue(std::max(0, row - visible / 3));
}

void CodeView::selectRow(int row)
{
    const QTextBlock b = document()->findBlockByNumber(row);
    if (!b.isValid())
        return;
    QTextCursor c(b);
    c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    setTextCursor(c);
}

int CodeView::rowAt(const QPoint &viewportPos) const
{
    QTextBlock block = firstVisibleBlock();
    qreal top = blockBoundingGeometry(block).translated(contentOffset()).top();
    while (block.isValid()) {
        const qreal bottom = top + blockBoundingRect(block).height();
        if (viewportPos.y() >= top && viewportPos.y() < bottom)
            return block.blockNumber();
        top = bottom;
        block = block.next();
    }
    return -1;
}

void CodeView::paintGutter(QPaintEvent *event)
{
    QPainter p(m_gutter);
    p.fillRect(event->rect(), m_pal.gutterBg);

    const QFontMetrics fm(font());
    const int digits = std::max(3, int(QString::number(maxLineNumber()).size()));
    const int numberWidth = fm.horizontalAdvance(u'9') * digits;
    const int w = m_gutter->width();

    QTextBlock block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    while (block.isValid() && top <= event->rect().bottom()) {
        const int h = qRound(blockBoundingRect(block).height());
        const int row = block.blockNumber();
        if (row < m_rows.size()) {
            const Row &r = m_rows[row];
            const QRect rowRect(0, top, w, h);
            if (r.filler)
                p.fillRect(rowRect, m_pal.filler);
            else if (const QColor col = rowColor(r); col.isValid())
                p.fillRect(rowRect, col);

            p.setPen(m_pal.gutterText);
            int x = kPad;
            auto drawNumber = [&](int n) {
                if (n > 0)
                    p.drawText(QRect(x, top, numberWidth, h), Qt::AlignRight | Qt::AlignVCenter, QString::number(n));
                x += numberWidth + kPad;
            };
            if (m_numbers != Numbers::New)
                drawNumber(r.oldNo);
            if (m_numbers != Numbers::Old)
                drawNumber(r.newNo);

            QString marker;
            if (r.moved)
                marker = QStringLiteral("⇄"); // ⇄
            else if (r.kind == LineKind::Added)
                marker = QStringLiteral("+");
            else if (r.kind == LineKind::Removed)
                marker = QStringLiteral("−"); // −
            if (!marker.isEmpty()) {
                if (r.moved)
                    p.setPen(m_pal.movedEmph);
                p.drawText(QRect(x, top, kMarkerWidth, h), Qt::AlignCenter, marker);
            }

            if (row >= m_currentFirst && row <= m_currentLast)
                p.fillRect(QRect(0, top, 3, h), m_pal.current);
        }
        block = block.next();
        top += h;
    }
    p.setPen(m_pal.gutterText);
    p.drawLine(w - 1, event->rect().top(), w - 1, event->rect().bottom());
}

void CodeView::paintEvent(QPaintEvent *event)
{
    QPlainTextEdit::paintEvent(event);

    // Mark moved blocks with dashed borders and a label so they read as one unit.
    QPainter p(viewport());
    QPen pen(m_pal.movedEmph, 1, Qt::DashLine);
    const int w = viewport()->width();
    QTextBlock block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    QFont labelFont = font();
    labelFont.setPointSizeF(std::max(6.0, labelFont.pointSizeF() * 0.85));
    const QFontMetrics lfm(labelFont);
    while (block.isValid() && top <= event->rect().bottom()) {
        const int h = qRound(blockBoundingRect(block).height());
        const int row = block.blockNumber();
        if (row < m_rows.size() && m_rows[row].moved) {
            const Row &r = m_rows[row];
            p.setPen(pen);
            if (r.moveStart)
                p.drawLine(0, top, w, top);
            if (r.moveEnd)
                p.drawLine(0, top + h - 1, w, top + h - 1);
            if (r.moveStart && !r.moveLabel.isEmpty()) {
                const int lw = lfm.horizontalAdvance(r.moveLabel) + 12;
                const QRect box(w - lw - 6, top + 1, lw, h - 2);
                p.setPen(Qt::NoPen);
                p.setBrush(m_pal.movedEmph);
                p.drawRoundedRect(box, 4, 4);
                p.setPen(m_pal.movedEmph.lightness() > 128 ? Qt::black : Qt::white);
                p.setFont(labelFont);
                p.drawText(box, Qt::AlignCenter, r.moveLabel);
                p.setBrush(Qt::NoBrush);
            }
        }
        block = block.next();
        top += h;
    }
}

bool CodeView::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        auto *he = static_cast<QHelpEvent *>(event);
        const int row = rowAt(he->pos());
        if (row >= 0 && row < m_rows.size() && !m_rows[row].tooltip.isEmpty())
            QToolTip::showText(he->globalPos(), m_rows[row].tooltip, viewport());
        else
            QToolTip::hideText();
        return true;
    }
    return QPlainTextEdit::viewportEvent(event);
}

bool CodeView::gutterEvent(QEvent *event)
{
    if (event->type() != QEvent::ToolTip)
        return false;
    auto *he = static_cast<QHelpEvent *>(event);
    const int row = rowAt(QPoint(0, he->pos().y()));
    if (row >= 0 && row < m_rows.size() && !m_rows[row].tooltip.isEmpty())
        QToolTip::showText(he->globalPos(), m_rows[row].tooltip, m_gutter);
    else
        QToolTip::hideText();
    return true;
}

void CodeView::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int row = rowAt(event->position().toPoint());
    if (row >= 0 && row < m_rows.size() && m_rows[row].moved) {
        emit moveJumpRequested(row);
        event->accept();
        return;
    }
    QPlainTextEdit::mouseDoubleClickEvent(event);
}
