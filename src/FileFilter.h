#pragma once

#include "GitRepo.h"

#include <QChar>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QVector>

// Which files the file list shows. Terms are separated by spaces: plain text matches
// anywhere in the path, terms with * ? [ are globs (matched against the path and the
// file name), and a leading ! excludes. A file is shown when it matches at least one
// including term (or there are none) and no excluding term.
// .gitignore-style glob: "*" stays within a folder, "**" crosses folders, "?" is one
// character. Without a "/", the pattern matches a file or folder name anywhere; with one,
// it matches the whole path from the top. A trailing "/" matches folders only.
bool globMatches(const QString &pattern, const QString &path);

class FileFilter
{
public:
    FileFilter() = default;
    explicit FileFilter(const QString &text);

    bool hideReviewed = false;  // "Hide marked files"
    QSet<QChar> hiddenStatuses; // git status letters to hide
    QSet<FileCategory> hiddenCategories; // Options > Show left-only / right-only / different / identical files
    bool hideBlankOnly = false;          // Options > Show blanks-only files (off)
    bool reversed = false;               // sides swapped (an untracked file is then on the left)

    bool isActive() const;
    bool matches(const QString &path, QChar status, bool reviewed) const;
    bool matches(const FileChange &fc, bool marked) const;
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
