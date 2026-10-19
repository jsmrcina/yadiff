#include "LineDiff.h"

#include <QHash>
#include <QRegularExpression>

#include <algorithm>

namespace LineDiff {

std::pair<QVector<int>, QVector<int>> lineIds(const QStringList &oldLines, const QStringList &newLines)
{
    QHash<QString, int> ids;
    auto idsOf = [&ids](const QStringList &lines) {
        QVector<int> out;
        out.reserve(lines.size());
        for (const QString &l : lines)
            out.push_back(*ids.insert(l, ids.value(l, int(ids.size()))));
        return out;
    };
    QVector<int> a = idsOf(oldLines);
    QVector<int> b = idsOf(newLines);
    return {std::move(a), std::move(b)};
}

// ---- Myers ------------------------------------------------------------------

namespace {

struct Myers {
    const QVector<int> &a;
    const QVector<int> &b;
    Alignment out;
    QVector<int> vf, vb; // indexed by diagonal + offset

    struct Snake {
        int x, y, u, v; // from (x, y) to (u, v), relative to the subproblem
    };

    // Finds the middle snake of a[aLo, aHi) against b[bLo, bHi); both non-empty.
    Snake middleSnake(int aLo, int aHi, int bLo, int bHi)
    {
        const int n = aHi - aLo, m = bHi - bLo;
        const int delta = n - m;
        const bool odd = delta & 1;
        const int maxD = (n + m + 1) / 2;
        const int offset = maxD + 1;
        vf.fill(0, 2 * offset + 1);
        vb.fill(0, 2 * offset + 1);
        for (int d = 0; d <= maxD; ++d) {
            for (int k = -d; k <= d; k += 2) {
                int x = (k == -d || (k != d && vf[offset + k - 1] < vf[offset + k + 1])) ? vf[offset + k + 1]
                                                                                       : vf[offset + k - 1] + 1;
                int y = x - k;
                const int x0 = x, y0 = y;
                while (x < n && y < m && a[aLo + x] == b[bLo + y])
                    ++x, ++y;
                vf[offset + k] = x;
                const int kr = delta - k;
                if (odd && kr >= -(d - 1) && kr <= d - 1 && x + vb[offset + kr] >= n)
                    return {x0, y0, x, y};
            }
            for (int kr = -d; kr <= d; kr += 2) {
                // Runs forwards over the reversed sequences.
                int x = (kr == -d || (kr != d && vb[offset + kr - 1] < vb[offset + kr + 1])) ? vb[offset + kr + 1]
                                                                                           : vb[offset + kr - 1] + 1;
                int y = x - kr;
                const int x0 = x, y0 = y;
                while (x < n && y < m && a[aHi - 1 - x] == b[bHi - 1 - y])
                    ++x, ++y;
                vb[offset + kr] = x;
                const int k = delta - kr;
                if (!odd && k >= -d && k <= d && x + vf[offset + k] >= n)
                    return {n - x, m - y, n - x0, m - y0};
            }
        }
        return {0, 0, 0, 0}; // unreachable
    }

    void run(int aLo, int aHi, int bLo, int bHi)
    {
        // Common prefix and suffix match outright.
        QVector<Match> suffix;
        while (aLo < aHi && bLo < bHi && a[aLo] == b[bLo])
            out.push_back({aLo++, bLo++});
        while (aLo < aHi && bLo < bHi && a[aHi - 1] == b[bHi - 1])
            suffix.push_back({--aHi, --bHi});
        // With both trimmed, a non-empty pair differs by at least two edits, so both
        // halves around the middle snake are smaller problems.
        if (aLo < aHi && bLo < bHi) {
            const Snake s = middleSnake(aLo, aHi, bLo, bHi);
            run(aLo, aLo + s.x, bLo, bLo + s.y);
            for (int i = 0; i < s.u - s.x; ++i)
                out.push_back({aLo + s.x + i, bLo + s.y + i});
            run(aLo + s.u, aHi, bLo + s.v, bHi);
        }
        for (auto it = suffix.crbegin(); it != suffix.crend(); ++it)
            out.push_back(*it);
    }
};

} // namespace

Alignment myers(const QVector<int> &a, const QVector<int> &b)
{
    Myers m{a, b, {}, {}, {}};
    m.run(0, int(a.size()), 0, int(b.size()));
    return std::move(m.out);
}

// ---- Heckel -----------------------------------------------------------------

namespace {

// Longest subsequence of `pairs` (in new-side order) whose old indices increase.
Alignment longestInOrder(const Alignment &pairs)
{
    QVector<int> tailIndex; // tailIndex[len - 1]: index in pairs of the smallest tail of that length
    QVector<int> prev(pairs.size(), -1);
    for (int i = 0; i < pairs.size(); ++i) {
        const int key = pairs[i].first;
        auto it = std::lower_bound(tailIndex.begin(), tailIndex.end(), key,
                                   [&pairs](int idx, int k) { return pairs[idx].first < k; });
        const int pos = int(it - tailIndex.begin());
        if (pos > 0)
            prev[i] = tailIndex[pos - 1];
        if (it == tailIndex.end())
            tailIndex.push_back(i);
        else
            *it = i;
    }
    Alignment out;
    for (int i = tailIndex.isEmpty() ? -1 : tailIndex.last(); i >= 0; i = prev[i])
        out.push_back(pairs[i]);
    std::reverse(out.begin(), out.end());
    return out;
}

} // namespace

Alignment heckel(const QVector<int> &a, const QVector<int> &b)
{
    const int n = int(a.size()), m = int(b.size());
    // Pass 1 and 2: how often each line occurs on each side.
    QHash<int, int> oldCount, newCount, oldAt;
    for (int j = 0; j < n; ++j) {
        ++oldCount[a[j]];
        oldAt[a[j]] = j;
    }
    for (int i = 0; i < m; ++i)
        ++newCount[b[i]];

    QVector<int> na(m, -1), oa(n, -1);
    auto link = [&](int j, int i) {
        oa[j] = i;
        na[i] = j;
    };
    // Pass 3: lines that occur exactly once on each side are anchors.
    for (int i = 0; i < m; ++i)
        if (newCount.value(b[i]) == 1 && oldCount.value(b[i]) == 1)
            link(oldAt.value(b[i]), i);
    // The file start and end act as anchors too.
    for (int i = 0; i < std::min(n, m) && na[i] < 0 && oa[i] < 0 && a[i] == b[i]; ++i)
        link(i, i);
    for (int i = m - 1, j = n - 1; i >= 0 && j >= 0 && na[i] < 0 && oa[j] < 0 && a[j] == b[i]; --i, --j)
        link(j, i);
    // Pass 4 and 5: matches spread to equal neighbours, forwards then backwards.
    for (int i = 0; i + 1 < m; ++i)
        if (const int j = na[i] + 1; na[i] >= 0 && na[i + 1] < 0 && j < n && oa[j] < 0 && a[j] == b[i + 1])
            link(j, i + 1);
    for (int i = m - 1; i > 0; --i)
        if (const int j = na[i] - 1; na[i] >= 0 && na[i - 1] < 0 && j >= 0 && oa[j] < 0 && a[j] == b[i - 1])
            link(j, i - 1);

    Alignment pairs;
    for (int i = 0; i < m; ++i)
        if (na[i] >= 0)
            pairs.push_back({na[i], i});
    const Alignment kept = longestInOrder(pairs);

    // Dropping crossing matches can leave equal lines next to the kept ones: extend
    // into each gap from both of its ends.
    Alignment out;
    auto fillGap = [&](int aLo, int aHi, int bLo, int bHi) {
        Alignment tail;
        while (aLo < aHi && bLo < bHi && a[aLo] == b[bLo])
            out.push_back({aLo++, bLo++});
        while (aLo < aHi && bLo < bHi && a[aHi - 1] == b[bHi - 1])
            tail.push_back({--aHi, --bHi});
        for (auto it = tail.crbegin(); it != tail.crend(); ++it)
            out.push_back(*it);
    };
    int pa = 0, pb = 0;
    for (const Match &mt : kept) {
        fillGap(pa, mt.first, pb, mt.second);
        out.push_back(mt);
        pa = mt.first + 1;
        pb = mt.second + 1;
    }
    fillGap(pa, n, pb, m);
    return out;
}

// ---- git -U0 ----------------------------------------------------------------

Alignment fromUnifiedZero(const QString &diff, int n, int m)
{
    static const QRegularExpression hunk(QStringLiteral("^@@ -(\\d+)(?:,(\\d+))? \\+(\\d+)(?:,(\\d+))? @@"),
                                         QRegularExpression::MultilineOption);
    Alignment out;
    int oi = 0, ni = 0;
    auto it = hunk.globalMatch(diff);
    while (it.hasNext()) {
        const auto mt = it.next();
        const int os = mt.captured(1).toInt();
        const int oc = mt.capturedLength(2) ? mt.captured(2).toInt() : 1;
        const int ns = mt.captured(3).toInt();
        const int nc = mt.capturedLength(4) ? mt.captured(4).toInt() : 1;
        // With no lines on a side, the start is the line before the change.
        const int oFirst = oc > 0 ? os - 1 : os;
        const int nFirst = nc > 0 ? ns - 1 : ns;
        while (oi < oFirst && ni < nFirst)
            out.push_back({oi++, ni++});
        oi = std::max(oi, oFirst + oc);
        ni = std::max(ni, nFirst + nc);
    }
    while (oi < n && ni < m)
        out.push_back({oi++, ni++});
    return out;
}

// ---- Building the diff ------------------------------------------------------

QStringList splitLines(const QString &content)
{
    QStringList lines = content.split(u'\n');
    if (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    return lines;
}

Alignment alignAround(const QStringList &oldNorm, const QStringList &newNorm, const Alignment &anchors,
                      const std::function<Alignment(const QStringList &, const QStringList &)> &align,
                      Alignment *kept)
{
    Alignment sorted = anchors;
    std::sort(sorted.begin(), sorted.end());
    Alignment use;
    for (const Match &a : std::as_const(sorted)) {
        if (a.first < 0 || a.second < 0 || a.first >= oldNorm.size() || a.second >= newNorm.size())
            continue;
        if (!use.isEmpty() && (a.first <= use.last().first || a.second <= use.last().second))
            continue; // crosses (or shares a line with) an earlier anchor
        use.push_back(a);
    }
    if (kept)
        *kept = use;
    Alignment out;
    int po = 0, pn = 0;
    auto segment = [&](int oEnd, int nEnd) {
        const Alignment part = align(oldNorm.mid(po, oEnd - po), newNorm.mid(pn, nEnd - pn));
        for (const Match &m : part)
            out.push_back({m.first + po, m.second + pn});
    };
    for (const Match &a : std::as_const(use)) {
        segment(a.first, a.second);
        if (oldNorm[a.first] == newNorm[a.second])
            out.push_back(a);
        po = a.first + 1;
        pn = a.second + 1;
    }
    segment(int(oldNorm.size()), int(newNorm.size()));
    return out;
}

FileDiff build(const QStringList &oldLines, const QStringList &newLines, const Alignment &alignment, bool detectMoves,
               const Alignment &anchors)
{
    auto display = [](const QString &l) { return l.endsWith(u'\r') ? l.chopped(1) : l; };
    FileDiff d;
    d.lines.reserve(std::max(oldLines.size(), newLines.size()));
    int oi = 0, ni = 0;
    auto emitUntil = [&](int oEnd, int nEnd) {
        for (; oi < oEnd; ++oi) {
            DiffLine dl;
            dl.kind = LineKind::Removed;
            dl.oldNo = oi + 1;
            dl.text = display(oldLines[oi]);
            d.lines.push_back(std::move(dl));
        }
        for (; ni < nEnd; ++ni) {
            DiffLine dl;
            dl.kind = LineKind::Added;
            dl.newNo = ni + 1;
            dl.text = display(newLines[ni]);
            d.lines.push_back(std::move(dl));
        }
    };
    for (const Match &mt : alignment) {
        emitUntil(mt.first, mt.second);
        DiffLine dl;
        dl.kind = LineKind::Equal;
        dl.oldNo = ++oi;
        dl.newNo = ++ni;
        dl.text = display(newLines[mt.second]);
        if (const QString old = display(oldLines[mt.first]); old != dl.text) {
            dl.oldText = old;
            dl.unimportant = true;
        }
        d.lines.push_back(std::move(dl));
    }
    emitUntil(int(oldLines.size()), int(newLines.size()));
    if (!anchors.isEmpty()) {
        QHash<int, int> oldRow, newRow; // line number -> index in d.lines
        for (int i = 0; i < d.lines.size(); ++i) {
            if (d.lines[i].kind != LineKind::Added)
                oldRow.insert(d.lines[i].oldNo, i);
            if (d.lines[i].kind != LineKind::Removed)
                newRow.insert(d.lines[i].newNo, i);
        }
        for (const Match &a : anchors) {
            const int r = oldRow.value(a.first + 1, -1);
            const int s = newRow.value(a.second + 1, -1);
            if (r < 0 || s < 0)
                continue;
            d.lines[r].anchored = d.lines[s].anchored = true;
            if (r != s) { // not equal: show them as an edit, on one row
                d.lines[r].pair = s;
                d.lines[s].pair = r;
            }
        }
    }
    DiffModel::finalize(d, detectMoves);
    return d;
}

} // namespace LineDiff
