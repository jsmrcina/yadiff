#pragma once

#include "DiffModel.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>

// Reading and writing the on-disk side of a comparison, preserving its line
// endings and final newline.
namespace TextFile {

struct Content {
    QStringList lines;
    QByteArray eol = "\n";
    bool trailingNewline = true;
};

std::optional<Content> read(const QString &path, QString *error);

// Writes in place (through symlinks, unlike an atomic rename), so git difftool's
// symlinked working-tree files keep pointing at the real files.
bool write(const QString &path, const Content &content, QString *error);

// Replaces one change's lines in `target` (the file on the side being written)
// with the other side's lines. Returns nullopt with *error set when the file no
// longer matches the diff (it changed on disk since the diff was taken).
std::optional<QStringList> applyChunk(const QStringList &target, const FileDiff &diff, int chunk, bool toNewSide,
                                      QString *error);

} // namespace TextFile
