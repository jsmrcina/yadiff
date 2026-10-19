#include "AdoAuth.h"
#include "ComparisonArgs.h"
#include "AdoLoginDialog.h"
#include "AdoPr.h"
#include "Difftool.h"
#include "GitHubPr.h"
#include "GitRepo.h"
#include "MainWindow.h"
#include "Report.h"
#include "ReviewStore.h"
#include "Scratch.h"
#include "Session.h"
#include "ShellMenu.h"
#include "Settings.h"
#include "Theme.h"
#include "Updater.h"
#include "Usage.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QProcess>

#include <cstdio>

namespace {

bool g_gui = false; // report errors with a dialog (no terminal to print to)

int fail(const QString &message)
{
    std::fprintf(stderr, "yadiff: %s\n", qPrintable(message));
    if (g_gui)
        QMessageBox::critical(nullptr, QStringLiteral("yadiff"), message);
    return 1;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("yadiff"));
    QApplication::setOrganizationName(QStringLiteral("yadiff"));
    QApplication::setApplicationVersion(QStringLiteral(YADIFF_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("yadiff"));
#ifdef Q_OS_WIN
    Updater::cleanUpOldFiles(QApplication::applicationDirPath()); // left by the last update
#endif
    Theme::initialize();
    {
        QIcon icon;
        for (int size : {16, 24, 32, 48, 64, 128, 256})
            icon.addFile(QStringLiteral(":/icons/yadiff-%1.png").arg(size), QSize(size, size));
        QApplication::setWindowIcon(icon);
    }

    QCommandLineParser parser;
    parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions); // so "-ghpr" works
    parser.setApplicationDescription(yadiffUsageText());
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption dirOpt(QStringList{QStringLiteral("C"), QStringLiteral("repo")},
                                    QStringLiteral("Run as if started in <dir>."), QStringLiteral("dir"));
    const QCommandLineOption waitOpt(QStringList{QStringLiteral("w"), QStringLiteral("wait")},
                                     QStringLiteral("Stay attached to the terminal until the window closes."));
    const QCommandLineOption allOpt(QStringList{QStringLiteral("a"), QStringLiteral("all")},
                                    QStringLiteral("Show all changes against HEAD in one list, not staged/unstaged."));
    const QCommandLineOption prOpt(QStringList{QStringLiteral("ghpr"), QStringLiteral("pr")},
                                   QStringLiteral("Open a GitHub pull request."), QStringLiteral("url"));
    const QCommandLineOption adoOpt(QStringLiteral("ado"), QStringLiteral("Open an Azure DevOps pull request."),
                                    QStringLiteral("url"));
    const QCommandLineOption adoLoginOpt(QStringLiteral("ado-login"),
                                         QStringLiteral("Sign in to Azure DevOps (for the -ado link or the organisation URL "
                                                        "given as an argument) and exit."));
    const QCommandLineOption adoLogoutOpt(QStringLiteral("ado-logout"),
                                          QStringLiteral("Forget the saved Azure DevOps token for an organisation and exit."));
    const QCommandLineOption algoOpt(QStringLiteral("algorithm"),
                                     QStringLiteral("Diff algorithm: patience (default), histogram, myers, minimal, myers-lcs, heckel or structural."),
                                     QStringLiteral("name"));
    const QCommandLineOption inlineOpt(QStringLiteral("inline"), QStringLiteral("Start in inline view."));
    const QCommandLineOption sideOpt(QStringLiteral("side-by-side"), QStringLiteral("Start in side-by-side view."));
    const QCommandLineOption exportOpt(QStringLiteral("export"),
                                       QStringLiteral("Write an HTML report of the comparison to <file> and exit."),
                                       QStringLiteral("file"));
    const QCommandLineOption noIndexOpt(QStringLiteral("no-index"),
                                        QStringLiteral("Compare two directories or two files directly."));
    const QCommandLineOption registerOpt(QStringLiteral("register-difftool"),
                                         QStringLiteral("Register yadiff as the default git difftool and exit."));
    const QCommandLineOption unregisterOpt(QStringLiteral("unregister-difftool"),
                                           QStringLiteral("Remove yadiff's git difftool registration and exit."));
    const QCommandLineOption titleOpt(QStringLiteral("title"), QStringLiteral("Window title for the comparison."),
                                      QStringLiteral("text"));
    const QCommandLineOption patchOpt(QStringLiteral("patch"),
                                      QStringLiteral("Show a patch file (.patch, .diff), on the files it applies to."),
                                      QStringLiteral("file"));
    const QCommandLineOption sessionOpt(QStringLiteral("session"), QStringLiteral("Open a saved session (.yadiff file)."),
                                        QStringLiteral("file"));
    const QCommandLineOption selectLeftOpt(QStringLiteral("select-left"),
                                           QStringLiteral("Remember <path> as the left side for --compare-to-left, and exit."),
                                           QStringLiteral("path"));
    const QCommandLineOption compareLeftOpt(QStringLiteral("compare-to-left"),
                                            QStringLiteral("Compare the remembered left side with <path>."),
                                            QStringLiteral("path"));
    const QCommandLineOption shellMenuOpt(QStringLiteral("add-file-manager-menu"),
                                          QStringLiteral("Add yadiff to the file manager's right-click menu and exit."));
    const QCommandLineOption noShellMenuOpt(QStringLiteral("remove-file-manager-menu"),
                                            QStringLiteral("Remove yadiff from the file manager's right-click menu and exit."));
    parser.addOptions({dirOpt, waitOpt, allOpt, prOpt, adoOpt, adoLoginOpt, adoLogoutOpt, algoOpt, inlineOpt, sideOpt,
                       noIndexOpt, registerOpt, unregisterOpt, exportOpt, titleOpt, patchOpt, sessionOpt, selectLeftOpt,
                       compareLeftOpt, shellMenuOpt, noShellMenuOpt});
    parser.addPositionalArgument(QStringLiteral("args"), QStringLiteral("Revisions and/or paths (see above)."),
                                 QStringLiteral("[REV [REV]] [[--] PATH...]"));
    parser.process(app);

    // A detached child (or a Windows GUI-subsystem exe) has no terminal to report errors to.
#ifdef Q_OS_WIN
    g_gui = true;
#else
    g_gui = qEnvironmentVariableIsSet("YADIFF_DETACHED_CHILD");
#endif

    if (parser.isSet(registerOpt) || parser.isSet(unregisterOpt)) {
        QString summary, toolError;
        const bool ok = parser.isSet(registerOpt)
                            ? Difftool::registerTool(Difftool::preferredExecutable(), true, &summary, &toolError)
                            : Difftool::unregisterTool(&summary, &toolError);
        if (!ok)
            return fail(toolError);
        std::fprintf(stdout, "%s", qPrintable(summary));
        return 0;
    }

    if (parser.isSet(shellMenuOpt) || parser.isSet(noShellMenuOpt)) {
        QString summary, menuError;
        const bool ok = parser.isSet(shellMenuOpt) ? ShellMenu::install(Difftool::stableExecutable(), &summary, &menuError)
                                                   : ShellMenu::uninstall(&summary, &menuError);
        if (!ok)
            return fail(menuError);
        std::fprintf(stdout, "%s\n", qPrintable(summary));
        return 0;
    }
    if (parser.isSet(selectLeftOpt)) {
        // From the file manager: nothing to show yet, just remember the path.
        ShellMenu::setSelectedLeft(QDir(QDir::currentPath()).absoluteFilePath(parser.value(selectLeftOpt)));
        return 0;
    }
    Scratch::cleanUp();

    if (parser.isSet(adoLoginOpt) || parser.isSet(adoLogoutOpt)) {
        // The organisation comes from the -ado link, or a URL argument.
        QUrl org;
        if (parser.isSet(adoOpt)) {
            if (const auto ref = AdoPr::parse(parser.value(adoOpt)))
                org = ref->collectionUrl;
        } else if (!parser.positionalArguments().isEmpty()) {
            org = QUrl(parser.positionalArguments().first());
            if (const auto ref = AdoPr::parse(org.toString()))
                org = ref->collectionUrl;
        }
        if (parser.isSet(adoLogoutOpt)) {
            if (org.isEmpty() || org.host().isEmpty())
                return fail(QStringLiteral("--ado-logout needs the organisation URL (e.g. https://dev.azure.com/ORG)"));
            QString err;
            if (!AdoAuth::forget(org, &err))
                return fail(err);
            std::fprintf(stdout, "Forgot the saved token for %s\n",
                         qPrintable(AdoAuth::organizationUrl(org).toString(QUrl::StripTrailingSlash)));
            return 0;
        }
        AdoLoginDialog dlg(org, QString());
        if (dlg.exec() != QDialog::Accepted)
            return 1;
        std::fprintf(stdout, "Signed in to %s as %s%s\n",
                     qPrintable(AdoAuth::organizationUrl(dlg.collectionUrl()).toString(QUrl::StripTrailingSlash)),
                     qPrintable(dlg.signedInAs()), dlg.remembered() ? " (token saved)" : "");
        return 0;
    }

    // Everything after a literal "--" is a path, never a revision.
    const QStringList rawArgs = QCoreApplication::arguments();
    const qsizetype dashDash = rawArgs.indexOf(QStringLiteral("--"));
    const qsizetype forcedPaths = dashDash < 0 ? 0 : rawArgs.size() - dashDash - 1;
    const QStringList positional = parser.positionalArguments();

    const QString cwd = parser.isSet(dirOpt) ? QDir(parser.value(dirOpt)).absolutePath() : QDir::currentPath();
    QString error;

    ComparisonArgs::Request req;
    req.positional = positional;
    req.forcedPaths = forcedPaths;
    req.cwd = cwd;
    req.all = parser.isSet(allOpt);
    req.noIndex = parser.isSet(noIndexOpt);
    req.ghpr = parser.value(prOpt);
    req.ado = parser.value(adoOpt);
    req.algorithm = parser.value(algoOpt);
    req.title = parser.value(titleOpt);
    req.patch = parser.value(patchOpt);
    req.allowEmpty = !parser.isSet(exportOpt);
    std::optional<Session> session;
    if (parser.isSet(sessionOpt)) {
        session = Session::read(QDir(cwd).absoluteFilePath(parser.value(sessionOpt)), &error);
        if (!session)
            return fail(error);
        auto fromSession = ComparisonArgs::fromArguments(session->arguments, cwd, &error);
        if (!fromSession)
            return fail(error);
        req.positional = fromSession->positional;
        req.forcedPaths = fromSession->forcedPaths;
        req.cwd = fromSession->cwd;
        req.all = fromSession->all;
        req.noIndex = fromSession->noIndex;
        req.ghpr = fromSession->ghpr;
        req.ado = fromSession->ado;
        req.algorithm = fromSession->algorithm;
        req.title = fromSession->title;
        req.patch = fromSession->patch;
    }
    if (parser.isSet(compareLeftOpt)) {
        // From the file manager, so there is no terminal: report problems in a dialog.
        g_gui = true;
        const QString left = ShellMenu::selectedLeft();
        if (left.isEmpty() || !QFileInfo::exists(left))
            return fail(QStringLiteral("Choose \"Select as left side for yadiff\" on a file or folder first."));
        req.positional = {left, QDir(cwd).absoluteFilePath(parser.value(compareLeftOpt))};
        req.forcedPaths = 0;
        req.noIndex = true;
    }
    // The parent process resolves a PR and hands the result to the detached child.
    req.handed = QJsonDocument::fromJson(qgetenv("YADIFF_PR_RESOLVED")).object();
    req.progress = [](const QString &msg) { std::fprintf(stderr, "yadiff: %s\n", qPrintable(msg)); };
    auto resolved = ComparisonArgs::resolve(req, &error);
    if (!resolved)
        return fail(error);
    if (!resolved->handed.isEmpty())
        qputenv("YADIFF_PR_RESOLVED", QJsonDocument(resolved->handed).toJson(QJsonDocument::Compact));
    std::optional<GitRepo> repo = std::move(resolved->repo);
    Comparison cmp = resolved->cmp;

    if (parser.isSet(exportOpt)) {
        // Headless report: same comparison, written straight to a file.
        QVector<FileChange> changes = repo->listChanges(cmp, &error);
        if (!error.isEmpty())
            return fail(error);
        std::sort(changes.begin(), changes.end(), [](const FileChange &a, const FileChange &b) {
            if (a.group != b.group)
                return int(a.group) < int(b.group);
            return QString::compare(a.path, b.path, Qt::CaseInsensitive) < 0;
        });
        const QString root = cmp.mode == Comparison::Mode::Paths ? cmp.leftPath + u'\n' + cmp.rightPath : repo->root();
        ReviewStore reviews(root);
        auto input = Report::gather(*repo, cmp, changes, [&](const FileChange &fc) {
            return reviews.hasAny(fc.path) && reviews.isReviewed(fc.path, repo->fingerprint(fc, cmp));
        });
        input->title = QStringLiteral("yadiff \u2014 %1").arg(cmp.mode == Comparison::Mode::Paths ? QStringLiteral("compare")
                                                                                                   : QDir(repo->root()).dirName());
        input->description = cmp.title.isEmpty() ? (cmp.mode == Comparison::Mode::Revisions
                                                        ? QStringLiteral("%1 \u2194 %2").arg(cmp.rev1Name, cmp.rev2Name.isEmpty() ? QStringLiteral("working tree") : cmp.rev2Name)
                                                        : QStringLiteral("working tree changes"))
                                                 : cmp.title;
        input->leftName = cmp.rev1Name.isEmpty() ? QStringLiteral("old") : cmp.rev1Name;
        input->rightName = cmp.rev2Name.isEmpty() ? QStringLiteral("new") : cmp.rev2Name;
        QFile out(parser.value(exportOpt));
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return fail(out.errorString());
        out.write(Report::html(*input).toUtf8());
        std::fprintf(stdout, "Wrote %s (%d files)\n", qPrintable(QFileInfo(out).absoluteFilePath()), int(changes.size()));
        return 0;
    }

    // By default, hand the window to a detached copy of ourselves so the
    // terminal is free immediately (like `gitk &`). Arguments were validated above,
    // so errors are still reported in the terminal.
    if (!parser.isSet(waitOpt) && qEnvironmentVariableIsEmpty("YADIFF_NO_DETACH")) {
        QProcess child;
        child.setProgram(QCoreApplication::applicationFilePath());
        QStringList args = rawArgs.mid(1);
        args.prepend(QStringLiteral("--wait"));
        if (!parser.isSet(dirOpt))
            args = QStringList{QStringLiteral("-C"), cwd} + args; // options go before a possible "--"
        child.setArguments(args);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("YADIFF_DETACHED_CHILD"), QStringLiteral("1"));
        child.setProcessEnvironment(env);
        child.setStandardOutputFile(QProcess::nullDevice());
        child.setStandardErrorFile(QProcess::nullDevice());
        if (child.startDetached())
            return 0;
        // Fall through and run in the foreground if detaching failed.
    }

    if (parser.isSet(inlineOpt))
        Settings::setSideBySide(false);
    else if (parser.isSet(sideOpt))
        Settings::setSideBySide(true);

    MainWindow w(std::move(*repo), cmp);
    w.show();
    if (session)
        w.applySession(*session);
    if (const QString grab = qEnvironmentVariable("YADIFF_GRAB"); !grab.isEmpty()) {
        w.resize(1300, 700);
        QApplication::processEvents();
        w.grabAll(grab);
        return 0;
    }
    return app.exec();
}
