#include "ComparisonArgs.h"

#include "AdoAuth.h"
#include "AdoLoginDialog.h"
#include "AdoPr.h"
#include "GitHubPr.h"
#include "Log.h"
#include "Settings.h"

#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>

namespace ComparisonArgs {

namespace {

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

std::optional<AdoAuth::Credential> signInWithDialog(const QUrl &collection, const QString &reason)
{
    AdoLoginDialog dlg(collection, reason);
    if (dlg.exec() != QDialog::Accepted || !dlg.credential())
        return std::nullopt;
    const AdoAuth::Credential cred = *dlg.credential();
    if (!dlg.remembered()) // let the window reach the PR without a keyring entry
        qputenv(cred.kind == AdoAuth::Credential::Kind::Bearer ? "YADIFF_ADO_TOKEN" : "YADIFF_ADO_PAT", cred.secret.toUtf8());
    return cred;
}

std::optional<Result> resolve(const Request &req, QString *errorOut)
{
    auto failed = [errorOut](const QString &message) -> std::optional<Result> {
        if (errorOut)
            *errorOut = message;
        return std::nullopt;
    };
    const QStringList &positional = req.positional;
    const qsizetype forcedPaths = req.forcedPaths;
    const QString &cwd = req.cwd;
    QString error;
    QJsonObject handed;

    Comparison cmp;
    cmp.unstagedAgainstHead = Settings::unstagedAgainstHead();
    cmp.allChanges = Settings::allChanges() || req.all;
    cmp.options = Settings::diffOptions();
    if (!req.algorithm.isEmpty()) {
        bool found = false;
        for (DiffAlgorithm a : allAlgorithms())
            if (algorithmName(a) == req.algorithm.toLower()) {
                cmp.options.algorithm = a;
                found = true;
            }
        if (!found)
            return failed(QStringLiteral("unknown diff algorithm '%1'").arg(req.algorithm));
    }

    std::optional<GitRepo> repo;
    QStringList revs, pathArgs;

    // Two directories or two files compared directly: explicitly with --no-index, or when
    // they are not both inside the current repository (e.g. git difftool's temp files).
    auto isNullPath = [](const QString &p) {
        return p == QLatin1String("/dev/null") || p.compare(QLatin1String("nul"), Qt::CaseInsensitive) == 0;
    };
    bool pathsMode = false;
    if (!req.patch.isEmpty()) {
        // A patch file: its paths are relative to the repository it is in, or to cwd.
        const QString patch = QDir(cwd).absoluteFilePath(req.patch);
        if (!QFileInfo(patch).isFile())
            return failed(QStringLiteral("no such patch file: %1").arg(req.patch));
        auto here = GitRepo::open(cwd, nullptr);
        cmp.mode = Comparison::Mode::Patch;
        cmp.patchPath = QDir::cleanPath(patch);
        cmp.patchBase = here ? here->root() : QDir::cleanPath(cwd);
        cmp.title = req.title.isEmpty() ? QStringLiteral("patch %1").arg(QFileInfo(patch).fileName()) : req.title;
        return Result{here ? std::move(*here) : GitRepo::forPaths(cwd), cmp};
    }
    if (positional.size() == 2 && forcedPaths == 0 && req.ghpr.isEmpty() && req.ado.isEmpty()) {
        const QFileInfo a(QDir(cwd).absoluteFilePath(positional[0]));
        const QFileInfo b(QDir(cwd).absoluteFilePath(positional[1]));
        const bool nullA = isNullPath(positional[0]), nullB = isNullPath(positional[1]);
        const bool files = (a.isFile() || nullA) && (b.isFile() || nullB) && !(nullA && nullB);
        const bool dirs = a.isDir() && b.isDir();
        if (req.noIndex && !files && !dirs)
            return failed(QStringLiteral("--no-index needs two existing directories or two existing files"));
        if (files || dirs) {
            pathsMode = req.noIndex;
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
    } else if (req.noIndex) {
        return failed(QStringLiteral("--no-index needs exactly two paths"));
    }

    if (pathsMode) {
        // Nothing else to resolve.
    } else if (!req.ghpr.isEmpty() || !req.ado.isEmpty()) {
        const bool isAdo = !req.ado.isEmpty();
        const QString link = isAdo ? req.ado : req.ghpr;
        {
            auto progress = [&req](const QString &msg) {
                if (req.progress)
                    req.progress(msg);
            };
            if (!isAdo) {
                const auto ref = GitHubPr::parse(link);
                if (!ref)
                    return failed(QStringLiteral("not a GitHub pull request link: %1").arg(link));
                auto pr = GitHubPr::resolve(*ref, cwd, &error, progress);
                if (!pr)
                    return failed(error);
                handed = {{QStringLiteral("host"), QStringLiteral("github")}, {QStringLiteral("owner"), pr->ref.owner},
                          {QStringLiteral("repo"), pr->ref.repo},           {QStringLiteral("number"), pr->ref.number},
                          {QStringLiteral("repoPath"), pr->repoPath},       {QStringLiteral("base"), pr->baseOid},
                          {QStringLiteral("head"), pr->headOid},            {QStringLiteral("baseName"), pr->baseName},
                          {QStringLiteral("headName"), pr->headName},       {QStringLiteral("title"), pr->title}};
            } else {
                const auto ref = AdoPr::parse(link);
                if (!ref)
                    return failed(QStringLiteral("not an Azure DevOps pull request link: %1\n"
                                               "Expected https://dev.azure.com/ORG/PROJECT/_git/REPO/pullrequest/ID")
                                    .arg(link));
                // Find a credential; sign in when there is none or it is rejected.
                QString diag;
                auto cred = AdoAuth::find(ref->collectionUrl, &diag);
                std::optional<AdoPr::Resolved> pr;
                for (int attempt = 0; attempt < 2 && !pr; ++attempt) {
                    if (!cred) {
                        qCInfo(lcAdo).noquote() << "asking the user to sign in:" << (attempt == 0 ? diag : error);
                        cred = req.signIn ? req.signIn(ref->collectionUrl, attempt == 0 ? diag : error)
                                          : signInWithDialog(ref->collectionUrl, attempt == 0 ? diag : error);
                        if (!cred)
                            return failed(attempt == 0 ? diag + QStringLiteral(" Sign-in was cancelled.") : error);
                    }
                    int status = 0;
                    pr = AdoPr::resolve(*ref, *cred, cwd, &error, &status, progress);
                    if (!pr && AdoAuth::authProblem(status).isEmpty())
                        return failed(error); // not a sign-in problem: retrying won't help
                    if (!pr) {
                        qCInfo(lcAdo).noquote() << "HTTP" << status << "with the credential from" << cred->source
                                                << (attempt == 0 ? "; signing in again" : "; giving up");
                        error = QStringLiteral("%1 (credential from %2)").arg(AdoAuth::authProblem(status), cred->source);
                        cred.reset();
                    }
                }
                if (!pr)
                    return failed(error);
                handed = {{QStringLiteral("host"), QStringLiteral("ado")},
                          {QStringLiteral("collection"), ref->collectionUrl.toString()},
                          {QStringLiteral("project"), ref->project},
                          {QStringLiteral("repo"), ref->repo},
                          {QStringLiteral("number"), ref->id},
                          {QStringLiteral("webUrl"), ref->webUrl.toString()},
                          {QStringLiteral("repoPath"), pr->repoPath},
                          {QStringLiteral("base"), pr->baseOid},
                          {QStringLiteral("head"), pr->headOid},
                          {QStringLiteral("baseName"), pr->baseName},
                          {QStringLiteral("headName"), pr->headName},
                          {QStringLiteral("title"), pr->title}};
            }
        }
        auto str = [&](const char *k) { return handed.value(QLatin1String(k)).toString(); };
        repo = GitRepo::open(str("repoPath"), &error);
        if (!repo)
            return failed(error);
        cmp.mode = Comparison::Mode::Revisions;
        cmp.rev1 = str("base");
        cmp.rev2 = str("head");
        cmp.rev1Name = str("baseName");
        cmp.rev2Name = str("headName");
        cmp.title = str("title");
        cmp.prHost = str("host");
        cmp.prOwner = str("owner");
        cmp.prRepo = str("repo");
        cmp.prCollectionUrl = str("collection");
        cmp.prProject = str("project");
        cmp.prWebUrl = str("webUrl");
        cmp.prNumber = handed.value(QStringLiteral("number")).toInt();
        pathArgs = positional; // anything else limits the PR to those paths
    } else {
        repo = GitRepo::open(cwd, &error);
        if (!repo && positional.isEmpty() && req.allowEmpty) {
            // Plain `yadiff` outside a repository: an empty window, ready for File > New comparison.
            repo = GitRepo::forPaths(cwd);
            cmp.empty = true;
        } else if (!repo) {
            return failed(error.isEmpty() ? QStringLiteral("not a git repository: %1").arg(cwd) : error);
        }

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
                return failed(QStringLiteral("'%1' is outside the repository").arg(arg));
            if (rel && (exists || repo->knowsPath(*rel))) {
                pathArgs << arg;
                continue;
            }
            return failed(QStringLiteral("'%1' is neither a revision nor a path in the repository").arg(arg));
        }
        if (revs.size() > 2)
            return failed(QStringLiteral("too many revisions (expected at most two)"));
    }

    for (const QString &arg : std::as_const(pathArgs)) {
        if (pathsMode)
            break;
        const auto rel = repoRelative(arg, cwd, repo->root());
        if (!rel)
            return failed(QStringLiteral("'%1' is outside the repository").arg(arg));
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
            return failed(error);
        std::optional<QString> shaB;
        if (!b.isEmpty()) {
            shaB = repo->resolveCommit(b, &error);
            if (!shaB)
                return failed(error);
        }
        cmp.rev1 = *shaA;
        cmp.rev1Name = shortName(a, *shaA);
        if (symmetric) {
            const auto base = repo->mergeBase(*shaA, *shaB, &error);
            if (!base)
                return failed(error);
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
    if (!req.title.isEmpty())
        cmp.title = req.title;

    return Result{std::move(*repo), cmp};
}

std::optional<Request> fromArguments(const QStringList &args, const QString &cwd, QString *error)
{
    // The subset of yadiff's command line that File > New comparison produces.
    Request req;
    req.cwd = cwd;
    bool afterDashDash = false;
    for (qsizetype i = 0; i < args.size(); ++i) {
        const QString &a = args[i];
        auto value = [&]() -> std::optional<QString> {
            if (i + 1 >= args.size())
                return std::nullopt;
            return args[++i];
        };
        if (afterDashDash) {
            req.positional << a;
            ++req.forcedPaths;
        } else if (a == QLatin1String("--")) {
            afterDashDash = true;
        } else if (a == QLatin1String("-C") || a == QLatin1String("--repo")) {
            const auto v = value();
            if (!v)
                break;
            req.cwd = QDir(*v).absolutePath();
        } else if (a == QLatin1String("--no-index")) {
            req.noIndex = true;
        } else if (a == QLatin1String("-a") || a == QLatin1String("--all")) {
            req.all = true;
        } else if (a == QLatin1String("-ghpr") || a == QLatin1String("--ghpr") || a == QLatin1String("-pr")
                   || a == QLatin1String("--pr")) {
            if (const auto v = value())
                req.ghpr = *v;
        } else if (a == QLatin1String("-ado") || a == QLatin1String("--ado")) {
            if (const auto v = value())
                req.ado = *v;
        } else if (a == QLatin1String("--algorithm")) {
            if (const auto v = value())
                req.algorithm = *v;
        } else if (a == QLatin1String("--title")) {
            if (const auto v = value())
                req.title = *v;
        } else if (a == QLatin1String("--patch")) {
            if (const auto v = value())
                req.patch = *v;
        } else if (a.startsWith(u'-') && a.size() > 1) {
            if (error)
                *error = QStringLiteral("unsupported option '%1'").arg(a);
            return std::nullopt;
        } else {
            req.positional << a;
        }
    }
    return req;
}

QStringList toArguments(const Request &req)
{
    QStringList args{QStringLiteral("-C"), req.cwd};
    if (req.all)
        args << QStringLiteral("-a");
    if (req.noIndex)
        args << QStringLiteral("--no-index");
    if (!req.ghpr.isEmpty())
        args << QStringLiteral("-ghpr") << req.ghpr;
    if (!req.ado.isEmpty())
        args << QStringLiteral("-ado") << req.ado;
    if (!req.algorithm.isEmpty())
        args << QStringLiteral("--algorithm") << req.algorithm;
    if (!req.title.isEmpty())
        args << QStringLiteral("--title") << req.title;
    if (!req.patch.isEmpty())
        args << QStringLiteral("--patch") << req.patch;
    const qsizetype unforced = req.positional.size() - req.forcedPaths;
    args += req.positional.mid(0, unforced);
    if (req.forcedPaths > 0)
        args << QStringLiteral("--") << req.positional.mid(unforced);
    return args;
}

} // namespace ComparisonArgs
