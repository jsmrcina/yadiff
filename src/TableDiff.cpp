#include "TableDiff.h"

#include "LineDiff.h"

#include <QFileInfo>
#include <QHash>
#include <QSet>

namespace TableDiff {

bool isTableFile(const QString &fileName)
{
    const QString s = QFileInfo(fileName).suffix().toLower();
    return s == QLatin1String("csv") || s == QLatin1String("tsv") || s == QLatin1String("tab") || s == QLatin1String("psv");
}

QChar delimiterFor(const QString &fileName, const QString &firstLine)
{
    const QString s = QFileInfo(fileName).suffix().toLower();
    if (s == QLatin1String("tsv") || s == QLatin1String("tab"))
        return u'\t';
    if (s == QLatin1String("psv"))
        return u'|';
    return firstLine.count(u';') > firstLine.count(u',') ? u';' : u',';
}

Table parse(const QString &text, QChar delimiter)
{
    Table rows;
    QStringList row;
    QString field;
    bool quoted = false, fieldStarted = false;
    const qsizetype n = text.size();
    auto endField = [&] {
        row << field;
        field.clear();
        fieldStarted = false;
    };
    auto endRow = [&] {
        endField();
        if (!(row.size() == 1 && row.first().isEmpty()))
            rows << row; // blank lines aren't rows
        row.clear();
    };
    for (qsizetype i = 0; i < n; ++i) {
        const QChar c = text.at(i);
        if (quoted) {
            if (c == u'"') {
                if (i + 1 < n && text.at(i + 1) == u'"') {
                    field += u'"';
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field += c;
            }
        } else if (c == u'"' && !fieldStarted) {
            quoted = fieldStarted = true;
        } else if (c == delimiter) {
            endField();
        } else if (c == u'\n') {
            endRow();
        } else if (c == u'\r' && i + 1 < n && text.at(i + 1) == u'\n') {
            // CRLF: the \n ends the row
        } else {
            field += c;
            fieldStarted = true;
        }
    }
    if (fieldStarted || !field.isEmpty() || !row.isEmpty())
        endRow();
    return rows;
}

QString Result::oldCell(const Table &oldData, int row, int column) const
{
    const int c = oldColumn.value(column, -1);
    return row < 0 || c < 0 || row >= oldData.size() ? QString() : oldData[row].value(c);
}

QString Result::newCell(const Table &newData, int row, int column) const
{
    const int c = newColumn.value(column, -1);
    return row < 0 || c < 0 || row >= newData.size() ? QString() : newData[row].value(c);
}

Result compare(const QStringList &oldHeader, const Table &oldData, const QStringList &newHeader, const Table &newData,
               int keyColumn)
{
    Result r;
    // Columns: by name with headers (new order first, then columns only the old file has),
    // by position without.
    if (!oldHeader.isEmpty() || !newHeader.isEmpty()) {
        QHash<QString, QVector<int>> oldByName;
        for (int i = 0; i < oldHeader.size(); ++i)
            oldByName[oldHeader[i]].push_back(i);
        QSet<int> usedOld;
        for (int i = 0; i < newHeader.size(); ++i) {
            r.columns << newHeader[i];
            r.newColumn << i;
            int match = -1;
            for (int o : oldByName.value(newHeader[i]))
                if (!usedOld.contains(o)) {
                    match = o;
                    break;
                }
            if (match >= 0)
                usedOld.insert(match);
            r.oldColumn << match;
        }
        for (int i = 0; i < oldHeader.size(); ++i)
            if (!usedOld.contains(i)) {
                r.columns << oldHeader[i];
                r.newColumn << -1;
                r.oldColumn << i;
            }
    } else {
        int width = 0;
        for (const QStringList &row : oldData)
            width = std::max(width, int(row.size()));
        for (const QStringList &row : newData)
            width = std::max(width, int(row.size()));
        for (int i = 0; i < width; ++i) {
            r.columns << QString::number(i + 1);
            r.oldColumn << i;
            r.newColumn << i;
        }
    }
    const int columns = int(r.columns.size());
    auto differences = [&](int o, int n) {
        QVector<int> out;
        for (int c = 0; c < columns; ++c)
            if (r.oldCell(oldData, o, c) != r.newCell(newData, n, c))
                out << c;
        return out;
    };
    auto addPair = [&](int o, int n) {
        Row row;
        row.oldRow = o;
        row.newRow = n;
        row.changedColumns = differences(o, n);
        row.kind = row.changedColumns.isEmpty() ? Row::Kind::Same : Row::Kind::Changed;
        r.changed += row.kind == Row::Kind::Changed;
        r.rows << row;
    };
    auto addOld = [&](int o) {
        Row row;
        row.kind = Row::Kind::Removed;
        row.oldRow = o;
        r.rows << row;
        ++r.removed;
    };
    auto addNew = [&](int n) {
        Row row;
        row.kind = Row::Kind::Added;
        row.newRow = n;
        r.rows << row;
        ++r.added;
    };

    if (keyColumn >= 0 && keyColumn < columns) {
        // Rows with the same key are the same row; the new file's order, with removed
        // rows where they were.
        QHash<QString, int> oldByKey;
        for (int o = int(oldData.size()) - 1; o >= 0; --o)
            oldByKey.insert(r.oldCell(oldData, o, keyColumn), o);
        QVector<bool> oldUsed(oldData.size(), false);
        QHash<QString, int> lastInNew; // key -> last new row that has it
        for (int n = 0; n < newData.size(); ++n)
            lastInNew.insert(r.newCell(newData, n, keyColumn), n);
        int nextOld = 0;
        for (int n = 0; n < newData.size(); ++n) {
            const auto it = oldByKey.constFind(r.newCell(newData, n, keyColumn));
            if (it == oldByKey.constEnd() || oldUsed[*it]) {
                addNew(n);
                continue;
            }
            // Old rows before this one that no later new row matches are gone: show them here.
            for (; nextOld < *it; ++nextOld)
                if (!oldUsed[nextOld] && lastInNew.value(r.oldCell(oldData, nextOld, keyColumn), -1) <= n) {
                    oldUsed[nextOld] = true;
                    addOld(nextOld);
                }
            oldUsed[*it] = true;
            addPair(*it, n);
        }
        for (int o = 0; o < oldData.size(); ++o)
            if (!oldUsed[o])
                addOld(o);
        return r;
    }

    // By order: equal rows anchor like lines; rows in between pair up when most of
    // their cells agree.
    QStringList a, b;
    for (int o = 0; o < oldData.size(); ++o) {
        QStringList cells;
        for (int c = 0; c < columns; ++c)
            cells << r.oldCell(oldData, o, c);
        a << cells.join(u'\x1f');
    }
    for (int n = 0; n < newData.size(); ++n) {
        QStringList cells;
        for (int c = 0; c < columns; ++c)
            cells << r.newCell(newData, n, c);
        b << cells.join(u'\x1f');
    }
    const auto [ia, ib] = LineDiff::lineIds(a, b);
    LineDiff::Alignment al = LineDiff::myers(ia, ib);
    al.push_back({int(oldData.size()), int(newData.size())});
    int po = 0, pn = 0;
    for (const LineDiff::Match &m : std::as_const(al)) {
        while (po < m.first || pn < m.second) {
            if (po < m.first && pn < m.second && differences(po, pn).size() * 2 <= columns) {
                addPair(po++, pn++);
            } else if (po < m.first && (pn >= m.second || m.first - po >= m.second - pn)) {
                addOld(po++);
            } else {
                addNew(pn++);
            }
        }
        if (m.first < oldData.size())
            addPair(m.first, m.second);
        po = m.first + 1;
        pn = m.second + 1;
    }
    return r;
}

} // namespace TableDiff
