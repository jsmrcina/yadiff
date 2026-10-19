#include "Comments.h"

#include <QFileInfo>
#include <QHash>

namespace Comments {

namespace {

Syntax make(QStringList line, QVector<std::pair<QString, QString>> block, QString quotes = QStringLiteral("\"'"))
{
    return Syntax{std::move(line), std::move(block), std::move(quotes)};
}

const QHash<QString, Syntax> &bySuffix()
{
    static const QHash<QString, Syntax> table = [] {
        QHash<QString, Syntax> t;
        const Syntax c = make({QStringLiteral("//")}, {{QStringLiteral("/*"), QStringLiteral("*/")}});
        for (const char *s : {"c", "h", "cc", "cpp", "cxx", "hpp", "hh", "hxx", "ino", "m", "mm", "cs", "java", "js",
                              "jsx", "mjs", "cjs", "ts", "tsx", "go", "swift", "kt", "kts", "scala", "dart", "groovy",
                              "gradle", "proto", "glsl", "hlsl", "vert", "frag", "zig", "v", "sv", "json5", "jsonc"})
            t.insert(QLatin1String(s), c);
        // Rust: ' starts lifetimes as well as chars, so only " delimits strings.
        t.insert(QStringLiteral("rs"), make({QStringLiteral("//")}, {{QStringLiteral("/*"), QStringLiteral("*/")}},
                                            QStringLiteral("\"")));
        t.insert(QStringLiteral("php"), make({QStringLiteral("//"), QStringLiteral("#")},
                                             {{QStringLiteral("/*"), QStringLiteral("*/")}}));
        const Syntax css = make({}, {{QStringLiteral("/*"), QStringLiteral("*/")}});
        t.insert(QStringLiteral("css"), css);
        t.insert(QStringLiteral("scss"), c);
        t.insert(QStringLiteral("less"), c);
        const Syntax hash = make({QStringLiteral("#")}, {});
        for (const char *s : {"py", "pyw", "sh", "bash", "zsh", "fish", "rb", "pl", "pm", "yaml", "yml", "toml", "cmake",
                              "r", "conf", "cfg", "mk", "make", "nix", "tf", "dockerfile", "gitignore", "env", "properties",
                              "jl", "nim", "cr", "ex", "exs", "coffee", "tcl", "awk"})
            t.insert(QLatin1String(s), hash);
        t.insert(QStringLiteral("ps1"), make({QStringLiteral("#")}, {{QStringLiteral("<#"), QStringLiteral("#>")}}));
        t.insert(QStringLiteral("psm1"), t.value(QStringLiteral("ps1")));
        t.insert(QStringLiteral("ini"), make({QStringLiteral(";"), QStringLiteral("#")}, {}));
        const Syntax dash = make({QStringLiteral("--")}, {{QStringLiteral("/*"), QStringLiteral("*/")}});
        t.insert(QStringLiteral("sql"), dash);
        t.insert(QStringLiteral("lua"), make({QStringLiteral("--")}, {{QStringLiteral("--[["), QStringLiteral("]]")}}));
        t.insert(QStringLiteral("hs"), make({QStringLiteral("--")}, {{QStringLiteral("{-"), QStringLiteral("-}")}},
                                            QStringLiteral("\"")));
        t.insert(QStringLiteral("elm"), t.value(QStringLiteral("hs")));
        t.insert(QStringLiteral("ada"), make({QStringLiteral("--")}, {}, QStringLiteral("\"")));
        const Syntax markup = make({}, {{QStringLiteral("<!--"), QStringLiteral("-->")}}, QString());
        for (const char *s : {"html", "htm", "xhtml", "xml", "svg", "xaml", "xsd", "xsl", "xslt", "plist", "csproj",
                              "vcxproj", "props", "targets", "resx", "ui", "qrc", "md", "markdown", "vue"})
            t.insert(QLatin1String(s), markup);
        const Syntax semi = make({QStringLiteral(";")}, {}, QStringLiteral("\""));
        for (const char *s : {"lisp", "el", "clj", "cljs", "scm", "rkt", "asm", "s"})
            t.insert(QLatin1String(s), semi);
        t.insert(QStringLiteral("vb"), make({QStringLiteral("'")}, {}, QStringLiteral("\"")));
        t.insert(QStringLiteral("vbs"), t.value(QStringLiteral("vb")));
        t.insert(QStringLiteral("bas"), t.value(QStringLiteral("vb")));
        const Syntax percent = make({QStringLiteral("%")}, {}, QString());
        t.insert(QStringLiteral("tex"), percent);
        t.insert(QStringLiteral("sty"), percent);
        t.insert(QStringLiteral("erl"), make({QStringLiteral("%")}, {}, QStringLiteral("\"")));
        t.insert(QStringLiteral("bat"), make({QStringLiteral("::"), QStringLiteral("REM "), QStringLiteral("rem ")}, {},
                                             QStringLiteral("\"")));
        t.insert(QStringLiteral("cmd"), t.value(QStringLiteral("bat")));
        t.insert(QStringLiteral("f90"), make({QStringLiteral("!")}, {}));
        t.insert(QStringLiteral("ml"), make({}, {{QStringLiteral("(*"), QStringLiteral("*)")}}, QStringLiteral("\"")));
        t.insert(QStringLiteral("pas"), make({QStringLiteral("//")},
                                             {{QStringLiteral("{"), QStringLiteral("}")},
                                              {QStringLiteral("(*"), QStringLiteral("*)")}},
                                             QStringLiteral("'")));
        return t;
    }();
    return table;
}

} // namespace

Syntax forFile(const QString &fileName)
{
    const QFileInfo fi(fileName);
    const QString name = fi.fileName().toLower();
    if (name == QLatin1String("cmakelists.txt") || name == QLatin1String("makefile") || name == QLatin1String("dockerfile")
        || name.startsWith(QLatin1String(".bash")) || name == QLatin1String(".zshrc") || name == QLatin1String(".profile")
        || name == QLatin1String(".gitignore") || name == QLatin1String(".gitattributes"))
        return bySuffix().value(QStringLiteral("sh"));
    return bySuffix().value(fi.suffix().toLower());
}

QStringList strip(const QStringList &lines, const Syntax &syntax)
{
    if (!syntax.isValid())
        return lines;
    QStringList out;
    out.reserve(lines.size());
    int openBlock = -1; // index into syntax.block while inside a block comment
    for (const QString &line : lines) {
        QString kept;
        const qsizetype n = line.size();
        qsizetype i = 0;
        while (i < n) {
            if (openBlock >= 0) {
                const QString &close = syntax.block[openBlock].second;
                const qsizetype end = line.indexOf(close, i);
                if (end < 0) {
                    i = n;
                    break;
                }
                i = end + close.size();
                openBlock = -1;
                continue;
            }
            const QChar c = line.at(i);
            if (syntax.quotes.contains(c)) {
                // A string literal: copy it through, escapes included, up to its closing quote.
                qsizetype j = i + 1;
                while (j < n && line.at(j) != c)
                    j += line.at(j) == u'\\' ? 2 : 1;
                j = std::min(n, j + 1);
                kept += QStringView(line).mid(i, j - i);
                i = j;
                continue;
            }
            bool comment = false;
            // Block openers first: "--[[" must win over "--".
            for (int b = 0; b < syntax.block.size(); ++b) {
                if (QStringView(line).mid(i).startsWith(syntax.block[b].first)) {
                    openBlock = b;
                    i += syntax.block[b].first.size();
                    comment = true;
                    break;
                }
            }
            if (comment)
                continue;
            for (const QString &l : syntax.line) {
                if (QStringView(line).mid(i).startsWith(l)) {
                    i = n;
                    comment = true;
                    break;
                }
            }
            if (comment)
                break;
            kept += c;
            ++i;
        }
        out << kept;
    }
    return out;
}

} // namespace Comments
