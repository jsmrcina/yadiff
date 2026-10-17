#pragma once

#include "DiffModel.h"

#include <QString>
#include <QVector>

// Renders parts of a FileDiff in `git diff` unified format, as plain text and
// as colour-coded HTML (for pasting into mail, docs or chat).
namespace DiffFormat {

struct Paths {
    QString oldPath;    // empty when the file did not exist on the old side
    QString newPath;    // empty when the file does not exist on the new side
};

struct Output {
    QString text;
    QString html;
};

// Unified diff of the given chunks (indices into FileDiff::chunks), with
// `context` unchanged lines around each; nearby chunks merge into one hunk as git does.
Output unified(const FileDiff &d, const Paths &paths, const QVector<int> &chunks, int context = 3);

// Font for the HTML (CSS font-family and point size); the default is a monospace stack.
void setHtmlFont(const QString &family, double pointSize);

// Every chunk of the file.
Output unifiedFile(const FileDiff &d, const Paths &paths, int context = 3);

// The text of one side of a chunk (removed lines for the old side, added for the new side).
QString sideText(const FileDiff &d, int chunk, LineKind side);

} // namespace DiffFormat
