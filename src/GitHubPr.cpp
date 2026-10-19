#include "GitHubPr.h"

#include "Log.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

namespace {

bool runProcess(const QString &program, const QStringList &args, const QString &workDir, QByteArray *out,
                QString *error)
{
    QProcess p;
    if (!workDir.isEmpty())
        p.setWorkingDirectory(workDir);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0")); // never hang on a password prompt
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    p.setProcessEnvironment(env);
    Log::ProcessTrace trace(program == QLatin1String("git") ? lcGit() : lcGitHub());
    p.start(program, args);
    if (!p.waitForStarted()) {
        trace.done(p);
        if (error)
            *error = QStringLiteral("Could not start %1.").arg(program);
        return false;
    }
    p.closeWriteChannel();
    trace.wait(p);
    const QByteArray stdOut = p.readAllStandardOutput(), stdErr = p.readAllStandardError();
    trace.done(p, stdOut.size(), stdErr);
    if (out)
        *out = stdOut;
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        if (error)
            *error = QString::fromUtf8(stdErr).trimmed();
        return false;
    }
    return true;
}

QString cacheRepoPath(const GitHubPr::Ref &ref)
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    return QDir(base).filePath(QStringLiteral("yadiff/github/%1/%2.git").arg(ref.owner.toLower(), ref.repo.toLower()));
}

// A bare, blob-less partial clone: commits and trees are fetched up front, file
// contents only when a diff needs them.
bool ensureCacheRepo(const GitHubPr::Ref &ref, const QString &path, QString *error)
{
    if (QDir(path).exists(QStringLiteral("HEAD")))
        return true;
    if (!QDir().mkpath(path)) {
        if (error)
            *error = QStringLiteral("Cannot create %1").arg(path);
        return false;
    }
    const QString url = QStringLiteral("https://github.com/%1/%2.git").arg(ref.owner, ref.repo);
    const QList<QStringList> steps{
        {QStringLiteral("init"), QStringLiteral("--bare"), QStringLiteral("-q")},
        {QStringLiteral("remote"), QStringLiteral("add"), QStringLiteral("origin"), url},
        {QStringLiteral("config"), QStringLiteral("core.repositoryformatversion"), QStringLiteral("1")},
        {QStringLiteral("config"), QStringLiteral("extensions.partialClone"), QStringLiteral("origin")},
        {QStringLiteral("config"), QStringLiteral("remote.origin.promisor"), QStringLiteral("true")},
        {QStringLiteral("config"), QStringLiteral("remote.origin.partialclonefilter"), QStringLiteral("blob:none")},
    };
    for (const QStringList &args : steps)
        if (!runProcess(QStringLiteral("git"), args, path, nullptr, error))
            return false;
    return true;
}

// Name of a remote of the repository at `dir` that points at the PR's repository.
QString matchingRemote(const QString &dir, const GitHubPr::Ref &ref)
{
    QByteArray out;
    if (!runProcess(QStringLiteral("git"), {QStringLiteral("remote"), QStringLiteral("-v")}, dir, &out, nullptr))
        return {};
    for (const QByteArray &line : out.split('\n')) {
        const QList<QByteArray> parts = line.simplified().split(' ');
        if (parts.size() >= 2 && GitHubPr::remoteMatches(QString::fromUtf8(parts[1]), ref.owner, ref.repo))
            return QString::fromUtf8(parts[0]);
    }
    return {};
}

} // namespace

namespace GitHubPr {

std::optional<Ref> parse(const QString &link)
{
    static const QRegularExpression urlRe(
        QStringLiteral(R"(^(?:https?://)?(?:www\.)?github\.com/([\w.-]+)/([\w.-]+)/pull/(\d+)(?:[/?#].*)?$)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression shortRe(QStringLiteral(R"(^([\w.-]+)/([\w.-]+)#(\d+)$)"));
    for (const auto *re : {&urlRe, &shortRe}) {
        const auto m = re->match(link.trimmed());
        if (m.hasMatch()) {
            Ref r{m.captured(1), m.captured(2), m.captured(3).toInt()};
            if (r.repo.endsWith(QLatin1String(".git")))
                r.repo.chop(4);
            if (r.number > 0)
                return r;
        }
    }
    return std::nullopt;
}

bool remoteMatches(const QString &url, const QString &owner, const QString &repo)
{
    // https://github.com/o/r(.git), git@github.com:o/r(.git), ssh://git@github.com/o/r(.git)
    static const QRegularExpression re(QStringLiteral(R"(github\.com[:/]+([\w.-]+)/([\w.-]+?)(?:\.git)?/?$)"),
                                       QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(url.trimmed());
    return m.hasMatch() && m.captured(1).compare(owner, Qt::CaseInsensitive) == 0
        && m.captured(2).compare(repo, Qt::CaseInsensitive) == 0;
}

std::optional<Resolved> resolve(const Ref &ref, const QString &localDir, QString *error,
                                const std::function<void(const QString &)> &progress)
{
    auto say = [&](const QString &s) {
        qCInfo(lcGitHub).noquote() << s;
        if (progress)
            progress(s);
    };
    qCInfo(lcGitHub).noquote() << QStringLiteral("resolving %1/%2#%3 (local folder %4)")
                                      .arg(ref.owner, ref.repo)
                                      .arg(ref.number)
                                      .arg(QDir::toNativeSeparators(localDir));
    const QString prUrl = QStringLiteral("https://github.com/%1/%2/pull/%3").arg(ref.owner, ref.repo).arg(ref.number);

    // 1. Metadata from gh (handles authentication for private repositories).
    QString title, baseRef, headRefName, headOid, baseOid;
    QByteArray json;
    QString ghError;
    if (runProcess(QStringLiteral("gh"),
                   {QStringLiteral("pr"), QStringLiteral("view"), prUrl, QStringLiteral("--json"),
                    QStringLiteral("title,baseRefName,headRefName,headRefOid,baseRefOid")},
                   QString(), &json, &ghError)) {
        const QJsonObject o = QJsonDocument::fromJson(json).object();
        title = o.value(QStringLiteral("title")).toString();
        baseRef = o.value(QStringLiteral("baseRefName")).toString();
        headRefName = o.value(QStringLiteral("headRefName")).toString();
        headOid = o.value(QStringLiteral("headRefOid")).toString();
        // For merged PRs this is where the base branch stood, which keeps their diff
        // meaningful after the base has moved past (or absorbed) the PR head.
        baseOid = o.value(QStringLiteral("baseRefOid")).toString();
        qCInfo(lcGitHub).noquote() << QStringLiteral("PR #%1: \"%2\", %3 (%4) into %5 (%6)")
                                          .arg(ref.number)
                                          .arg(title, headRefName, headOid, baseRef, baseOid);
    } else {
        say(QStringLiteral("gh unavailable (%1); using GitHub's pull refs").arg(ghError.section(u'\n', 0, 0)));
    }

    // 2. Pick the repository: the user's clone when it has a matching remote, else the cache.
    QString repoPath, remote;
    if (auto local = GitRepo::open(localDir, nullptr)) {
        remote = matchingRemote(local->root(), ref);
        if (!remote.isEmpty())
            repoPath = local->root();
    }
    if (repoPath.isEmpty()) {
        repoPath = cacheRepoPath(ref);
        remote = QStringLiteral("origin");
        qCInfo(lcGitHub).noquote() << "using yadiff's cache" << QDir::toNativeSeparators(repoPath);
        if (!ensureCacheRepo(ref, repoPath, error)) {
            qCWarning(lcGitHub).noquote() << "could not create the cache repository:" << (error ? *error : QString());
            return std::nullopt;
        }
    } else {
        qCInfo(lcGitHub).noquote() << "using the local clone" << QDir::toNativeSeparators(repoPath) << "remote" << remote;
    }
    auto repo = GitRepo::open(repoPath, error);
    if (!repo)
        return std::nullopt;

    // 3. Fetch the PR head and the base branch (without creating local branches).
    const QString headSpec = QStringLiteral("refs/pull/%1/head").arg(ref.number);
    QStringList fetch{QStringLiteral("fetch"), QStringLiteral("--no-tags"), QStringLiteral("--quiet")};
    if (repo->isBare())
        fetch << QStringLiteral("--filter=blob:none");
    fetch << remote << headSpec;
    if (!baseRef.isEmpty())
        fetch << QStringLiteral("refs/heads/") + baseRef;
    else
        fetch << QStringLiteral("refs/pull/%1/merge").arg(ref.number);
    say(QStringLiteral("Fetching %1#%2 from %3 ...").arg(ref.repo).arg(ref.number).arg(remote));
    QString fetchError;
    if (!runProcess(QStringLiteral("git"), fetch, repoPath, nullptr, &fetchError)) {
        if (error) {
            *error = QStringLiteral("git fetch failed: %1").arg(fetchError);
            if (baseRef.isEmpty())
                *error += QStringLiteral("\nWithout the gh CLI yadiff relies on GitHub's merge ref, which closed and "
                                         "conflicting PRs don't have. Install gh and run `gh auth login`.");
        }
        qCWarning(lcGitHub).noquote() << "fetch failed:" << fetchError;
        return std::nullopt;
    }

    // FETCH_HEAD lists the fetched refs in order: head first, then base (or merge).
    QByteArray fetchHeadPath;
    runProcess(QStringLiteral("git"), {QStringLiteral("rev-parse"), QStringLiteral("--git-path"), QStringLiteral("FETCH_HEAD")},
               repoPath, &fetchHeadPath, nullptr);
    QFile fh(QDir(repoPath).absoluteFilePath(QString::fromUtf8(fetchHeadPath).trimmed()));
    QStringList fetched;
    if (fh.open(QIODevice::ReadOnly))
        for (const QByteArray &line : fh.readAll().split('\n'))
            if (line.indexOf('\t') >= 40)
                fetched << QString::fromLatin1(line.left(line.indexOf('\t')));
    if (fetched.size() < 2) {
        qCWarning(lcGitHub).noquote() << "FETCH_HEAD" << fh.fileName() << "lists" << fetched.size() << "commits, expected 2";
        if (error)
            *error = QStringLiteral("Could not read the fetched PR refs.");
        return std::nullopt;
    }
    if (headOid.isEmpty())
        headOid = fetched[0];

    QString baseTip = fetched[1];
    if (!baseOid.isEmpty()) {
        if (!repo->hasCommit(baseOid))
            runProcess(QStringLiteral("git"), {QStringLiteral("fetch"), QStringLiteral("--no-tags"), QStringLiteral("--quiet"), remote, baseOid},
                       repoPath, nullptr, nullptr);
        if (repo->hasCommit(baseOid))
            baseTip = baseOid;
    }
    if (baseRef.isEmpty()) {
        // refs/pull/N/merge is a test merge whose first parent is the base branch tip.
        const auto parent = repo->resolveCommit(fetched[1] + QStringLiteral("^1"), error);
        if (!parent)
            return std::nullopt;
        baseTip = *parent;
    }

    // GitHub's "Files changed" shows the PR head against the merge base.
    const auto base = repo->mergeBase(baseTip, headOid, error);
    if (!base) {
        qCWarning(lcGitHub).noquote() << "no merge base of" << baseTip << "and" << headOid << ":" << (error ? *error : QString());
        return std::nullopt;
    }
    qCInfo(lcGitHub).noquote() << "PR" << ref.number << "resolved: merge base" << *base << "head" << headOid;

    Resolved r;
    r.ref = ref;
    r.repoPath = repoPath;
    r.baseOid = *base;
    r.headOid = headOid;
    r.baseName = QStringLiteral("%1 (merge base %2)").arg(baseRef.isEmpty() ? QStringLiteral("base") : baseRef,
                                                          base->left(10));
    r.headName = QStringLiteral("PR #%1 %2 (%3)")
                     .arg(ref.number)
                     .arg(headRefName.isEmpty() ? QStringLiteral("head") : headRefName, headOid.left(10));
    r.title = QStringLiteral("%1/%2#%3%4")
                  .arg(ref.owner, ref.repo)
                  .arg(ref.number)
                  .arg(title.isEmpty() ? QString() : QStringLiteral(": ") + title);
    return r;
}

} // namespace GitHubPr
