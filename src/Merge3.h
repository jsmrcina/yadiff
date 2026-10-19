#pragma once

#include <QStringList>
#include <QVector>

// Three-way merge (diff3): base, ours and theirs split into regions that are unchanged,
// changed on one side, changed the same way on both, or in conflict.
namespace Merge3 {

struct Region {
    enum class Kind { Unchanged, Ours, Theirs, Both, Conflict };
    Kind kind = Kind::Unchanged;
    int baseStart = 0, baseCount = 0; // 0-based line ranges
    int oursStart = 0, oursCount = 0;
    int theirsStart = 0, theirsCount = 0;
};

QVector<Region> regions(const QStringList &base, const QStringList &ours, const QStringList &theirs);

// How a region is taken into the result.
enum class Pick { Auto, Ours, Theirs, OursThenTheirs, TheirsThenOurs, Base, Markers };

// The lines a region contributes. Auto: the changed side (or either, for Both); for a
// conflict, git-style conflict markers with the base in between (diff3 style).
QStringList take(const Region &r, Pick pick, const QStringList &base, const QStringList &ours, const QStringList &theirs,
                 const QString &oursLabel = QStringLiteral("ours"), const QString &theirsLabel = QStringLiteral("theirs"));

// The whole merge with every region on Auto.
QStringList merge(const QVector<Region> &regions, const QStringList &base, const QStringList &ours,
                  const QStringList &theirs, int *conflicts = nullptr);

} // namespace Merge3
