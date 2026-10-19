#include "MergeWindow.h"

#include "CodeView.h"
#include "DiffPane.h"
#include "LineDiff.h"
#include "Log.h"
#include "Settings.h"
#include "Syntax.h"

#include <QCloseEvent>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSplitter>
#include <QStatusBar>
#include <QTextBlock>
#include <QToolBar>
#include <QVBoxLayout>

namespace {

QStringList lines(const QByteArray &data)
{
    QStringList out = LineDiff::splitLines(QString::fromUtf8(data));
    for (QString &l : out)
        if (l.endsWith(u'\r'))
            l.chop(1);
    return out;
}

QWidget *labelled(const QString &text, QWidget *w)
{
    auto *box = new QWidget;
    auto *v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    auto *label = new QLabel(text);
    label->setContentsMargins(6, 2, 6, 0);
    v->addWidget(label);
    v->addWidget(w, 1);
    return box;
}

} // namespace

MergeWindow::MergeWindow(const Input &input, QWidget *parent) : QMainWindow(parent), m_in(input)
{
    setAttribute(Qt::WA_DeleteOnClose, false);
    const QString name = QFileInfo(input.fileName.isEmpty() ? input.outputPath : input.fileName).fileName();
    setWindowTitle(tr("Merge %1 — yadiff").arg(name));
    m_base = lines(input.base);
    m_ours = lines(input.ours);
    m_theirs = lines(input.theirs);
    // The result keeps our side's line endings and final newline.
    const QByteArray &style = input.ours.isEmpty() ? input.theirs : input.ours;
    m_eol = style.contains("\r\n") ? QByteArray("\r\n") : QByteArray("\n");
    m_finalNewline = style.isEmpty() || style.endsWith('\n');
    m_regions = Merge3::regions(m_base, m_ours, m_theirs);
    m_picks.fill(Merge3::Pick::Auto, m_regions.size());
    m_edited.fill(false, m_regions.size());

    m_oursView = new CodeView(CodeView::Numbers::New);
    m_baseView = new CodeView(CodeView::Numbers::New);
    m_theirsView = new CodeView(CodeView::Numbers::New);
    m_result = new QPlainTextEdit;
    m_result->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_result->setFont(Settings::font());
    m_result->setTabStopDistance(m_result->fontMetrics().horizontalAdvance(u' ') * Settings::tabWidth());

    auto *top = new QSplitter(Qt::Horizontal);
    auto side = [](const QString &role, const QString &label) {
        return label.compare(role, Qt::CaseInsensitive) == 0 || label.startsWith(role) ? label
                                                                                       : QStringLiteral("%1: %2").arg(role, label);
    };
    top->addWidget(labelled(side(tr("Ours"), input.oursLabel), m_oursView));
    top->addWidget(labelled(side(tr("Base"), input.baseLabel), m_baseView));
    top->addWidget(labelled(side(tr("Theirs"), input.theirsLabel), m_theirsView));
    top->setChildrenCollapsible(false);
    auto *split = new QSplitter(Qt::Vertical);
    split->addWidget(top);
    split->addWidget(labelled(tr("Result: %1").arg(QDir::toNativeSeparators(input.outputPath)), m_result));
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    setCentralWidget(split);

    // Actions, in a Merge menu and on the toolbar.
    QMenu *menu = menuBar()->addMenu(tr("&Merge"));
    QToolBar *bar = addToolBar(tr("Merge"));
    bar->setMovable(false);
    auto add = [&](const QString &text, const QKeySequence &key, const QString &tip, auto fn, bool onBar = true) {
        QAction *a = menu->addAction(text, this, fn);
        a->setShortcut(key);
        a->setToolTip(QStringLiteral("%1 (%2)").arg(tip, key.toString(QKeySequence::NativeText)));
        a->setStatusTip(tip);
        if (onBar)
            bar->addAction(a);
        return a;
    };
    add(tr("Previous conflict"), QKeySequence(Qt::ALT | Qt::Key_Up), tr("Go to the previous conflict"),
        [this] { goToConflict(-1); });
    add(tr("Next conflict"), QKeySequence(Qt::ALT | Qt::Key_Down), tr("Go to the next conflict"), [this] { goToConflict(1); });
    menu->addSeparator();
    bar->addSeparator();
    add(tr("Take ours"), QKeySequence(Qt::CTRL | Qt::Key_1), tr("Use our version of this conflict"),
        [this] { take(Merge3::Pick::Ours); });
    add(tr("Take base"), QKeySequence(Qt::CTRL | Qt::Key_2), tr("Use the common ancestor's version"),
        [this] { take(Merge3::Pick::Base); });
    add(tr("Take theirs"), QKeySequence(Qt::CTRL | Qt::Key_3), tr("Use their version of this conflict"),
        [this] { take(Merge3::Pick::Theirs); });
    add(tr("Both, ours first"), QKeySequence(Qt::CTRL | Qt::Key_4), tr("Keep both versions, ours above theirs"),
        [this] { take(Merge3::Pick::OursThenTheirs); });
    add(tr("Both, theirs first"), QKeySequence(Qt::CTRL | Qt::Key_5), tr("Keep both versions, theirs above ours"),
        [this] { take(Merge3::Pick::TheirsThenOurs); });
    add(tr("Reset to conflict markers"), QKeySequence(Qt::CTRL | Qt::Key_0),
        tr("Put this conflict back as <<<<<<< ... >>>>>>> markers"), [this] { take(Merge3::Pick::Markers); });
    menu->addSeparator();
    add(tr("Take ours for every unresolved conflict"), QKeySequence(), tr("Resolve all remaining conflicts with our version"),
        [this] { takeForAll(Merge3::Pick::Ours); }, false);
    add(tr("Take theirs for every unresolved conflict"), QKeySequence(),
        tr("Resolve all remaining conflicts with their version"), [this] { takeForAll(Merge3::Pick::Theirs); }, false);
    menu->addSeparator();
    bar->addSeparator();
    add(tr("&Save"), QKeySequence::Save, tr("Write the result"), [this] { save(); });
    add(tr("Save and &close"), QKeySequence(Qt::CTRL | Qt::Key_Return), tr("Write the result and close this window"), [this] {
        if (save())
            close();
    });
    add(tr("&Close"), QKeySequence::Close, tr("Close (asks about unsaved changes)"), [this] { close(); }, false);
    m_status = new QLabel;
    statusBar()->addPermanentWidget(m_status);

    buildViews();
    buildResult();
    connect(m_result->document(), &QTextDocument::contentsChange, this, &MergeWindow::onEdited);
    connect(m_result, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        if (!m_applying)
            setCurrent(regionAt(m_result->textCursor().position()), false);
    });
    // The three sides scroll together.
    const QList<CodeView *> views{m_oursView, m_baseView, m_theirsView};
    for (CodeView *v : views) {
        connect(v->verticalScrollBar(), &QScrollBar::valueChanged, this, [views](int value) {
            for (CodeView *o : views)
                if (o->verticalScrollBar()->value() != value)
                    o->verticalScrollBar()->setValue(value);
        });
        connect(v, &QPlainTextEdit::cursorPositionChanged, this, [this, v] {
            const int row = v->cursorRow();
            int region = -1;
            for (int i = 0; i < m_rowOfRegion.size() && m_rowOfRegion[i] <= row; ++i)
                region = i;
            if (region >= 0 && region != m_current)
                setCurrent(region, false);
        });
    }
    resize(1400, 900);
    goToConflict(1);
    if (m_current < 0)
        updateMarks();
}

void MergeWindow::buildViews()
{
    const ViewPalette pal = diffPalette(this);
    QFont font = Settings::font();
    QVector<Row> o, b, t;
    m_rowOfRegion.clear();
    auto lineRow = [](const QString &text, int no, LineKind kind, bool grey) {
        Row r;
        r.text = text;
        r.newNo = no;
        r.lineIndex = no - 1;
        r.kind = kind;
        r.unimportant = grey;
        return r;
    };
    Row filler;
    filler.filler = true;
    using K = Merge3::Region::Kind;
    for (const Merge3::Region &r : std::as_const(m_regions)) {
        m_rowOfRegion << int(o.size());
        const int h = std::max({r.oursCount, r.baseCount, r.theirsCount});
        // Changed sides are green, conflicting ones red; the base of a change is grey.
        const LineKind oursKind = r.kind == K::Conflict ? LineKind::Removed
                                  : r.kind == K::Ours || r.kind == K::Both ? LineKind::Added
                                                                          : LineKind::Equal;
        const LineKind theirsKind = r.kind == K::Conflict ? LineKind::Removed
                                    : r.kind == K::Theirs || r.kind == K::Both ? LineKind::Added
                                                                              : LineKind::Equal;
        const bool baseGrey = r.kind != K::Unchanged;
        for (int k = 0; k < h; ++k) {
            o << (k < r.oursCount ? lineRow(m_ours[r.oursStart + k], r.oursStart + k + 1, oursKind, false) : filler);
            b << (k < r.baseCount ? lineRow(m_base[r.baseStart + k], r.baseStart + k + 1, LineKind::Equal, baseGrey) : filler);
            t << (k < r.theirsCount ? lineRow(m_theirs[r.theirsStart + k], r.theirsStart + k + 1, theirsKind, false)
                                    : filler);
        }
    }
    m_rowOfRegion << int(o.size());
    const QString language = Syntax::detect(m_in.fileName.isEmpty() ? m_in.outputPath : m_in.fileName);
    for (CodeView *v : {m_oursView, m_baseView, m_theirsView}) {
        v->setRows(v == m_oursView ? o : v == m_baseView ? b : t, pal, font, Settings::tabWidth());
        v->setSyntax(language, Settings::isDarkTheme());
    }
    m_highlighter = Syntax::apply(m_highlighter, m_result->document(), language, Settings::isDarkTheme());
}

void MergeWindow::buildResult()
{
    // Every line ends with a newline, so region i is exactly [start i, start i+1).
    QString text;
    QVector<int> starts;
    for (int i = 0; i < m_regions.size(); ++i) {
        starts << int(text.size());
        for (const QString &l : Merge3::take(m_regions[i], m_picks[i], m_base, m_ours, m_theirs, m_in.oursLabel,
                                              m_in.theirsLabel))
            text += l + u'\n';
    }
    m_applying = true;
    m_result->setPlainText(text);
    m_applying = false;
    m_starts.clear();
    for (int pos : std::as_const(starts)) {
        QTextCursor c(m_result->document());
        c.setPosition(pos);
        c.setKeepPositionOnInsert(true); // typing at a region's start goes into the one before
        m_starts << c;
    }
    m_result->document()->setModified(false);
}

int MergeWindow::regionStart(int index) const
{
    return m_starts[index].position();
}

int MergeWindow::regionEnd(int index) const
{
    return index + 1 < m_starts.size() ? m_starts[index + 1].position() : m_result->document()->characterCount() - 1;
}

int MergeWindow::regionAt(int position) const
{
    int found = -1;
    for (int i = 0; i < m_starts.size() && regionStart(i) <= position; ++i)
        if (regionEnd(i) > position || i + 1 == m_starts.size() || regionStart(i) == regionEnd(i))
            found = i;
    return found;
}

void MergeWindow::replaceRegion(int index, const QStringList &newLines)
{
    QString text;
    for (const QString &l : newLines)
        text += l + u'\n';
    const int s = regionStart(index), e = regionEnd(index);
    QVector<int> later;
    for (int j = index + 1; j < m_starts.size(); ++j)
        later << m_starts[j].position();
    m_applying = true;
    QTextCursor c(m_result->document());
    c.setPosition(s);
    c.setPosition(e, QTextCursor::KeepAnchor);
    c.insertText(text);
    m_applying = false;
    // The regions after this one move by the change in length (keepPositionOnInsert
    // would have left the next region's start in front of the new text).
    const int delta = int(text.size()) - (e - s);
    for (int j = index + 1; j < m_starts.size(); ++j)
        m_starts[j].setPosition(later[j - index - 1] + delta);
}

void MergeWindow::onEdited(int position, int removed, int added)
{
    if (m_applying || m_starts.isEmpty())
        return;
    Q_UNUSED(removed);
    // Hand edits resolve the regions they touch.
    const int first = regionAt(position), last = regionAt(position + added);
    for (int i = std::max(0, first); i <= last && i < m_edited.size(); ++i)
        m_edited[i] = true;
    updateMarks();
}

int MergeWindow::conflictCount() const
{
    int n = 0;
    for (const Merge3::Region &r : m_regions)
        n += r.kind == Merge3::Region::Kind::Conflict;
    return n;
}

int MergeWindow::unresolvedCount() const
{
    int n = 0;
    for (int i = 0; i < m_regions.size(); ++i)
        n += m_regions[i].kind == Merge3::Region::Kind::Conflict && !m_edited[i]
             && (m_picks[i] == Merge3::Pick::Auto || m_picks[i] == Merge3::Pick::Markers);
    return n;
}

QString MergeWindow::resultText() const
{
    return m_result->toPlainText();
}

bool MergeWindow::isModified() const
{
    return m_result->document()->isModified();
}

void MergeWindow::setCurrent(int index, bool scroll)
{
    if (index < 0 || index >= m_regions.size())
        return;
    m_current = index;
    const int first = m_rowOfRegion[index], last = m_rowOfRegion[index + 1] - 1;
    for (CodeView *v : {m_oursView, m_baseView, m_theirsView}) {
        v->setCurrentRange(first, std::max(first, last));
        if (scroll)
            v->scrollToRow(first);
    }
    if (scroll) {
        m_applying = true;
        QTextCursor c(m_result->document());
        c.setPosition(regionStart(index));
        m_result->setTextCursor(c);
        m_result->centerCursor();
        m_applying = false;
    }
    updateMarks();
}

void MergeWindow::goToConflict(int delta)
{
    // Conflicts first; with none, every change.
    auto wanted = [this](int i, bool conflictsOnly) {
        return conflictsOnly ? m_regions[i].kind == Merge3::Region::Kind::Conflict
                             : m_regions[i].kind != Merge3::Region::Kind::Unchanged;
    };
    const bool conflictsOnly = conflictCount() > 0;
    for (int i = m_current + delta; i >= 0 && i < m_regions.size(); i += delta)
        if (wanted(i, conflictsOnly)) {
            setCurrent(i, true);
            return;
        }
    if (m_current < 0)
        for (int i = 0; i < m_regions.size(); ++i)
            if (wanted(i, conflictsOnly)) {
                setCurrent(i, true);
                return;
            }
    statusBar()->showMessage(delta > 0 ? tr("No more conflicts below") : tr("No more conflicts above"), 3000);
}

void MergeWindow::take(Merge3::Pick pick)
{
    if (m_current < 0)
        return;
    const int i = m_current;
    replaceRegion(i, Merge3::take(m_regions[i], pick, m_base, m_ours, m_theirs, m_in.oursLabel, m_in.theirsLabel));
    m_picks[i] = pick;
    m_edited[i] = false;
    // On to the next conflict still to do.
    for (int j = i + 1; j < m_regions.size(); ++j)
        if (m_regions[j].kind == Merge3::Region::Kind::Conflict && !m_edited[j]
            && (m_picks[j] == Merge3::Pick::Auto || m_picks[j] == Merge3::Pick::Markers)) {
            setCurrent(j, true);
            return;
        }
    setCurrent(i, false);
}

void MergeWindow::takeForAll(Merge3::Pick pick)
{
    for (int i = 0; i < m_regions.size(); ++i)
        if (m_regions[i].kind == Merge3::Region::Kind::Conflict && !m_edited[i]
            && (m_picks[i] == Merge3::Pick::Auto || m_picks[i] == Merge3::Pick::Markers)) {
            replaceRegion(i, Merge3::take(m_regions[i], pick, m_base, m_ours, m_theirs));
            m_picks[i] = pick;
        }
    updateMarks();
}

void MergeWindow::updateMarks()
{
    const ViewPalette pal = diffPalette(this);
    QList<QTextEdit::ExtraSelection> marks;
    for (int i = 0; i < m_regions.size(); ++i) {
        const auto kind = m_regions[i].kind;
        if (kind == Merge3::Region::Kind::Unchanged)
            continue;
        const bool conflict = kind == Merge3::Region::Kind::Conflict;
        const bool open = conflict && !m_edited[i] && (m_picks[i] == Merge3::Pick::Auto || m_picks[i] == Merge3::Pick::Markers);
        QColor color = open ? pal.removed : conflict ? pal.added : pal.unimportant;
        if (i == m_current) // a shade stronger, so the text stays readable
            color = Settings::isDarkTheme() ? color.lighter(130) : color.darker(112);
        const int s = regionStart(i), e = regionEnd(i);
        for (QTextBlock b = m_result->document()->findBlock(s); b.isValid() && b.position() < e; b = b.next()) {
            QTextEdit::ExtraSelection sel;
            sel.cursor = QTextCursor(b);
            sel.format.setBackground(color);
            sel.format.setProperty(QTextFormat::FullWidthSelection, true);
            marks << sel;
        }
    }
    m_result->setExtraSelections(marks);
    const int conflicts = conflictCount(), open = unresolvedCount();
    m_status->setText(conflicts == 0 ? tr("No conflicts: everything merged automatically")
                      : open == 0    ? tr("All %n conflict(s) resolved", nullptr, conflicts)
                                     : tr("%1 of %n conflict(s) unresolved", nullptr, conflicts).arg(open));
}

bool MergeWindow::save()
{
    const int open = unresolvedCount();
    QStringList out = m_result->toPlainText().split(u'\n');
    if (!out.isEmpty() && out.last().isEmpty())
        out.removeLast(); // the newline after the last line
    bool markers = false;
    for (const QString &l : std::as_const(out))
        markers |= l.startsWith(QLatin1String("<<<<<<< ")) || l.startsWith(QLatin1String(">>>>>>> "));
    if ((open > 0 || markers)
        && QMessageBox::question(this, tr("Save"),
                                 open > 0 ? tr("%n conflict(s) are still unresolved. Save with the conflict markers?", nullptr, open)
                                          : tr("The result still has conflict markers. Save it anyway?"))
               != QMessageBox::Yes)
        return false;
    QByteArray data;
    for (qsizetype i = 0; i < out.size(); ++i) {
        data += out[i].toUtf8();
        if (i + 1 < out.size() || m_finalNewline)
            data += m_eol;
    }
    QFile f(m_in.outputPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(data) != data.size()) {
        qCWarning(lcFile).noquote() << "could not write the merge result" << m_in.outputPath << ":" << f.errorString();
        QMessageBox::warning(this, tr("Save"), tr("Could not write %1: %2").arg(m_in.outputPath, f.errorString()));
        return false;
    }
    f.close();
    m_result->document()->setModified(false);
    m_savedResolved = open == 0 && !markers;
    qCInfo(lcFile).noquote() << "saved the merge result" << m_in.outputPath << "(" << data.size() << "bytes," << open
                             << "conflicts unresolved," << (markers ? "with" : "no") << "conflict markers)";
    statusBar()->showMessage(tr("Saved %1").arg(m_in.outputPath), 4000);
    emit saved(m_savedResolved);
    return true;
}

void MergeWindow::closeEvent(QCloseEvent *event)
{
    if (isModified()) {
        const auto answer = QMessageBox::question(this, tr("Close"), tr("Save the merge result before closing?"),
                                                  QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel || (answer == QMessageBox::Save && !save())) {
            event->ignore();
            return;
        }
    }
    QMainWindow::closeEvent(event);
}
