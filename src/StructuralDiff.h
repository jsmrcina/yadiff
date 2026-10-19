#pragma once

#include "Comments.h"
#include "DiffModel.h"
#include "LineDiff.h"

#include <QStringList>
#include <QVector>

// yadiff's "Structural" algorithm: files compared as tokens (names, numbers, strings,
// operators, comment words) instead of lines. Lines with the same tokens match whatever
// their whitespace; in a changed region, lines whose tokens all reappear on the other
// side (code re-wrapped, re-indented or split over lines) are unimportant, and only the
// tokens that really changed are highlighted.
namespace StructuralDiff {

struct Token {
    int line = 0;          // index of the line it is on
    int start = 0, length = 0;
    bool comment = false;
    QString key;           // what is compared
};

// The tokens of a file. Block comments and strings may carry over to the next line.
QVector<Token> tokenize(const QStringList &lines, const Comments::Syntax &syntax, bool ignoreCase);

struct Options {
    Comments::Syntax syntax;
    bool ignoreCase = false;
    bool ignoreComments = false; // comment tokens don't count
};

// The diff of two files; `anchors` are manual alignment points (0-based line pairs).
FileDiff build(const QStringList &oldLines, const QStringList &newLines, const Options &options,
               const LineDiff::Alignment &anchors = {});

// Unique-line anchors (Heckel) with exact Myers between them: fast, and close to minimal.
LineDiff::Alignment hybridAlignment(const QVector<int> &a, const QVector<int> &b);

} // namespace StructuralDiff
