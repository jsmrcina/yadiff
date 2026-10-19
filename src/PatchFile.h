#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

// Reading unified diffs (`git diff`, `git format-patch`, `diff -u`) and applying them
// to the files they were made from.
namespace PatchFile {

struct Hunk {
    int oldStart = 0, oldCount = 0, newStart = 0, newCount = 0;
    QStringList lines; // as in the patch: ' ', '-' or '+' and the line (no line ending)
};

struct File {
    QString oldPath, newPath; // without a/ and b/; empty for /dev/null
    QChar status = u'M';      // M, A, D or R
    bool binary = false;
    QString text;             // this file's part of the patch, headers included
    QVector<Hunk> hunks;
    int added = 0, removed = 0;
    QString path() const { return newPath.isEmpty() ? oldPath : newPath; }
};

QVector<File> parse(const QString &patch);

// Applies the hunks to `base` (lines without line endings). A hunk goes where it says,
// or to the nearest place its old lines match (the file may have moved on a little).
// nullopt when a hunk's old lines aren't in the file.
std::optional<QStringList> apply(const QStringList &base, const File &file);

} // namespace PatchFile
