#include "FileFilter.h"

FileFilter::FileFilter(const QString &text) : m_text(text)
{
    for (QString t : text.split(u' ', Qt::SkipEmptyParts)) {
        Term term;
        if (t.startsWith(u'!')) {
            term.exclude = true;
            t = t.mid(1);
        }
        if (t.isEmpty())
            continue;
        term.text = t;
        term.glob = t.contains(u'*') || t.contains(u'?') || t.contains(u'[');
        if (term.glob)
            term.re = QRegularExpression::fromWildcard(t, Qt::CaseInsensitive, QRegularExpression::NonPathWildcardConversion);
        m_terms.push_back(term);
    }
}

bool FileFilter::isActive() const
{
    return !m_terms.isEmpty() || hideReviewed || !hiddenStatuses.isEmpty();
}

bool FileFilter::termMatches(const Term &t, const QString &path) const
{
    if (!t.glob)
        return path.contains(t.text, Qt::CaseInsensitive);
    const QString name = path.section(u'/', -1);
    return t.re.match(path).hasMatch() || t.re.match(name).hasMatch();
}

bool FileFilter::matches(const QString &path, QChar status, bool reviewed) const
{
    if (hideReviewed && reviewed)
        return false;
    if (hiddenStatuses.contains(status))
        return false;
    bool anyInclude = false, included = false;
    for (const Term &t : m_terms) {
        if (t.exclude) {
            if (termMatches(t, path))
                return false;
        } else {
            anyInclude = true;
            included = included || termMatches(t, path);
        }
    }
    return !anyInclude || included;
}
