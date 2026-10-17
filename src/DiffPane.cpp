#include "DiffPane.h"

#include "ImageDiffView.h"
#include "OverviewBar.h"
#include "Settings.h"
#include "Syntax.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QPushButton>
#include <QUrl>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBlock>
#include <QTextDocumentFragment>
#include <QRegularExpression>
#include <QTimer>
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

void copyToClipboard(const QString &text, const QString &html = {})
{
    auto *mime = new QMimeData;
    mime->setText(text);
    if (!html.isEmpty())
        mime->setHtml(html);
    QApplication::clipboard()->setMimeData(mime);
}

OverviewBar::Mark markFor(const Row &r)
{
    if (r.filler)
        return OverviewBar::Mark::Filler;
    if (r.moved)
        return OverviewBar::Mark::Moved;
    if (r.kind == LineKind::Added)
        return OverviewBar::Mark::Added;
    if (r.kind == LineKind::Removed)
        return OverviewBar::Mark::Removed;
    return OverviewBar::Mark::None;
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

    // Keep both side-by-side panes scrolled together. Rows are aligned with filler
    // lines (and, with word wrap, padding), so equal values mean equal positions.
    auto sync = [](QScrollBar *a, QScrollBar *b) {
        QObject::connect(a, &QScrollBar::valueChanged, b, &QScrollBar::setValue);
        QObject::connect(b, &QScrollBar::valueChanged, a, &QScrollBar::setValue);
    };
    sync(m_left->verticalScrollBar(), m_right->verticalScrollBar());
    sync(m_left->horizontalScrollBar(), m_right->horizontalScrollBar());
    m_left->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *splitter = new QSplitter(Qt::Horizontal);
    m_leftBox = labelled(m_leftLabel, m_left);
    m_rightBox = labelled(m_rightLabel, m_right);
    splitter->addWidget(m_leftBox);
    splitter->addWidget(m_rightBox);
    splitter->setChildrenCollapsible(false);
    m_sidePage = splitter;

    m_message = new QLabel;
    m_message->setAlignment(Qt::AlignCenter);
    m_message->setWordWrap(true);
    m_showAnyway = new QPushButton(tr("Show anyway"));
    m_showAnyway->setVisible(false);
    connect(m_showAnyway, &QPushButton::clicked, this, &DiffPane::showLargeRequested);
    m_messagePage = new QWidget;
    {
        auto *v = new QVBoxLayout(m_messagePage);
        v->addStretch(1);
        v->addWidget(m_message);
        v->addWidget(m_showAnyway, 0, Qt::AlignHCenter);
        v->addStretch(1);
    }
    m_image = new ImageDiffView;

    m_stack = new QStackedWidget;
    m_stack->addWidget(labelled(m_inlineLabel, m_inline)); // 0
    m_stack->addWidget(m_sidePage);                        // 1
    m_stack->addWidget(m_messagePage);                     // 2
    m_stack->addWidget(m_image);                           // 3

    m_overview = new OverviewBar;
    auto *body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addWidget(m_stack, 1);
    body->addWidget(m_overview);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_title);
    layout->addLayout(body, 1);

    m_wrapTimer = new QTimer(this);
    m_wrapTimer->setSingleShot(true);
    m_wrapTimer->setInterval(80);
    connect(m_wrapTimer, &QTimer::timeout, this, &DiffPane::relayout);

    for (CodeView *v : {m_inline, m_left, m_right}) {
        connect(v, &CodeView::moveJumpRequested, this, [this, v](int row) { jumpToMovePartner(v, row); });
        connect(v, &CodeView::contextMenuRequested, this,
                [this, v](int row, const QPoint &pos) { showContextMenu(v, row, pos); });
        connect(v, &CodeView::commentToggled, this, &DiffPane::toggleThread);
        connect(v, &CodeView::wordClicked, this, &DiffPane::highlightWord);
        connect(v, &CodeView::blameClicked, this, &DiffPane::blameCommitRequested);
        connect(v, &CodeView::cursorPositionChanged, this, [this, v] { m_lastFocused = v; });
        connect(v, &CodeView::foldClicked, this, [v, this](int row) {
            if (row >= 0 && row < v->rows().size())
                expandFold(v->rows()[row].foldFirstLine);
        });
        connect(v->verticalScrollBar(), &QScrollBar::valueChanged, this, &DiffPane::updateVisibleRange);
    }
    connect(m_right, &CodeView::modifiedChanged, this, &DiffPane::modifiedChanged);
    connect(m_overview, &OverviewBar::rowRequested, this, [this](int row) {
        CodeView *v = activeView();
        const int visible = v->viewport()->height() / std::max(1, v->fontMetrics().lineSpacing());
        const QTextBlock b = v->document()->findBlockByNumber(row);
        if (b.isValid())
            v->verticalScrollBar()->setValue(std::max(0, b.firstLineNumber() - visible / 2));
    });

    // Re-pad wrapped rows when a pane changes width.
    class ResizeWatcher : public QObject
    {
    public:
        explicit ResizeWatcher(DiffPane *pane) : QObject(pane), m_pane(pane) {}
        bool eventFilter(QObject *, QEvent *e) override
        {
            if (e->type() == QEvent::Resize) {
                m_pane->scheduleWrapPadding();
                m_pane->updateVisibleRange();
            }
            return false;
        }

    private:
        DiffPane *m_pane;
    };
    auto *watcher = new ResizeWatcher(this);
    for (CodeView *v : {m_inline, m_left, m_right})
        v->viewport()->installEventFilter(watcher);

    showMessage(QString(), tr("No file selected"));
}

CodeView *DiffPane::activeView() const
{
    return m_mode == Mode::Inline ? m_inline : m_right;
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
    const bool dark = Settings::isDarkTheme();
    p.fold = dark ? base.lighter(140) : base.darker(110);
    p.gutterBg = dark ? base.lighter(125) : base.darker(106);
    p.gutterText = palette().color(QPalette::PlaceholderText);
    p.current = palette().color(QPalette::Highlight);
    p.comment = dark ? QColor(0x3a, 0x36, 0x1c) : QColor(0xff, 0xf8, 0xc5);
    p.match = QColor(0xff, 0xe0, 0x66);
    p.currentMatch = QColor(0xff, 0x96, 0x32);
    return p;
}

Row DiffPane::makeRow(int i) const
{
    const FileDiff &d = m_file.diff;
    const DiffLine &l = d.lines[i];
    Row r;
    r.kind = l.kind;
    r.oldNo = l.oldNo;
    r.newNo = l.newNo;
    r.lineIndex = i;
    r.text = l.text;
    r.highlights = l.highlights;
    if (l.moveId >= 0) {
        const MoveBlock &mb = d.moves[l.moveId];
        const int ownFirst = l.kind == LineKind::Removed ? mb.removedFirst : mb.addedFirst;
        const int otherFirst = l.kind == LineKind::Removed ? mb.addedFirst : mb.removedFirst;
        r.moved = true;
        r.moveStart = i == ownFirst;
        r.moveEnd = i == ownFirst + mb.count - 1;
        const QString arrow = otherFirst > ownFirst ? QStringLiteral("↓") : QStringLiteral("↑");
        if (l.kind == LineKind::Removed) {
            const QString target = rangeText(d.lines[mb.addedFirst].newNo, d.lines[mb.addedFirst + mb.count - 1].newNo);
            r.moveLabel = QStringLiteral("%1 moved to new %2").arg(arrow, target);
            r.tooltip = tr("Moved block: these lines were moved to new %1.\nDouble-click to jump there.").arg(target);
        } else {
            const QString source = rangeText(d.lines[mb.removedFirst].oldNo, d.lines[mb.removedFirst + mb.count - 1].oldNo);
            r.moveLabel = QStringLiteral("%1 moved from old %2").arg(arrow, source);
            r.tooltip = tr("Moved block: these lines were moved here from old %1.\nDouble-click to jump there.").arg(source);
        }
    }
    return r;
}

Row DiffPane::makeFoldRow(int firstHidden, int count) const
{
    Row r;
    r.fold = true;
    r.foldFirstLine = firstHidden;
    r.foldCount = count;
    // En spaces (U+2002), not ordinary ones, so "show whitespace" leaves the label alone.
    r.text = tr("\u2002\u2002\u22EF\u2002%1\u2002unchanged\u2002lines\u2002\u2014\u2002click\u2002to\u2002expand").arg(count);
    r.tooltip = tr("Click to show these %1 unchanged lines").arg(count);
    return r;
}

void DiffPane::setFile(const FileView &file)
{
    if (isEditing())
        m_right->setEditing(false);
    m_file = file;
    m_hasDiff = true;
    m_expanded.clear();
    m_title->setText(file.title);
    m_leftLabel->setText(file.leftLabel);
    m_rightLabel->setText(file.rightLabel);
    m_leftName = file.leftLabel.section(QStringLiteral(": "), 0, 0);
    m_rightName = file.rightLabel.section(QStringLiteral(": "), 0, 0);
    m_inlineLabel->setText(QStringLiteral("%1   →   %2").arg(file.leftLabel, file.rightLabel));
    m_current = -1;
    m_matches.clear();
    m_currentMatch = -1;
    m_selView = nullptr;
    m_showAnyway->setVisible(false);
    indexThreads();
    if (!m_file.isImage)
        rebuild();
    if (m_file.isImage) {
        m_image->setImages(m_file.oldImage, m_file.newImage, m_file.oldBytes, m_file.newBytes);
        m_stack->setCurrentIndex(3);
        m_overview->setRows({}, {});
    } else if (!m_file.diff.message.isEmpty() && m_file.diff.lines.isEmpty()) {
        m_message->setText(m_file.diff.message);
        m_stack->setCurrentIndex(2);
    }
    if (!m_query.isEmpty())
        recomputeMatches();
    goToFirstChange();
    if (m_file.diff.chunks.isEmpty())
        emit currentChangeChanged(-1, 0);
}

void DiffPane::showLargeNotice(const QString &title, int lineCount)
{
    showMessage(title, tr("This diff has %1 lines, which may take a while to show.").arg(lineCount));
    m_showAnyway->setVisible(true);
}

void DiffPane::setThreads(const QVector<GitHubReview::Thread> &threads)
{
    m_file.threads = threads;
    indexThreads();
    if (m_hasDiff && !m_file.isImage && !m_file.diff.lines.isEmpty() && m_stack->currentIndex() != 2) {
        // Already showing this file: rebuild in place.
        CodeView *v = activeView();
        const int top = v->verticalScrollBar()->value();
        rebuild();
        if (m_current >= 0) {
            const auto &ranges = m_mode == Mode::Inline ? m_inlineChunkRows : m_sideChunkRows;
            if (m_current < ranges.size())
                for (CodeView *cv : {m_inline, m_left, m_right})
                    cv->setCurrentRange(ranges[m_current].first, ranges[m_current].second);
        }
        v->verticalScrollBar()->setValue(top);
    }
}

void DiffPane::indexThreads()
{
    const QVector<GitHubReview::Thread> &threads = m_file.threads;
    m_threadsAt.clear();
    const FileDiff &d = m_file.diff;
    QHash<int, int> byOld, byNew; // line number -> FileDiff line index
    for (int i = 0; i < d.lines.size(); ++i) {
        if (d.lines[i].kind != LineKind::Added && d.lines[i].oldNo > 0)
            byOld.insert(d.lines[i].oldNo, i);
        if (d.lines[i].kind != LineKind::Removed && d.lines[i].newNo > 0)
            byNew.insert(d.lines[i].newNo, i);
    }
    for (int t = 0; t < threads.size(); ++t) {
        const GitHubReview::Comment &c = threads[t].first();
        if (c.outdated())
            continue;
        const int idx = (c.side == QLatin1String("LEFT") ? byOld : byNew).value(c.line, -1);
        if (idx >= 0)
            m_threadsAt[idx].push_back(t);
    }
}

namespace {
// Tidies comment markup the way Azure DevOps shows it: hidden HTML comments dropped,
// <details> folded to its summary, and badge-like adjacent tags kept apart.
QString cleanMarkup(QString body)
{
    static const QRegularExpression htmlComment(QStringLiteral("<!--[\\s\\S]*?-->"));
    static const QRegularExpression details(
        QStringLiteral("<details[^>]*>(?:\\s*<summary[^>]*>([\\s\\S]*?)</summary>)?[\\s\\S]*?</details>"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tags(QStringLiteral("<[^>]+>"));
    static const QRegularExpression adjacent(QStringLiteral("(</(?:span|small|b|strong|em|code)>)(?=<)"),
                                             QRegularExpression::CaseInsensitiveOption);
    body.remove(htmlComment);
    QString out;
    qsizetype pos = 0;
    for (auto it = details.globalMatch(body); it.hasNext();) {
        const auto m = it.next();
        out += body.mid(pos, m.capturedStart() - pos);
        QString summary = m.captured(1);
        summary.remove(tags);
        summary = summary.simplified();
        if (!summary.isEmpty())
            out += QStringLiteral("\n\n*▸ %1*\n\n").arg(summary);
        pos = m.capturedEnd();
    }
    out += body.mid(pos);
    out.replace(adjacent, QStringLiteral("\\1 "));
    return out;
}

// Plain-text gist of a comment for its summary row: the first substantial line (short
// title lines such as a bot's name come second), else the first non-empty one.
QString firstLine(const QString &body)
{
    const QString text =
        QTextDocumentFragment::fromMarkdown(cleanMarkup(body), QTextDocument::MarkdownDialectGitHub).toPlainText();
    QString fallback;
    for (const QString &line : text.split(QRegularExpression(QStringLiteral("[\\n\\x{2029}\\x{2028}]")))) {
        const QString t = line.simplified();
        if (t.isEmpty())
            continue;
        if (fallback.isEmpty())
            fallback = t;
        if (t.size() >= 30)
            return t;
    }
    return fallback;
}
} // namespace

Row DiffPane::makeCommentRow(const GitHubReview::Thread &t) const
{
    const GitHubReview::Comment &first = t.first();
    Row r;
    r.comment = true;
    r.threadId = first.id;
    r.commentExpanded = m_expandedThreads.contains(first.id);
    if (r.commentExpanded) {
        // The text is only reserved space; the thread is drawn over it (see CodeView).
        r.commentHtml = threadHtml(t);
        r.tooltip = tr("Right-click to reply.");
        return r;
    }
    // Collapsed: one line. Click it (or the arrow in the gutter) to read the thread.
    QString summary = QStringLiteral("\U0001F4AC %1 · %2").arg(first.author, GitHubReview::displayTime(first.createdAt));
    if (t.size() > 1)
        summary += t.size() == 2 ? tr(" · 1 reply") : tr(" · %1 replies").arg(t.size() - 1);
    QString gist = firstLine(first.body);
    if (gist.size() > 110)
        gist = gist.left(108) + QStringLiteral("…");
    if (!gist.isEmpty())
        summary += QStringLiteral(" — ") + gist;
    r.text = summary;
    r.tooltip = tr("Review comment by %1. Click to read it; right-click to reply.").arg(first.author);
    return r;
}

QString DiffPane::threadHtml(const GitHubReview::Thread &t) const
{
    // Built as a document so each comment's Markdown (and the HTML inside it, as Azure
    // DevOps bots write) is rendered properly, then handed to the overlay as HTML.
    QTextDocument doc;
    QTextCursor c(&doc);
    const QString muted = palette().color(QPalette::PlaceholderText).name();
    for (int i = 0; i < t.size(); ++i) {
        const GitHubReview::Comment &cm = t[i];
        QTextBlockFormat head;
        head.setTopMargin(i == 0 ? 2 : 10);
        head.setBottomMargin(4);
        head.setLeftMargin(i == 0 ? 0 : 24);
        if (i > 0)
            c.insertBlock(head);
        else
            c.setBlockFormat(head);
        QString line;
        if (i == 0)
            line += QStringLiteral("<a href=\"yadiff:collapse\" style=\"text-decoration:none\">▾</a>&nbsp;");
        else
            line += QStringLiteral("↳&nbsp;");
        const QString who = QStringLiteral("<b>%1</b>").arg(cm.author.toHtmlEscaped());
        line += cm.url.isEmpty() ? who : QStringLiteral("<a href=\"%1\">%2</a>").arg(cm.url.toHtmlEscaped(), who);
        line += QStringLiteral(" <span style=\"color:%1\">· %2</span>").arg(muted, GitHubReview::displayTime(cm.createdAt));
        if (i == 0 && cm.startLine > 0 && cm.startLine < cm.line)
            line += QStringLiteral(" <span style=\"color:%1\">· %2</span>")
                        .arg(muted, tr("lines %1–%2").arg(cm.startLine).arg(cm.line));
        c.insertHtml(line);
        // The body, indented like its header.
        QTextBlockFormat bodyFormat;
        bodyFormat.setLeftMargin(i == 0 ? 16 : 40);
        c.insertBlock(bodyFormat);
        const int bodyStart = c.position();
        c.insertFragment(QTextDocumentFragment::fromMarkdown(cleanMarkup(cm.body), QTextDocument::MarkdownDialectGitHub));
        for (QTextBlock b = doc.findBlock(bodyStart); b.isValid() && b.position() <= c.position(); b = b.next()) {
            QTextCursor bc(b);
            QTextBlockFormat f = b.blockFormat();
            f.setLeftMargin(f.leftMargin() + (i == 0 ? 16 : 40));
            bc.setBlockFormat(f);
        }
    }
    // Code (inline and fenced blocks) in the diff's monospace font; blocks on a shaded band.
    const QFont mono = Settings::font();
    QColor codeBg = palette().color(QPalette::Text);
    codeBg.setAlpha(22);
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        const bool fence = b.blockFormat().hasProperty(QTextFormat::BlockCodeFence);
        if (fence) {
            QTextCursor bc(b);
            QTextBlockFormat f = b.blockFormat();
            f.setBackground(codeBg);
            bc.setBlockFormat(f);
        }
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!fence && !frag.charFormat().fontFixedPitch())
                continue;
            QTextCursor fc(&doc);
            fc.setPosition(frag.position());
            fc.setPosition(frag.position() + frag.length(), QTextCursor::KeepAnchor);
            QTextCharFormat cf;
            cf.setFontFamilies({mono.family()});
            cf.setFontPointSize(std::max(7.0, mono.pointSizeF() * 0.95));
            if (!fence)
                cf.setBackground(codeBg);
            fc.mergeCharFormat(cf);
        }
    }
    return doc.toHtml();
}

void DiffPane::toggleThread(qint64 threadId)
{
    if (!m_expandedThreads.remove(threadId))
        m_expandedThreads.insert(threadId);
    CodeView *v = activeView();
    const int top = v->verticalScrollBar()->value();
    rebuild();
    v->verticalScrollBar()->setValue(top);
}

void DiffPane::setCommentsExpanded(bool expanded)
{
    m_expandedThreads.clear();
    if (expanded)
        for (const GitHubReview::Thread &t : std::as_const(m_file.threads))
            m_expandedThreads.insert(t.first().id);
    if (!m_hasDiff || m_file.threads.isEmpty())
        return;
    CodeView *v = activeView();
    const int top = v->verticalScrollBar()->value();
    rebuild();
    v->verticalScrollBar()->setValue(top);
}

void DiffPane::applyCommentHeights()
{
    // Expanded threads reserve exactly the height their text needs, on both sides.
    auto apply = [](CodeView *v, CodeView *other) {
        const QVector<Row> &rows = v->rows();
        for (int i = 0; i < rows.size(); ++i) {
            if (!rows[i].comment || !rows[i].commentExpanded)
                continue;
            const int lines = v->commentLinesNeeded(i);
            v->setRowLineCount(i, lines);
            if (other)
                other->setRowLineCount(i, lines);
        }
    };
    if (m_mode == Mode::Inline) {
        apply(m_inline, nullptr);
    } else {
        apply(m_left, m_right);
        apply(m_right, m_left);
    }
}

void DiffPane::relayout()
{
    applyCommentHeights();
    applyWrapPadding();
}

Row DiffPane::makeSpacerRow(const Row &comment) const
{
    // Opposite a comment: an empty row, resized to match when the thread is expanded.
    Q_UNUSED(comment);
    Row r;
    r.filler = true;
    return r;
}

bool DiffPane::nearChange(int lineIndex) const
{
    // GitHub only accepts comments inside its diff hunks (3 lines of context); keep the
    // same rule for Azure DevOps so comments land where reviewers will see them.
    for (const Chunk &c : m_file.diff.chunks)
        if (lineIndex >= c.first - 3 && lineIndex <= c.last + 3)
            return true;
    return false;
}

void DiffPane::showMessage(const QString &title, const QString &message)
{
    m_showAnyway->setVisible(false);
    if (isEditing())
        m_right->setEditing(false);
    m_file = FileView();
    m_hasDiff = false;
    m_current = -1;
    m_matches.clear();
    m_currentMatch = -1;
    m_title->setText(title);
    m_message->setText(message);
    m_stack->setCurrentIndex(2);
    m_overview->setRows({}, {});
    emit currentChangeChanged(-1, 0);
    emit searchResultsChanged(0, -1);
}

void DiffPane::setMode(Mode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    emit modeChanged(mode);
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

void DiffPane::setCollapseUnchanged(bool on)
{
    if (m_collapse == on)
        return;
    m_collapse = on;
    m_expanded.clear();
    applySettings();
}

void DiffPane::expandAll()
{
    if (!m_collapse || !m_hasDiff)
        return;
    for (int key : std::as_const(m_foldOfLine))
        if (key >= 0)
            m_expanded.insert(key);
    applySettings();
}

void DiffPane::expandFold(int foldKey)
{
    if (foldKey < 0)
        return;
    CodeView *v = activeView();
    const int top = v->verticalScrollBar()->value();
    m_expanded.insert(foldKey);
    rebuild();
    if (m_current >= 0) {
        const auto &ranges = m_mode == Mode::Inline ? m_inlineChunkRows : m_sideChunkRows;
        if (m_current < ranges.size())
            for (CodeView *cv : {m_inline, m_left, m_right})
                cv->setCurrentRange(ranges[m_current].first, ranges[m_current].second);
    }
    v->verticalScrollBar()->setValue(top);
}

void DiffPane::setShowWhitespace(bool on)
{
    m_showWhitespace = on;
    for (CodeView *v : {m_inline, m_left, m_right})
        v->setShowWhitespace(on);
}

void DiffPane::setWordWrap(bool on)
{
    m_wrap = on;
    for (CodeView *v : {m_inline, m_left, m_right})
        v->setWordWrap(on);
    scheduleWrapPadding();
}

void DiffPane::setLanguageOverride(const QString &language)
{
    m_languageOverride = language;
    applySyntax();
}

QString DiffPane::detectedLanguage() const
{
    return Syntax::detect(m_file.fileName);
}

QString DiffPane::effectiveLanguage() const
{
    if (m_languageOverride == QLatin1String("-"))
        return {}; // explicitly "None"
    return m_languageOverride.isEmpty() ? detectedLanguage() : m_languageOverride;
}

void DiffPane::applySyntax()
{
    // Highlighting very large documents makes the UI sluggish; skip it there.
    const bool tooBig = m_file.diff.lines.size() > 30000;
    const QString lang = m_hasDiff && !tooBig && m_syntaxEnabled ? effectiveLanguage() : QString();
    const bool dark = Settings::isDarkTheme();
    for (CodeView *v : {m_inline, m_left, m_right})
        v->setSyntax(lang, dark);
}

void DiffPane::rebuild()
{
    const ViewPalette pal = makePalette();
    QFont font = Settings::font();
    if (m_zoom != 100)
        font.setPointSizeF(std::max(4.0, font.pointSizeF() * m_zoom / 100.0));
    const int tab = Settings::tabWidth();
    const FileDiff &d = m_file.diff;
    const int n = int(d.lines.size());
    const bool folding = m_collapse && !isEditing();
    for (CodeView *v : {m_inline, m_left, m_right})
        v->setBlame(m_file.oldBlame, m_file.newBlame);

    m_inlineChunkRows.clear();
    m_sideChunkRows.clear();
    m_inlineRowOfLine.fill(-1, n);
    m_leftRowOfLine.fill(-1, n);
    m_rightRowOfLine.fill(-1, n);
    m_foldOfLine.fill(-1, n);

    // Splits an unchanged run [s, e] into shown head, hidden middle and shown tail.
    struct RunPlan {
        int headEnd, hiddenFirst, hiddenCount;
    };
    auto plan = [&](int s, int e) -> RunPlan {
        const int len = e - s + 1;
        if (!folding)
            return {e, -1, 0};
        const int head = s == 0 ? 0 : m_context;
        const int tail = e == n - 1 ? 0 : m_context;
        const int hidden = len - head - tail;
        if (hidden < 2 || m_expanded.contains(s + head))
            return {e, -1, 0};
        for (int k = s + head; k < s + head + hidden; ++k)
            if (m_threadsAt.contains(k))
                return {e, -1, 0}; // keep review comments visible
        for (int i = s + head; i < s + head + hidden; ++i)
            m_foldOfLine[i] = s + head;
        return {s + head - 1, s + head, hidden};
    };

    if (m_mode == Mode::Inline) {
        QVector<Row> rows;
        rows.reserve(n);
        int i = 0;
        while (i < n) {
            if (d.lines[i].kind == LineKind::Equal) {
                int e = i;
                while (e + 1 < n && d.lines[e + 1].kind == LineKind::Equal)
                    ++e;
                const RunPlan p = plan(i, e);
                for (int k = i; k <= e; ++k) {
                    if (p.hiddenCount && k >= p.hiddenFirst && k < p.hiddenFirst + p.hiddenCount) {
                        if (k == p.hiddenFirst)
                            rows.push_back(makeFoldRow(p.hiddenFirst, p.hiddenCount));
                        continue;
                    }
                    m_inlineRowOfLine[k] = int(rows.size());
                    rows.push_back(makeRow(k));
                    for (int t : m_threadsAt.value(k))
                        rows.push_back(makeCommentRow(m_file.threads[t]));
                }
                i = e + 1;
                continue;
            }
            const int start = int(rows.size());
            while (i < n && d.lines[i].kind != LineKind::Equal) {
                m_inlineRowOfLine[i] = int(rows.size());
                rows.push_back(makeRow(i));
                for (int t : m_threadsAt.value(i))
                    rows.push_back(makeCommentRow(m_file.threads[t]));
                ++i;
            }
            m_inlineChunkRows.push_back({start, int(rows.size()) - 1});
        }
        m_inline->setRows(rows, pal, font, tab);
        m_stack->setCurrentIndex(0);
    } else {
        QVector<Row> left, right;
        left.reserve(n);
        right.reserve(n);
        Row filler;
        filler.filler = true;
        // Review threads go under their line, on their side, with a spacer opposite.
        auto addThreads = [&](int lineIndex) {
            for (int t : m_threadsAt.value(lineIndex)) {
                const Row c = makeCommentRow(m_file.threads[t]);
                const Row sp = makeSpacerRow(c);
                const bool onLeft = m_file.threads[t].first().side == QLatin1String("LEFT");
                left.push_back(onLeft ? c : sp);
                right.push_back(onLeft ? sp : c);
            }
        };
        int i = 0;
        while (i < n) {
            if (d.lines[i].kind == LineKind::Equal) {
                int e = i;
                while (e + 1 < n && d.lines[e + 1].kind == LineKind::Equal)
                    ++e;
                const RunPlan p = plan(i, e);
                for (int k = i; k <= e; ++k) {
                    if (p.hiddenCount && k >= p.hiddenFirst && k < p.hiddenFirst + p.hiddenCount) {
                        if (k == p.hiddenFirst) {
                            const Row fold = makeFoldRow(p.hiddenFirst, p.hiddenCount);
                            left.push_back(fold);
                            right.push_back(fold);
                        }
                        continue;
                    }
                    m_leftRowOfLine[k] = int(left.size());
                    m_rightRowOfLine[k] = int(right.size());
                    const Row r = makeRow(k);
                    Row l = r;
                    if (!m_file.diff.lines[k].oldText.isNull())
                        l.text = m_file.diff.lines[k].oldText; // differs only in ignored ways
                    left.push_back(l);
                    right.push_back(r);
                    addThreads(k);
                }
                i = e + 1;
                continue;
            }
            QVector<int> removed, added;
            while (i < n && d.lines[i].kind != LineKind::Equal) {
                (d.lines[i].kind == LineKind::Removed ? removed : added).push_back(i);
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
                    if (rFrom + k < rTo)
                        addThreads(removed[rFrom + k]);
                    if (aFrom + k < aTo)
                        addThreads(added[aFrom + k]);
                }
            };
            qsizetype r = 0, a = 0;
            for (qsizetype ri = 0; ri < removed.size(); ++ri) {
                const int partner = d.lines[removed[ri]].pair;
                if (partner < 0)
                    continue;
                const qsizetype ai = added.indexOf(partner, a);
                if (ai < 0)
                    continue;
                emitRows(r, ri, a, ai);
                emitRows(ri, ri + 1, ai, ai + 1);
                r = ri + 1;
                a = ai + 1;
            }
            emitRows(r, removed.size(), a, added.size());
            m_sideChunkRows.push_back({startRow, int(left.size()) - 1});
        }
        m_left->setRows(left, pal, font, tab);
        m_right->setRows(right, pal, font, tab);
        m_stack->setCurrentIndex(1);
    }
    applySyntax();
    for (CodeView *v : {m_inline, m_left, m_right}) {
        v->setShowWhitespace(m_showWhitespace);
        v->setShowLineNumbers(m_lineNumbers);
        v->setHighlightClickedWord(m_clickWord);
        v->setWordWrap(m_wrap);
    }
    m_overview->setColors(Settings::emphasis(pal.added), Settings::emphasis(pal.removed), pal.movedEmph,
                          pal.currentMatch);
    applyCommentHeights();
    updateOverview();
    applySearchHighlights();
    if (!m_highlightedWord.isEmpty())
        highlightWord(m_highlightedWord);
    scheduleWrapPadding();
}

void DiffPane::updateOverview()
{
    QVector<OverviewBar::Mark> left, right;
    if (m_mode == Mode::Inline) {
        for (const Row &r : m_inline->rows())
            left << markFor(r);
        m_overview->setRows(left, left);
    } else {
        for (const Row &r : m_left->rows())
            left << markFor(r);
        for (const Row &r : m_right->rows())
            right << markFor(r);
        m_overview->setRows(left, right);
    }
    updateVisibleRange();
}

void DiffPane::updateVisibleRange()
{
    CodeView *v = activeView();
    const int visible = v->viewport()->height() / std::max(1, v->fontMetrics().lineSpacing());
    m_overview->setVisibleRange(v->topRow(), visible);
}

void DiffPane::scheduleWrapPadding()
{
    if (m_hasDiff)
        m_wrapTimer->start(); // comment heights and (side by side) wrap padding
}

void DiffPane::applyWrapPadding()
{
    if (m_mode != Mode::SideBySide || isEditing())
        return;
    const int rows = int(std::min(m_left->rows().size(), m_right->rows().size()));
    if (!m_wrap) {
        m_left->setWrapPadding({});
        m_right->setWrapPadding({});
        return;
    }
    const int top = m_right->topRow();
    QVector<int> padLeft(rows, 0), padRight(rows, 0);
    for (int i = 0; i < rows; ++i) {
        const int a = m_left->naturalLineCount(i);
        const int b = m_right->naturalLineCount(i);
        padLeft[i] = std::max(0, b - a);
        padRight[i] = std::max(0, a - b);
    }
    m_left->setWrapPadding(padLeft);
    m_right->setWrapPadding(padRight);
    // Keep the same row at the top after the line counts changed.
    const QTextBlock b = m_right->document()->findBlockByNumber(top);
    if (b.isValid())
        m_right->verticalScrollBar()->setValue(b.firstLineNumber());
    applySearchHighlights();
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
    m_overview->setCurrentRange(first, last);
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

void DiffPane::goToChange(int index)
{
    if (changeCount() > 0)
        showChange(std::clamp(index, 0, changeCount() - 1));
}

// ---- Search ---------------------------------------------------------------

bool DiffPane::hasSelection() const
{
    for (CodeView *v : {m_inline, m_left, m_right})
        if (v->isVisible() && v->textCursor().hasSelection())
            return true;
    return false;
}

void DiffPane::setSearch(const SearchQuery &query, SearchScope scope)
{
    m_query = query;
    m_scope = scope;
    m_selView = nullptr;
    if (scope == SearchScope::Selection) {
        // Remember the selection now: moving to a match replaces the text cursor.
        CodeView *focused = nullptr;
        for (CodeView *v : {m_inline, m_left, m_right})
            if (v->isVisible() && v->textCursor().hasSelection() && (!focused || v->hasFocus()))
                focused = v;
        if (focused) {
            m_selView = focused;
            m_selStart = focused->textCursor().selectionStart();
            m_selEnd = focused->textCursor().selectionEnd();
        }
    }
    recomputeMatches();
    if (!m_matches.isEmpty())
        revealMatch(0);
}

void DiffPane::clearSearch()
{
    m_query = SearchQuery();
    m_matches.clear();
    m_currentMatch = -1;
    m_selView = nullptr;
    applySearchHighlights();
    emit searchResultsChanged(0, -1);
}

void DiffPane::recomputeMatches()
{
    m_matches.clear();
    m_currentMatch = -1;
    if (m_hasDiff && m_query.isValid()) {
        const QVector<LineMatch> all = findInDiff(m_file.diff, m_query);
        if (m_scope == SearchScope::Selection) {
            if (m_selView) {
                // Keep matches whose characters lie inside the remembered selection.
                const QVector<int> &rowOf = m_selView == m_inline ? m_inlineRowOfLine
                                            : m_selView == m_left ? m_leftRowOfLine
                                                                  : m_rightRowOfLine;
                for (const LineMatch &m : all) {
                    const int row = rowOf.value(m.lineIndex, -1);
                    if (row < 0)
                        continue;
                    const QTextBlock b = m_selView->document()->findBlockByNumber(row);
                    const int pos = b.position() + m.span.start;
                    if (pos >= m_selStart && pos + m.span.length <= m_selEnd)
                        m_matches.push_back(m);
                }
            }
        } else {
            m_matches = all;
        }
    }
    applySearchHighlights();
    emit searchResultsChanged(int(m_matches.size()), m_currentMatch);
}

void DiffPane::applySearchHighlights()
{
    QVector<ViewHit> inlineHits, leftHits, rightHits;
    int curInline = -1, curLeft = -1, curRight = -1;
    QVector<int> overviewRows;
    for (int i = 0; i < m_matches.size(); ++i) {
        const LineMatch &m = m_matches[i];
        const LineKind kind = m_file.diff.lines[m.lineIndex].kind;
        if (m_mode == Mode::Inline) {
            const int row = m_inlineRowOfLine.value(m.lineIndex, -1);
            if (row < 0)
                continue;
            if (i == m_currentMatch)
                curInline = int(inlineHits.size());
            inlineHits.push_back({row, m.span.start, m.span.length});
            overviewRows << row;
        } else {
            const int lrow = kind != LineKind::Added ? m_leftRowOfLine.value(m.lineIndex, -1) : -1;
            const int rrow = kind != LineKind::Removed ? m_rightRowOfLine.value(m.lineIndex, -1) : -1;
            if (lrow >= 0) {
                if (i == m_currentMatch)
                    curLeft = int(leftHits.size());
                leftHits.push_back({lrow, m.span.start, m.span.length});
            }
            if (rrow >= 0) {
                if (i == m_currentMatch)
                    curRight = int(rightHits.size());
                rightHits.push_back({rrow, m.span.start, m.span.length});
            }
            if (lrow >= 0 || rrow >= 0)
                overviewRows << std::max(lrow, rrow);
        }
    }
    m_inline->setSearchHits(inlineHits, curInline);
    m_left->setSearchHits(leftHits, curLeft);
    m_right->setSearchHits(rightHits, curRight);
    m_overview->setMatchRows(overviewRows);
}

void DiffPane::revealMatch(int index)
{
    if (index < 0 || index >= m_matches.size())
        return;
    m_currentMatch = index;
    const LineMatch &m = m_matches[index];
    // A match inside a collapsed region opens that region first.
    if (const int fold = m_foldOfLine.value(m.lineIndex, -1); fold >= 0) {
        m_expanded.insert(fold);
        rebuild();
    }
    const LineKind kind = m_file.diff.lines[m.lineIndex].kind;
    CodeView *v = m_inline;
    int row = m_inlineRowOfLine.value(m.lineIndex, -1);
    if (m_mode == Mode::SideBySide) {
        const bool onLeft = kind == LineKind::Removed;
        v = onLeft ? m_left : m_right;
        row = (onLeft ? m_leftRowOfLine : m_rightRowOfLine).value(m.lineIndex, -1);
    }
    if (row >= 0) {
        if (m_mode == Mode::SideBySide) {
            m_right->scrollToRow(row);
            m_left->verticalScrollBar()->setValue(m_right->verticalScrollBar()->value());
        } else {
            v->scrollToRow(row);
        }
        // Bring the match into view horizontally too.
        const QTextBlock b = v->document()->findBlockByNumber(row);
        QTextCursor c(b);
        c.setPosition(b.position() + m.span.start + m.span.length);
        if (!v->isEditing())
            v->setTextCursor(c);
        v->ensureCursorVisible();
    }
    applySearchHighlights();
    emit searchResultsChanged(int(m_matches.size()), m_currentMatch);
}

bool DiffPane::nextMatch()
{
    if (m_currentMatch + 1 >= m_matches.size())
        return false;
    revealMatch(m_currentMatch + 1);
    return true;
}

bool DiffPane::prevMatch()
{
    if (m_currentMatch <= 0)
        return false;
    revealMatch(m_currentMatch - 1);
    return true;
}

void DiffPane::goToMatch(int index)
{
    if (!m_matches.isEmpty())
        revealMatch(index < 0 ? int(m_matches.size()) - 1 : std::min(index, int(m_matches.size()) - 1));
}

// ---- Editing --------------------------------------------------------------

void DiffPane::setEditing(bool on)
{
    if (on == isEditing() || (on && !canEdit()))
        return;
    if (on) {
        const int keep = m_current;
        const int top = m_right->verticalScrollBar()->value();
        if (m_mode != Mode::SideBySide) {
            m_mode = Mode::SideBySide;
            emit modeChanged(m_mode);
        }
        // Editing needs every line present: rebuild without folds or wrap padding.
        const bool collapse = m_collapse;
        m_collapse = false;
        rebuild();
        m_collapse = collapse;
        m_right->setWrapPadding({});
        m_left->setWrapPadding({});
        m_right->setEditing(true);
        if (keep >= 0)
            showChange(keep);
        else
            m_right->verticalScrollBar()->setValue(top);
        m_right->setFocus();
    } else {
        m_right->setEditing(false);
        rebuild();
    }
}

bool DiffPane::isEditing() const
{
    return m_right->isEditing();
}

bool DiffPane::isModified() const
{
    return m_right->isModified();
}

QStringList DiffPane::editedLines() const
{
    return m_right->editedLines();
}

// ---- Context menu and moves ----------------------------------------------

int DiffPane::chunkAtRow(CodeView *view, int row) const
{
    if (row < 0)
        return -1;
    const auto &ranges = view == m_inline ? m_inlineChunkRows : m_sideChunkRows;
    for (int c = 0; c < ranges.size(); ++c)
        if (row >= ranges[c].first && row <= ranges[c].second)
            return c;
    return -1;
}

void DiffPane::showContextMenu(CodeView *view, int row, const QPoint &globalPos)
{
    if (!m_hasDiff)
        return;
    const FileDiff &d = m_file.diff;
    const int chunk = isEditing() ? -1 : chunkAtRow(view, row);
    const bool hasRemoved = chunk >= 0 && !DiffFormat::sideText(d, chunk, LineKind::Removed).isEmpty();
    const bool hasAdded = chunk >= 0 && !DiffFormat::sideText(d, chunk, LineKind::Added).isEmpty();

    QMenu menu(this);
    QAction *copy = menu.addAction(tr("&Copy"), view, &QPlainTextEdit::copy);
    copy->setShortcut(QKeySequence::Copy);
    copy->setEnabled(view->textCursor().hasSelection());
    menu.addSeparator();

    QAction *block = menu.addAction(tr("Copy this change as &diff"), this, [this, chunk] {
        const auto out = DiffFormat::unified(m_file.diff, m_file.paths, {chunk});
        copyToClipboard(out.text, out.html);
    });
    block->setEnabled(chunk >= 0);
    if (chunk < 0)
        block->setText(tr("Copy this change as diff (right-click a change)"));

    QAction *oldText = menu.addAction(tr("Copy &old text of this change (%1)").arg(m_leftName), this, [this, chunk] {
        copyToClipboard(DiffFormat::sideText(m_file.diff, chunk, LineKind::Removed));
    });
    oldText->setEnabled(hasRemoved);
    QAction *newText = menu.addAction(tr("Copy &new text of this change (%1)").arg(m_rightName), this, [this, chunk] {
        copyToClipboard(DiffFormat::sideText(m_file.diff, chunk, LineKind::Added));
    });
    newText->setEnabled(hasAdded);

    menu.addSeparator();
    QAction *toNew = menu.addAction(tr("Apply this change's old version to %1 (→)").arg(m_rightName), this,
                                    [this, chunk] { emit copyChangeRequested(chunk, true); });
    toNew->setEnabled(chunk >= 0 && canCopyTo(true));
    toNew->setStatusTip(tr("Replace the new side of this change with the old side, on disk"));
    QAction *toOld = menu.addAction(tr("Apply this change's new version to %1 (←)").arg(m_leftName), this,
                                    [this, chunk] { emit copyChangeRequested(chunk, false); });
    toOld->setEnabled(chunk >= 0 && canCopyTo(false));
    toOld->setStatusTip(tr("Replace the old side of this change with the new side, on disk"));

    if (m_file.reviewEnabled) {
        menu.addSeparator();
        const Row *r = row >= 0 && row < view->rows().size() ? &view->rows()[row] : nullptr;
        if (r && r->comment) {
            const qint64 id = r->threadId;
            menu.addAction(tr("&Reply to this comment\u2026"), this, [this, id] { emit replyRequested(id); });
            for (const GitHubReview::Thread &t : std::as_const(m_file.threads))
                if (t.first().id == id && !t.first().url.isEmpty()) {
                    const QString url = t.first().url;
                    menu.addAction(tr("Open comment in the browser"), this, [url] { QDesktopServices::openUrl(QUrl(url)); });
                }
        } else {
            QString side;
            int line = 0, startLine = 0;
            if (r && r->lineIndex >= 0) {
                const bool leftSide = view == m_left || (view == m_inline && r->kind == LineKind::Removed);
                side = leftSide ? QStringLiteral("LEFT") : QStringLiteral("RIGHT");
                line = leftSide ? r->oldNo : r->newNo;
                // A selection over several rows of this view makes a multi-line comment.
                const QTextCursor sel = view->textCursor();
                if (sel.hasSelection()) {
                    const int a = view->document()->findBlock(sel.selectionStart()).blockNumber();
                    const int b = view->document()->findBlock(sel.selectionEnd()).blockNumber();
                    if (row >= a && row <= b && a < b) {
                        for (int k = a; k <= b && k < view->rows().size(); ++k) {
                            const Row &rk = view->rows()[k];
                            const int no = leftSide ? rk.oldNo : rk.newNo;
                            if (rk.lineIndex < 0 || no <= 0)
                                continue;
                            if (!startLine)
                                startLine = no;
                            line = no;
                        }
                        if (startLine == line)
                            startLine = 0;
                    }
                }
            }
            const QString what = startLine ? tr("lines %1\u2013%2").arg(startLine).arg(line) : tr("line %1").arg(line);
            QAction *add = menu.addAction(tr("Add review &comment on %1\u2026").arg(what), this,
                                          [this, side, line, startLine] {
                                              emit commentRequested(m_file.paths.newPath.isEmpty() ? m_file.paths.oldPath
                                                                                                   : m_file.paths.newPath,
                                                                    side, line, startLine);
                                          });
            const bool ok = r && r->lineIndex >= 0 && line > 0 && nearChange(r->lineIndex);
            add->setEnabled(ok);
            if (!ok)
                add->setText(tr("Add review comment (only on or near changed lines)"));
        }
    }

    menu.addSeparator();
    QAction *file = menu.addAction(tr("Copy whole &file as diff"), this, [this] {
        const auto out = DiffFormat::unifiedFile(m_file.diff, m_file.paths);
        copyToClipboard(out.text, out.html);
    });
    file->setEnabled(!d.chunks.isEmpty());
    if (m_collapse) {
        QAction *expand = menu.addAction(tr("&Expand all unchanged regions"), this, &DiffPane::expandAll);
        expand->setEnabled(!isEditing());
    }
    menu.addSeparator();
    menu.addAction(tr("Select &all"), view, &QPlainTextEdit::selectAll);
    menu.exec(globalPos);
}

void DiffPane::jumpToMovePartner(CodeView *source, int row)
{
    const QVector<Row> &rows = source->rows();
    if (row < 0 || row >= rows.size() || rows[row].lineIndex < 0)
        return;
    const int partner = m_file.diff.lines[rows[row].lineIndex].partner;
    if (partner < 0)
        return;
    if (m_mode == Mode::Inline) {
        const int target = m_inlineRowOfLine.value(partner, -1);
        if (target < 0)
            return;
        m_inline->scrollToRow(target);
        m_inline->selectRow(target);
        return;
    }
    const bool toRight = m_file.diff.lines[partner].kind == LineKind::Added;
    CodeView *target = toRight ? m_right : m_left;
    const int targetRow = (toRight ? m_rightRowOfLine : m_leftRowOfLine).value(partner, -1);
    if (targetRow < 0)
        return;
    m_right->scrollToRow(targetRow);
    m_left->verticalScrollBar()->setValue(m_right->verticalScrollBar()->value());
    target->selectRow(targetRow);
    target->setFocus();
}

// ---- View options, copying, positions --------------------------------------

void DiffPane::setPaneVisibility(int which)
{
    m_paneVisibility = std::clamp(which, 0, 2);
    m_leftBox->setVisible(m_paneVisibility != 2);
    m_rightBox->setVisible(m_paneVisibility != 1);
    // The left pane's scrollbar is normally hidden (it follows the right one).
    m_left->setVerticalScrollBarPolicy(m_paneVisibility == 1 ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
}

void DiffPane::setShowLineNumbers(bool on)
{
    m_lineNumbers = on;
    for (CodeView *v : {m_inline, m_left, m_right})
        v->setShowLineNumbers(on);
}

void DiffPane::setBlame(const QVector<BlameLine> &oldSide, const QVector<BlameLine> &newSide)
{
    m_file.oldBlame = oldSide;
    m_file.newBlame = newSide;
    for (CodeView *v : {m_inline, m_left, m_right})
        v->setBlame(oldSide, newSide);
}

void DiffPane::setSyntaxEnabled(bool on)
{
    m_syntaxEnabled = on;
    applySyntax();
}

void DiffPane::setHighlightClickedWord(bool on)
{
    m_clickWord = on;
    for (CodeView *v : {m_inline, m_left, m_right})
        v->setHighlightClickedWord(on);
    if (!on)
        highlightWord({});
}

void DiffPane::setZoom(int percent)
{
    m_zoom = std::clamp(percent, 30, 400);
    applySettings();
}

void DiffPane::highlightWord(const QString &word)
{
    m_highlightedWord = word;
    QColor c = palette().color(QPalette::Highlight);
    c.setAlpha(70);
    for (CodeView *v : {m_inline, m_left, m_right})
        v->setWordHighlight(word, c);
}

CodeView *DiffPane::focusedView() const
{
    for (CodeView *v : {m_inline, m_left, m_right})
        if (v->hasFocus() && v->isVisible())
            return v;
    if (m_lastFocused && m_lastFocused->isVisible())
        return m_lastFocused;
    return activeView();
}

bool DiffPane::goToLine(int line, bool newSide)
{
    const FileDiff &d = m_file.diff;
    for (int i = 0; i < d.lines.size(); ++i) {
        const DiffLine &l = d.lines[i];
        if ((newSide ? l.newNo : l.oldNo) != line)
            continue;
        if (const int fold = m_foldOfLine.value(i, -1); fold >= 0) {
            m_expanded.insert(fold);
            rebuild();
        }
        CodeView *v = m_inline;
        int row = m_inlineRowOfLine.value(i, -1);
        if (m_mode == Mode::SideBySide) {
            v = newSide ? m_right : m_left;
            row = (newSide ? m_rightRowOfLine : m_leftRowOfLine).value(i, -1);
        }
        if (row < 0)
            return false;
        if (m_mode == Mode::SideBySide) {
            m_right->scrollToRow(row);
            m_left->verticalScrollBar()->setValue(m_right->verticalScrollBar()->value());
        } else {
            v->scrollToRow(row);
        }
        v->selectRow(row);
        v->setFocus();
        return true;
    }
    return false;
}

int DiffPane::currentLine(bool newSide) const
{
    CodeView *v = m_mode == Mode::Inline ? m_inline : (newSide ? m_right : m_left);
    const int start = v->hasFocus() || v == m_lastFocused ? v->cursorRow() : v->topRow();
    const QVector<Row> &rows = v->rows();
    for (int r = std::max(0, start); r < rows.size(); ++r) {
        const int n = newSide ? rows[r].newNo : rows[r].oldNo;
        if (n > 0 && !rows[r].filler)
            return n;
    }
    return 0;
}

bool DiffPane::copySelection(CopyKind kind)
{
    CodeView *v = focusedView();
    const QString text = v->selectedPlainText();
    if (text.isEmpty())
        return false;
    auto *mime = new QMimeData;
    const QString lang = effectiveLanguage().toLower().replace(QStringLiteral("++"), QStringLiteral("pp"));
    if (kind == CopyKind::Rich) {
        mime->setText(text);
        mime->setHtml(v->selectedHtml());
    } else if (kind == CopyKind::Plain) {
        mime->setText(text);
    } else if (kind == CopyKind::Markdown) {
        mime->setText(QStringLiteral("```%1\n%2\n```\n").arg(lang, text));
    } else {
        // For pasting into a review: where it is, then the code.
        const auto [first, last] = v->selectedRows();
        const bool newSide = v != m_left;
        int from = 0, to = 0;
        for (int r = first; r <= last && r < v->rows().size(); ++r) {
            const Row &row = v->rows()[r];
            const int n = newSide ? (row.newNo ? row.newNo : row.oldNo) : row.oldNo;
            if (n <= 0)
                continue;
            if (!from)
                from = n;
            to = n;
        }
        const QString path = newSide ? m_file.paths.newPath : m_file.paths.oldPath;
        const QString where = from == to ? QStringLiteral("%1:%2").arg(path).arg(from)
                                         : QStringLiteral("%1:%2-%3").arg(path).arg(from).arg(to);
        mime->setText(QStringLiteral("`%1`\n```%2\n%3\n```\n").arg(where, lang, text));
    }
    QApplication::clipboard()->setMimeData(mime);
    return true;
}

void DiffPane::selectAll()
{
    focusedView()->selectAll();
}

DiffPane::ScrollState DiffPane::scrollState() const
{
    ScrollState s;
    if (!m_hasDiff)
        return s;
    s.top = activeView()->topRow();
    s.change = m_current;
    return s;
}

void DiffPane::restoreScroll(const ScrollState &state)
{
    if (!state.valid() || !m_hasDiff)
        return;
    if (state.change >= 0 && state.change < changeCount()) {
        const auto &ranges = m_mode == Mode::Inline ? m_inlineChunkRows : m_sideChunkRows;
        m_current = state.change;
        if (m_current < ranges.size()) {
            for (CodeView *cv : {m_inline, m_left, m_right})
                cv->setCurrentRange(ranges[m_current].first, ranges[m_current].second);
            m_overview->setCurrentRange(ranges[m_current].first, ranges[m_current].second);
        }
        emit currentChangeChanged(m_current, changeCount());
    }
    CodeView *v = activeView();
    const QTextBlock b = v->document()->findBlockByNumber(state.top);
    if (b.isValid()) {
        v->verticalScrollBar()->setValue(b.firstLineNumber());
        if (m_mode == Mode::SideBySide)
            m_left->verticalScrollBar()->setValue(m_right->verticalScrollBar()->value());
    }
}
