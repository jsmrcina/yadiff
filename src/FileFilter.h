#pragma once

#include <QChar>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QVector>

// Which files the file list shows. Terms are separated by spaces: plain text matches
// anywhere in the path, terms with * ? [ are globs (matched against the path and the
// file name), and a leading ! excludes. A file is shown when it matches at least one
// including term (or there are none) and no excluding term.
class FileFilter
{
public:
    FileFilter() = default;
    explicit FileFilter(const QString &text);

    bool hideReviewed = false;
    QSet<QChar> hiddenStatuses; // git status letters to hide

    bool isActive() const;
    bool matches(const QString &path, QChar status, bool reviewed) const;
    QString text() const { return m_text; }

private:
    struct Term {
        bool exclude = false;
        bool glob = false;
        QString text;
        QRegularExpression re;
    };
    bool termMatches(const Term &t, const QString &path) const;

    QString m_text;
    QVector<Term> m_terms;
};
