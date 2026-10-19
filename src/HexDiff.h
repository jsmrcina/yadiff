#pragma once

#include "DiffModel.h"

#include <QByteArray>

// Binary files as a hex dump diff: 16 bytes a row, offset, hex and ASCII. Changed bytes
// are highlighted; inserted or removed bytes show as added or removed rows, and the
// rows after them line up again (content-defined chunks find the shift).
namespace HexDiff {

constexpr int kBytesPerRow = 16;

// One row of a dump: "00000010  48 65 6c ...  |Hel...|". `offset` is the first byte's.
QString row(const QByteArray &data, qsizetype offset, qsizetype count);

// The diff of two binary files as rows (no syntax, no moves).
FileDiff build(const QByteArray &oldData, const QByteArray &newData);

} // namespace HexDiff
