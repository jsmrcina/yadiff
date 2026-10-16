#include "FileList.h"

#include <QHeaderView>
#include <QMap>
#include <QPainter>
#include <QStyle>

#include <algorithm>
#include <numeric>

namespace {

constexpr int kIndexRole = Qt::UserRole + 1;

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
    setRootIsDecorated(true);
    setUniformRowHeights(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setIconSize(QSize(16, 16));
    header()->setStretchLastSection(true);
    connect(this, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *cur) {
        if (cur && cur->data(0, kIndexRole).isValid())
            emit fileActivated(cur->data(0, kIndexRole).toInt());
    });
}

void FileList::setChanges(const QVector<FileChange> &changes, bool showGroups)
{
    const QSignalBlocker blocker(this);
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
            dirItem->setFlags(Qt::ItemIsEnabled);

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
                QString tip = QStringLiteral("%1\n%2").arg(fc.path, statusDescription(fc.status));
                if (fc.oldPath != fc.path)
                    tip += tr(" from %1").arg(fc.oldPath);
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
            groupItem->setFlags(Qt::ItemIsEnabled);
            addFiles(groupItem, indices);
        }
    } else {
        QVector<int> all(changes.size());
        std::iota(all.begin(), all.end(), 0);
        addFiles(nullptr, all);
    }
    expandAll();
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
    if (m_leaves.isEmpty())
        return false;
    setCurrentItem(m_leaves.first());
    return true;
}

bool FileList::selectNext()
{
    const qsizetype pos = m_leaves.indexOf(currentItem());
    if (pos + 1 >= m_leaves.size())
        return false;
    setCurrentItem(m_leaves[pos + 1]);
    scrollToItem(m_leaves[pos + 1]);
    return true;
}

bool FileList::selectPrev()
{
    const qsizetype pos = m_leaves.indexOf(currentItem());
    if (pos <= 0)
        return false;
    setCurrentItem(m_leaves[pos - 1]);
    scrollToItem(m_leaves[pos - 1]);
    return true;
}
