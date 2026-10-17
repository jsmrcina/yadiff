#include "FileFilter.h"

namespace {
QRegularExpression globRegex(const QString &glob)
{
    QString re;
    for (qsizetype i = 0; i < glob.size(); ++i) {
        const QChar c = glob.at(i);
        if (c == u'*') {
            if (i + 1 < glob.size() && glob.at(i + 1) == u'*') {
                ++i;
                if (i + 1 < glob.size() && glob.at(i + 1) == u'/') {
                    ++i;
                    re += QStringLiteral("(?:.*/)?"); // "**/": any folders, or none
                } else {
                    re += QStringLiteral(".*");
                }
            } else {
                re += QStringLiteral("[^/]*");
            }
        } else if (c == u'?') {
            re += QStringLiteral("[^/]");
        } else if (c == u'[') {
            const qsizetype end = glob.indexOf(u']', i + 1);
            if (end < 0) {
                re += QStringLiteral("\\[");
            } else {
                re += glob.mid(i, end - i + 1);
                i = end;
            }
        } else {
            re += QRegularExpression::escape(QString(c));
        }
    }
    return QRegularExpression(QStringLiteral("^") + re + QStringLiteral("$"), QRegularExpression::CaseInsensitiveOption);
}
} // namespace

bool globMatches(const QString &rawPattern, const QString &path)
{
    QString pattern = rawPattern.trimmed();
    if (pattern.isEmpty() || pattern.startsWith(u'#'))
        return false;
    const bool dirOnly = pattern.endsWith(u'/');
    if (dirOnly)
        pattern.chop(1);
    if (pattern.startsWith(u'/'))
        pattern = pattern.mid(1);
    const QStringList parts = path.split(u'/');
    const QRegularExpression re = globRegex(pattern);
    if (!pattern.contains(u'/')) {
        // Any folder name (and, unless dirOnly, the file name) may match.
        const qsizetype names = dirOnly ? parts.size() - 1 : parts.size();
        for (qsizetype i = 0; i < names; ++i)
            if (re.match(parts[i]).hasMatch())
                return true;
        return false;
    }
    // Anchored: the path itself, or (for folders) any leading part of it.
    for (qsizetype n = dirOnly ? parts.size() - 1 : parts.size(); n >= 1; --n)
        if (re.match(parts.mid(0, n).join(u'/')).hasMatch())
            return true;
    return false;
}

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
    return !m_terms.isEmpty() || hideReviewed || !hiddenStatuses.isEmpty() || !hiddenCategories.isEmpty() || hideBlankOnly;
}

bool FileFilter::matches(const FileChange &fc, bool marked) const
{
    if (hiddenCategories.contains(categoryOf(fc, reversed)))
        return false;
    if (hideBlankOnly && fc.blankOnly)
        return false;
    return matches(fc.path, fc.status, marked);
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
