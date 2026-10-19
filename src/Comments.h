#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <utility>

// Finding comments in source files without a parser, for "Ignore comments": the
// comment syntax comes from the file name, and string literals are skipped so that
// "//" inside a string is not a comment.
namespace Comments {

struct Syntax {
    QStringList line;                          // e.g. "//", "#"
    QVector<std::pair<QString, QString>> block; // e.g. {"/*", "*/"}
    QString quotes;                            // string delimiters, e.g. "\"'"
    bool isValid() const { return !line.isEmpty() || !block.isEmpty(); }
};

// The comment syntax for a file, from its extension or name (invalid when unknown).
Syntax forFile(const QString &fileName);

// The lines with every comment removed (the code around a comment stays). Block
// comments may span lines.
QStringList strip(const QStringList &lines, const Syntax &syntax);

} // namespace Comments
