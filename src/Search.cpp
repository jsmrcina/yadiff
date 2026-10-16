#include "Search.h"

SearchQuery::SearchQuery(const QString &text, bool regex, bool caseSensitive)
    : m_text(text), m_regex(regex), m_caseSensitive(caseSensitive)
{
    if (m_regex) {
        m_re.setPattern(text);
        m_re.setPatternOptions(caseSensitive ? QRegularExpression::NoPatternOption
                                             : QRegularExpression::CaseInsensitiveOption);
    }
}

QString SearchQuery::errorString() const
{
    return m_regex && !m_re.isValid() ? m_re.errorString() : QString();
}

QVector<Span> SearchQuery::find(const QString &line) const
{
    QVector<Span> out;
    if (!isValid())
        return out;
    if (m_regex) {
        auto it = m_re.globalMatch(line);
        while (it.hasNext()) {
            const auto m = it.next();
            if (m.capturedLength() > 0)
                out.push_back({int(m.capturedStart()), int(m.capturedLength())});
        }
        return out;
    }
    const Qt::CaseSensitivity cs = m_caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    qsizetype from = 0;
    while ((from = line.indexOf(m_text, from, cs)) >= 0) {
        out.push_back({int(from), int(m_text.size())});
        from += m_text.size();
    }
    return out;
}

QVector<LineMatch> findInDiff(const FileDiff &d, const SearchQuery &q)
{
    QVector<LineMatch> out;
    for (int i = 0; i < d.lines.size(); ++i)
        for (const Span &s : q.find(d.lines[i].text))
            out.push_back({i, s});
    return out;
}
