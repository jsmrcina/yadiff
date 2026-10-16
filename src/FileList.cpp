#include "FileList.h"

#include <QHeaderView>
#include <QStyledItemDelegate>
#include <QMap>
#include <QPainter>
#include <QStyle>

#include <algorithm>
#include <functional>
#include <numeric>

namespace {

constexpr int kIndexRole = Qt::UserRole + 1;
constexpr int kReviewedRole = Qt::UserRole + 2;
constexpr int kStatsRole = Qt::UserRole + 3;
constexpr int kPathRole = Qt::UserRole + 4; // QPoint(added, removed); absent when unknown

// Draws the file name as usual, plus "+12 −3" in green and red at the right edge
// when there is room for both (the name always wins when the pane is narrow).
class StatsDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
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
        const bool room = option.rect.width() >= statsW + nameW + 60; // 60: checkbox and icon

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
    setSelectionMode(QAbstractItemView::SingleSelection);
    setIconSize(QSize(16, 16));
    setToolTip(tr("Tick a file once you have reviewed it"));
    connect(this, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *cur) {
        if (cur && cur->data(0, kIndexRole).isValid())
            emit fileActivated(cur->data(0, kIndexRole).toInt());
    });
    connect(this, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item) {
        if (m_populating || !item->data(0, kIndexRole).isValid())
            return;
        const bool checked = item->checkState(0) == Qt::Checked;
        if (checked == item->data(0, kReviewedRole).toBool())
            return; // some other data changed
        m_populating = true;
        item->setData(0, kReviewedRole, checked);
        styleLeaf(item);
        m_populating = false;
        emit reviewToggled(item->data(0, kIndexRole).toInt(), checked);
        if (m_filter.hideReviewed)
            applyFilter();
    });
}

void FileList::styleLeaf(QTreeWidgetItem *item)
{
    const bool reviewed = item->data(0, kReviewedRole).toBool();
    item->setForeground(0, reviewed ? palette().brush(QPalette::Disabled, QPalette::Text) : palette().brush(QPalette::Text));
}

QTreeWidgetItem *FileList::leafFor(int index) const
{
    for (QTreeWidgetItem *item : m_leaves)
        if (item->data(0, kIndexRole).toInt() == index)
            return item;
    return nullptr;
}

bool FileList::isReviewed(int index) const
{
    const QTreeWidgetItem *item = leafFor(index);
    return item && item->data(0, kReviewedRole).toBool();
}

void FileList::setReviewed(int index, bool reviewed)
{
    if (QTreeWidgetItem *item = leafFor(index))
        item->setCheckState(0, reviewed ? Qt::Checked : Qt::Unchecked); // itemChanged does the rest
}

int FileList::reviewedCount() const
{
    int n = 0;
    for (const QTreeWidgetItem *item : m_leaves)
        n += item->data(0, kReviewedRole).toBool();
    return n;
}

void FileList::setChanges(const QVector<FileChange> &changes, bool showGroups, const QVector<bool> &reviewed)
{
    const QSignalBlocker blocker(this);
    m_populating = true;
    clear();
    m_leaves.clear();

    auto addFiles = [&](QTreeWidgetItem *parent, const QVector<int> &indices) {
        // Group by directory. QMap keeps directories sorted; the root ("") comes first.
        QMap<QString, QVector<int>> byDir;
        for (int idx : indices) {
            const QString &path = changes[idx].path;
            const qsizetype slash = path.lastIndexOf(u'/');
            byDir[slash < 0 ? QString() : path.left(slash)].push_back(idx);
        }
        for (auto it = byDir.begin(); it != byDir.end(); ++it) {
            auto *dirItem = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(this);
            dirItem->setText(0, it.key().isEmpty() ? QStringLiteral(".") : it.key());
            dirItem->setToolTip(0, it.key().isEmpty() ? tr("Repository root") : it.key());
            dirItem->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
            // Ticking a directory ticks all its files; its state reflects theirs.
            dirItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);

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
                const bool done = reviewed.value(idx, false);
                item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
                item->setData(0, kReviewedRole, done);
                item->setCheckState(0, done ? Qt::Checked : Qt::Unchecked);
                styleLeaf(item);
                QString tip = QStringLiteral("%1\n%2").arg(fc.path, statusDescription(fc.status));
                if (fc.oldPath != fc.path)
                    tip += tr(" from %1").arg(fc.oldPath);
                if (fc.added >= 0 && fc.removed >= 0) {
                    item->setData(0, kStatsRole, QPoint(fc.added, fc.removed));
                    tip += tr("\n%1 added, %2 removed lines").arg(fc.added).arg(fc.removed);
                }
                item->setToolTip(0, tip);
                m_leaves.push_back(item);
            }
        }
    };

    if (showGroups) {
        const struct {
            ChangeGroup group;
            QString title;
        } groups[] = {{ChangeGroup::Staged, tr("Staged changes")}, {ChangeGroup::Unstaged, tr("Unstaged changes")}};
        for (const auto &g : groups) {
            QVector<int> indices;
            for (int i = 0; i < changes.size(); ++i)
                if (changes[i].group == g.group)
                    indices.push_back(i);
            auto *groupItem = new QTreeWidgetItem(this);
            groupItem->setText(0, QStringLiteral("%1 (%2)").arg(g.title).arg(indices.size()));
            QFont f = groupItem->font(0);
            f.setBold(true);
            groupItem->setFont(0, f);
            groupItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
            addFiles(groupItem, indices);
        }
    } else {
        QVector<int> all(changes.size());
        std::iota(all.begin(), all.end(), 0);
        addFiles(nullptr, all);
    }
    m_status.clear();
    for (const FileChange &fc : changes)
        m_status << fc.status;
    expandAll();
    m_populating = false;
    applyFilter();
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
        const QString path = item->data(0, kPathRole).toString();
        const bool show = m_filter.matches(path, m_status.value(idx), item->data(0, kReviewedRole).toBool());
        item->setHidden(!show);
    }
    // Directories (and sections) with nothing visible inside disappear too.
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
    for (QTreeWidgetItem *item : std::as_const(m_leaves)) {
        if (item->data(0, kIndexRole).toInt() == index) {
            setCurrentItem(item);
            scrollToItem(item);
            return true;
        }
    }
    return false;
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
    m_populating = true;
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
    m_populating = false;
}

QVector<int> FileList::displayOrder(bool includeHidden) const
{
    QVector<int> out;
    for (const QTreeWidgetItem *item : m_leaves)
        if (includeHidden || !item->isHidden())
            out << item->data(0, kIndexRole).toInt();
    return out;
}
