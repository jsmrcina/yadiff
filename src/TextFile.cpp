#include "TextFile.h"

#include "Log.h"

#include <QFile>

namespace TextFile {

std::optional<Content> read(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return std::nullopt;
    }
    const QByteArray data = f.readAll();
    Content c;
    c.eol = data.contains("\r\n") ? QByteArray("\r\n") : QByteArray("\n");
    c.trailingNewline = data.isEmpty() || data.endsWith('\n');
    QString text = QString::fromUtf8(data);
    if (c.eol == "\r\n")
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    c.lines = text.split(u'\n');
    if (c.trailingNewline && !c.lines.isEmpty())
        c.lines.removeLast(); // the empty string after the final newline
    return c;
}

bool write(const QString &path, const Content &c, QString *error)
{
    QByteArray data;
    for (qsizetype i = 0; i < c.lines.size(); ++i) {
        data += c.lines[i].toUtf8();
        if (i + 1 < c.lines.size() || c.trailingNewline)
            data += c.eol;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(data) != data.size()) {
        qCWarning(lcFile).noquote() << "could not write" << path << ":" << f.errorString();
        if (error)
            *error = f.errorString();
        return false;
    }
    qCInfo(lcFile).noquote() << "saved" << path << "(" << c.lines.size() << "lines," << data.size() << "bytes)";
    return true;
}

std::optional<QStringList> applyChunk(const QStringList &target, const FileDiff &d, int chunk, bool toNewSide,
                                      QString *error)
{
    if (chunk < 0 || chunk >= d.chunks.size()) {
        if (error)
            *error = QStringLiteral("No such change.");
        return std::nullopt;
    }
    const LineKind targetKind = toNewSide ? LineKind::Added : LineKind::Removed;
    const LineKind sourceKind = toNewSide ? LineKind::Removed : LineKind::Added;
    auto lineNo = [&](const DiffLine &l) { return toNewSide ? l.newNo : l.oldNo; };

    QStringList replace, expected;
    int first = 0; // 1-based first target line being replaced, or insertion point
    for (int i = d.chunks[chunk].first; i <= d.chunks[chunk].last; ++i) {
        const DiffLine &l = d.lines[i];
        if (l.kind == sourceKind)
            replace << l.text;
        else if (l.kind == targetKind) {
            if (expected.isEmpty())
                first = lineNo(l);
            expected << l.text;
        }
    }
    if (expected.isEmpty()) {
        // Pure insertion: after the closest preceding line on the target side.
        int before = 0;
        for (int i = d.chunks[chunk].first - 1; i >= 0 && !before; --i)
            before = lineNo(d.lines[i]);
        first = before + 1;
    }

    const qsizetype start = first - 1;
    if (start < 0 || start + expected.size() > target.size()) {
        if (error)
            *error = QStringLiteral("The file changed on disk. Reload and try again.");
        return std::nullopt;
    }
    for (qsizetype k = 0; k < expected.size(); ++k) {
        QString t = target[start + k];
        if (t.endsWith(u'\r'))
            t.chop(1);
        if (t != expected[k]) {
            if (error)
                *error = QStringLiteral("The file changed on disk. Reload and try again.");
            return std::nullopt;
        }
    }
    QStringList out = target.mid(0, start);
    out += replace;
    out += target.mid(start + expected.size());
    return out;
}

} // namespace TextFile
