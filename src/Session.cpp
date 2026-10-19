#include "Session.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

std::optional<Session> Session::read(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("%1: %2").arg(path, f.errorString());
        return std::nullopt;
    }
    QJsonParseError parseError;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll(), &parseError).object();
    if (parseError.error != QJsonParseError::NoError || o.value(QStringLiteral("yadiffSession")).toInt() != 1) {
        if (error)
            *error = QStringLiteral("%1 is not a yadiff session file").arg(path);
        return std::nullopt;
    }
    Session s;
    for (const QJsonValue &v : o.value(QStringLiteral("arguments")).toArray())
        s.arguments << v.toString();
    s.title = o.value(QStringLiteral("title")).toString();
    s.currentFile = o.value(QStringLiteral("currentFile")).toString();
    s.sideBySide = o.contains(QStringLiteral("sideBySide")) ? int(o.value(QStringLiteral("sideBySide")).toBool()) : -1;
    if (const QJsonObject opt = o.value(QStringLiteral("options")).toObject(); !opt.isEmpty()) {
        s.hasOptions = true;
        const QString algo = opt.value(QStringLiteral("algorithm")).toString();
        for (DiffAlgorithm a : allAlgorithms())
            if (algorithmName(a) == algo)
                s.options.algorithm = a;
        auto flag = [&](const char *key, bool &field) {
            if (opt.contains(QLatin1String(key)))
                field = opt.value(QLatin1String(key)).toBool();
        };
        flag("ignoreSpaceChange", s.options.ignoreSpaceChange);
        flag("ignoreAllSpace", s.options.ignoreAllSpace);
        flag("ignoreBlankLines", s.options.ignoreBlankLines);
        flag("ignoreCrAtEol", s.options.ignoreCrAtEol);
        flag("ignoreTrim", s.options.ignoreTrim);
        flag("ignoreCase", s.options.ignoreCase);
        flag("ignoreComments", s.options.ignoreComments);
        flag("detectMoves", s.options.detectMoves);
        for (const QJsonValue &v : opt.value(QStringLiteral("unimportantPatterns")).toArray())
            s.options.unimportantPatterns << v.toString();
    }
    return s;
}

bool Session::write(const QString &path, QString *error) const
{
    QJsonObject o{{QStringLiteral("yadiffSession"), 1}, {QStringLiteral("arguments"), QJsonArray::fromStringList(arguments)}};
    if (!title.isEmpty())
        o.insert(QStringLiteral("title"), title);
    if (!currentFile.isEmpty())
        o.insert(QStringLiteral("currentFile"), currentFile);
    if (sideBySide >= 0)
        o.insert(QStringLiteral("sideBySide"), sideBySide == 1);
    if (hasOptions) {
        const DiffOptions &d = options;
        o.insert(QStringLiteral("options"),
                 QJsonObject{{QStringLiteral("algorithm"), algorithmName(d.algorithm)},
                             {QStringLiteral("ignoreSpaceChange"), d.ignoreSpaceChange},
                             {QStringLiteral("ignoreAllSpace"), d.ignoreAllSpace},
                             {QStringLiteral("ignoreBlankLines"), d.ignoreBlankLines},
                             {QStringLiteral("ignoreCrAtEol"), d.ignoreCrAtEol},
                             {QStringLiteral("ignoreTrim"), d.ignoreTrim},
                             {QStringLiteral("ignoreCase"), d.ignoreCase},
                             {QStringLiteral("ignoreComments"), d.ignoreComments},
                             {QStringLiteral("detectMoves"), d.detectMoves},
                             {QStringLiteral("unimportantPatterns"), QJsonArray::fromStringList(d.unimportantPatterns)}});
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(QJsonDocument(o).toJson()) < 0) {
        if (error)
            *error = QStringLiteral("%1: %2").arg(path, f.errorString());
        return false;
    }
    return true;
}
