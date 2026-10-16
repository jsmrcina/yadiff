#include "DiffPane.h"

#include "Settings.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {

QLabel *makeHeaderLabel()
{
    auto *l = new QLabel;
    l->setTextFormat(Qt::PlainText);
    l->setContentsMargins(6, 3, 6, 3);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return l;
}

QWidget *labelled(QLabel *label, QWidget *content)
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(label);
    v->addWidget(content, 1);
    return w;
}

QString rangeText(int first, int last)
{
    return first == last ? QStringLiteral("line %1").arg(first) : QStringLiteral("lines %1–%2").arg(first).arg(last);
}

} // namespace

DiffPane::DiffPane(QWidget *parent) : QWidget(parent)
{
    m_title = makeHeaderLabel();
    QFont tf = m_title->font();
    tf.setBold(true);
    m_title->setFont(tf);

    m_inline = new CodeView(CodeView::Numbers::Both);
    m_left = new CodeView(CodeView::Numbers::Old);
    m_right = new CodeView(CodeView::Numbers::New);
    m_inlineLabel = makeHeaderLabel();
    m_leftLabel = makeHeaderLabel();
    m_rightLabel = makeHeaderLabel();

    // Keep both side-by-side panes scrolled together. Rows are aligned with
    // filler lines, so equal scrollbar values mean equal positions.
    auto sync = [](QScrollBar *a, QScrollBar *b) {
        QObject::connect(a, &QScrollBar::valueChanged, b, &QScrollBar::setValue);
        QObject::connect(b, &QScrollBar::valueChanged, a, &QScrollBar::setValue);
    };
    sync(m_left->verticalScrollBar(), m_right->verticalScrollBar());
    sync(m_left->horizontalScrollBar(), m_right->horizontalScrollBar());
    m_left->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(labelled(m_leftLabel, m_left));
    splitter->addWidget(labelled(m_rightLabel, m_right));
    splitter->setChildrenCollapsible(false);
    m_sidePage = splitter;

    m_message = new QLabel;
    m_message->setAlignment(Qt::AlignCenter);
    m_message->setWordWrap(true);

    m_stack = new QStackedWidget;
    m_stack->addWidget(labelled(m_inlineLabel, m_inline)); // 0
    m_stack->addWidget(m_sidePage);                        // 1
    m_stack->addWidget(m_message);                         // 2

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_title);
    layout->addWidget(m_stack, 1);

    for (CodeView *v : {m_inline, m_left, m_right})
        connect(v, &CodeView::moveJumpRequested, this, [this, v](int row) { jumpToMovePartner(v, row); });

    showMessage(QString(), tr("No file selected"));
}

ViewPalette DiffPane::makePalette() const
{
    const DiffColors c = Settings::colors();
    ViewPalette p;
    p.added = c.added;
    p.removed = c.removed;
    p.moved = c.moved;
    p.addedEmph = Settings::emphasis(c.added);
    p.removedEmph = Settings::emphasis(c.removed);
    p.movedEmph = Settings::emphasis(c.moved);
    QColor hatch = palette().color(QPalette::Text);
    hatch.setAlpha(45);
    p.filler = QBrush(hatch, Qt::BDiagPattern);
    const QColor base = palette().color(QPalette::Base);
    p.gutterBg = Settings::isDarkTheme() ? base.lighter(125) : base.darker(106);
    p.gutterText = palette().color(QPalette::PlaceholderText);
    p.current = palette().color(QPalette::Highlight);
    return p;
}

Row DiffPane::makeRow(int i) const
{
    const DiffLine &l = m_diff.lines[i];
    Row r;
    r.kind = l.kind;
    r.oldNo = l.oldNo;
    r.newNo = l.newNo;
    r.lineIndex = i;
    r.text = l.text;
    r.highlights = l.highlights;
    if (l.moveId >= 0) {
        const MoveBlock &mb = m_diff.moves[l.moveId];
        const int ownFirst = l.kind == LineKind::Removed ? mb.removedFirst : mb.addedFirst;
        const int otherFirst = l.kind == LineKind::Removed ? mb.addedFirst : mb.removedFirst;
        r.moved = true;
        r.moveStart = i == ownFirst;
        r.moveEnd = i == ownFirst + mb.count - 1;
        const bool down = otherFirst > ownFirst;
        const QString arrow = down ? QStringLiteral("↓") : QStringLiteral("↑");
        if (l.kind == LineKind::Removed) {
            const QString target = rangeText(m_diff.lines[mb.addedFirst].newNo,
                                             m_diff.lines[mb.addedFirst + mb.count - 1].newNo);
            r.moveLabel = QStringLiteral("%1 moved to new %2").arg(arrow, target);
            r.tooltip = tr("Moved block: these lines were moved to new %1.\nDouble-click to jump there.").arg(target);
        } else {
            const QString source = rangeText(m_diff.lines[mb.removedFirst].oldNo,
                                             m_diff.lines[mb.removedFirst + mb.count - 1].oldNo);
            r.moveLabel = QStringLiteral("%1 moved from old %2").arg(arrow, source);
            r.tooltip = tr("Moved block: these lines were moved here from old %1.\nDouble-click to jump there.").arg(source);
        }
    }
    return r;
}

void DiffPane::setDiff(const FileDiff &diff, const QString &title, const QString &leftLabel, const QString &rightLabel)
{
    m_diff = diff;
    m_hasDiff = true;
    m_title->setText(title);
    m_leftLabel->setText(leftLabel);
    m_rightLabel->setText(rightLabel);
    m_inlineLabel->setText(QStringLiteral("%1   →   %2").arg(leftLabel, rightLabel));
    m_current = -1;
    rebuild();
    if (!m_diff.message.isEmpty() && m_diff.lines.isEmpty()) {
        m_message->setText(m_diff.message);
        m_stack->setCurrentIndex(2);
    }
    goToFirstChange();
    if (m_diff.chunks.isEmpty())
        emit currentChangeChanged(-1, 0);
}

void DiffPane::showMessage(const QString &title, const QString &message)
{
    m_diff = FileDiff();
    m_hasDiff = false;
    m_current = -1;
    m_title->setText(title);
    m_message->setText(message);
    m_stack->setCurrentIndex(2);
    emit currentChangeChanged(-1, 0);
}

void DiffPane::setMode(Mode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    if (!m_hasDiff)
        return;
    const int keep = m_current;
    rebuild();
    if (keep >= 0)
        showChange(keep);
}

void DiffPane::applySettings()
{
    if (!m_hasDiff)
        return;
    const int keep = m_current;
    rebuild();
    if (keep >= 0)
        showChange(keep);
}

void DiffPane::rebuild()
{
    const ViewPalette pal = makePalette();
    const QFont font = Settings::font();
    const int tab = Settings::tabWidth();
    const int n = int(m_diff.lines.size());

    m_inlineChunkRows.clear();
    m_sideChunkRows.clear();
    m_leftRowOfLine.fill(-1, n);
    m_rightRowOfLine.fill(-1, n);

    if (m_mode == Mode::Inline) {
        QVector<Row> rows;
        rows.reserve(n);
        for (int i = 0; i < n; ++i)
            rows.push_back(makeRow(i));
        for (const Chunk &c : std::as_const(m_diff.chunks))
            m_inlineChunkRows.push_back({c.first, c.last});
        m_inline->setRows(rows, pal, font, tab);
        m_stack->setCurrentIndex(0);
        return;
    }

    QVector<Row> left, right;
    left.reserve(n);
    right.reserve(n);
    Row filler;
    filler.filler = true;
    int i = 0;
    while (i < n) {
        if (m_diff.lines[i].kind == LineKind::Equal) {
            m_leftRowOfLine[i] = int(left.size());
            m_rightRowOfLine[i] = int(right.size());
            Row r = makeRow(i);
            left.push_back(r);
            right.push_back(r);
            ++i;
            continue;
        }
        QVector<int> removed, added;
        while (i < n && m_diff.lines[i].kind != LineKind::Equal) {
            (m_diff.lines[i].kind == LineKind::Removed ? removed : added).push_back(i);
            ++i;
        }
        const int startRow = int(left.size());
        // Lay out unpaired lines stacked, and put each similar (edited) pair on one row.
        auto emitRows = [&](qsizetype rFrom, qsizetype rTo, qsizetype aFrom, qsizetype aTo) {
            const qsizetype count = std::max(rTo - rFrom, aTo - aFrom);
            for (qsizetype k = 0; k < count; ++k) {
                if (rFrom + k < rTo) {
                    m_leftRowOfLine[removed[rFrom + k]] = int(left.size());
                    left.push_back(makeRow(removed[rFrom + k]));
                } else {
                    left.push_back(filler);
                }
                if (aFrom + k < aTo) {
                    m_rightRowOfLine[added[aFrom + k]] = int(right.size());
                    right.push_back(makeRow(added[aFrom + k]));
                } else {
                    right.push_back(filler);
                }
            }
        };
        qsizetype r = 0, a = 0;
        for (qsizetype ri = 0; ri < removed.size(); ++ri) {
            const int partner = m_diff.lines[removed[ri]].pair;
            if (partner < 0)
                continue;
            const qsizetype ai = added.indexOf(partner, a);
            if (ai < 0)
                continue;
            emitRows(r, ri, a, ai);       // unpaired lines before the pair
            emitRows(ri, ri + 1, ai, ai + 1); // the pair itself
            r = ri + 1;
            a = ai + 1;
        }
        emitRows(r, removed.size(), a, added.size());
        const int count = int(left.size()) - startRow;
        m_sideChunkRows.push_back({startRow, startRow + count - 1});
    }
    m_left->setRows(left, pal, font, tab);
    m_right->setRows(right, pal, font, tab);
    m_stack->setCurrentIndex(1);
}

void DiffPane::showChange(int index)
{
    const auto &ranges = m_mode == Mode::Inline ? m_inlineChunkRows : m_sideChunkRows;
    if (index < 0 || index >= ranges.size())
        return;
    m_current = index;
    const auto [first, last] = ranges[index];
    if (m_mode == Mode::Inline) {
        m_inline->setCurrentRange(first, last);
        m_inline->scrollToRow(first);
    } else {
        m_left->setCurrentRange(first, last);
        m_right->setCurrentRange(first, last);
        m_right->scrollToRow(first);
        m_left->verticalScrollBar()->setValue(m_right->verticalScrollBar()->value());
    }
    emit currentChangeChanged(m_current, changeCount());
}

bool DiffPane::nextChange()
{
    if (m_current + 1 >= changeCount())
        return false;
    showChange(m_current + 1);
    return true;
}

bool DiffPane::prevChange()
{
    if (m_current <= 0)
        return false;
    showChange(m_current - 1);
    return true;
}

void DiffPane::goToFirstChange()
{
    if (changeCount() > 0)
        showChange(0);
}

void DiffPane::goToLastChange()
{
    if (changeCount() > 0)
        showChange(changeCount() - 1);
}

void DiffPane::jumpToMovePartner(CodeView *source, int row)
{
    const QVector<Row> &rows = source->rows();
    if (row < 0 || row >= rows.size() || rows[row].lineIndex < 0)
        return;
    const int partner = m_diff.lines[rows[row].lineIndex].partner;
    if (partner < 0)
        return;
    if (m_mode == Mode::Inline) {
        m_inline->scrollToRow(partner);
        m_inline->selectRow(partner);
        return;
    }
    const bool toRight = m_diff.lines[partner].kind == LineKind::Added;
    CodeView *target = toRight ? m_right : m_left;
    const int targetRow = (toRight ? m_rightRowOfLine : m_leftRowOfLine).value(partner, -1);
    if (targetRow < 0)
        return;
    m_right->scrollToRow(targetRow);
    m_left->verticalScrollBar()->setValue(m_right->verticalScrollBar()->value());
    target->selectRow(targetRow);
    target->setFocus();
}
