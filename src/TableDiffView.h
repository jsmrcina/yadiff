#pragma once

#include "TableDiff.h"

#include <QColor>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QTableWidget;

// CSV and TSV files as a table: added rows green, removed rows red, and changed cells
// as "old → new". Rows match by a key column or by their order.
class TableDiffView : public QWidget
{
    Q_OBJECT
public:
    explicit TableDiffView(QWidget *parent = nullptr);

    // Either side may be empty (added or deleted file).
    void setTables(const QString &oldText, const QString &newText, const QString &fileName);
    void setColors(const QColor &added, const QColor &removed, const QColor &changed);

    // Changed, added and removed rows, in order.
    int changeCount() const { return int(m_changedRows.size()); }
    bool nextChange();
    bool prevChange();
    void goToChange(int index);
    int currentChange() const { return m_current; }

    const TableDiff::Result &result() const { return m_result; }
    void setKeyColumn(int column); // -1: rows by order

signals:
    void textViewRequested();
    void currentChangeChanged(int index, int count);

private:
    void recompute();
    void fill();

    QString m_oldText, m_newText, m_fileName;
    TableDiff::Table m_old, m_new;                 // as parsed, header included
    TableDiff::Table m_oldDataCache, m_newDataCache; // data rows only
    TableDiff::Result m_result;
    QVector<int> m_changedRows; // table rows (as shown) that are changes
    int m_current = -1;
    QColor m_added, m_removed, m_changed;

    QComboBox *m_key = nullptr;
    QCheckBox *m_header = nullptr;
    QCheckBox *m_onlyChanged = nullptr;
    QLabel *m_summary = nullptr;
    QTableWidget *m_table = nullptr;
};
