#include "PatchFile.h"

#include <QRegularExpression>

namespace PatchFile {

namespace {

// A path from a ---/+++ or diff --git line: unquoted, without its timestamp and a/ or b/.
QString cleanPath(QString p, bool stripPrefix)
{
    if (const qsizetype tab = p.indexOf(u'\t'); tab >= 0)
        p.truncate(tab);
    p = p.trimmed();
    if (p.size() >= 2 && p.startsWith(u'"') && p.endsWith(u'"')) {
        p = p.mid(1, p.size() - 2);
        p.replace(QLatin1String("\\\""), QLatin1String("\""));
        p.replace(QLatin1String("\\\\"), QLatin1String("\\"));
    }
    if (p == QLatin1String("/dev/null"))
        return {};
    if (stripPrefix && p.size() > 2 && p.at(1) == u'/' && (p.at(0) == u'a' || p.at(0) == u'b'))
        p = p.mid(2);
    return p;
}

} // namespace

QVector<File> parse(const QString &patch)
{
    static const QRegularExpression hunkRe(QStringLiteral(R"(^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@)"));
    // Unquoted paths may contain spaces; "rename from/to" lines, when there, settle those.
    static const QRegularExpression gitRe(QStringLiteral(R"(^diff --git ("[^"]*"|a/.*?|\S+) ("[^"]*"|b/.*|\S+)$)"));
    QVector<File> files;
    const QStringList lines = patch.split(u'\n');
    File *cur = nullptr;
    int oldLeft = 0, newLeft = 0; // lines still to come in the current hunk
    bool git = false;
    auto startFile = [&] {
        files.push_back(File());
        cur = &files.last();
    };
    for (qsizetype i = 0; i < lines.size(); ++i) {
        QString line = lines[i];
        if (line.endsWith(u'\r'))
            line.chop(1);
        if (cur && (oldLeft > 0 || newLeft > 0)) {
            // Inside a hunk: the counts say how many lines belong to it.
            const QChar c = line.isEmpty() ? u' ' : line.at(0);
            if (c == u'\\') {
                cur->text += lines[i] + u'\n';
                continue; // "\ No newline at end of file"
            }
            if (c == u' ' || c == u'-' || c == u'+') {
                // An empty line is an unchanged empty line whose space was trimmed.
                cur->hunks.last().lines << (line.isEmpty() ? QStringLiteral(" ") : line);
                cur->text += (line.isEmpty() ? QStringLiteral(" ") : lines[i]) + u'\n';
                if (c != u'+')
                    --oldLeft;
                if (c != u'-')
                    --newLeft;
                cur->added += c == u'+';
                cur->removed += c == u'-';
                continue;
            }
            oldLeft = newLeft = 0; // a malformed hunk: carry on with the headers
        }
        if (const auto m = gitRe.match(line); m.hasMatch()) {
            startFile();
            git = true;
            cur->oldPath = cleanPath(m.captured(1), true);
            cur->newPath = cleanPath(m.captured(2), true);
            cur->text = lines[i] + u'\n';
            continue;
        }
        if (line.startsWith(QLatin1String("--- ")) && i + 1 < lines.size() && lines[i + 1].startsWith(QLatin1String("+++ "))) {
            // A plain `diff -u` file starts here; in a git patch the ---/+++ lines follow diff --git.
            if (!cur || !git || !cur->hunks.isEmpty()) {
                startFile();
                git = false;
            }
            const bool strip = git || (lines[i].mid(4).startsWith(QLatin1String("a/")) && lines[i + 1].mid(4).startsWith(QLatin1String("b/")));
            cur->oldPath = cleanPath(line.mid(4), strip);
            cur->newPath = cleanPath(lines[i + 1].mid(4), strip);
            if (cur->oldPath.isEmpty())
                cur->status = u'A';
            else if (cur->newPath.isEmpty())
                cur->status = u'D';
            cur->text += lines[i] + u'\n' + lines[i + 1] + u'\n';
            ++i;
            continue;
        }
        if (!cur)
            continue; // the mail header and message of a format-patch file
        if (const auto m = hunkRe.match(line); m.hasMatch()) {
            Hunk h;
            h.oldStart = m.captured(1).toInt();
            h.oldCount = m.capturedLength(2) ? m.captured(2).toInt() : 1;
            h.newStart = m.captured(3).toInt();
            h.newCount = m.capturedLength(4) ? m.captured(4).toInt() : 1;
            oldLeft = h.oldCount;
            newLeft = h.newCount;
            cur->hunks.push_back(h);
            cur->text += lines[i] + u'\n';
            continue;
        }
        if (line.startsWith(QLatin1String("new file mode")))
            cur->status = u'A';
        else if (line.startsWith(QLatin1String("deleted file mode")))
            cur->status = u'D';
        else if (line.startsWith(QLatin1String("rename from ")))
            cur->oldPath = line.mid(12), cur->status = u'R';
        else if (line.startsWith(QLatin1String("rename to ")))
            cur->newPath = line.mid(10), cur->status = u'R';
        else if (line.startsWith(QLatin1String("Binary files ")) || line.startsWith(QLatin1String("GIT binary patch")))
            cur->binary = true;
        else if (line.startsWith(QLatin1String("-- ")) && git && !cur->hunks.isEmpty())
            continue; // format-patch signature
        cur->text += lines[i] + u'\n';
    }
    for (File &f : files) {
        if (f.status == u'A')
            f.oldPath.clear();
        else if (f.status == u'D')
            f.newPath.clear();
        else if (f.status == u'M' && f.oldPath != f.newPath && !f.oldPath.isEmpty() && !f.newPath.isEmpty())
            f.status = u'R';
    }
    files.removeIf([](const File &f) { return f.oldPath.isEmpty() && f.newPath.isEmpty(); });
    return files;
}

std::optional<QStringList> apply(const QStringList &base, const File &file)
{
    QStringList out;
    qsizetype next = 0;   // first base line not yet copied
    qsizetype offset = 0; // how far the file has moved from the patch's line numbers so far
    for (const Hunk &h : file.hunks) {
        QStringList oldLines, newLines;
        for (const QString &l : h.lines) {
            if (l.at(0) != u'+')
                oldLines << l.mid(1);
            if (l.at(0) != u'-')
                newLines << l.mid(1);
        }
        auto fits = [&](qsizetype at) {
            if (at < next || at + oldLines.size() > base.size())
                return false;
            for (qsizetype k = 0; k < oldLines.size(); ++k) {
                QString b = base[at + k];
                if (b.endsWith(u'\r'))
                    b.chop(1);
                if (b != oldLines[k])
                    return false;
            }
            return true;
        };
        // With no old lines, the start is the line before the insertion.
        const qsizetype want = (h.oldCount == 0 ? h.oldStart : h.oldStart - 1) + offset;
        qsizetype at = -1;
        for (qsizetype d = 0; at < 0 && d <= base.size(); ++d) {
            if (fits(want - d))
                at = want - d;
            else if (fits(want + d))
                at = want + d;
        }
        if (at < 0)
            return std::nullopt;
        for (; next < at; ++next)
            out << base[next];
        out += newLines;
        next = at + oldLines.size();
        offset = at - (h.oldCount == 0 ? h.oldStart : h.oldStart - 1);
    }
    for (; next < base.size(); ++next)
        out << base[next];
    return out;
}

} // namespace PatchFile
