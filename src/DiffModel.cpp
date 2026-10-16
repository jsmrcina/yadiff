#include "DiffModel.h"

#include <QHash>
#include <QRegularExpression>

#include <algorithm>
#include <vector>

namespace {

QString stripCR(QString s)
{
    if (s.endsWith(u'\r'))
        s.chop(1);
    return s;
}

int nonSpaceCount(const QString &s)
{
    int n = 0;
    for (QChar c : s)
        if (!c.isSpace())
            ++n;
    return n;
}

// Tokens for intra-line diffing: identifier runs, whitespace runs, or single
// punctuation characters.
QVector<Span> tokenize(const QString &s)
{
    QVector<Span> out;
    const int n = s.size();
    int i = 0;
    auto isWord = [](QChar c) { return c.isLetterOrNumber() || c == u'_'; };
    while (i < n) {
        const QChar c = s.at(i);
        int j = i + 1;
        if (isWord(c)) {
            while (j < n && isWord(s.at(j)))
                ++j;
        } else if (c.isSpace()) {
            while (j < n && s.at(j).isSpace())
                ++j;
        }
        out.push_back({i, j - i});
        i = j;
    }
    return out;
}

void appendSpan(QVector<Span> &spans, const Span &s)
{
    if (!spans.isEmpty() && spans.last().start + spans.last().length == s.start)
        spans.last().length += s.length;
    else
        spans.push_back(s);
}

// Token-level LCS between two lines; fills highlights with the unmatched parts.
void intraLineDiff(DiffLine &a, DiffLine &b)
{
    const QString &ta = a.text;
    const QString &tb = b.text;
    if (ta == tb)
        return;
    const QVector<Span> xa = tokenize(ta);
    const QVector<Span> xb = tokenize(tb);
    const qsizetype n = xa.size();
    const qsizetype m = xb.size();
    if (n == 0 || m == 0 || n * m > 400000)
        return;

    auto tok = [](const QString &s, const Span &sp) { return QStringView(s).mid(sp.start, sp.length); };

    std::vector<int> dp(size_t((n + 1) * (m + 1)), 0);
    auto at = [&](qsizetype i, qsizetype j) -> int & { return dp[size_t(i * (m + 1) + j)]; };
    for (qsizetype i = n - 1; i >= 0; --i)
        for (qsizetype j = m - 1; j >= 0; --j)
            at(i, j) = tok(ta, xa[i]) == tok(tb, xb[j]) ? at(i + 1, j + 1) + 1
                                                         : std::max(at(i + 1, j), at(i, j + 1));

    QVector<Span> ha, hb;
    int matchedChars = 0;
    qsizetype i = 0, j = 0;
    while (i < n && j < m) {
        if (tok(ta, xa[i]) == tok(tb, xb[j])) {
            matchedChars += xa[i].length;
            ++i;
            ++j;
        } else if (at(i + 1, j) >= at(i, j + 1)) {
            appendSpan(ha, xa[i++]);
        } else {
            appendSpan(hb, xb[j++]);
        }
    }
    while (i < n)
        appendSpan(ha, xa[i++]);
    while (j < m)
        appendSpan(hb, xb[j++]);

    // When the lines barely resemble each other, word highlights are noise.
    const double similarity = 2.0 * matchedChars / double(ta.size() + tb.size());
    if (similarity < 0.4)
        return;
    a.highlights = ha;
    b.highlights = hb;
}

// Dice coefficient over character bigrams of the trimmed lines, in [0, 1].
double similarity(const QString &x, const QString &y)
{
    const QString a = x.trimmed();
    const QString b = y.trimmed();
    if (a == b)
        return 1.0;
    if (a.size() < 2 || b.size() < 2)
        return 0.0;
    QHash<quint32, int> grams;
    for (qsizetype i = 0; i + 1 < a.size(); ++i)
        ++grams[(quint32(a.at(i).unicode()) << 16) | a.at(i + 1).unicode()];
    int common = 0;
    for (qsizetype i = 0; i + 1 < b.size(); ++i) {
        auto it = grams.find((quint32(b.at(i).unicode()) << 16) | b.at(i + 1).unicode());
        if (it != grams.end() && *it > 0) {
            --*it;
            ++common;
        }
    }
    return 2.0 * common / double(a.size() - 1 + b.size() - 1);
}

const QRegularExpression &hunkRe()
{
    static const QRegularExpression re(QStringLiteral(R"(^@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@)"));
    return re;
}

} // namespace

namespace DiffModel {

FileDiff parseUnified(const QString &diffText, bool detectMoves)
{
    FileDiff d;
    const QStringList raw = diffText.split(u'\n');
    bool inHunk = false;
    int oldNo = 0;
    int newNo = 0;
    for (const QString &line : raw) {
        if (line.startsWith(QLatin1String("@@"))) {
            const auto m = hunkRe().match(line);
            if (m.hasMatch()) {
                oldNo = m.captured(1).toInt();
                newNo = m.captured(2).toInt();
                inHunk = true;
            }
            continue;
        }
        if (!inHunk) {
            if (line.startsWith(QLatin1String("Binary files ")) || line.startsWith(QLatin1String("GIT binary patch")))
                d.binary = true;
            continue;
        }
        if (line.isEmpty())
            continue; // trailing newline of the output
        if (line.startsWith(QLatin1String("diff ")))
            break; // a second file: not expected, stop here

        DiffLine dl;
        dl.text = stripCR(line.mid(1));
        switch (line.at(0).unicode()) {
        case ' ':
            dl.kind = LineKind::Equal;
            dl.oldNo = oldNo++;
            dl.newNo = newNo++;
            break;
        case '-':
            dl.kind = LineKind::Removed;
            dl.oldNo = oldNo++;
            break;
        case '+':
            dl.kind = LineKind::Added;
            dl.newNo = newNo++;
            break;
        default:
            continue; // "\ No newline at end of file" and anything unknown
        }
        d.lines.push_back(std::move(dl));
    }
    finalize(d, detectMoves);
    return d;
}

FileDiff allAdded(const QString &content)
{
    FileDiff d;
    QStringList lines = content.split(u'\n');
    if (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    int no = 1;
    for (const QString &l : std::as_const(lines)) {
        DiffLine dl;
        dl.kind = LineKind::Added;
        dl.newNo = no++;
        dl.text = stripCR(l);
        d.lines.push_back(std::move(dl));
    }
    finalize(d);
    return d;
}

void computeChunks(FileDiff &d)
{
    d.chunks.clear();
    const int n = int(d.lines.size());
    int i = 0;
    while (i < n) {
        if (d.lines[i].kind == LineKind::Equal) {
            ++i;
            continue;
        }
        Chunk c;
        c.first = i;
        while (i < n && d.lines[i].kind != LineKind::Equal)
            ++i;
        c.last = i - 1;
        d.chunks.push_back(c);
    }
}

void detectMoves(FileDiff &d, int minChars)
{
    d.moves.clear();
    const int n = int(d.lines.size());
    if (n == 0)
        return;

    QVector<int> chunkOf(n, -1);
    for (int c = 0; c < d.chunks.size(); ++c)
        for (int i = d.chunks[c].first; i <= d.chunks[c].last; ++i)
            chunkOf[i] = c;

    // Moves are compared ignoring leading/trailing whitespace so that
    // re-indented code still counts as moved.
    QVector<QString> key(n);
    QHash<QString, QVector<int>> addedByKey;
    for (int i = 0; i < n; ++i) {
        if (d.lines[i].kind == LineKind::Equal)
            continue;
        key[i] = d.lines[i].text.trimmed();
        if (d.lines[i].kind == LineKind::Added && !key[i].isEmpty())
            addedByKey[key[i]].push_back(i);
    }

    for (int i = 0; i < n;) {
        const DiffLine &line = d.lines[i];
        if (line.kind != LineKind::Removed || line.moveId >= 0 || key[i].isEmpty()) {
            ++i;
            continue;
        }
        const auto it = addedByKey.constFind(key[i]);
        if (it == addedByKey.constEnd()) {
            ++i;
            continue;
        }

        int bestJ = -1, bestLen = 0, bestChars = 0, tried = 0;
        for (int j : *it) {
            // A match inside the same chunk is an in-place edit (e.g. re-indent), not a move.
            if (d.lines[j].moveId >= 0 || chunkOf[j] == chunkOf[i])
                continue;
            if (++tried > 1000)
                break;
            int len = 0, chars = 0;
            while (i + len < n && j + len < n) {
                const DiffLine &a = d.lines[i + len];
                const DiffLine &b = d.lines[j + len];
                if (a.kind != LineKind::Removed || b.kind != LineKind::Added || a.moveId >= 0 || b.moveId >= 0
                    || key[i + len] != key[j + len])
                    break;
                chars += nonSpaceCount(a.text);
                ++len;
            }
            if (chars > bestChars) {
                bestChars = chars;
                bestLen = len;
                bestJ = j;
            }
        }

        if (bestJ < 0 || bestChars < minChars) {
            ++i;
            continue;
        }
        const int id = int(d.moves.size());
        d.moves.push_back({i, bestJ, bestLen});
        for (int k = 0; k < bestLen; ++k) {
            d.lines[i + k].moveId = id;
            d.lines[i + k].partner = bestJ + k;
            d.lines[bestJ + k].moveId = id;
            d.lines[bestJ + k].partner = i + k;
        }
        i += bestLen;
    }
}

void computeIntraLine(FileDiff &d)
{
    for (const Chunk &c : std::as_const(d.chunks)) {
        QVector<int> removed, added;
        for (int i = c.first; i <= c.last; ++i) {
            if (d.lines[i].moveId >= 0)
                continue;
            (d.lines[i].kind == LineKind::Removed ? removed : added).push_back(i);
        }
        // Pair each removed line with the most similar later added line, keeping
        // pairs in order so side-by-side rows never cross.
        qsizetype nextAdded = 0;
        for (int r : std::as_const(removed)) {
            qsizetype best = -1;
            double bestScore = 0.5; // minimum similarity to count as an edit of the same line
            const qsizetype end = std::min(added.size(), nextAdded + 64);
            for (qsizetype k = nextAdded; k < end; ++k) {
                const double score = similarity(d.lines[r].text, d.lines[added[k]].text);
                if (score > bestScore) {
                    bestScore = score;
                    best = k;
                }
            }
            if (best < 0)
                continue;
            DiffLine &a = d.lines[r];
            DiffLine &b = d.lines[added[best]];
            a.pair = added[best];
            b.pair = r;
            intraLineDiff(a, b);
            nextAdded = best + 1;
        }
    }
}

void finalize(FileDiff &d, bool moves)
{
    computeChunks(d);
    if (moves)
        detectMoves(d);
    computeIntraLine(d);
}

} // namespace DiffModel
