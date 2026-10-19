#pragma once

#include "DiffModel.h"

#include <QStringList>
#include <QVector>

#include <functional>
#include <utility>

// yadiff's own line diff, used for the in-process algorithms and whenever lines are
// compared after normalising them (ignore case, ignore leading/trailing whitespace).
// Lines are compared as ids: equal ids mean equal (normalised) lines.
namespace LineDiff {

// A pair of equal lines: (index in old, index in new). Alignments are strictly
// increasing in both indices.
using Match = std::pair<int, int>;
using Alignment = QVector<Match>;

// Ids for normalised lines: equal strings get the same id, across both sides.
std::pair<QVector<int>, QVector<int>> lineIds(const QStringList &oldLines, const QStringList &newLines);

// Myers' O(ND) algorithm with linear space (the middle-snake divide and conquer):
// a longest common subsequence, so the smallest possible diff.
Alignment myers(const QVector<int> &a, const QVector<int> &b);

// Heckel (1978): lines unique to both sides anchor the alignment, and matches spread
// to their neighbours. Fast, and keeps blocks together, but only lines connected to
// an anchor match. Crossing anchors (moved blocks) are reduced to the longest
// in-order set, so the moved lines show as removed and added.
Alignment heckel(const QVector<int> &a, const QVector<int> &b);

// The alignment implied by `git diff -U0` output for two files of n and m lines.
Alignment fromUnifiedZero(const QString &diff, int n, int m);

// Splits file content into lines (no trailing empty line for a final newline).
QStringList splitLines(const QString &content);

// Builds the full-file diff from the original lines and an alignment. Equal lines
// whose original text differs keep the old side's text in `oldText` (and are
// unimportant). `anchors` are manually aligned lines: the ones not in `alignment`
// are shown side by side as an edit.
FileDiff build(const QStringList &oldLines, const QStringList &newLines, const Alignment &alignment,
               bool detectMoves, const Alignment &anchors = {});

// Alignment with manual anchors: `align` aligns each stretch between two anchors
// (given the normalised lines of that stretch), and anchors whose lines are equal
// join the result. Anchors that cross or are out of range are dropped; the ones
// kept are returned in `kept`.
Alignment alignAround(const QStringList &oldNorm, const QStringList &newNorm, const Alignment &anchors,
                      const std::function<Alignment(const QStringList &, const QStringList &)> &align,
                      Alignment *kept);

} // namespace LineDiff
