#include "Report.h"

#include <QDateTime>
#include <QFileInfo>

namespace {

QString esc(const QString &s)
{
    return s.toHtmlEscaped();
}

// Line text with intra-line highlights wrapped in <mark>.
QString codeHtml(const DiffLine &l)
{
    if (l.highlights.isEmpty())
        return esc(l.text);
    QString out;
    int pos = 0;
    for (const Span &sp : l.highlights) {
        out += esc(l.text.mid(pos, sp.start - pos));
        out += QStringLiteral("<mark>") + esc(l.text.mid(sp.start, sp.length)) + QStringLiteral("</mark>");
        pos = sp.start + sp.length;
    }
    return out + esc(l.text.mid(pos));
}

QString statusClass(QChar s)
{
    switch (s.unicode()) {
    case 'A':
    case '?': return QStringLiteral("s-add");
    case 'D': return QStringLiteral("s-del");
    case 'R':
    case 'C': return QStringLiteral("s-ren");
    default: return QStringLiteral("s-mod");
    }
}

const char *kStyle = R"CSS(
:root{--bg:#fff;--fg:#1f2328;--muted:#656d76;--line:#d0d7de;--add:#dafbe1;--addm:#aceebb;--del:#ffebe9;--delm:#ffcecb;
--mov:#e6e9ff;--fold:#f6f8fa;--card:#f6f8fa}
@media (prefers-color-scheme:dark){:root{--bg:#0d1117;--fg:#e6edf3;--muted:#8d96a0;--line:#30363d;--add:#12261e;--addm:#1f6f3b;
--del:#25171c;--delm:#7d2a32;--mov:#1d2340;--fold:#161b22;--card:#161b22}}
*{box-sizing:border-box}body{margin:0;padding:24px 16px;background:var(--bg);color:var(--fg);
font:14px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif}main{max-width:1200px;margin:0 auto}
h1{font-size:22px;margin:0 0 4px}h2{font-size:15px;margin:0;padding:8px 12px;background:var(--card);
border-bottom:1px solid var(--line);display:flex;gap:10px;align-items:center;flex-wrap:wrap;position:sticky;top:0}
.meta{color:var(--muted);margin:0 0 20px}table{border-collapse:collapse;width:100%}
.summary td,.summary th{padding:4px 8px;border-bottom:1px solid var(--line);text-align:left}
.summary td.num{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}
.badge{display:inline-block;min-width:20px;padding:0 5px;border-radius:4px;color:#fff;font-weight:700;font-size:12px;text-align:center}
.s-add{background:#2ea043}.s-del{background:#d13b3b}.s-ren{background:#3b7bd1}.s-mod{background:#d98e04}
.plus{color:#1a7f37}.minus{color:#cf222e}.reviewed{color:#1a7f37;font-size:12px}
section.file{border:1px solid var(--line);border-radius:6px;margin:24px 0;overflow:hidden}
.diff{font:12px/1.45 ui-monospace,"Cascadia Mono","Source Code Pro",Consolas,monospace;overflow-x:auto;display:block}
.diff td{padding:0 8px;white-space:pre;vertical-align:top}.diff td.ln{color:var(--muted);text-align:right;user-select:none;
width:1%;border-right:1px solid var(--line)}.diff td.sg{width:1%;color:var(--muted);user-select:none;padding:0 4px}
tr.add td{background:var(--add)}tr.del td{background:var(--del)}tr.mov td{background:var(--mov)}
tr.add mark{background:var(--addm);color:inherit}tr.del mark{background:var(--delm);color:inherit}
tr.fold td{background:var(--fold);color:var(--muted);font-style:italic}.note{padding:12px;color:var(--muted)}
.images{display:flex;gap:16px;padding:12px;flex-wrap:wrap}.images figure{margin:0;flex:1;min-width:200px}
.images img{max-width:100%;background:repeating-conic-gradient(#ccc 0 25%,#f2f2f2 0 50%) 0 0/16px 16px;border:1px solid var(--line)}
.images figcaption{color:var(--muted);font-size:12px}a{color:inherit}
)CSS";

} // namespace

namespace Report {

QString imageMimeType(const QString &path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == QLatin1String("png"))
        return QStringLiteral("image/png");
    if (ext == QLatin1String("jpg") || ext == QLatin1String("jpeg"))
        return QStringLiteral("image/jpeg");
    if (ext == QLatin1String("gif"))
        return QStringLiteral("image/gif");
    if (ext == QLatin1String("webp"))
        return QStringLiteral("image/webp");
    if (ext == QLatin1String("bmp"))
        return QStringLiteral("image/bmp");
    if (ext == QLatin1String("ico"))
        return QStringLiteral("image/x-icon");
    return {};
}

QString html(const Input &in)
{
    int added = 0, removed = 0, reviewed = 0;
    for (const File &f : in.files) {
        added += std::max(0, f.change.added);
        removed += std::max(0, f.change.removed);
        reviewed += f.reviewed;
    }

    QString out;
    out.reserve(1 << 20);
    out += QStringLiteral("<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
                          "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                          "<title>%1</title><style>%2</style></head><body><main>\n")
               .arg(esc(in.title), QString::fromUtf8(kStyle));
    out += QStringLiteral("<h1>%1</h1>\n<p class=\"meta\">%2<br>%3 files &middot; <span class=\"plus\">+%4</span> "
                          "<span class=\"minus\">&minus;%5</span> &middot; %6 of %3 reviewed &middot; generated %7</p>\n")
               .arg(esc(in.title), esc(in.description), QString::number(in.files.size()), QString::number(added),
                    QString::number(removed), QString::number(reviewed),
                    esc(QDateTime::currentDateTime().toString(Qt::ISODate)));

    // Summary table.
    out += QStringLiteral("<table class=\"summary\"><tr><th></th><th>File</th><th class=\"num\">Lines</th><th>Reviewed</th></tr>\n");
    for (int i = 0; i < in.files.size(); ++i) {
        const File &f = in.files[i];
        const FileChange &c = f.change;
        QString name = esc(c.path);
        if (c.oldPath != c.path)
            name += QStringLiteral(" <span class=\"meta\">(from %1)</span>").arg(esc(c.oldPath));
        const QString lines = c.added >= 0 ? QStringLiteral("<span class=\"plus\">+%1</span> <span class=\"minus\">&minus;%2</span>")
                                                 .arg(c.added)
                                                 .arg(c.removed)
                                           : QStringLiteral("binary");
        out += QStringLiteral("<tr><td><span class=\"badge %1\">%2</span></td><td><a href=\"#f%3\">%4</a></td>"
                              "<td class=\"num\">%5</td><td>%6</td></tr>\n")
                   .arg(statusClass(c.status), esc(QString(c.status)))
                   .arg(i)
                   .arg(name, lines, f.reviewed ? QStringLiteral("&#10003;") : QString());
    }
    out += QStringLiteral("</table>\n");

    // One section per file.
    for (int i = 0; i < in.files.size(); ++i) {
        const File &f = in.files[i];
        const FileChange &c = f.change;
        const FileDiff &d = f.diff;
        out += QStringLiteral("<section class=\"file\" id=\"f%1\"><h2><span class=\"badge %2\">%3</span>%4")
                   .arg(i)
                   .arg(statusClass(c.status), esc(QString(c.status)), esc(c.path));
        if (c.added >= 0)
            out += QStringLiteral(" <small><span class=\"plus\">+%1</span> <span class=\"minus\">&minus;%2</span></small>")
                       .arg(c.added)
                       .arg(c.removed);
        if (f.reviewed)
            out += QStringLiteral(" <span class=\"reviewed\">&#10003; reviewed</span>");
        out += QStringLiteral("</h2>\n");

        if (f.oldImage || f.newImage) {
            out += QStringLiteral("<div class=\"images\">");
            auto figure = [&](const std::optional<QByteArray> &img, const QString &path, const QString &caption) {
                out += QStringLiteral("<figure>");
                if (img)
                    out += QStringLiteral("<img alt=\"%1\" src=\"data:%2;base64,%3\">")
                               .arg(esc(caption), imageMimeType(path), QString::fromLatin1(img->toBase64()));
                else
                    out += QStringLiteral("<div class=\"note\">(none)</div>");
                out += QStringLiteral("<figcaption>%1</figcaption></figure>").arg(esc(caption));
            };
            figure(f.oldImage, c.oldPath, in.leftName);
            figure(f.newImage, c.path, in.rightName);
            out += QStringLiteral("</div></section>\n");
            continue;
        }
        if (d.lines.isEmpty()) {
            out += QStringLiteral("<div class=\"note\">%1</div></section>\n")
                       .arg(esc(d.message.isEmpty() ? QStringLiteral("No content differences") : d.message));
            continue;
        }

        // Keep `context` unchanged lines around changes; fold the rest.
        const int n = int(d.lines.size());
        QVector<bool> keep(n, false);
        for (const Chunk &ch : d.chunks)
            for (int k = std::max(0, ch.first - in.context); k <= std::min(n - 1, ch.last + in.context); ++k)
                keep[k] = true;
        out += QStringLiteral("<table class=\"diff\">");
        for (int k = 0; k < n;) {
            if (!keep[k]) {
                int e = k;
                while (e + 1 < n && !keep[e + 1])
                    ++e;
                out += QStringLiteral("<tr class=\"fold\"><td class=\"ln\"></td><td class=\"ln\"></td><td class=\"sg\"></td>"
                                      "<td>&#8943; %1 unchanged lines</td></tr>")
                           .arg(e - k + 1);
                k = e + 1;
                continue;
            }
            const DiffLine &l = d.lines[k];
            const bool moved = l.moveId >= 0;
            QString cls, sign;
            switch (l.kind) {
            case LineKind::Equal: break;
            case LineKind::Added:
                cls = moved ? QStringLiteral("mov") : QStringLiteral("add");
                sign = QStringLiteral("+");
                break;
            case LineKind::Removed:
                cls = moved ? QStringLiteral("mov") : QStringLiteral("del");
                sign = QStringLiteral("&minus;");
                break;
            }
            out += QStringLiteral("<tr%1%2><td class=\"ln\">%3</td><td class=\"ln\">%4</td><td class=\"sg\">%5</td><td>%6</td></tr>")
                       .arg(cls.isEmpty() ? QString() : QStringLiteral(" class=\"%1\"").arg(cls),
                            moved ? QStringLiteral(" title=\"moved\"") : QString(),
                            l.oldNo > 0 ? QString::number(l.oldNo) : QString(),
                            l.newNo > 0 ? QString::number(l.newNo) : QString(), sign, codeHtml(l));
            ++k;
        }
        out += QStringLiteral("</table></section>\n");
    }
    out += QStringLiteral("</main></body></html>\n");
    return out;
}

} // namespace Report

namespace Report {

std::optional<Input> gather(const GitRepo &repo, const Comparison &cmp, const QVector<FileChange> &changes,
                            const std::function<bool(const FileChange &)> &isReviewed,
                            const std::function<bool(int, int)> &progress)
{
    Input in;
    for (int i = 0; i < changes.size(); ++i) {
        if (progress && !progress(i, int(changes.size())))
            return std::nullopt;
        File f;
        f.change = changes[i];
        QString error;
        f.diff = repo.loadDiff(f.change, cmp, &error);
        if (!error.isEmpty())
            f.diff.message = QStringLiteral("git failed: ") + error;
        f.reviewed = isReviewed && isReviewed(f.change);
        const bool image = !imageMimeType(f.change.path).isEmpty() || !imageMimeType(f.change.oldPath).isEmpty();
        if (image && f.diff.binary) {
            f.oldImage = repo.fileContent(f.change, cmp, false, 8 * 1024 * 1024);
            f.newImage = repo.fileContent(f.change, cmp, true, 8 * 1024 * 1024);
        }
        in.files.push_back(std::move(f));
    }
    if (progress)
        progress(int(changes.size()), int(changes.size()));
    return in;
}

} // namespace Report
