#include "HexDiff.h"

#include "LineDiff.h"

#include <QHash>

#include <algorithm>

namespace HexDiff {

namespace {

constexpr int kOffsetWidth = 8;
constexpr int kHexStart = kOffsetWidth + 2;            // after the offset and two spaces
constexpr int kAsciiStart = kHexStart + kBytesPerRow * 3 + 2; // 16 "xx " plus the mid gap and a space, then '|'

int hexColumn(int byteInRow)
{
    return kHexStart + byteInRow * 3 + (byteInRow >= 8 ? 1 : 0);
}

// A stretch of the alignment: equal bytes, or a replaced region.
struct Segment {
    bool equal = false;
    qsizetype oldStart = 0, oldLen = 0, newStart = 0, newLen = 0;
};

// Content-defined chunks: boundaries depend on the bytes around them, not their
// offset, so an insertion only changes the chunks near it.
QVector<QPair<qsizetype, qsizetype>> chunks(const QByteArray &data)
{
    QVector<QPair<qsizetype, qsizetype>> out;
    quint32 h = 0;
    qsizetype start = 0;
    for (qsizetype i = 0; i < data.size(); ++i) {
        h = (h << 1) + quint32(quint8(data[i])) * 2654435761u;
        const qsizetype len = i + 1 - start;
        if ((len >= 16 && (h & 0x3f) == 0) || len >= 256) {
            out.push_back({start, len});
            start = i + 1;
            h = 0;
        }
    }
    if (start < data.size())
        out.push_back({start, data.size() - start});
    return out;
}

// Myers on bytes, for small replaced regions: which bytes match.
LineDiff::Alignment byteAlignment(const QByteArray &a, const QByteArray &b)
{
    QVector<int> x(a.size()), y(b.size());
    for (qsizetype i = 0; i < a.size(); ++i)
        x[i] = quint8(a[i]);
    for (qsizetype i = 0; i < b.size(); ++i)
        y[i] = quint8(b[i]);
    return LineDiff::myers(x, y);
}

QVector<Segment> align(const QByteArray &a, const QByteArray &b)
{
    QVector<Segment> segs;
    auto addEqual = [&](qsizetype o, qsizetype n, qsizetype len) {
        if (len <= 0)
            return;
        if (!segs.isEmpty() && segs.last().equal && segs.last().oldStart + segs.last().oldLen == o
            && segs.last().newStart + segs.last().newLen == n) {
            segs.last().oldLen += len;
            segs.last().newLen += len;
            return;
        }
        segs.push_back({true, o, len, n, len});
    };
    auto addChange = [&](qsizetype o, qsizetype ol, qsizetype n, qsizetype nl) {
        if (ol <= 0 && nl <= 0)
            return;
        segs.push_back({false, o, ol, n, nl});
    };
    // A region between matched chunks: byte by byte when small enough.
    auto region = [&](qsizetype o, qsizetype ol, qsizetype n, qsizetype nl) {
        if (ol == 0 || nl == 0 || qint64(ol) * nl > 4'000'000) {
            addChange(o, ol, n, nl);
            return;
        }
        qsizetype po = 0, pn = 0;
        for (const LineDiff::Match &m : byteAlignment(a.mid(o, ol), b.mid(n, nl))) {
            addChange(o + po, m.first - po, n + pn, m.second - pn);
            addEqual(o + m.first, n + m.second, 1);
            po = m.first + 1;
            pn = m.second + 1;
        }
        addChange(o + po, ol - po, n + pn, nl - pn);
    };

    if (a.size() == b.size()) {
        // Same size: most likely bytes were overwritten in place; compare by position.
        qsizetype i = 0;
        while (i < a.size()) {
            qsizetype j = i;
            const bool eq = a[i] == b[i];
            while (j < a.size() && (a[j] == b[j]) == eq)
                ++j;
            if (eq)
                addEqual(i, i, j - i);
            else
                addChange(i, j - i, i, j - i);
            i = j;
        }
        return segs;
    }

    const auto ca = chunks(a), cb = chunks(b);
    QHash<QByteArray, int> ids;
    auto idOf = [&ids](const QByteArray &key) {
        auto it = ids.constFind(key);
        return it == ids.constEnd() ? ids.insert(key, int(ids.size())).value() : it.value();
    };
    QVector<int> ia, ib;
    for (const auto &c : ca)
        ia << idOf(a.mid(c.first, c.second));
    for (const auto &c : cb)
        ib << idOf(b.mid(c.first, c.second));
    qsizetype po = 0, pn = 0; // chunk indices
    auto bytesOf = [](const QVector<QPair<qsizetype, qsizetype>> &cs, qsizetype from, qsizetype to, qsizetype total) {
        const qsizetype s = from < cs.size() ? cs[from].first : total;
        const qsizetype e = to < cs.size() ? cs[to].first : total;
        return QPair<qsizetype, qsizetype>(s, e - s);
    };
    for (const LineDiff::Match &m : LineDiff::myers(ia, ib)) {
        const auto ro = bytesOf(ca, po, m.first, a.size());
        const auto rn = bytesOf(cb, pn, m.second, b.size());
        region(ro.first, ro.second, rn.first, rn.second);
        addEqual(ca[m.first].first, cb[m.second].first, ca[m.first].second);
        po = m.first + 1;
        pn = m.second + 1;
    }
    const auto ro = bytesOf(ca, po, ca.size(), a.size());
    const auto rn = bytesOf(cb, pn, cb.size(), b.size());
    region(ro.first, ro.second, rn.first, rn.second);
    return segs;
}

void highlightBytes(DiffLine &l, const QVector<bool> &changed, qsizetype from, qsizetype count)
{
    for (qsizetype k = 0; k < count; ++k) {
        if (!changed[from + k])
            continue;
        l.highlights.push_back({hexColumn(int(k)), 2});
        l.highlights.push_back({kAsciiStart + 1 + int(k), 1});
    }
}

} // namespace

QString row(const QByteArray &data, qsizetype offset, qsizetype count)
{
    QString s = QStringLiteral("%1  ").arg(quint64(offset), kOffsetWidth, 16, QLatin1Char('0'));
    QString ascii;
    for (int k = 0; k < kBytesPerRow; ++k) {
        if (k == 8)
            s += u' ';
        if (k < count) {
            const quint8 c = quint8(data[offset + k]);
            s += QStringLiteral("%1 ").arg(c, 2, 16, QLatin1Char('0'));
            ascii += c >= 0x20 && c < 0x7f ? QChar(c) : QChar(u'.');
        } else {
            s += QStringLiteral("   ");
        }
    }
    return s + QStringLiteral(" |") + ascii + u'|';
}

FileDiff build(const QByteArray &oldData, const QByteArray &newData)
{
    const QVector<Segment> segs = align(oldData, newData);
    // Blocks: long equal runs, and changed regions (short equal runs between changes
    // join the region, so the rows around them show as changed with only the changed
    // bytes highlighted).
    struct Block {
        bool equal = false;
        qsizetype o = 0, ol = 0, n = 0, nl = 0;
        QVector<bool> oldChanged, newChanged;
    };
    constexpr qsizetype kResync = 2 * kBytesPerRow;
    QVector<Block> blocks;
    for (qsizetype i = 0; i < segs.size();) {
        if (segs[i].equal && (segs[i].oldLen >= kResync || i == 0 || i == segs.size() - 1)) {
            blocks.push_back({true, segs[i].oldStart, segs[i].oldLen, segs[i].newStart, segs[i].newLen, {}, {}});
            ++i;
            continue;
        }
        Block b;
        b.o = segs[i].oldStart;
        b.n = segs[i].newStart;
        while (i < segs.size() && !(segs[i].equal && segs[i].oldLen >= kResync)) {
            b.oldChanged.insert(b.oldChanged.size(), segs[i].oldLen, !segs[i].equal);
            b.newChanged.insert(b.newChanged.size(), segs[i].newLen, !segs[i].equal);
            ++i;
        }
        b.ol = b.oldChanged.size();
        b.nl = b.newChanged.size();
        blocks.push_back(b);
    }
    // Widen each changed region to whole rows (on the new side), borrowing unchanged
    // bytes from the equal blocks around it, so rows keep to 16-byte boundaries.
    for (qsizetype i = 0; i < blocks.size(); ++i) {
        Block &b = blocks[i];
        if (b.equal)
            continue;
        if (i > 0 && blocks[i - 1].equal) {
            Block &p = blocks[i - 1];
            const qsizetype take = std::min(b.n % kBytesPerRow, p.ol);
            p.ol -= take;
            p.nl -= take;
            b.o -= take;
            b.n -= take;
            b.ol += take;
            b.nl += take;
            b.oldChanged.insert(0, take, false);
            b.newChanged.insert(0, take, false);
        }
        if (i + 1 < blocks.size() && blocks[i + 1].equal) {
            Block &q = blocks[i + 1];
            const qsizetype take = std::min((kBytesPerRow - (b.n + b.nl) % kBytesPerRow) % kBytesPerRow, q.ol);
            q.o += take;
            q.n += take;
            q.ol -= take;
            q.nl -= take;
            b.ol += take;
            b.nl += take;
            b.oldChanged.insert(b.oldChanged.size(), take, false);
            b.newChanged.insert(b.newChanged.size(), take, false);
        }
    }

    FileDiff d;
    int oldNo = 0, newNo = 0;
    for (const Block &b : std::as_const(blocks)) {
        if (b.equal) {
            for (qsizetype k = 0; k < b.ol; k += kBytesPerRow) {
                const qsizetype n = std::min<qsizetype>(kBytesPerRow, b.ol - k);
                DiffLine l;
                l.kind = LineKind::Equal;
                l.oldNo = ++oldNo;
                l.newNo = ++newNo;
                l.text = row(newData, b.n + k, n);
                if (b.o != b.n)
                    l.oldText = row(oldData, b.o + k, n); // its own offset on the left
                d.lines.push_back(l);
            }
            continue;
        }
        const int firstRemoved = int(d.lines.size());
        for (qsizetype k = 0; k < b.ol; k += kBytesPerRow) {
            const qsizetype n = std::min<qsizetype>(kBytesPerRow, b.ol - k);
            DiffLine l;
            l.kind = LineKind::Removed;
            l.oldNo = ++oldNo;
            l.text = row(oldData, b.o + k, n);
            highlightBytes(l, b.oldChanged, k, n);
            d.lines.push_back(l);
        }
        const int firstAdded = int(d.lines.size());
        for (qsizetype k = 0; k < b.nl; k += kBytesPerRow) {
            const qsizetype n = std::min<qsizetype>(kBytesPerRow, b.nl - k);
            DiffLine l;
            l.kind = LineKind::Added;
            l.newNo = ++newNo;
            l.text = row(newData, b.n + k, n);
            highlightBytes(l, b.newChanged, k, n);
            d.lines.push_back(l);
        }
        // Row k of the old bytes beside row k of the new ones.
        for (int k = 0; firstRemoved + k < firstAdded && firstAdded + k < d.lines.size(); ++k) {
            d.lines[firstRemoved + k].pair = firstAdded + k;
            d.lines[firstAdded + k].pair = firstRemoved + k;
        }
    }
    DiffModel::computeChunks(d);
    d.hexDump = true;
    return d;
}

} // namespace HexDiff
