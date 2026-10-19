#include "Merge3.h"

#include "LineDiff.h"
#include "StructuralDiff.h"

namespace Merge3 {

namespace {

// For each base line, the matching line on the other side (-1 when it changed).
QVector<int> matches(const QStringList &base, const QStringList &other)
{
    const auto [a, b] = LineDiff::lineIds(base, other);
    QVector<int> out(base.size(), -1);
    for (const LineDiff::Match &m : StructuralDiff::hybridAlignment(a, b))
        out[m.first] = m.second;
    return out;
}

bool sameLines(const QStringList &x, int xs, int xc, const QStringList &y, int ys, int yc)
{
    if (xc != yc)
        return false;
    for (int i = 0; i < xc; ++i)
        if (x[xs + i] != y[ys + i])
            return false;
    return true;
}

} // namespace

QVector<Region> regions(const QStringList &base, const QStringList &ours, const QStringList &theirs)
{
    const QVector<int> mo = matches(base, ours), mt = matches(base, theirs);
    const int n = int(base.size());
    QVector<Region> out;
    int i = 0, j = 0, k = 0;
    while (i < n || j < ours.size() || k < theirs.size()) {
        // Stable: base lines both sides still have, in step.
        const int i0 = i, j0 = j, k0 = k;
        while (i < n && mo[i] == j && mt[i] == k)
            ++i, ++j, ++k;
        if (i > i0)
            out.push_back({Region::Kind::Unchanged, i0, i - i0, j0, j - j0, k0, k - k0});
        if (i >= n && j >= ours.size() && k >= theirs.size())
            break;
        // Unstable up to the next base line both sides kept.
        int s = i;
        while (s < n && (mo[s] < 0 || mt[s] < 0))
            ++s;
        const int oe = s < n ? mo[s] : int(ours.size());
        const int te = s < n ? mt[s] : int(theirs.size());
        Region r{Region::Kind::Conflict, i, s - i, j, oe - j, k, te - k};
        const bool oursSame = sameLines(base, r.baseStart, r.baseCount, ours, r.oursStart, r.oursCount);
        const bool theirsSame = sameLines(base, r.baseStart, r.baseCount, theirs, r.theirsStart, r.theirsCount);
        if (oursSame && theirsSame)
            r.kind = Region::Kind::Unchanged; // (a base line kept on both, but not in step)
        else if (oursSame)
            r.kind = Region::Kind::Theirs;
        else if (theirsSame)
            r.kind = Region::Kind::Ours;
        else if (sameLines(ours, r.oursStart, r.oursCount, theirs, r.theirsStart, r.theirsCount))
            r.kind = Region::Kind::Both;
        if (r.baseCount || r.oursCount || r.theirsCount)
            out.push_back(r);
        i = s;
        j = oe;
        k = te;
    }
    return out;
}

QStringList take(const Region &r, Pick pick, const QStringList &base, const QStringList &ours, const QStringList &theirs,
                 const QString &oursLabel, const QString &theirsLabel)
{
    const QStringList o = ours.mid(r.oursStart, r.oursCount);
    const QStringList t = theirs.mid(r.theirsStart, r.theirsCount);
    const QStringList b = base.mid(r.baseStart, r.baseCount);
    if (pick == Pick::Auto) {
        switch (r.kind) {
        case Region::Kind::Unchanged:
        case Region::Kind::Ours:
        case Region::Kind::Both: return o;
        case Region::Kind::Theirs: return t;
        case Region::Kind::Conflict: pick = Pick::Markers; break;
        }
    }
    switch (pick) {
    case Pick::Ours: return o;
    case Pick::Theirs: return t;
    case Pick::OursThenTheirs: return o + t;
    case Pick::TheirsThenOurs: return t + o;
    case Pick::Base: return b;
    case Pick::Markers:
    case Pick::Auto:
        break;
    }
    return QStringList{QStringLiteral("<<<<<<< ") + oursLabel} + o + QStringList{QStringLiteral("||||||| base")} + b
         + QStringList{QStringLiteral("=======")} + t + QStringList{QStringLiteral(">>>>>>> ") + theirsLabel};
}

QStringList merge(const QVector<Region> &regions, const QStringList &base, const QStringList &ours,
                  const QStringList &theirs, int *conflicts)
{
    QStringList out;
    int c = 0;
    for (const Region &r : regions) {
        out += take(r, Pick::Auto, base, ours, theirs);
        c += r.kind == Region::Kind::Conflict;
    }
    if (conflicts)
        *conflicts = c;
    return out;
}

} // namespace Merge3
