#include "Difftool.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace {

bool git(const QStringList &args, QString *out, QString *error)
{
    QProcess p;
    p.start(QStringLiteral("git"), args);
    if (!p.waitForStarted()) {
        if (error)
            *error = QStringLiteral("Could not start git.");
        return false;
    }
    p.waitForFinished(-1);
    if (out)
        *out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    // `git config --unset` exits 5 when the key is absent; treat that as success.
    if (p.exitCode() != 0 && !(args.contains(QStringLiteral("--unset")) && p.exitCode() == 5)) {
        if (error)
            *error = QString::fromUtf8(p.readAllStandardError()).trimmed();
        return false;
    }
    return true;
}

QString shellQuote(const QString &s)
{
    // git runs the command through sh (on Windows too, via Git's bundled shell).
    QString q = s;
    q.replace(u'\'', QStringLiteral("'\\''"));
    return u'\'' + q + u'\'';
}

} // namespace

namespace Difftool {

QString preferredExecutable()
{
    const QString self = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("yadiff"));
    if (!onPath.isEmpty() && QFileInfo(onPath).canonicalFilePath() == self)
        return QStringLiteral("yadiff");
    return self.isEmpty() ? QCoreApplication::applicationFilePath() : self;
}

QString command(const QString &executable)
{
    const QString exe = executable == QLatin1String("yadiff") ? executable : shellQuote(executable);
    return exe + QStringLiteral(" --wait --no-index \"$LOCAL\" \"$REMOTE\"");
}

bool registerTool(const QString &executable, bool makeDefault, QString *summary, QString *error)
{
    const QString cmd = command(executable);
    if (!git({QStringLiteral("config"), QStringLiteral("--global"), QStringLiteral("difftool.yadiff.cmd"), cmd}, nullptr, error))
        return false;
    QString text = QStringLiteral("git config --global difftool.yadiff.cmd '%1'\n").arg(cmd);
    if (makeDefault) {
        if (!git({QStringLiteral("config"), QStringLiteral("--global"), QStringLiteral("diff.tool"), QStringLiteral("yadiff")},
                 nullptr, error)
            || !git({QStringLiteral("config"), QStringLiteral("--global"), QStringLiteral("difftool.prompt"),
                     QStringLiteral("false")},
                    nullptr, error))
            return false;
        text += QStringLiteral("git config --global diff.tool yadiff\ngit config --global difftool.prompt false\n");
    }
    if (summary)
        *summary = text;
    return true;
}

bool unregisterTool(QString *summary, QString *error)
{
    if (!git({QStringLiteral("config"), QStringLiteral("--global"), QStringLiteral("--unset"), QStringLiteral("difftool.yadiff.cmd")},
             nullptr, error))
        return false;
    QString text = QStringLiteral("git config --global --unset difftool.yadiff.cmd\n");
    QString tool;
    git({QStringLiteral("config"), QStringLiteral("--global"), QStringLiteral("diff.tool")}, &tool, nullptr);
    if (tool == QLatin1String("yadiff")) {
        if (!git({QStringLiteral("config"), QStringLiteral("--global"), QStringLiteral("--unset"), QStringLiteral("diff.tool")},
                 nullptr, error))
            return false;
        text += QStringLiteral("git config --global --unset diff.tool\n");
        // registerTool() set this together with diff.tool, so remove it together too.
        git({QStringLiteral("config"), QStringLiteral("--global"), QStringLiteral("--unset"), QStringLiteral("difftool.prompt")},
            nullptr, nullptr);
        text += QStringLiteral("git config --global --unset difftool.prompt\n");
    }
    if (summary)
        *summary = text;
    return true;
}

bool isRegistered()
{
    QString out;
    return git({QStringLiteral("config"), QStringLiteral("--global"), QStringLiteral("difftool.yadiff.cmd")}, &out, nullptr)
        && !out.isEmpty();
}

} // namespace Difftool
