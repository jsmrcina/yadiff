#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// Comparing CSV and TSV files as tables: rows matched by a key column (or by position,
// like lines), columns matched by their header, and the cells that changed.
namespace TableDiff {

using Table = QVector<QStringList>;

bool isTableFile(const QString &fileName); // .csv, .tsv, .tab, .psv
// The delimiter: tab for .tsv/.tab, | for .psv; for .csv a comma, or a semicolon when
// the first line has more of those.
QChar delimiterFor(const QString &fileName, const QString &firstLine);
// RFC 4180: quoted fields may hold delimiters, doubled quotes and line breaks.
Table parse(const QString &text, QChar delimiter);

struct Row {
    enum class Kind { Same, Changed, Added, Removed };
    Kind kind = Kind::Same;
    int oldRow = -1, newRow = -1;   // index in each table (data rows, after any header)
    QVector<int> changedColumns;    // result columns whose cells differ
};

struct Result {
    QStringList columns;            // header names, or "1", "2", ... without a header
    QVector<int> oldColumn, newColumn; // per result column: its index in each table, -1 when missing
    QVector<Row> rows;
    int changed = 0, added = 0, removed = 0;

    // A cell of a table, by result column ("" when the row or column is missing).
    QString oldCell(const Table &oldData, int row, int column) const;
    QString newCell(const Table &newData, int row, int column) const;
};

// `header`: the first row names the columns (they are matched by name). `keyColumn`:
// a result column whose values identify rows, or -1 to match rows by their order.
// Tables passed here are the data rows only; headers go in separately.
Result compare(const QStringList &oldHeader, const Table &oldData, const QStringList &newHeader, const Table &newData,
               int keyColumn);

} // namespace TableDiff
