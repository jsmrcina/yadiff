#include "TableDiffView.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

TableDiffView::TableDiffView(QWidget *parent) : QWidget(parent)
{
    m_key = new QComboBox;
    m_key->setToolTip(tr("Rows with the same value in this column are the same row; without one, rows match by order"));
    m_header = new QCheckBox(tr("First row is a header"));
    m_header->setChecked(true);
    m_onlyChanged = new QCheckBox(tr("Only changed rows"));
    m_summary = new QLabel;
    auto *text = new QPushButton(tr("Text view"));
    text->setToolTip(tr("Show this file as text (View > Show CSV and TSV files as tables)"));
    m_table = new QTableWidget;
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setWordWrap(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->verticalHeader()->setDefaultSectionSize(m_table->fontMetrics().height() + 6);

    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(6, 4, 6, 4);
    bar->addWidget(new QLabel(tr("Key column:")));
    bar->addWidget(m_key);
    bar->addWidget(m_header);
    bar->addWidget(m_onlyChanged);
    bar->addWidget(m_summary, 1);
    bar->addWidget(text);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(m_table, 1);

    connect(m_key, &QComboBox::activated, this, [this] { recompute(); });
    connect(m_header, &QCheckBox::toggled, this, [this] {
        m_key->setCurrentIndex(0); // column names change
        recompute();
    });
    connect(m_onlyChanged, &QCheckBox::toggled, this, &TableDiffView::fill);
    connect(text, &QPushButton::clicked, this, &TableDiffView::textViewRequested);
}

void TableDiffView::setColors(const QColor &added, const QColor &removed, const QColor &changed)
{
    m_added = added;
    m_removed = removed;
    m_changed = changed;
}

void TableDiffView::setTables(const QString &oldText, const QString &newText, const QString &fileName)
{
    const bool sameFile = fileName == m_fileName;
    m_oldText = oldText;
    m_newText = newText;
    m_fileName = fileName;
    const QString first = (newText.isEmpty() ? oldText : newText).section(u'\n', 0, 0);
    const QChar delimiter = TableDiff::delimiterFor(fileName, first);
    m_old = TableDiff::parse(oldText, delimiter);
    m_new = TableDiff::parse(newText, delimiter);
    if (!sameFile)
        m_key->setCurrentIndex(0); // keep the key while the same file reloads
    recompute();
}

void TableDiffView::setKeyColumn(int column)
{
    m_key->setCurrentIndex(column + 1);
    recompute();
}

void TableDiffView::recompute()
{
    const bool header = m_header->isChecked();
    const QStringList oldHeader = header && !m_old.isEmpty() ? m_old.first() : QStringList();
    const QStringList newHeader = header && !m_new.isEmpty() ? m_new.first() : QStringList();
    const TableDiff::Table oldData = header && !m_old.isEmpty() ? m_old.mid(1) : m_old;
    const TableDiff::Table newData = header && !m_new.isEmpty() ? m_new.mid(1) : m_new;
    const int key = m_key->currentIndex() - 1;
    m_result = TableDiff::compare(oldHeader, oldData, newHeader, newData, key);
    // The key choices follow the columns.
    const int keep = m_key->currentIndex();
    m_key->clear();
    m_key->addItem(tr("None (match rows by order)"));
    for (const QString &c : std::as_const(m_result.columns))
        m_key->addItem(header ? c : tr("Column %1").arg(c));
    m_key->setCurrentIndex(std::clamp(keep, 0, m_key->count() - 1));
    m_summary->setText(tr("%1 changed, %2 added, %3 removed rows").arg(m_result.changed).arg(m_result.added).arg(m_result.removed));
    m_oldDataCache = oldData;
    m_newDataCache = newData;
    fill();
}

void TableDiffView::fill()
{
    const auto &r = m_result;
    m_table->clear();
    m_table->setColumnCount(int(r.columns.size()));
    m_table->setHorizontalHeaderLabels(m_header->isChecked() ? r.columns : QStringList());
    m_changedRows.clear();
    m_current = -1;
    QVector<int> shown;
    for (int i = 0; i < r.rows.size(); ++i)
        if (!m_onlyChanged->isChecked() || r.rows[i].kind != TableDiff::Row::Kind::Same)
            shown << i;
    m_table->setRowCount(int(shown.size()));
    QStringList rowLabels;
    for (int t = 0; t < shown.size(); ++t) {
        const TableDiff::Row &row = r.rows[shown[t]];
        using K = TableDiff::Row::Kind;
        const bool removed = row.kind == K::Removed;
        // Numbers as in the files (1-based, counting the header line).
        const int offset = m_header->isChecked() ? 2 : 1;
        rowLabels << (removed ? QStringLiteral("−%1").arg(row.oldRow + offset)
                              : row.kind == K::Added ? QStringLiteral("+%1").arg(row.newRow + offset)
                                                     : QString::number(row.newRow + offset));
        if (row.kind != K::Same)
            m_changedRows << t;
        for (int c = 0; c < r.columns.size(); ++c) {
            const QString oldValue = r.oldCell(m_oldDataCache, row.oldRow, c);
            const QString newValue = r.newCell(m_newDataCache, row.newRow, c);
            auto *item = new QTableWidgetItem;
            if (row.kind == K::Changed && row.changedColumns.contains(c)) {
                item->setText(QStringLiteral("%1 → %2").arg(oldValue, newValue));
                item->setToolTip(tr("Was: %1\nNow: %2").arg(oldValue, newValue));
                item->setBackground(m_changed);
            } else {
                item->setText(removed ? oldValue : newValue);
                if (row.kind == K::Added)
                    item->setBackground(m_added);
                else if (removed)
                    item->setBackground(m_removed);
            }
            m_table->setItem(t, c, item);
        }
    }
    m_table->setVerticalHeaderLabels(rowLabels);
    m_table->resizeColumnsToContents();
    for (int c = 0; c < m_table->columnCount(); ++c)
        m_table->setColumnWidth(c, std::min(m_table->columnWidth(c), 400));
    if (m_changedRows.isEmpty())
        emit currentChangeChanged(-1, 0);
    else
        goToChange(0);
}

void TableDiffView::goToChange(int index)
{
    if (m_changedRows.isEmpty())
        return;
    m_current = std::clamp(index, 0, int(m_changedRows.size()) - 1);
    m_table->selectRow(m_changedRows[m_current]);
    m_table->scrollToItem(m_table->item(m_changedRows[m_current], 0), QAbstractItemView::PositionAtCenter);
    emit currentChangeChanged(m_current, changeCount());
}

bool TableDiffView::nextChange()
{
    if (m_current + 1 >= changeCount())
        return false;
    goToChange(m_current + 1);
    return true;
}

bool TableDiffView::prevChange()
{
    if (m_current <= 0)
        return false;
    goToChange(m_current - 1);
    return true;
}
