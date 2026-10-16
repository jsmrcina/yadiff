#pragma once

#include "DiffModel.h"

#include <QRegularExpression>
#include <QString>
#include <QVector>

// A find-in-diff query: plain substring or regular expression.
class SearchQuery
{
public:
    SearchQuery() = default;
    SearchQuery(const QString &text, bool regex, bool caseSensitive);

    bool isEmpty() const { return m_text.isEmpty(); }
    bool isValid() const { return !m_text.isEmpty() && (!m_regex || m_re.isValid()); }
    QString errorString() const;
    QString text() const { return m_text; }
    bool isRegex() const { return m_regex; }
    bool isCaseSensitive() const { return m_caseSensitive; }

    // Non-overlapping matches in `line`. Empty regex matches are skipped.
    QVector<Span> find(const QString &line) const;

    bool operator==(const SearchQuery &o) const
    {
        return m_text == o.m_text && m_regex == o.m_regex && m_caseSensitive == o.m_caseSensitive;
    }

private:
    QString m_text;
    bool m_regex = false;
    bool m_caseSensitive = false;
    QRegularExpression m_re;
};

// Matches in a whole diff: one entry per match, in line order.
struct LineMatch {
    int lineIndex = 0; // into FileDiff::lines
    Span span;
};
QVector<LineMatch> findInDiff(const FileDiff &d, const SearchQuery &q);
