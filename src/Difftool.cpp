#include "Difftool.h"

#include "Log.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace {

bool git(const QStringList &args, QString *out, QString *error)
{
    QProcess p;
    Log::ProcessTrace trace(lcGit());
    p.start(QStringLiteral("git"), args);
    if (!p.waitForStarted()) {
        trace.done(p);
        if (error)
            *error = QStringLiteral("Could not start git.");
        return false;
    }
    p.waitForFinished(-1);
    const QByteArray stdOut = p.readAllStandardOutput(), stdErr = p.readAllStandardError();
    if (out)
        *out = QString::fromUtf8(stdOut).trimmed();
    // `git config --unset` exits 5 when the key is absent; treat that as success.
    const bool ok = p.exitCode() == 0 || (args.contains(QStringLiteral("--unset")) && p.exitCode() == 5);
    trace.done(p, stdOut.size(), stdErr, ok);
    if (!ok) {
        if (error)
            *error = QString::fromUtf8(stdErr).trimmed();
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

QString stableExecutable()
{
    // An absolute path that keeps working: the yadiff on PATH (install.sh's link, which
    // follows updates) when it is this program, else this program itself.
    const QString self = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("yadiff"));
    if (!onPath.isEmpty() && QFileInfo(onPath).canonicalFilePath() == self)
        return QFileInfo(onPath).absoluteFilePath();
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

QString mergeCommand(const QString &executable)
{
    const QString exe = executable == QLatin1String("yadiff") ? executable : shellQuote(executable);
    return exe + QStringLiteral(" --merge \"$BASE\" \"$LOCAL\" \"$REMOTE\" \"$MERGED\"");
}

bool registerMergeTool(const QString &executable, bool makeDefault, QString *summary, QString *error)
{
    const QString cmd = mergeCommand(executable);
    auto set = [&](const char *key, const QString &value) {
        return git({QStringLiteral("config"), QStringLiteral("--global"), QLatin1String(key), value}, nullptr, error);
    };
    // trustExitCode: yadiff exits 0 only when the result was saved with every conflict resolved.
    if (!set("mergetool.yadiff.cmd", cmd) || !set("mergetool.yadiff.trustExitCode", QStringLiteral("true")))
        return false;
    QString text = QStringLiteral("git config --global mergetool.yadiff.cmd '%1'\n"
                                  "git config --global mergetool.yadiff.trustExitCode true\n")
                       .arg(cmd);
    if (makeDefault) {
        if (!set("merge.tool", QStringLiteral("yadiff")) || !set("mergetool.prompt", QStringLiteral("false")))
            return false;
        text += QStringLiteral("git config --global merge.tool yadiff\ngit config --global mergetool.prompt false\n");
    }
    if (summary)
        *summary = text;
    return true;
}

bool unregisterMergeTool(QString *summary, QString *error)
{
    auto unset = [&](const char *key) {
        return git({QStringLiteral("config"), QStringLiteral("--global"), QStringLiteral("--unset"), QLatin1String(key)},
                   nullptr, nullptr);
    };
    if (!unset("mergetool.yadiff.cmd")) {
        if (error)
            *error = QStringLiteral("yadiff is not registered as a git mergetool");
        return false;
    }
    unset("mergetool.yadiff.trustExitCode");
    QString text = QStringLiteral("git config --global --unset mergetool.yadiff.cmd\n"
                                  "git config --global --unset mergetool.yadiff.trustExitCode\n");
    QString tool;
    git({QStringLiteral("config"), QStringLiteral("--global"), QStringLiteral("merge.tool")}, &tool, nullptr);
    if (tool == QLatin1String("yadiff")) {
        unset("merge.tool");
        unset("mergetool.prompt");
        text += QStringLiteral("git config --global --unset merge.tool\ngit config --global --unset mergetool.prompt\n");
    }
    if (summary)
        *summary = text;
    return true;
}

bool isMergeToolRegistered()
{
    QString out;
    return git({QStringLiteral("config"), QStringLiteral("--global"), QStringLiteral("mergetool.yadiff.cmd")}, &out, nullptr)
        && !out.isEmpty();
}

} // namespace Difftool
