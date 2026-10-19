#include "StructuralDiff.h"

#include <QHash>
#include <QSet>

#include <algorithm>

namespace StructuralDiff {

namespace {

bool isWordChar(QChar c)
{
    return c.isLetterOrNumber() || c == u'_' || c == u'$';
}

// Operators that read as one token.
const QStringList &operators()
{
    static const QStringList ops{QStringLiteral("<<="), QStringLiteral(">>="), QStringLiteral("..."), QStringLiteral("==="),
                                 QStringLiteral("!=="), QStringLiteral("->*"), QStringLiteral("<=>"), QStringLiteral("=="),
                                 QStringLiteral("!="),  QStringLiteral("<="),  QStringLiteral(">="),  QStringLiteral("->"),
                                 QStringLiteral("=>"),  QStringLiteral("::"),  QStringLiteral("&&"),  QStringLiteral("||"),
                                 QStringLiteral("++"),  QStringLiteral("--"),  QStringLiteral("<<"),  QStringLiteral(">>"),
                                 QStringLiteral("+="),  QStringLiteral("-="),  QStringLiteral("*="),  QStringLiteral("/="),
                                 QStringLiteral("%="),  QStringLiteral("&="),  QStringLiteral("|="),  QStringLiteral("^="),
                                 QStringLiteral("**"),  QStringLiteral("//"),  QStringLiteral("??"),  QStringLiteral("?.")};
    return ops;
}

} // namespace

QVector<Token> tokenize(const QStringList &lines, const Comments::Syntax &syntax, bool ignoreCase)
{
    QVector<Token> out;
    int openBlock = -1; // inside this block comment (index into syntax.block)
    auto add = [&](int line, int start, int length, bool comment, const QString &text) {
        out.push_back({line, start, length, comment, ignoreCase ? text.toCaseFolded() : text});
    };
    // Words of comment text, so a changed word in a comment is all that's highlighted.
    auto commentWords = [&](int line, const QString &text, int from, int to) {
        int i = from;
        while (i < to) {
            if (text.at(i).isSpace()) {
                ++i;
                continue;
            }
            int j = i + 1;
            if (isWordChar(text.at(i)))
                while (j < to && isWordChar(text.at(j)))
                    ++j;
            add(line, i, j - i, true, text.mid(i, j - i));
            i = j;
        }
    };
    for (int ln = 0; ln < lines.size(); ++ln) {
        const QString &s = lines[ln];
        const int n = int(s.size());
        int i = 0, skip = 0;
        while (i < n) {
            if (openBlock >= 0) {
                // In a block comment (from an earlier line, or `skip` past its opener here).
                const QString &close = syntax.block[openBlock].second;
                const int end = int(s.indexOf(close, i + skip));
                const int stop = end < 0 ? n : end + int(close.size());
                commentWords(ln, s, i, stop);
                i = stop;
                skip = 0;
                if (end >= 0)
                    openBlock = -1;
                continue;
            }
            const QChar c = s.at(i);
            if (c.isSpace()) {
                ++i;
                continue;
            }
            bool done = false;
            for (int b = 0; b < syntax.block.size() && !done; ++b)
                if (QStringView(s).mid(i).startsWith(syntax.block[b].first)) {
                    openBlock = b;
                    skip = int(syntax.block[b].first.size()); // the closer comes after the opener
                    done = true;
                }
            if (done)
                continue;
            for (const QString &l : syntax.line)
                if (QStringView(s).mid(i).startsWith(l)) {
                    commentWords(ln, s, i, n);
                    i = n;
                    done = true;
                    break;
                }
            if (done)
                continue;
            if (syntax.quotes.contains(c)) {
                int j = i + 1;
                while (j < n && s.at(j) != c)
                    j += s.at(j) == u'\\' ? 2 : 1;
                j = std::min(n, j + 1);
                add(ln, i, j - i, false, s.mid(i, j - i));
                i = j;
                continue;
            }
            if (isWordChar(c)) {
                int j = i + 1;
                while (j < n && isWordChar(s.at(j)))
                    ++j;
                add(ln, i, j - i, false, s.mid(i, j - i));
                i = j;
                continue;
            }
            int len = 1;
            for (const QString &op : operators())
                if (QStringView(s).mid(i).startsWith(op)) {
                    len = int(op.size());
                    break;
                }
            add(ln, i, len, false, s.mid(i, len));
            i += len;
        }
    }
    return out;
}

LineDiff::Alignment hybridAlignment(const QVector<int> &a, const QVector<int> &b)
{
    const LineDiff::Alignment anchors = LineDiff::heckel(a, b);
    LineDiff::Alignment out;
    int pa = 0, pb = 0;
    auto gap = [&](int aEnd, int bEnd) {
        if (aEnd - pa <= 0 || bEnd - pb <= 0)
            return;
        if (qint64(aEnd - pa) * (bEnd - pb) > 50'000'000)
            return; // too big to search exhaustively: leave it changed
        const QVector<int> sa = a.mid(pa, aEnd - pa), sb = b.mid(pb, bEnd - pb);
        for (const LineDiff::Match &m : LineDiff::myers(sa, sb))
            out.push_back({m.first + pa, m.second + pb});
    };
    for (const LineDiff::Match &m : anchors) {
        gap(m.first, m.second);
        out.push_back(m);
        pa = m.first + 1;
        pb = m.second + 1;
    }
    gap(int(a.size()), int(b.size()));
    return out;
}

FileDiff build(const QStringList &oldLines, const QStringList &newLines, const Options &options,
               const LineDiff::Alignment &anchors)
{
    const QVector<Token> oldTokens = tokenize(oldLines, options.syntax, options.ignoreCase);
    const QVector<Token> newTokens = tokenize(newLines, options.syntax, options.ignoreCase);
    auto counts = [&](const Token &t) { return !(options.ignoreComments && t.comment); };

    // Lines first: a line's key is its tokens, so whitespace never matters.
    auto lineKeys = [&](const QStringList &lines, const QVector<Token> &tokens) {
        QStringList keys(lines.size());
        for (const Token &t : tokens)
            if (counts(t))
                keys[t.line] += t.key + u'\x1f';
        return keys;
    };
    const QStringList oldKeys = lineKeys(oldLines, oldTokens), newKeys = lineKeys(newLines, newTokens);
    LineDiff::Alignment kept;
    const LineDiff::Alignment alignment = LineDiff::alignAround(
        oldKeys, newKeys, anchors,
        [](const QStringList &a, const QStringList &b) {
            const auto [ia, ib] = LineDiff::lineIds(a, b);
            return hybridAlignment(ia, ib);
        },
        &kept);
    FileDiff d = LineDiff::build(oldLines, newLines, alignment, false, kept);
    for (DiffLine &l : d.lines) {
        l.highlights.clear();
        if (!l.anchored)
            l.pair = -1;
    }

    // Then each changed region token by token.
    QVector<QVector<int>> oldByLine(oldLines.size()), newByLine(newLines.size()); // token indices
    for (int i = 0; i < oldTokens.size(); ++i)
        oldByLine[oldTokens[i].line] << i;
    for (int i = 0; i < newTokens.size(); ++i)
        newByLine[newTokens[i].line] << i;
    const int n = int(d.lines.size());
    for (int i = 0; i < n;) {
        if (d.lines[i].kind == LineKind::Equal) {
            ++i;
            continue;
        }
        int e = i;
        while (e < n && d.lines[e].kind != LineKind::Equal)
            ++e;
        QVector<int> removed, added; // indices into d.lines
        for (int k = i; k < e; ++k)
            (d.lines[k].kind == LineKind::Removed ? removed : added) << k;
        QVector<int> ot, nt; // the region's tokens that count
        for (int r : std::as_const(removed))
            for (int t : oldByLine[d.lines[r].oldNo - 1])
                if (counts(oldTokens[t]))
                    ot << t;
        for (int a : std::as_const(added))
            for (int t : newByLine[d.lines[a].newNo - 1])
                if (counts(newTokens[t]))
                    nt << t;
        QSet<int> oldMatched, newMatched;
        if (!ot.isEmpty() && !nt.isEmpty() && qint64(ot.size()) * nt.size() <= 400'000'000) {
            QStringList ka, kb;
            for (int t : std::as_const(ot))
                ka << oldTokens[t].key;
            for (int t : std::as_const(nt))
                kb << newTokens[t].key;
            const auto [ia, ib] = LineDiff::lineIds(ka, kb);
            for (const LineDiff::Match &m : hybridAlignment(ia, ib)) {
                oldMatched.insert(ot[m.first]);
                newMatched.insert(nt[m.second]);
            }
        }
        // Lines whose tokens all match (or that have none) only moved around: unimportant.
        // The others highlight their unmatched tokens.
        auto mark = [&](DiffLine &l, const QVector<int> &tokens, const QVector<Token> &all, const QSet<int> &matched) {
            bool changed = false;
            for (int t : tokens) {
                if (!counts(all[t]) || matched.contains(t))
                    continue;
                changed = true;
                const Token &tok = all[t];
                if (!l.highlights.isEmpty() && l.highlights.last().start + l.highlights.last().length >= tok.start - 1)
                    l.highlights.last().length = tok.start + tok.length - l.highlights.last().start;
                else
                    l.highlights.push_back({tok.start, tok.length});
            }
            if (!changed) {
                l.unimportant = true;
                l.highlights.clear();
            }
        };
        for (int r : std::as_const(removed))
            mark(d.lines[r], oldByLine[d.lines[r].oldNo - 1], oldTokens, oldMatched);
        for (int a : std::as_const(added))
            mark(d.lines[a], newByLine[d.lines[a].newNo - 1], newTokens, newMatched);
        // Side by side: changed lines pair up in order, so an edit shows on one row.
        QVector<int> ri, ai;
        for (int r : std::as_const(removed))
            if (!d.lines[r].unimportant && d.lines[r].pair < 0)
                ri << r;
        for (int a : std::as_const(added))
            if (!d.lines[a].unimportant && d.lines[a].pair < 0)
                ai << a;
        for (qsizetype k = 0; k < std::min(ri.size(), ai.size()); ++k) {
            d.lines[ri[k]].pair = ai[k];
            d.lines[ai[k]].pair = ri[k];
        }
        i = e;
    }
    DiffModel::computeChunks(d);
    return d;
}

} // namespace StructuralDiff
