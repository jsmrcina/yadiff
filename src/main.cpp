#include "Difftool.h"
#include "GitHubPr.h"
#include "GitRepo.h"
#include "MainWindow.h"
#include "Report.h"
#include "ReviewStore.h"
#include "Settings.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
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

QString shortName(const QString &name, const QString &sha)
{
    const QString s = sha.left(10);
    return name == sha || sha.startsWith(name) ? s : QStringLiteral("%1 (%2)").arg(name, s);
}

// Repository-relative form of a path given on the command line (relative to `cwd`).
// Returns nullopt when it lies outside the repository.
std::optional<QString> repoRelative(const QString &arg, const QString &cwd, const QString &root)
{
    QFileInfo fi(QDir(cwd).absoluteFilePath(arg));
    QString abs = fi.exists() ? fi.canonicalFilePath() : QString();
    if (abs.isEmpty()) {
        // Deleted files: canonicalise the parent directory instead.
        const QString parent = QFileInfo(fi.absolutePath()).canonicalFilePath();
        abs = (parent.isEmpty() ? fi.absolutePath() : parent) + u'/' + fi.fileName();
    }
    const QString canonRoot = QFileInfo(root).canonicalFilePath();
    QString rel = QDir(canonRoot.isEmpty() ? root : canonRoot).relativeFilePath(abs);
    rel = QDir::cleanPath(rel);
    if (rel == QLatin1String(".."))
        return std::nullopt;
    if (rel.startsWith(QLatin1String("../")))
        return std::nullopt;
    if (rel == QLatin1String("."))
        return QString();
    return rel;
}

// "A..B" or "A...B" where each non-empty end is a commit (so "../file" is not a range).
bool isRange(const GitRepo &repo, const QString &arg)
{
    const QString sep = arg.contains(QLatin1String("...")) ? QStringLiteral("...") : QStringLiteral("..");
    if (!arg.contains(sep))
        return false;
    const QString a = arg.section(sep, 0, 0);
    const QString b = arg.section(sep, 1);
    if (a.isEmpty() && b.isEmpty())
        return false;
    return (a.isEmpty() || repo.resolveCommit(a, nullptr)) && (b.isEmpty() || repo.resolveCommit(b, nullptr));
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("yadiff"));
    QApplication::setOrganizationName(QStringLiteral("yadiff"));
    QApplication::setApplicationVersion(QStringLiteral(YADIFF_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("yadiff"));
    {
        QIcon icon;
        for (int size : {16, 24, 32, 48, 64, 128, 256})
            icon.addFile(QStringLiteral(":/icons/yadiff-%1.png").arg(size), QSize(size, size));
        QApplication::setWindowIcon(icon);
    }

    QCommandLineParser parser;
    parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions); // so "-ghpr" works
    parser.setApplicationDescription(QStringLiteral(
        "Graphical diff viewer for Git.\n\n"
        "  yadiff                      staged and unstaged changes, compared against HEAD\n"
        "  yadiff PATH...              just these files or directories, compared against HEAD\n"
        "  yadiff REV                  REV compared against the working tree\n"
        "  yadiff REV1 REV2            REV1 compared against REV2\n"
        "  yadiff REV1..REV2           same as REV1 REV2\n"
        "  yadiff REV1...REV2          merge-base of REV1 and REV2 compared against REV2\n"
        "  yadiff [REV...] -- PATH...  limit any of the above to PATHs\n"
        "  yadiff -ghpr URL            a GitHub pull request (https://github.com/OWNER/REPO/pull/N)\n"
        "  yadiff --no-index A B       two directories or two files, in or outside a repository\n"
        "                             (automatic when A and B are outside the current repository)\n"
        "  yadiff --register-difftool  make yadiff a git difftool (git difftool -t yadiff, git difftool -d)"));
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
    const QCommandLineOption algoOpt(QStringLiteral("algorithm"),
                                     QStringLiteral("Diff algorithm: patience (default), histogram, myers or minimal."),
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
    parser.addOptions({dirOpt, waitOpt, allOpt, prOpt, algoOpt, inlineOpt, sideOpt, noIndexOpt, registerOpt, unregisterOpt,
                       exportOpt});
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

    // Everything after a literal "--" is a path, never a revision.
    const QStringList rawArgs = QCoreApplication::arguments();
    const qsizetype dashDash = rawArgs.indexOf(QStringLiteral("--"));
    const qsizetype forcedPaths = dashDash < 0 ? 0 : rawArgs.size() - dashDash - 1;
    const QStringList positional = parser.positionalArguments();

    const QString cwd = parser.isSet(dirOpt) ? QDir(parser.value(dirOpt)).absolutePath() : QDir::currentPath();
    QString error;

    Comparison cmp;
    cmp.unstagedAgainstHead = Settings::unstagedAgainstHead();
    cmp.allChanges = Settings::allChanges() || parser.isSet(allOpt);
    cmp.options = Settings::diffOptions();
    if (parser.isSet(algoOpt)) {
        bool found = false;
        for (DiffAlgorithm a : {DiffAlgorithm::Patience, DiffAlgorithm::Histogram, DiffAlgorithm::Myers, DiffAlgorithm::Minimal})
            if (algorithmName(a) == parser.value(algoOpt).toLower()) {
                cmp.options.algorithm = a;
                found = true;
            }
        if (!found)
            return fail(QStringLiteral("unknown diff algorithm '%1'").arg(parser.value(algoOpt)));
    }

    std::optional<GitRepo> repo;
    QStringList revs, pathArgs;

    // Two directories or two files compared directly: explicitly with --no-index, or when
    // they are not both inside the current repository (e.g. git difftool's temp files).
    auto isNullPath = [](const QString &p) {
        return p == QLatin1String("/dev/null") || p.compare(QLatin1String("nul"), Qt::CaseInsensitive) == 0;
    };
    bool pathsMode = false;
    if (positional.size() == 2 && forcedPaths == 0 && !parser.isSet(prOpt)) {
        const QFileInfo a(QDir(cwd).absoluteFilePath(positional[0]));
        const QFileInfo b(QDir(cwd).absoluteFilePath(positional[1]));
        const bool nullA = isNullPath(positional[0]), nullB = isNullPath(positional[1]);
        const bool files = (a.isFile() || nullA) && (b.isFile() || nullB) && !(nullA && nullB);
        const bool dirs = a.isDir() && b.isDir();
        if (parser.isSet(noIndexOpt) && !files && !dirs)
            return fail(QStringLiteral("--no-index needs two existing directories or two existing files"));
        if (files || dirs) {
            pathsMode = parser.isSet(noIndexOpt);
            if (!pathsMode) {
                auto here = GitRepo::open(cwd, nullptr);
                pathsMode = !here || (!nullA && !repoRelative(positional[0], cwd, here->root()))
                         || (!nullB && !repoRelative(positional[1], cwd, here->root()));
            }
        }
        if (pathsMode) {
            cmp.mode = Comparison::Mode::Paths;
            cmp.pathsAreFiles = files;
            cmp.leftPath = nullA ? QStringLiteral("/dev/null") : QDir::cleanPath(a.absoluteFilePath());
            cmp.rightPath = nullB ? QStringLiteral("/dev/null") : QDir::cleanPath(b.absoluteFilePath());
            repo = GitRepo::forPaths(cwd);
        }
    } else if (parser.isSet(noIndexOpt)) {
        return fail(QStringLiteral("--no-index needs exactly two paths"));
    }

    if (pathsMode) {
        // Nothing else to resolve.
    } else if (parser.isSet(prOpt)) {
        const auto ref = GitHubPr::parse(parser.value(prOpt));
        if (!ref)
            return fail(QStringLiteral("not a GitHub pull request link: %1").arg(parser.value(prOpt)));
        // The parent process resolves the PR and hands the result to the detached child.
        GitHubPr::Resolved pr;
        const QStringList handedOver = qEnvironmentVariable("YADIFF_PR_RESOLVED").split(u'\n');
        if (handedOver.size() == 9) {
            pr.ref = {handedOver[0], handedOver[1], handedOver[2].toInt()};
            pr.repoPath = handedOver[3];
            pr.baseOid = handedOver[4];
            pr.headOid = handedOver[5];
            pr.baseName = handedOver[6];
            pr.headName = handedOver[7];
            pr.title = handedOver[8];
        } else {
            auto resolved = GitHubPr::resolve(*ref, cwd, &error, [](const QString &msg) {
                std::fprintf(stderr, "yadiff: %s\n", qPrintable(msg));
            });
            if (!resolved)
                return fail(error);
            pr = *resolved;
        }
        repo = GitRepo::open(pr.repoPath, &error);
        if (!repo)
            return fail(error);
        cmp.mode = Comparison::Mode::Revisions;
        cmp.rev1 = pr.baseOid;
        cmp.rev2 = pr.headOid;
        cmp.rev1Name = pr.baseName;
        cmp.rev2Name = pr.headName;
        cmp.title = pr.title;
        cmp.prOwner = pr.ref.owner;
        cmp.prRepo = pr.ref.repo;
        cmp.prNumber = pr.ref.number;
        qputenv("YADIFF_PR_RESOLVED", QStringList{pr.ref.owner, pr.ref.repo, QString::number(pr.ref.number), pr.repoPath,
                                                 pr.baseOid, pr.headOid, pr.baseName, pr.headName, pr.title}
                                         .join(u'\n')
                                         .toUtf8());
        pathArgs = positional; // anything else limits the PR to those paths
    } else {
        repo = GitRepo::open(cwd, &error);
        if (!repo)
            return fail(error.isEmpty() ? QStringLiteral("not a git repository: %1").arg(cwd) : error);

        // Classify arguments: leading ones that resolve as revisions are revisions,
        // the rest (and everything after "--") are paths.
        const qsizetype unforced = positional.size() - forcedPaths;
        for (qsizetype i = 0; i < positional.size(); ++i) {
            const QString &arg = positional[i];
            if (i >= unforced || !pathArgs.isEmpty()) {
                pathArgs << arg;
                continue;
            }
            if (revs.size() < 2 && (isRange(*repo, arg) || repo->resolveCommit(arg, nullptr))) {
                revs << arg;
                continue;
            }
            const auto rel = repoRelative(arg, cwd, repo->root());
            const bool exists = QFileInfo::exists(QDir(cwd).absoluteFilePath(arg));
            if (!rel && exists)
                return fail(QStringLiteral("'%1' is outside the repository").arg(arg));
            if (rel && (exists || repo->knowsPath(*rel))) {
                pathArgs << arg;
                continue;
            }
            return fail(QStringLiteral("'%1' is neither a revision nor a path in the repository").arg(arg));
        }
        if (revs.size() > 2)
            return fail(QStringLiteral("too many revisions (expected at most two)"));
    }

    for (const QString &arg : std::as_const(pathArgs)) {
        if (pathsMode)
            break;
        const auto rel = repoRelative(arg, cwd, repo->root());
        if (!rel)
            return fail(QStringLiteral("'%1' is outside the repository").arg(arg));
        if (!rel->isEmpty())
            cmp.paths << *rel;
    }

    if (!revs.isEmpty()) {
        cmp.mode = Comparison::Mode::Revisions;
        QString a = revs.value(0), b = revs.value(1);
        bool symmetric = false;
        if (revs.size() == 1 && a.contains(QLatin1String("..."))) {
            symmetric = true;
            b = a.section(QStringLiteral("..."), 1);
            a = a.section(QStringLiteral("..."), 0, 0);
        } else if (revs.size() == 1 && a.contains(QLatin1String(".."))) {
            b = a.section(QStringLiteral(".."), 1);
            a = a.section(QStringLiteral(".."), 0, 0);
            if (b.isEmpty())
                b = QStringLiteral("HEAD");
        }
        if (a.isEmpty())
            a = QStringLiteral("HEAD");
        if (symmetric && b.isEmpty())
            b = QStringLiteral("HEAD");

        const auto shaA = repo->resolveCommit(a, &error);
        if (!shaA)
            return fail(error);
        std::optional<QString> shaB;
        if (!b.isEmpty()) {
            shaB = repo->resolveCommit(b, &error);
            if (!shaB)
                return fail(error);
        }
        cmp.rev1 = *shaA;
        cmp.rev1Name = shortName(a, *shaA);
        if (symmetric) {
            const auto base = repo->mergeBase(*shaA, *shaB, &error);
            if (!base)
                return fail(error);
            cmp.rev1 = *base;
            cmp.rev1Name = QStringLiteral("merge-base(%1, %2)").arg(a, b);
        }
        if (shaB) {
            cmp.rev2 = *shaB;
            cmp.rev2Name = shortName(b, *shaB);
        }
    } else if (cmp.mode == Comparison::Mode::WorkingTree && !cmp.paths.isEmpty()) {
        // `yadiff foo.txt`: the file's current state against HEAD.
        cmp.allChanges = true;
    }

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
    if (const QString grab = qEnvironmentVariable("YADIFF_GRAB"); !grab.isEmpty()) {
        w.resize(1300, 700);
        QApplication::processEvents();
        w.grabAll(grab);
        return 0;
    }
    return app.exec();
}
