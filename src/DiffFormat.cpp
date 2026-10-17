#include "DiffFormat.h"

#include <algorithm>

namespace {

QString &htmlFontFamily()
{
    static QString f = QStringLiteral("'Cascadia Mono','Source Code Pro',Consolas,'DejaVu Sans Mono',monospace");
    return f;
}
double &htmlFontSize()
{
    static double size = 10;
    return size;
}

enum class Style { Meta, Hunk, Context, Removed, Added };

struct OutLine {
    Style style;
    QString text;
};

QString range(int start, int count)
{
    // git omits the count when it is 1.
    return count == 1 ? QString::number(start) : QStringLiteral("%1,%2").arg(start).arg(count);
}

QString lineStyle(Style s)
{
    switch (s) {
    case Style::Meta: return QStringLiteral("color:#24292f;font-weight:bold;");
    case Style::Hunk: return QStringLiteral("color:#0550ae;background:#ddf4ff;");
    case Style::Removed: return QStringLiteral("color:#82071e;background:#ffebe9;");
    case Style::Added: return QStringLiteral("color:#116329;background:#dafbe1;");
    case Style::Context: break;
    }
    return QStringLiteral("color:#24292f;");
}

DiffFormat::Output render(const QVector<OutLine> &lines)
{
    DiffFormat::Output out;
    out.html = QStringLiteral("<div style=\"font-family:%1;font-size:%2pt;background:#ffffff;\">")
                   .arg(htmlFontFamily(), QString::number(htmlFontSize()));
    for (const OutLine &l : lines) {
        out.text += l.text + u'\n';
        QString escaped = l.text.toHtmlEscaped();
        if (escaped.isEmpty())
            escaped = QStringLiteral("&nbsp;");
        out.html += QStringLiteral("<div style=\"white-space:pre;margin:0;%1\">%2</div>").arg(lineStyle(l.style), escaped);
    }
    out.html += QStringLiteral("</div>");
    return out;
}

void appendHeader(QVector<OutLine> &out, const DiffFormat::Paths &p)
{
    const QString a = p.oldPath.isEmpty() ? p.newPath : p.oldPath;
    const QString b = p.newPath.isEmpty() ? p.oldPath : p.newPath;
    out.push_back({Style::Meta, QStringLiteral("diff --git a/%1 b/%2").arg(a, b)});
    out.push_back({Style::Meta, p.oldPath.isEmpty() ? QStringLiteral("--- /dev/null") : QStringLiteral("--- a/") + p.oldPath});
    out.push_back({Style::Meta, p.newPath.isEmpty() ? QStringLiteral("+++ /dev/null") : QStringLiteral("+++ b/") + p.newPath});
}

void appendHunk(QVector<OutLine> &out, const FileDiff &d, int first, int last)
{
    int oldCount = 0, newCount = 0, oldStart = 0, newStart = 0;
    for (int i = first; i <= last; ++i) {
        const DiffLine &l = d.lines[i];
        if (l.kind != LineKind::Added) {
            if (!oldCount)
                oldStart = l.oldNo;
            ++oldCount;
        }
        if (l.kind != LineKind::Removed) {
            if (!newCount)
                newStart = l.newNo;
            ++newCount;
        }
    }
    // With an empty side, git reports the line *before* the hunk.
    auto lineBefore = [&](bool oldSide) {
        for (int i = first - 1; i >= 0; --i) {
            const int n = oldSide ? d.lines[i].oldNo : d.lines[i].newNo;
            if (n > 0)
                return n;
        }
        return 0;
    };
    if (!oldCount)
        oldStart = lineBefore(true);
    if (!newCount)
        newStart = lineBefore(false);

    out.push_back({Style::Hunk, QStringLiteral("@@ -%1 +%2 @@").arg(range(oldStart, oldCount), range(newStart, newCount))});
    for (int i = first; i <= last; ++i) {
        const DiffLine &l = d.lines[i];
        switch (l.kind) {
        case LineKind::Equal: out.push_back({Style::Context, u' ' + l.text}); break;
        case LineKind::Removed: out.push_back({Style::Removed, u'-' + l.text}); break;
        case LineKind::Added: out.push_back({Style::Added, u'+' + l.text}); break;
        }
    }
}

} // namespace

namespace DiffFormat {

void setHtmlFont(const QString &family, double pointSize)
{
    htmlFontFamily() = QStringLiteral("'%1',monospace").arg(family);
    htmlFontSize() = pointSize > 0 ? pointSize : 10;
}

Output unified(const FileDiff &d, const Paths &paths, const QVector<int> &chunks, int context)
{
    QVector<int> sorted = chunks;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

    QVector<OutLine> out;
    appendHeader(out, paths);
    const int n = int(d.lines.size());
    int hunkFirst = -1, hunkLast = -1;
    for (int c : std::as_const(sorted)) {
        if (c < 0 || c >= d.chunks.size())
            continue;
        // Context is unchanged lines only, so an unselected neighbouring change never leaks in.
        int first = d.chunks[c].first;
        for (int k = 0; k < context && first > 0 && d.lines[first - 1].kind == LineKind::Equal; ++k)
            --first;
        int last = d.chunks[c].last;
        for (int k = 0; k < context && last + 1 < n && d.lines[last + 1].kind == LineKind::Equal; ++k)
            ++last;
        if (hunkFirst >= 0 && first <= hunkLast + 1) {
            hunkLast = last; // overlapping context: extend the current hunk
            continue;
        }
        if (hunkFirst >= 0)
            appendHunk(out, d, hunkFirst, hunkLast);
        hunkFirst = first;
        hunkLast = last;
    }
    if (hunkFirst >= 0)
        appendHunk(out, d, hunkFirst, hunkLast);
    return render(out);
}

Output unifiedFile(const FileDiff &d, const Paths &paths, int context)
{
    QVector<int> all(d.chunks.size());
    for (int i = 0; i < all.size(); ++i)
        all[i] = i;
    return unified(d, paths, all, context);
}

QString sideText(const FileDiff &d, int chunk, LineKind side)
{
    if (chunk < 0 || chunk >= d.chunks.size())
        return {};
    QString out;
    for (int i = d.chunks[chunk].first; i <= d.chunks[chunk].last; ++i)
        if (d.lines[i].kind == side)
            out += d.lines[i].text + u'\n';
    return out;
}

} // namespace DiffFormat
