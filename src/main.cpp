#include "GitRepo.h"
#include "MainWindow.h"
#include "Settings.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QMessageBox>
#include <QProcess>

#include <cstdio>

namespace {

int fail(const QString &message, bool gui)
{
    std::fprintf(stderr, "yadiff: %s\n", qPrintable(message));
    if (gui)
        QMessageBox::critical(nullptr, QStringLiteral("yadiff"), message);
    return 1;
}

QString shortName(const QString &name, const QString &sha)
{
    const QString s = sha.left(10);
    return name == sha || sha.startsWith(name) ? s : QStringLiteral("%1 (%2)").arg(name, s);
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("yadiff"));
    QApplication::setOrganizationName(QStringLiteral("yadiff"));
    QApplication::setApplicationVersion(QStringLiteral(YADIFF_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("yadiff"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Graphical diff viewer for Git.\n\n"
        "  yadiff                 staged and unstaged changes, compared against HEAD\n"
        "  yadiff REV             REV compared against the working tree\n"
        "  yadiff REV1 REV2       REV1 compared against REV2\n"
        "  yadiff REV1..REV2      same as REV1 REV2\n"
        "  yadiff REV1...REV2     merge-base of REV1 and REV2 compared against REV2"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption dirOpt({QStringLiteral("C"), QStringLiteral("repo")},
                                    QStringLiteral("Run as if started in <dir>."), QStringLiteral("dir"));
    const QCommandLineOption waitOpt({QStringLiteral("w"), QStringLiteral("wait")},
                                     QStringLiteral("Stay attached to the terminal until the window closes."));
    const QCommandLineOption inlineOpt(QStringLiteral("inline"), QStringLiteral("Start in inline view."));
    const QCommandLineOption sideOpt(QStringLiteral("side-by-side"), QStringLiteral("Start in side-by-side view."));
    parser.addOptions({dirOpt, waitOpt, inlineOpt, sideOpt});
    parser.addPositionalArgument(QStringLiteral("REV1"), QStringLiteral("Old revision (optional)."), QStringLiteral("[REV1"));
    parser.addPositionalArgument(QStringLiteral("REV2"), QStringLiteral("New revision (optional)."), QStringLiteral("[REV2]]"));
    parser.process(app);

    // A detached child has no terminal to report errors to, so it uses a dialog.
#ifdef Q_OS_WIN
    const bool gui = true; // a GUI-subsystem executable has no console to print to
#else
    const bool gui = qEnvironmentVariableIsSet("YADIFF_DETACHED_CHILD");
#endif
    const QStringList revs = parser.positionalArguments();
    if (revs.size() > 2)
        return fail(QStringLiteral("too many arguments (expected at most two revisions)"), gui);

    const QString dir = parser.isSet(dirOpt) ? parser.value(dirOpt) : QDir::currentPath();
    QString error;
    std::optional<GitRepo> repo = GitRepo::open(dir, &error);
    if (!repo)
        return fail(error.isEmpty() ? QStringLiteral("not a git repository: %1").arg(dir) : error, gui);

    Comparison cmp;
    cmp.unstagedAgainstHead = Settings::unstagedAgainstHead();
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
            return fail(error, gui);
        std::optional<QString> shaB;
        if (!b.isEmpty()) {
            shaB = repo->resolveCommit(b, &error);
            if (!shaB)
                return fail(error, gui);
        }
        cmp.rev1 = *shaA;
        cmp.rev1Name = shortName(a, *shaA);
        if (symmetric) {
            const auto base = repo->mergeBase(*shaA, *shaB, &error);
            if (!base)
                return fail(error, gui);
            cmp.rev1 = *base;
            cmp.rev1Name = QStringLiteral("merge-base(%1, %2)").arg(a, b);
        }
        if (shaB) {
            cmp.rev2 = *shaB;
            cmp.rev2Name = shortName(b, *shaB);
        }
    }

    // By default, hand the window to a detached copy of ourselves so the
    // terminal is free immediately (like `gitk &`). Arguments were validated above,
    // so errors are still reported in the terminal.
    if (!parser.isSet(waitOpt) && qEnvironmentVariableIsEmpty("YADIFF_NO_DETACH")) {
        QProcess child;
        child.setProgram(QCoreApplication::applicationFilePath());
        QStringList args = QCoreApplication::arguments().mid(1);
        args.prepend(QStringLiteral("--wait"));
        if (!parser.isSet(dirOpt))
            args << QStringLiteral("-C") << dir;
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
