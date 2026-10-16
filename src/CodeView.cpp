#include "CodeView.h"

#include "Syntax.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QDesktopServices>
#include <QTextBrowser>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QHelpEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QToolTip>

#include <cmath>

namespace {
constexpr int kPad = 6;
constexpr int kMarkerWidth = 16;
constexpr QChar kPadChar = QChar(0x2028); // line separator: an extra visual line inside one block
// Each block records which row it came from in a block-format property (userState()
// can't be used: the syntax highlighter owns it). Real rows store their row index
// (>= 0); blocks the user creates while editing have no property.
constexpr int kRowProperty = QTextFormat::UserProperty + 1;
constexpr int kFillerState = -2;
constexpr int kFoldState = -3;
constexpr int kNewState = -1;
constexpr int kCommentState = -4;

int rowState(const QTextBlock &b)
{
    const QVariant v = b.blockFormat().property(kRowProperty);
    return v.isValid() ? v.toInt() : kNewState;
}
} // namespace

class CodeView::Gutter : public QWidget
{
public:
    explicit Gutter(CodeView *view) : QWidget(view), m_view(view) {}
    QSize sizeHint() const override { return {m_view->gutterWidth(), 0}; }

protected:
    void paintEvent(QPaintEvent *e) override { m_view->paintGutter(e); }
    bool event(QEvent *e) override { return m_view->gutterEvent(e) || QWidget::event(e); }
    void mousePressEvent(QMouseEvent *e) override
    {
        const int row = m_view->rowAt(QPoint(0, int(e->position().y())));
        if (row < 0 || row >= m_view->rows().size())
            return;
        if (m_view->rows()[row].fold)
            emit m_view->foldClicked(row);
        else if (m_view->rows()[row].comment)
            emit m_view->commentToggled(m_view->rows()[row].threadId);
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        const int row = m_view->rowAt(QPoint(0, int(e->position().y())));
        if (row >= 0 && row < m_view->rows().size() && m_view->rows()[row].moved)
            emit m_view->moveJumpRequested(row);
    }
    void contextMenuEvent(QContextMenuEvent *e) override
    {
        emit m_view->contextMenuRequested(m_view->rowAt(QPoint(0, e->pos().y())), e->globalPos());
    }

private:
    CodeView *m_view;
};

CodeView::CodeView(Numbers numbers, QWidget *parent) : QPlainTextEdit(parent), m_numbers(numbers)
{
    m_gutter = new Gutter(this);
    setReadOnly(true);
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setWordWrapMode(QTextOption::NoWrap);
    setUndoRedoEnabled(false);
    setFrameShape(QFrame::NoFrame);
    viewport()->setMouseTracking(true);
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect &rect, int dy) {
        if (dy)
            m_gutter->scroll(0, dy);
        else
            m_gutter->update(0, rect.y(), m_gutter->width(), rect.height());
        if (!m_commentWidgets.isEmpty())
            layoutCommentWidgets(); // keep comment overlays on their rows while scrolling
    });
    connect(document(), &QTextDocument::contentsChanged, this, [this] {
        if (m_editing && !m_rebuildingAfterEdit)
            rebuildRowsAfterEdit();
    });
    connect(document(), &QTextDocument::modificationChanged, this, [this](bool m) {
        if (m_editing)
            emit modifiedChanged(m);
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
    layoutCommentWidgets();
}

QColor CodeView::rowColor(const Row &r) const
{
    if (r.filler)
        return {};
    if (r.fold)
        return m_pal.fold;
    if (r.comment)
        return m_pal.comment;
    if (r.moved)
        return m_pal.moved;
    switch (r.kind) {
    case LineKind::Added: return m_pal.added;
    case LineKind::Removed: return m_pal.removed;
    case LineKind::Equal: break;
    }
    return {};
}

void CodeView::applyTextOptions()
{
    QTextOption opt = document()->defaultTextOption();
    QTextOption::Flags flags = opt.flags();
    flags.setFlag(QTextOption::ShowTabsAndSpaces, m_showWhitespace);
    opt.setFlags(flags);
    opt.setWrapMode(m_wrap ? QTextOption::WrapAnywhere : QTextOption::NoWrap);
    document()->setDefaultTextOption(opt);
}

void CodeView::setRows(const QVector<Row> &rows, const ViewPalette &palette, const QFont &font, int tabWidth)
{
    if (m_editing)
        setEditing(false);
    clearCommentWidgets();
    m_rows = rows;
    m_pal = palette;
    m_padding.fill(0, rows.size());
    m_currentFirst = m_currentLast = -1;

    setFont(font);
    m_gutter->setFont(font);
    setTabStopDistance(fontMetrics().horizontalAdvance(u' ') * tabWidth);

    QStringList texts;
    texts.reserve(rows.size());
    for (const Row &r : rows)
        texts << r.text;

    Syntax::suspend(m_highlighter);
    // Rebuild the document with explicitly empty formats. setPlainText() would insert
    // the new text using the text cursor's current character format, so a cursor
    // left inside a highlighted word painted every line of the next file.
    setTextCursor(QTextCursor(document()));
    setCurrentCharFormat(QTextCharFormat());
    document()->clear();
    {
        QTextCursor init(document());
        init.setBlockFormat(QTextBlockFormat());
        init.setBlockCharFormat(QTextCharFormat());
        init.insertText(texts.join(u'\n'), QTextCharFormat());
    }
    setTextCursor(QTextCursor(document()));

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
        c.setPosition(b.position());
        QTextBlockFormat f;
        if (bg.style() != Qt::NoBrush)
            f.setBackground(bg);
        f.setProperty(kRowProperty, r.filler ? kFillerState : r.fold ? kFoldState : r.comment ? kCommentState : i);
        if (r.fold || r.comment || r.filler)
            f.setProperty(Syntax::kPlainBlockProperty, true);
        c.setBlockFormat(f);
        if (r.fold) {
            QTextCharFormat cf;
            cf.setForeground(m_pal.gutterText);
            cf.setFontItalic(true);
            c.setPosition(b.position());
            c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            c.mergeCharFormat(cf);
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
    applyTextOptions();
    Syntax::resume(m_highlighter, document());
    document()->setModified(false);

    m_gutter->setPalette(this->palette());
    updateGutterWidth();
    setExtraSelections({});
    for (int i = 0; i < m_rows.size(); ++i)
        if (m_rows[i].comment && m_rows[i].commentExpanded)
            m_commentWidgets.insert(i, makeCommentWidget(i));
    verticalScrollBar()->setValue(0);
    horizontalScrollBar()->setValue(0);
}

void CodeView::setSyntax(const QString &language, bool darkTheme)
{
    if (language == m_language && darkTheme == m_dark && (m_highlighter || language.isEmpty()))
        return;
    m_language = language;
    m_dark = darkTheme;
    m_highlighter = Syntax::apply(m_highlighter, document(), language, darkTheme);
}

void CodeView::setShowWhitespace(bool on)
{
    m_showWhitespace = on;
    applyTextOptions();
}

void CodeView::setWordWrap(bool on)
{
    m_wrap = on;
    setLineWrapMode(on ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    setWordWrapMode(on ? QTextOption::WrapAnywhere : QTextOption::NoWrap);
    applyTextOptions();
    if (!on)
        setWrapPadding({});
}

QString CodeView::stripPadding(const QString &s)
{
    qsizetype n = s.size();
    while (n > 0 && s.at(n - 1) == kPadChar)
        --n;
    return s.left(n);
}

int CodeView::naturalLineCount(int row) const
{
    const QTextBlock b = document()->findBlockByNumber(row);
    if (!b.isValid())
        return 1;
    document()->documentLayout()->blockBoundingRect(b); // forces the block's layout
    const int lines = b.layout() ? b.layout()->lineCount() : 1;
    return std::max(1, lines - m_padding.value(row, 0));
}

void CodeView::setWrapPadding(const QVector<int> &extraLines)
{
    if (m_editing)
        return;
    QTextCursor c(document());
    bool changed = false;
    for (int row = 0; row < m_rows.size(); ++row) {
        const int want = extraLines.value(row, 0);
        const int have = m_padding.value(row, 0);
        if (want == have)
            continue;
        if (!changed) {
            c.beginEditBlock();
            changed = true;
        }
        const QTextBlock b = document()->findBlockByNumber(row);
        c.setPosition(b.position() + b.length() - 1);
        if (have > 0) {
            c.setPosition(b.position() + b.length() - 1 - have, QTextCursor::KeepAnchor);
            c.removeSelectedText();
        }
        if (want > 0)
            c.insertText(QString(want, kPadChar), QTextCharFormat());
        m_padding[row] = want;
    }
    if (changed) {
        c.endEditBlock();
        document()->setModified(false);
    }
}

void CodeView::setSearchHits(const QVector<ViewHit> &hits, int current)
{
    QList<QTextEdit::ExtraSelection> sels;
    sels.reserve(hits.size());
    for (int i = 0; i < hits.size(); ++i) {
        const QTextBlock b = document()->findBlockByNumber(hits[i].row);
        if (!b.isValid())
            continue;
        QTextEdit::ExtraSelection s;
        s.cursor = QTextCursor(document());
        s.cursor.setPosition(b.position() + hits[i].start);
        s.cursor.setPosition(b.position() + hits[i].start + hits[i].length, QTextCursor::KeepAnchor);
        s.format.setBackground(i == current ? m_pal.currentMatch : m_pal.match);
        s.format.setForeground(Qt::black);
        sels << s;
    }
    setExtraSelections(sels);
}

void CodeView::setEditing(bool on)
{
    if (on == m_editing)
        return;
    if (on) {
        setWrapPadding({});
        m_editing = true;
        setReadOnly(false);
        setTextInteractionFlags(Qt::TextEditorInteraction);
        setUndoRedoEnabled(true);
        document()->clearUndoRedoStacks();
        document()->setModified(false);
        viewport()->setCursor(Qt::IBeamCursor);
    } else {
        m_editing = false;
        setReadOnly(true);
        setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        setUndoRedoEnabled(false);
    }
}

bool CodeView::isModified() const
{
    return m_editing && document()->isModified();
}

QStringList CodeView::editedLines() const
{
    QStringList out;
    for (QTextBlock b = document()->firstBlock(); b.isValid(); b = b.next()) {
        const QString text = stripPadding(b.text());
        const int state = rowState(b);
        if (state == kCommentState || ((state == kFillerState || state == kFoldState) && text.isEmpty()))
            continue;
        out << text;
    }
    return out;
}

void CodeView::rebuildRowsAfterEdit()
{
    // Keep the gutter in step with what the user typed: original rows keep their
    // look, new lines show as additions, and numbers count the real lines.
    m_rebuildingAfterEdit = true;
    const QVector<Row> original = m_rows;
    QVector<Row> rows;
    int newNo = 0;
    for (QTextBlock b = document()->firstBlock(); b.isValid(); b = b.next()) {
        const int state = rowState(b);
        const bool emptyFiller = state == kCommentState
                              || ((state == kFillerState || state == kFoldState) && stripPadding(b.text()).isEmpty());
        Row r;
        if (emptyFiller) {
            r.filler = true;
        } else {
            if (b.blockFormat().boolProperty(Syntax::kPlainBlockProperty)) {
                // Typed into a former filler row: it is code now, so highlight it.
                QTextCursor bc(b);
                QTextBlockFormat f = b.blockFormat();
                f.clearProperty(Syntax::kPlainBlockProperty);
                f.clearProperty(kRowProperty);
                f.clearBackground();
                bc.setBlockFormat(f);
            }
            if (state >= 0 && state < original.size())
                r = original[state];
            else
                r.kind = LineKind::Added;
            r.filler = false;
            r.fold = false;
            r.newNo = ++newNo;
            r.text = b.text();
        }
        rows << r;
    }
    m_rows = rows;
    m_rebuildingAfterEdit = false;
    m_gutter->update();
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
    if (!m_editing)
        setTextCursor(QTextCursor(b));
    const int lineHeight = std::max(1, fontMetrics().lineSpacing());
    const int visible = viewport()->height() / lineHeight;
    // With wrapping the scrollbar counts visual lines, not rows.
    verticalScrollBar()->setValue(std::max(0, b.firstLineNumber() - visible / 3));
}

void CodeView::selectRow(int row)
{
    const QTextBlock b = document()->findBlockByNumber(row);
    if (!b.isValid())
        return;
    QTextCursor c(b);
    c.setPosition(b.position() + stripPadding(b.text()).size(), QTextCursor::KeepAnchor);
    setTextCursor(c);
}

int CodeView::topRow() const
{
    return firstVisibleBlock().blockNumber();
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
    const int lineH = fm.lineSpacing();

    QTextBlock block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    while (block.isValid() && top <= event->rect().bottom()) {
        const int h = qRound(blockBoundingRect(block).height());
        const int row = block.blockNumber();
        if (row < m_rows.size()) {
            const Row &r = m_rows[row];
            const QRect rowRect(0, top, w, h);
            const int textH = std::min(h, lineH); // numbers sit on the first visual line
            if (r.filler)
                p.fillRect(rowRect, m_pal.filler);
            else if (const QColor col = rowColor(r); col.isValid())
                p.fillRect(rowRect, col);

            p.setPen(m_pal.gutterText);
            if (r.fold) {
                p.drawText(QRect(0, top, w, textH), Qt::AlignCenter, QStringLiteral("⋯"));
            } else if (r.comment) {
                // Click the arrow (or the summary line) to open or close the thread.
                p.drawText(QRect(0, top, w - 4, textH), Qt::AlignRight | Qt::AlignVCenter,
                           r.commentExpanded ? QStringLiteral("\u25BE") : QStringLiteral("\u25B8"));
            } else {
                int x = kPad;
                auto drawNumber = [&](int n) {
                    if (n > 0)
                        p.drawText(QRect(x, top, numberWidth, textH), Qt::AlignRight | Qt::AlignVCenter, QString::number(n));
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
                    p.drawText(QRect(x, top, kMarkerWidth, textH), Qt::AlignCenter, marker);
                }
            }

            if (row >= m_currentFirst && row <= m_currentLast)
                p.fillRect(QRect(0, top, 3, h), m_pal.current);
        }
        block = block.next();
        top += h;
    }
    p.setPen(m_pal.gutterText);
    p.drawLine(w - 1, event->rect().top(), w - 1, event->rect().bottom());
    if (m_editing) {
        // A coloured edge shows this side is being edited.
        p.fillRect(QRect(w - 3, event->rect().top(), 3, event->rect().height()), m_pal.current);
    }
}

void CodeView::paintEvent(QPaintEvent *event)
{
    QPlainTextEdit::paintEvent(event);

    // Mark moved blocks with dashed borders and a label so they read as one unit.
    QPainter p(viewport());
    QPen pen(m_pal.movedEmph, 1, Qt::DashLine);
    const int w = viewport()->width();
    const int lineH = fontMetrics().lineSpacing();
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
                const QRect box(w - lw - 6, top + 1, lw, std::min(h, lineH) - 2);
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

void CodeView::mousePressEvent(QMouseEvent *event)
{
    const int row = rowAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && row >= 0 && row < m_rows.size() && m_rows[row].fold) {
        emit foldClicked(row);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && row >= 0 && row < m_rows.size() && m_rows[row].comment) {
        emit commentToggled(m_rows[row].threadId);
        event->accept();
        return;
    }
    QPlainTextEdit::mousePressEvent(event);
}

void CodeView::mouseMoveEvent(QMouseEvent *event)
{
    const int row = rowAt(event->position().toPoint());
    const bool overFold = row >= 0 && row < m_rows.size() && (m_rows[row].fold || m_rows[row].comment);
    viewport()->setCursor(overFold ? Qt::PointingHandCursor : Qt::IBeamCursor);
    QPlainTextEdit::mouseMoveEvent(event);
}

void CodeView::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int row = rowAt(event->position().toPoint());
    if (!m_editing && row >= 0 && row < m_rows.size() && m_rows[row].moved) {
        emit moveJumpRequested(row);
        event->accept();
        return;
    }
    QPlainTextEdit::mouseDoubleClickEvent(event);
}

void CodeView::contextMenuEvent(QContextMenuEvent *event)
{
    // QAbstractScrollArea forwards the viewport's event, so pos() is viewport-relative.
    emit contextMenuRequested(rowAt(event->pos()), event->globalPos());
}

// ---- Review comment overlays ---------------------------------------------

namespace {
// Forwards wheel scrolling over a comment to the code view underneath.
class WheelForwarder : public QObject
{
public:
    WheelForwarder(QObject *parent, QWidget *target) : QObject(parent), m_target(target) {}
    bool eventFilter(QObject *, QEvent *e) override
    {
        if (e->type() != QEvent::Wheel)
            return false;
        QApplication::sendEvent(m_target, e);
        return true;
    }

private:
    QWidget *m_target;
};
} // namespace

void CodeView::clearCommentWidgets()
{
    qDeleteAll(m_commentWidgets);
    m_commentWidgets.clear();
}

QTextBrowser *CodeView::makeCommentWidget(int row)
{
    const Row &r = m_rows[row];
    auto *w = new QTextBrowser(viewport());
    w->setFrameShape(QFrame::NoFrame);
    w->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    w->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    w->setOpenLinks(false);
    w->setContextMenuPolicy(Qt::CustomContextMenu);
    QPalette pal = w->palette();
    pal.setColor(QPalette::Base, m_pal.comment);
    w->setPalette(pal);
    w->setHtml(r.commentHtml);
    const qint64 thread = r.threadId;
    connect(w, &QTextBrowser::anchorClicked, this, [this, thread](const QUrl &url) {
        if (url.scheme() == QLatin1String("yadiff"))
            emit commentToggled(thread); // the thread's collapse link
        else
            QDesktopServices::openUrl(url);
    });
    connect(w, &QWidget::customContextMenuRequested, this, [this, w, row](const QPoint &pos) {
        emit contextMenuRequested(row, w->mapToGlobal(pos));
    });
    w->viewport()->installEventFilter(new WheelForwarder(w, viewport()));
    w->show();
    return w;
}

int CodeView::commentLinesNeeded(int row)
{
    QTextBrowser *w = m_commentWidgets.value(row);
    if (!w)
        return 1;
    const int width = std::max(200, viewport()->width() - 8);
    w->document()->setTextWidth(width);
    const qreal height = w->document()->size().height();
    return std::max(1, int(std::ceil((height + 4) / std::max(1, fontMetrics().lineSpacing()))));
}

void CodeView::setRowLineCount(int row, int lines)
{
    const QTextBlock b = document()->findBlockByNumber(row);
    if (!b.isValid())
        return;
    const QString want(std::max(0, lines - 1), QChar(0x2028));
    if (b.text() == want)
        return;
    const bool modified = document()->isModified();
    QTextCursor c(b);
    c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    c.insertText(want, QTextCharFormat());
    if (row < m_padding.size())
        m_padding[row] = 0;
    document()->setModified(modified);
    layoutCommentWidgets();
}

void CodeView::layoutCommentWidgets()
{
    const int width = std::max(200, viewport()->width() - 8);
    for (auto it = m_commentWidgets.constBegin(); it != m_commentWidgets.constEnd(); ++it) {
        const QTextBlock b = document()->findBlockByNumber(it.key());
        if (!b.isValid())
            continue;
        const QRectF r = blockBoundingGeometry(b).translated(contentOffset());
        QTextBrowser *w = it.value();
        if (r.bottom() < 0 || r.top() > viewport()->height()) {
            w->hide();
            continue;
        }
        // Pinned to the left edge: scrolling the code sideways doesn't move comments.
        w->setGeometry(QRect(4, int(r.top()), width, std::max(1, int(r.height()))));
        w->show();
    }
}
