#include "FileList.h"

#include <QContextMenuEvent>
#include <QHeaderView>
#include <QKeyEvent>
#include <QStyledItemDelegate>
#include <QMap>
#include <QPainter>
#include <QStyle>
#include <QTime>
#include <QTimer>

#include <algorithm>
#include <functional>
#include <numeric>

namespace {

constexpr int kIndexRole = Qt::UserRole + 1;
constexpr int kMarkedRole = Qt::UserRole + 2;
constexpr int kFolderRole = Qt::UserRole + 5; // folder items: repository-relative folder
constexpr int kStatsRole = Qt::UserRole + 3;
constexpr int kPathRole = Qt::UserRole + 4; // QPoint(added, removed); absent when unknown
constexpr int kLoadRole = Qt::UserRole + 6;   // FileList::LoadState
constexpr int kPlaceholderRole = Qt::UserRole + 7;

// Draws the file name as usual, plus "+12 −3" in green and red at the right edge
// when there is room for both (the name always wins when the pane is narrow).
class StatsDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        paintRow(p, option, index);
        if (index.data(kMarkedRole).toBool())
            paintMark(p, option, index);
    }

private:
    // A marked file: a stripe of the mark colour down its left edge, and when it is
    // selected (the selection hides the mark's background) a frame of it too.
    void paintMark(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        QColor mark = index.data(Qt::BackgroundRole).value<QBrush>().color();
        if (!mark.isValid() || mark.alpha() == 0)
            return;
        mark.setAlpha(255);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->fillRect(QRect(option.rect.left(), option.rect.top(), 4, option.rect.height()), mark);
        if (option.state & QStyle::State_Selected) {
            p->setPen(QPen(mark, 2));
            p->setBrush(Qt::NoBrush);
            p->drawRect(QRectF(option.rect).adjusted(1, 1, -1, -1));
        }
        p->restore();
    }

    void paintRow(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        const auto load = FileList::LoadState(index.data(kLoadRole).toInt());
        const bool pending = load == FileList::LoadState::Queued || load == FileList::LoadState::Loading;
        if (pending || load == FileList::LoadState::Failed) {
            paintLoadState(p, option, index, load);
            return;
        }
        const QVariant v = index.data(kStatsRole);
        if (!v.isValid()) {
            QStyledItemDelegate::paint(p, option, index);
            return;
        }
        const QPoint st = v.toPoint();
        const QString plus = QStringLiteral("+%1").arg(st.x());
        const QString minus = QStringLiteral("\u2212%1").arg(st.y());
        const QFontMetrics fm(option.font);
        const int gap = fm.horizontalAdvance(u' ');
        const int statsW = fm.horizontalAdvance(plus) + gap + fm.horizontalAdvance(minus) + 8;
        const int nameW = fm.horizontalAdvance(index.data(Qt::DisplayRole).toString());
        const bool room = option.rect.width() >= statsW + nameW + 40; // 40: icon and padding

        QStyleOptionViewItem opt = option;
        if (room)
            opt.rect.setRight(opt.rect.right() - statsW);
        QStyledItemDelegate::paint(p, opt, index);
        if (!room)
            return;
        // Fill behind the numbers like the rest of the row (selection / hover).
        QStyleOptionViewItem bg = option;
        initStyleOption(&bg, index);
        bg.rect = QRect(option.rect.right() - statsW + 1, option.rect.top(), statsW, option.rect.height());
        bg.text.clear();
        bg.icon = QIcon();
        bg.features &= ~QStyleOptionViewItem::HasCheckIndicator;
        bg.widget->style()->drawControl(QStyle::CE_ItemViewItem, &bg, p, bg.widget);
        const QRect r = bg.rect.adjusted(0, 0, -4, 0);
        p->save();
        p->setFont(option.font);
        p->setPen(QColor(0x2e, 0xa0, 0x43));
        p->drawText(r.adjusted(0, 0, -(fm.horizontalAdvance(minus) + gap), 0), Qt::AlignRight | Qt::AlignVCenter, plus);
        p->setPen(QColor(0xd1, 0x3b, 0x3b));
        p->drawText(r, Qt::AlignRight | Qt::AlignVCenter, minus);
        p->restore();
    }

    // The name greyed, and the state at the right edge: a hollow dot (waiting), a turning
    // arc (loading) or a red "!" (failed).
    void paintLoadState(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index,
                        FileList::LoadState load) const
    {
        const int mark = option.rect.height();
        const bool selected = option.state & QStyle::State_Selected;
        QStyleOptionViewItem opt = option;
        opt.rect.setRight(opt.rect.right() - mark);
        if (load != FileList::LoadState::Failed && !selected)
            opt.palette.setColor(QPalette::Text, option.palette.color(QPalette::PlaceholderText));
        QStyledItemDelegate::paint(p, opt, index);
        QStyleOptionViewItem bg = option;
        initStyleOption(&bg, index);
        bg.rect = QRect(option.rect.right() - mark + 1, option.rect.top(), mark, option.rect.height());
        bg.text.clear();
        bg.icon = QIcon();
        bg.widget->style()->drawControl(QStyle::CE_ItemViewItem, &bg, p, bg.widget);
        const QRectF r = QRectF(bg.rect).adjusted(mark * 0.3, mark * 0.3, -mark * 0.3, -mark * 0.3);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        // On a selected row the marks take the selection's text colour, to stay visible.
        const QColor grey = selected ? option.palette.color(QPalette::HighlightedText)
                                     : option.palette.color(QPalette::PlaceholderText);
        if (load == FileList::LoadState::Queued) {
            p->setPen(QPen(grey, 1.2));
            p->drawEllipse(r.adjusted(r.width() * 0.2, r.height() * 0.2, -r.width() * 0.2, -r.height() * 0.2));
        } else if (load == FileList::LoadState::Loading) {
            const int angle = int((QTime::currentTime().msecsSinceStartOfDay() / 3) % 360);
            p->setPen(QPen(selected ? option.palette.color(QPalette::HighlightedText) : option.palette.color(QPalette::Highlight),
                           2.0, Qt::SolidLine, Qt::RoundCap));
            p->drawArc(r, -angle * 16, 270 * 16);
        } else {
            QFont f = option.font;
            f.setBold(true);
            p->setFont(f);
            p->setPen(QColor(0xd1, 0x3b, 0x3b));
            p->drawText(bg.rect, Qt::AlignCenter, QStringLiteral("!"));
        }
        p->restore();
    }
};

QColor statusColor(QChar s)
{
    switch (s.unicode()) {
    case 'A': return QColor(0x2e, 0xa0, 0x43);
    case 'D': return QColor(0xd1, 0x3b, 0x3b);
    case 'R':
    case 'C': return QColor(0x3b, 0x7b, 0xd1);
    case 'U': return QColor(0x9b, 0x4d, 0xca);
    case '?': return QColor(0x80, 0x80, 0x80);
    case 'T': return QColor(0x1f, 0x9c, 0x9c);
    default: return QColor(0xd9, 0x8e, 0x04);
    }
}

QIcon statusIcon(QChar s)
{
    static QHash<QChar, QIcon> cache;
    if (auto it = cache.constFind(s); it != cache.constEnd())
        return *it;
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(statusColor(s));
    p.drawRoundedRect(QRectF(2, 2, 28, 28), 6, 6);
    QFont f;
    f.setBold(true);
    f.setPixelSize(20);
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(pm.rect(), Qt::AlignCenter, s == u'?' ? QStringLiteral("?") : QString(s));
    p.end();
    QIcon icon(pm);
    cache.insert(s, icon);
    return icon;
}

} // namespace

FileList::FileList(QWidget *parent) : QTreeWidget(parent)
{
    setHeaderHidden(true);
    setColumnCount(1);
    setItemDelegateForColumn(0, new StatsDelegate(this));
    header()->setStretchLastSection(true);
    setRootIsDecorated(true);
    setUniformRowHeights(true);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setIconSize(QSize(16, 16));
    // Long, fully qualified folder names keep their distinctive end visible.
    setTextElideMode(Qt::ElideMiddle);
    m_markColor = QColor(0xff, 0xd8, 0x4d, 110);
    connect(this, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *cur) {
        if (cur && cur->data(0, kIndexRole).isValid())
            emit fileActivated(cur->data(0, kIndexRole).toInt());
    });
    connect(this, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        if (item && item->data(0, kIndexRole).isValid())
            emit viewRequested(item->data(0, kIndexRole).toInt());
    });
}

void FileList::keyPressEvent(QKeyEvent *event)
{
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && currentIndex() >= 0) {
        emit viewRequested(currentIndex());
        return;
    }
    QTreeWidget::keyPressEvent(event);
}

void FileList::contextMenuEvent(QContextMenuEvent *event)
{
    // Right-clicking a file that isn't selected selects just it, as file managers do.
    if (QTreeWidgetItem *item = itemAt(event->pos()); item && item->data(0, kIndexRole).isValid() && !item->isSelected()) {
        clearSelection();
        setCurrentItem(item);
        item->setSelected(true);
    }
    emit contextMenuRequested(event->globalPos());
}

void FileList::styleLeaf(QTreeWidgetItem *item)
{
    const bool marked = item->data(0, kMarkedRole).toBool();
    item->setBackground(0, marked ? QBrush(m_markColor) : QBrush());
}

void FileList::setMarkColor(const QColor &color)
{
    m_markColor = color;
    for (QTreeWidgetItem *item : std::as_const(m_leaves))
        styleLeaf(item);
}

QTreeWidgetItem *FileList::leafFor(int index) const
{
    for (QTreeWidgetItem *item : m_leaves)
        if (item->data(0, kIndexRole).toInt() == index)
            return item;
    return nullptr;
}

bool FileList::isMarked(int index) const
{
    const QTreeWidgetItem *item = leafFor(index);
    return item && item->data(0, kMarkedRole).toBool();
}

void FileList::setMarked(int index, bool marked)
{
    QTreeWidgetItem *item = leafFor(index);
    if (!item || item->data(0, kMarkedRole).toBool() == marked)
        return;
    item->setData(0, kMarkedRole, marked);
    styleLeaf(item);
    emit markToggled(index, marked);
    if (m_filter.hideReviewed)
        applyFilter();
}

int FileList::markedCount() const
{
    int n = 0;
    for (const QTreeWidgetItem *item : m_leaves)
        n += item->data(0, kMarkedRole).toBool();
    return n;
}

QVector<int> FileList::markedIndices() const
{
    QVector<int> out;
    for (const QTreeWidgetItem *item : m_leaves)
        if (item->data(0, kMarkedRole).toBool())
            out << item->data(0, kIndexRole).toInt();
    return out;
}

QVector<int> FileList::selectedIndices() const
{
    QVector<int> out;
    for (const QTreeWidgetItem *item : m_leaves)
        if (item->isSelected() && !item->isHidden())
            out << item->data(0, kIndexRole).toInt();
    if (out.isEmpty() && currentIndex() >= 0)
        out << currentIndex();
    return out;
}

void FileList::selectIndices(const QVector<int> &indices)
{
    const QSet<int> want(indices.begin(), indices.end());
    QTreeWidgetItem *first = nullptr;
    {
        const QSignalBlocker blocker(this);
        clearSelection();
        for (QTreeWidgetItem *item : std::as_const(m_leaves)) {
            const bool on = want.contains(item->data(0, kIndexRole).toInt()) && !item->isHidden();
            item->setSelected(on);
            if (on && !first)
                first = item;
        }
    }
    if (first && !first->isSelected())
        first->setSelected(true);
}

void FileList::setLoadState(int index, LoadState state)
{
    if (QTreeWidgetItem *item = leafFor(index)) {
        item->setData(0, kLoadRole, int(state));
        if (state == LoadState::Failed)
            item->setToolTip(0, item->toolTip(0).section(tr("\nCould not load"), 0, 0) + tr("\nCould not load the diff"));
    }
    updateSpinner();
}

FileList::LoadState FileList::loadState(int index) const
{
    const QTreeWidgetItem *item = leafFor(index);
    return item ? LoadState(item->data(0, kLoadRole).toInt()) : LoadState::None;
}

void FileList::clearLoadStates()
{
    for (QTreeWidgetItem *item : std::as_const(m_leaves))
        item->setData(0, kLoadRole, int(LoadState::None));
    updateSpinner();
}

void FileList::updateSpinner()
{
    bool loading = false;
    for (const QTreeWidgetItem *item : std::as_const(m_leaves))
        loading = loading || LoadState(item->data(0, kLoadRole).toInt()) == LoadState::Loading;
    if (!m_spinTimer) {
        m_spinTimer = new QTimer(this);
        m_spinTimer->setInterval(50);
        connect(m_spinTimer, &QTimer::timeout, viewport(), qOverload<>(&QWidget::update));
    }
    if (loading && !m_spinTimer->isActive())
        m_spinTimer->start();
    else if (!loading)
        m_spinTimer->stop();
}

void FileList::setStats(int index, int added, int removed, bool blankOnly)
{
    if (index >= 0 && index < m_changes.size()) {
        m_changes[index].added = added;
        m_changes[index].removed = removed;
        m_changes[index].blankOnly = blankOnly;
    }
    QTreeWidgetItem *item = leafFor(index);
    if (!item || added < 0 || removed < 0)
        return;
    const FileChange &fc = m_changes[index];
    if (fc.status != u'=')
        item->setData(0, kStatsRole, QPoint(added, removed));
    QString tip = item->toolTip(0).section(u'\n', 0, 1); // the path, and the status (with any rename)
    tip += tr("\n%1 added, %2 removed lines").arg(added).arg(removed);
    if (blankOnly)
        tip += tr("\nOnly blank lines differ");
    item->setToolTip(0, tip);
}

void FileList::setPlaceholder(const QString &text)
{
    for (int i = topLevelItemCount() - 1; i >= 0; --i)
        if (topLevelItem(i)->data(0, kPlaceholderRole).toBool())
            delete takeTopLevelItem(i);
    if (text.isEmpty())
        return;
    auto *item = new QTreeWidgetItem(this);
    item->setText(0, text);
    item->setData(0, kPlaceholderRole, true);
    item->setFlags(Qt::NoItemFlags);
    QFont f = font();
    f.setItalic(true);
    item->setFont(0, f);
}

void FileList::setChanges(const QVector<FileChange> &changes, bool showGroups, const QVector<bool> &marked)
{
    const QSignalBlocker blocker(this);
    clear();
    m_leaves.clear();
    m_folders.clear();
    m_changes = changes;

    auto addFiles = [&](QTreeWidgetItem *parent, const QVector<int> &indices) {
        // Group by folder. QMap keeps folders sorted; the root ("") comes first.
        QMap<QString, QVector<int>> byDir;
        for (int idx : indices) {
            const QString &path = changes[idx].path;
            const qsizetype slash = path.lastIndexOf(u'/');
            byDir[slash < 0 ? QString() : path.left(slash)].push_back(idx);
        }
        for (auto it = byDir.begin(); it != byDir.end(); ++it) {
            auto *dirItem = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(this);
            dirItem->setData(0, kFolderRole, it.key());
            dirItem->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
            dirItem->setFlags(Qt::ItemIsEnabled);
            m_folders.push_back(dirItem);

            QVector<int> files = it.value();
            std::sort(files.begin(), files.end(), [&](int a, int b) {
                return QString::compare(changes[a].path, changes[b].path, Qt::CaseInsensitive) < 0;
            });
            for (int idx : files) {
                const FileChange &fc = changes[idx];
                auto *item = new QTreeWidgetItem(dirItem);
                item->setText(0, fc.path.section(u'/', -1));
                item->setIcon(0, statusIcon(fc.status));
                item->setData(0, kIndexRole, idx);
                item->setData(0, kPathRole, fc.path);
                item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                item->setData(0, kMarkedRole, marked.value(idx, false));
                styleLeaf(item);
                QString tip = QStringLiteral("%1\n%2").arg(fc.path, statusDescription(fc.status));
                if (fc.oldPath != fc.path)
                    tip += tr(" from %1").arg(fc.oldPath);
                if (fc.added >= 0 && fc.removed >= 0) {
                    if (fc.status != u'=')
                        item->setData(0, kStatsRole, QPoint(fc.added, fc.removed));
                    tip += tr("\n%1 added, %2 removed lines").arg(fc.added).arg(fc.removed);
                }
                if (fc.blankOnly)
                    tip += tr("\nOnly blank lines differ");
                item->setToolTip(0, tip);
                m_leaves.push_back(item);
            }
        }
    };

    if (showGroups) {
        const struct {
            ChangeGroup group;
            QString title;
        } groups[] = {{ChangeGroup::Staged, tr("Staged changes")},
                      {ChangeGroup::Unstaged, tr("Unstaged changes")},
                      {ChangeGroup::Unchanged, tr("Unchanged files")}};
        for (const auto &g : groups) {
            QVector<int> indices;
            for (int i = 0; i < changes.size(); ++i)
                if (changes[i].group == g.group)
                    indices.push_back(i);
            if (g.group == ChangeGroup::Unchanged && indices.isEmpty())
                continue;
            auto *groupItem = new QTreeWidgetItem(this);
            groupItem->setText(0, QStringLiteral("%1 (%2)").arg(g.title).arg(indices.size()));
            QFont f = groupItem->font(0);
            f.setBold(true);
            groupItem->setFont(0, f);
            groupItem->setFlags(Qt::ItemIsEnabled);
            addFiles(groupItem, indices);
        }
    } else {
        QVector<int> all(changes.size());
        std::iota(all.begin(), all.end(), 0);
        addFiles(nullptr, all);
    }
    updateFolderNames();
    expandAll();
    applyFilter();
}

void FileList::setFolderDisplay(const QString &root, bool trimCommonPrefix)
{
    m_root = root;
    m_trimPrefix = trimCommonPrefix;
    updateFolderNames();
}

void FileList::updateFolderNames()
{
    auto full = [&](const QString &rel) {
        if (m_root.isEmpty())
            return rel.isEmpty() ? QStringLiteral(".") : rel;
        return rel.isEmpty() ? m_root : m_root + u'/' + rel;
    };
    // The folders' shared leading components, cut at a folder boundary.
    QString common;
    if (m_trimPrefix && !m_folders.isEmpty()) {
        QStringList shared = full(m_folders.first()->data(0, kFolderRole).toString()).split(u'/');
        for (const QTreeWidgetItem *f : std::as_const(m_folders)) {
            const QStringList parts = full(f->data(0, kFolderRole).toString()).split(u'/');
            qsizetype n = 0;
            while (n < shared.size() && n < parts.size() && shared[n] == parts[n])
                ++n;
            shared = shared.mid(0, n);
        }
        common = shared.join(u'/');
    }
    for (QTreeWidgetItem *f : std::as_const(m_folders)) {
        const QString name = full(f->data(0, kFolderRole).toString());
        QString shown = name;
        if (!common.isEmpty() && name.startsWith(common)) {
            shown = name.mid(common.size());
            if (shown.startsWith(u'/'))
                shown = shown.mid(1);
            if (shown.isEmpty())
                shown = QStringLiteral(".");
        }
        f->setText(0, shown);
        f->setToolTip(0, name);
    }
}

void FileList::setFilter(const FileFilter &filter)
{
    m_filter = filter;
    applyFilter();
}

void FileList::applyFilter()
{
    for (QTreeWidgetItem *item : std::as_const(m_leaves)) {
        const int idx = item->data(0, kIndexRole).toInt();
        if (idx < 0 || idx >= m_changes.size())
            continue;
        item->setHidden(!m_filter.matches(m_changes[idx], item->data(0, kMarkedRole).toBool()));
    }
    // Folders (and sections) with nothing visible inside disappear too.
    std::function<bool(QTreeWidgetItem *)> update = [&](QTreeWidgetItem *item) {
        if (item->data(0, kIndexRole).isValid())
            return !item->isHidden();
        bool any = false;
        for (int i = 0; i < item->childCount(); ++i)
            any = update(item->child(i)) || any;
        item->setHidden(!any && m_filter.isActive());
        return any;
    };
    for (int i = 0; i < topLevelItemCount(); ++i)
        update(topLevelItem(i));
}

int FileList::visibleCount() const
{
    int n = 0;
    for (const QTreeWidgetItem *item : m_leaves)
        n += !item->isHidden();
    return n;
}

int FileList::currentIndex() const
{
    const QTreeWidgetItem *cur = currentItem();
    return cur && cur->data(0, kIndexRole).isValid() ? cur->data(0, kIndexRole).toInt() : -1;
}

bool FileList::selectIndex(int index)
{
    QTreeWidgetItem *item = leafFor(index);
    if (!item)
        return false;
    setCurrentItem(item);
    scrollToItem(item);
    return true;
}

bool FileList::selectFirst()
{
    for (QTreeWidgetItem *item : std::as_const(m_leaves)) {
        if (!item->isHidden()) {
            setCurrentItem(item);
            return true;
        }
    }
    return false;
}

bool FileList::selectNext()
{
    for (qsizetype i = m_leaves.indexOf(currentItem()) + 1; i < m_leaves.size(); ++i) {
        if (!m_leaves[i]->isHidden()) {
            setCurrentItem(m_leaves[i]);
            scrollToItem(m_leaves[i]);
            return true;
        }
    }
    return false;
}

bool FileList::selectPrev()
{
    qsizetype pos = m_leaves.indexOf(currentItem());
    if (pos < 0)
        pos = m_leaves.size();
    for (qsizetype i = pos - 1; i >= 0; --i) {
        if (!m_leaves[i]->isHidden()) {
            setCurrentItem(m_leaves[i]);
            scrollToItem(m_leaves[i]);
            return true;
        }
    }
    return false;
}

void FileList::setMatchCounts(const QHash<int, int> &counts)
{
    for (QTreeWidgetItem *item : std::as_const(m_leaves)) {
        const int idx = item->data(0, kIndexRole).toInt();
        const int n = counts.value(idx, 0);
        QFont f = item->font(0);
        f.setBold(n > 0);
        item->setFont(0, f);
        QString tip = item->toolTip(0).section(QStringLiteral("\n["), 0, 0);
        if (n > 0)
            tip += n == 1 ? tr("\n[1 search match]") : tr("\n[%1 search matches]").arg(n);
        item->setToolTip(0, tip);
    }
}

QVector<int> FileList::displayOrder(bool includeHidden) const
{
    QVector<int> out;
    for (const QTreeWidgetItem *item : m_leaves)
        if (includeHidden || !item->isHidden())
            out << item->data(0, kIndexRole).toInt();
    return out;
}
