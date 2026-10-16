#pragma once

#include <QString>
#include <QVector>

// A line-oriented model of a single file's diff, independent of any UI.

enum class LineKind { Equal, Removed, Added };

// A highlighted range of characters inside a line (intra-line change).
struct Span {
    int start = 0;
    int length = 0;
};

struct DiffLine {
    LineKind kind = LineKind::Equal;
    int oldNo = 0;          // 1-based line number on the old side, 0 if none
    int newNo = 0;          // 1-based line number on the new side, 0 if none
    QString text;
    int moveId = -1;        // index into FileDiff::moves when part of a moved block
    int partner = -1;       // index of the matching line on the other side of a move
    int pair = -1;          // index of the similar line on the other side of an edit
    QVector<Span> highlights;
};

// A run of consecutive non-equal lines: indices into FileDiff::lines, inclusive.
struct Chunk {
    int first = 0;
    int last = 0;
};

// A block of removed lines that reappears, unchanged, as added lines elsewhere.
struct MoveBlock {
    int removedFirst = 0;   // index into FileDiff::lines
    int addedFirst = 0;     // index into FileDiff::lines
    int count = 0;
};

struct FileDiff {
    QVector<DiffLine> lines;
    QVector<Chunk> chunks;
    QVector<MoveBlock> moves;
    bool binary = false;
    QString message;        // shown instead of content when set (binary, empty, ...)
};

namespace DiffModel {

// Parse `git diff` unified output for exactly one file. Use a very large -U so
// the whole file is present; multiple hunks are still handled.
FileDiff parseUnified(const QString &diffText);

// Treat `content` as an entirely new file (e.g. an untracked file).
FileDiff allAdded(const QString &content);

// Splits lines into chunks, detects moved blocks and intra-line differences.
void finalize(FileDiff &d);

// Exposed for tests.
void computeChunks(FileDiff &d);
void detectMoves(FileDiff &d, int minChars = 20);
void computeIntraLine(FileDiff &d);

} // namespace DiffModel
