#include "AdoPr.h"

#include "GitRepo.h"
#include "Http.h"
#include "Log.h"

#include <QDir>
#include <QThreadPool>
#include <QDirIterator>
#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentMap>

#include <atomic>
#include <tuple>

namespace {

QString seg(const QString &s)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(s));
}

QUrl apiUrl(const AdoPr::Ref &ref, const QString &tail)
{
    QString base = ref.collectionUrl.toString(QUrl::StripTrailingSlash) + u'/' + seg(ref.project)
                 + QStringLiteral("/_apis/git/repositories/") + seg(ref.repo) + QStringLiteral("/pullRequests/")
                 + QString::number(ref.id) + tail;
    base += (tail.contains(u'?') ? u'&' : u'?') + QStringLiteral("api-version=7.1");
    return QUrl(base, QUrl::StrictMode);
}

// Repository-level API (not under the pull request), e.g. "/blobs/ID".
QUrl repoApiUrl(const AdoPr::Ref &ref, const QString &tail)
{
    QString base = ref.collectionUrl.toString(QUrl::StripTrailingSlash) + u'/' + seg(ref.project)
                 + QStringLiteral("/_apis/git/repositories/") + seg(ref.repo) + tail;
    base += (tail.contains(u'?') ? u'&' : u'?') + QStringLiteral("api-version=7.1");
    return QUrl(base, QUrl::StrictMode);
}

Http::Response call(const QByteArray &method, const QUrl &url, const AdoAuth::Credential &cred, const QByteArray &body,
                    QString *error, int *httpStatus = nullptr, const QByteArray &accept = "application/json")
{
    if (!Http::safeForCredentials(url)) {
        qCWarning(lcAdo).noquote() << "refusing to send credentials to" << Log::url(url);
        if (error)
            *error = QStringLiteral("Refusing to send credentials over an unencrypted connection to %1.").arg(url.host());
        return {};
    }
    Http::Response r = Http::request(method, url, {{"Authorization", cred.authorizationHeader()}, {"Accept", accept}}, body);
    if (httpStatus)
        *httpStatus = r.status;
    if (!r.error.isEmpty()) {
        if (error)
            *error = r.error;
    } else if (!r.ok() || (r.status == 203)) {
        if (error) {
            *error = AdoAuth::authProblem(r.status);
            if (error->isEmpty()) {
                const QString msg = QJsonDocument::fromJson(r.body).object().value(QStringLiteral("message")).toString();
                *error = QStringLiteral("Azure DevOps answered HTTP %1%2").arg(r.status).arg(msg.isEmpty() ? QString() : QStringLiteral(": ") + msg);
            }
        }
        r.error = error ? *error : QStringLiteral("HTTP %1").arg(r.status);
    }
    if (!r.error.isEmpty())
        qCInfo(lcAdo).noquote() << method << "failed, credential from" << cred.source << "(" + cred.kindName() + "):" << r.error;
    return r;
}

bool git(const QString &dir, const QStringList &args, const QProcessEnvironment &env, QString *error, QByteArray *out = nullptr)
{
    QProcess p;
    p.setWorkingDirectory(dir);
    p.setProcessEnvironment(env);
    Log::ProcessTrace trace(lcGit());
    p.start(QStringLiteral("git"), args);
    if (!p.waitForStarted()) {
        trace.done(p);
        if (error)
            *error = QStringLiteral("Could not start git.");
        return false;
    }
    p.closeWriteChannel();
    trace.wait(p);
    const QByteArray stdOut = p.readAllStandardOutput(), stdErr = p.readAllStandardError();
    trace.done(p, stdOut.size(), stdErr);
    if (out)
        *out = stdOut;
    if (p.exitCode() != 0) {
        if (error)
            *error = QString::fromUtf8(stdErr).trimmed();
        return false;
    }
    return true;
}

// Environment for git that authenticates HTTPS fetches without putting the token on
// the command line (git >= 2.31 reads GIT_CONFIG_COUNT/KEY/VALUE).
QProcessEnvironment fetchEnv(const AdoAuth::Credential *cred)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    if (cred) {
        Log::addSecret(cred->secret);
        env.insert(QStringLiteral("GIT_CONFIG_COUNT"), QStringLiteral("1"));
        env.insert(QStringLiteral("GIT_CONFIG_KEY_0"), QStringLiteral("http.extraHeader"));
        env.insert(QStringLiteral("GIT_CONFIG_VALUE_0"), QStringLiteral("Authorization: ") + QString::fromUtf8(cred->authorizationHeader()));
    }
    return env;
}

QString shortRef(const QString &ref)
{
    return ref.startsWith(QLatin1String("refs/heads/")) ? ref.mid(11) : ref;
}

GitHubReview::Comment commentFrom(const QJsonObject &c, qint64 threadId, const QJsonObject &context, const QUrl &webUrl)
{
    GitHubReview::Comment out;
    const qint64 commentId = qint64(c.value(QStringLiteral("id")).toDouble());
    const bool root = c.value(QStringLiteral("parentCommentId")).toInt() == 0 && commentId == 1;
    out.id = root ? threadId : threadId * AdoPr::kReplyBase + commentId;
    out.inReplyTo = root ? 0 : threadId;
    out.author = c.value(QStringLiteral("author")).toObject().value(QStringLiteral("displayName")).toString();
    out.body = c.value(QStringLiteral("content")).toString();
    out.createdAt = c.value(QStringLiteral("publishedDate")).toString();
    QUrl link = webUrl;
    link.setQuery(QStringLiteral("discussionId=%1").arg(threadId));
    out.url = link.toString();
    out.path = context.value(QStringLiteral("filePath")).toString();
    if (out.path.startsWith(u'/'))
        out.path = out.path.mid(1);
    const QJsonObject rs = context.value(QStringLiteral("rightFileStart")).toObject();
    const QJsonObject re = context.value(QStringLiteral("rightFileEnd")).toObject();
    const QJsonObject ls = context.value(QStringLiteral("leftFileStart")).toObject();
    const QJsonObject le = context.value(QStringLiteral("leftFileEnd")).toObject();
    if (!rs.isEmpty()) {
        out.side = QStringLiteral("RIGHT");
        out.line = (re.isEmpty() ? rs : re).value(QStringLiteral("line")).toInt();
        out.startLine = rs.value(QStringLiteral("line")).toInt();
    } else {
        out.side = QStringLiteral("LEFT");
        out.line = (le.isEmpty() ? ls : le).value(QStringLiteral("line")).toInt();
        out.startLine = ls.value(QStringLiteral("line")).toInt();
    }
    if (out.startLine == out.line)
        out.startLine = 0;
    return out;
}

QString downloadRoot()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)).filePath(QStringLiteral("yadiff/ado-pr"));
}

// Where one version of a PR is downloaded: by organisation, repository, PR and commits.
QString downloadPath(const AdoPr::Ref &ref, const AdoPr::Iteration &it)
{
    QString org = AdoAuth::organizationUrl(ref.collectionUrl).toString(QUrl::RemoveScheme | QUrl::StripTrailingSlash).mid(2);
    org.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    QString name = ref.project + u'_' + ref.repo;
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    return QDir(downloadRoot())
        .filePath(QStringLiteral("%1/%2/%3-%4-%5")
                      .arg(org.toLower(), name.toLower())
                      .arg(ref.id)
                      .arg(it.headCommit.left(12), it.baseCommit.left(12)));
}

// Downloads older than a week go (a PR looked at again is downloaded again).
void removeOldDownloads()
{
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-7);
    QDirIterator it(downloadRoot(), {QStringLiteral(".yadiff-complete")}, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    QStringList old;
    while (it.hasNext()) {
        it.next();
        if (it.fileInfo().lastModified() < cutoff)
            old << it.fileInfo().absolutePath();
    }
    for (const QString &dir : std::as_const(old)) {
        qCInfo(lcAdo).noquote() << "removing an old download:" << QDir::toNativeSeparators(dir);
        QDir(dir).removeRecursively();
    }
}

} // namespace

namespace AdoPr {

std::optional<Ref> parse(const QString &link)
{
    QUrl url(link.trimmed());
    if (!url.isValid() || (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http")))
        return std::nullopt;
    // .../PROJECT/_git/REPO/pullrequest/ID[/...]
    static const QRegularExpression re(QStringLiteral(R"(^(.*)/([^/]+)/_git/([^/]+)/pullrequest/(\d+)(?:/.*)?$)"),
                                       QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(url.path(QUrl::FullyDecoded));
    if (!m.hasMatch())
        return std::nullopt;
    Ref r;
    r.collectionUrl = url.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::RemoveUserInfo);
    r.collectionUrl.setPath(m.captured(1));
    r.project = m.captured(2);
    r.repo = m.captured(3);
    r.id = m.captured(4).toInt();
    r.webUrl = url.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::RemoveUserInfo);
    r.webUrl.setPath(QStringLiteral("%1/%2/_git/%3/pullrequest/%4").arg(m.captured(1), r.project, r.repo).arg(r.id));
    if (r.id <= 0)
        return std::nullopt;
    return r;
}

bool remoteMatches(const QString &remoteUrl, const Ref &ref)
{
    const QString org = AdoAuth::organizationUrl(ref.collectionUrl).path().section(u'/', 1, 1).toLower();
    auto same = [&](const QString &o, const QString &p, const QString &r) {
        return (o.isEmpty() || o.compare(org, Qt::CaseInsensitive) == 0)
            && QUrl::fromPercentEncoding(p.toUtf8()).compare(ref.project, Qt::CaseInsensitive) == 0
            && QUrl::fromPercentEncoding(r.toUtf8()).compare(ref.repo, Qt::CaseInsensitive) == 0;
    };
    const QString u = remoteUrl.trimmed();
    // SSH: git@ssh.dev.azure.com:v3/ORG/PROJECT/REPO or ORG@vs-ssh.visualstudio.com:v3/ORG/PROJECT/REPO
    static const QRegularExpression ssh(QStringLiteral(R"(:v3/([^/]+)/([^/]+)/([^/]+?)/?$)"));
    if (const auto m = ssh.match(u); m.hasMatch() && (u.contains(QLatin1String("ssh.dev.azure.com")) || u.contains(QLatin1String("vs-ssh."))))
        return same(m.captured(1), m.captured(2), m.captured(3));
    // HTTPS: same collection, then /PROJECT/_git/REPO
    const QUrl remote(u);
    if (!remote.isValid() || remote.host().isEmpty())
        return false;
    const QUrl a = AdoAuth::organizationUrl(remote);
    const QUrl b = AdoAuth::organizationUrl(ref.collectionUrl);
    if (a.host().compare(b.host(), Qt::CaseInsensitive) != 0)
        return false;
    static const QRegularExpression http(QStringLiteral(R"(/([^/]+)/_git/([^/]+?)/?$)"));
    const auto m = http.match(remote.path(QUrl::FullyEncoded));
    if (!m.hasMatch())
        return false;
    const QString remoteOrg = b.host() == QLatin1String("dev.azure.com") ? a.path().section(u'/', 1, 1) : QString();
    return same(remoteOrg, m.captured(1), m.captured(2));
}

std::optional<Info> info(const Ref &ref, const AdoAuth::Credential &cred, QString *error, int *httpStatus)
{
    const Http::Response r = call("GET", apiUrl(ref, {}), cred, {}, error, httpStatus);
    if (!r.error.isEmpty())
        return std::nullopt;
    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    Info i;
    i.title = o.value(QStringLiteral("title")).toString();
    i.status = o.value(QStringLiteral("status")).toString();
    i.sourceRef = o.value(QStringLiteral("sourceRefName")).toString();
    i.targetRef = o.value(QStringLiteral("targetRefName")).toString();
    i.sourceCommit = o.value(QStringLiteral("lastMergeSourceCommit")).toObject().value(QStringLiteral("commitId")).toString();
    i.targetCommit = o.value(QStringLiteral("lastMergeTargetCommit")).toObject().value(QStringLiteral("commitId")).toString();
    i.remoteUrl = o.value(QStringLiteral("repository")).toObject().value(QStringLiteral("remoteUrl")).toString();
    qCInfo(lcAdo).noquote() << QStringLiteral("PR %1: \"%2\", status %3, %4 (%5) into %6 (%7), repository %8")
                                   .arg(ref.id)
                                   .arg(i.title, i.status, i.sourceRef, i.sourceCommit, i.targetRef, i.targetCommit,
                                        Log::url(QUrl(i.remoteUrl)));
    if (i.sourceCommit.isEmpty() || i.targetCommit.isEmpty()) {
        qCWarning(lcAdo) << "the PR has no lastMergeSourceCommit/lastMergeTargetCommit; response:"
                         << QString::fromUtf8(r.body.left(2000));
        if (error)
            *error = QStringLiteral("Azure DevOps did not report the pull request's commits.");
        return std::nullopt;
    }
    return i;
}

std::optional<Iteration> parseLatestIteration(const QByteArray &json)
{
    std::optional<Iteration> latest;
    for (const QJsonValue &v : QJsonDocument::fromJson(json).object().value(QStringLiteral("value")).toArray()) {
        const QJsonObject o = v.toObject();
        Iteration it;
        it.id = o.value(QStringLiteral("id")).toInt();
        it.headCommit = o.value(QStringLiteral("sourceRefCommit")).toObject().value(QStringLiteral("commitId")).toString();
        it.baseCommit = o.value(QStringLiteral("commonRefCommit")).toObject().value(QStringLiteral("commitId")).toString();
        if (it.id > 0 && !it.headCommit.isEmpty() && !it.baseCommit.isEmpty() && (!latest || it.id > latest->id))
            latest = it;
    }
    return latest;
}

QVector<Change> parseChanges(const QByteArray &json, int *nextSkip)
{
    const QJsonObject top = QJsonDocument::fromJson(json).object();
    if (nextSkip)
        *nextSkip = top.value(QStringLiteral("nextSkip")).toInt();
    QVector<Change> out;
    for (const QJsonValue &v : top.value(QStringLiteral("changeEntries")).toArray()) {
        const QJsonObject e = v.toObject();
        const QJsonObject item = e.value(QStringLiteral("item")).toObject();
        if (item.value(QStringLiteral("isFolder")).toBool() || item.value(QStringLiteral("gitObjectType")).toString() == QLatin1String("tree"))
            continue;
        const QString type = e.value(QStringLiteral("changeType")).toString().toLower();
        const QString path = item.value(QStringLiteral("path")).toString();
        QString original = e.value(QStringLiteral("originalPath")).toString();
        if (original.isEmpty())
            original = e.value(QStringLiteral("sourceServerItem")).toString();
        Change c;
        c.objectId = item.value(QStringLiteral("objectId")).toString();
        c.oldObjectId = item.value(QStringLiteral("originalObjectId")).toString();
        if (type.contains(QLatin1String("delete"))) {
            c.oldPath = path;
            if (c.oldObjectId.isEmpty())
                c.oldObjectId = c.objectId;
            c.objectId.clear();
        } else if (type.contains(QLatin1String("add"))) {
            c.path = path;
            c.oldObjectId.clear();
        } else {
            c.path = path;
            c.oldPath = original.isEmpty() ? path : original;
        }
        if (c.path.isEmpty() && c.oldPath.isEmpty())
            continue;
        out.push_back(c);
    }
    return out;
}

QString safeRelativePath(const QString &apiPath)
{
    QString p = apiPath;
    while (p.startsWith(u'/'))
        p.remove(0, 1);
    if (p.isEmpty() || p.contains(u'\\') || p.contains(u':') || p.contains(QChar(0)))
        return {};
    for (const QString &part : p.split(u'/'))
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String(".."))
            return {};
    return p;
}

std::optional<Resolved> download(const Ref &ref, const AdoAuth::Credential &cred, const QString &title,
                                 const QString &sourceRef, const QString &targetRef, QString *error,
                                 const std::function<void(const QString &)> &progress)
{
    auto say = [&](const QString &s) {
        qCInfo(lcAdo).noquote() << s;
        if (progress)
            progress(s);
    };
    // The latest iteration: the PR's head and its merge base, as the Files tab shows them.
    const Http::Response its = call("GET", apiUrl(ref, QStringLiteral("/iterations")), cred, {}, error);
    if (!its.error.isEmpty())
        return std::nullopt;
    const auto it = parseLatestIteration(its.body);
    if (!it) {
        if (error)
            *error = QStringLiteral("Azure DevOps did not report the pull request's iterations.");
        return std::nullopt;
    }
    Resolved r;
    r.repoPath = downloadPath(ref, *it);
    r.baseDir = r.repoPath + QStringLiteral("/base");
    r.headDir = r.repoPath + QStringLiteral("/head");
    r.baseOid = it->baseCommit;
    r.headOid = it->headCommit;
    r.baseName = QStringLiteral("%1 (merge base %2)").arg(shortRef(targetRef), it->baseCommit.left(10));
    r.headName = QStringLiteral("PR %1 %2 (%3)").arg(ref.id).arg(shortRef(sourceRef), it->headCommit.left(10));
    r.title = QStringLiteral("%1/%2!%3%4").arg(ref.project, ref.repo).arg(ref.id).arg(title.isEmpty() ? QString() : QStringLiteral(": ") + title);
    const QString marker = r.repoPath + QStringLiteral("/.yadiff-complete");
    if (QFileInfo::exists(marker)) {
        qCInfo(lcAdo).noquote() << "PR" << ref.id << "iteration" << it->id << "is downloaded already:"
                                << QDir::toNativeSeparators(r.repoPath);
        QFile touch(marker); // still in use: not old yet
        if (touch.open(QIODevice::ReadWrite))
            touch.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
        return r;
    }
    removeOldDownloads();

    say(QStringLiteral("Listing the pull request's files (iteration %1)…").arg(it->id));
    QVector<Change> changes;
    for (int skip = 0;;) {
        const Http::Response page = call(
            "GET", apiUrl(ref, QStringLiteral("/iterations/%1/changes?$compareTo=0&$top=2000&$skip=%2").arg(it->id).arg(skip)), cred,
            {}, error);
        if (!page.error.isEmpty())
            return std::nullopt;
        int next = 0;
        changes += parseChanges(page.body, &next);
        if (next <= skip)
            break;
        skip = next;
    }
    qCInfo(lcAdo).noquote() << "PR" << ref.id << "changes" << changes.size() << "files";

    // Every version to download: (blob id, file to write).
    QVector<std::pair<QString, QString>> files;
    QDir(r.repoPath).removeRecursively();
    for (const Change &c : std::as_const(changes)) {
        for (const auto &[apiPath, id, dir] : {std::make_tuple(c.oldPath, c.oldObjectId, r.baseDir),
                                              std::make_tuple(c.path, c.objectId, r.headDir)}) {
            if (apiPath.isEmpty() || id.isEmpty())
                continue;
            const QString rel = safeRelativePath(apiPath);
            if (rel.isEmpty()) {
                qCWarning(lcAdo).noquote() << "skipping a file with an unsafe path:" << apiPath;
                continue;
            }
            files.push_back({id, dir + u'/' + rel});
        }
    }
    QDir().mkpath(r.baseDir);
    QDir().mkpath(r.headDir);

    // A few at a time: each file is its own request.
    std::atomic<int> done{0};
    std::atomic<bool> failed{false};
    QMutex errorMutex;
    QString firstError;
    QThreadPool pool;
    pool.setMaxThreadCount(8);
    const int total = int(files.size());
    say(QStringLiteral("Downloading %1 files…").arg(total));
    QtConcurrent::blockingMap(&pool, files, [&](const std::pair<QString, QString> &f) {
        if (failed || Log::stopping())
            return;
        QString err;
        const Http::Response blob = call("GET", repoApiUrl(ref, QStringLiteral("/blobs/%1?$format=octetstream").arg(f.first)), cred,
                                         {}, &err, nullptr, "application/octet-stream");
        QDir().mkpath(QFileInfo(f.second).absolutePath());
        QFile out(f.second);
        if (blob.error.isEmpty() && !(out.open(QIODevice::WriteOnly | QIODevice::Truncate) && out.write(blob.body) == blob.body.size()))
            err = QStringLiteral("Could not write %1: %2").arg(QDir::toNativeSeparators(f.second), out.errorString());
        if (!blob.error.isEmpty() || !err.isEmpty()) {
            failed = true;
            QMutexLocker lock(&errorMutex);
            if (firstError.isEmpty())
                firstError = err.isEmpty() ? blob.error : err;
            return;
        }
        const int n = ++done;
        if (progress && (n == total || n % 5 == 0))
            progress(QStringLiteral("Downloading the changed files: %1 of %2…").arg(n).arg(total));
    });
    if (failed || Log::stopping()) {
        if (error)
            *error = firstError.isEmpty() ? QStringLiteral("The download was stopped.")
                                          : QStringLiteral("Could not download the pull request's files: %1").arg(firstError);
        QDir(r.repoPath).removeRecursively();
        return std::nullopt;
    }
    if (QFile complete(marker); !complete.open(QIODevice::WriteOnly))
        qCWarning(lcAdo).noquote() << "could not write" << marker << ":" << complete.errorString();
    qCInfo(lcAdo).noquote() << "PR" << ref.id << "downloaded" << total << "files into" << QDir::toNativeSeparators(r.repoPath);
    return r;
}

std::optional<Resolved> resolve(const Ref &ref, const AdoAuth::Credential &cred, const QString &localDir, QString *error,
                                int *httpStatus, const std::function<void(const QString &)> &progress)
{
    auto say = [&](const QString &s) {
        qCInfo(lcAdo).noquote() << s;
        if (progress)
            progress(s);
    };
    auto failed = [&](const char *step) {
        qCInfo(lcAdo).noquote() << "resolving PR" << ref.id << "failed at" << step << ":" << (error ? *error : QString());
        return std::nullopt;
    };
    qCInfo(lcAdo).noquote() << QStringLiteral("resolving PR %1 in %2 / %3 / %4 (local folder %5), credential from %6 (%7)")
                                   .arg(ref.id)
                                   .arg(Log::url(ref.collectionUrl), ref.project, ref.repo, QDir::toNativeSeparators(localDir),
                                        cred.source, cred.kindName());
    const auto pr = info(ref, cred, error, httpStatus);
    if (!pr)
        return failed("reading the pull request");

    // The user's clone when one of its remotes is this repository, else the cache.
    QString repoPath, remote;
    bool own = false;
    if (auto local = GitRepo::open(localDir, nullptr); local && !local->isBare()) {
        QByteArray out;
        git(local->root(), {QStringLiteral("remote"), QStringLiteral("-v")}, fetchEnv(nullptr), nullptr, &out);
        for (const QByteArray &line : out.split('\n')) {
            const QList<QByteArray> parts = line.simplified().split(' ');
            if (parts.size() >= 2 && remoteMatches(QString::fromUtf8(parts[1]), ref)) {
                repoPath = local->root();
                remote = QString::fromUtf8(parts[0]);
                own = true;
                break;
            }
        }
        if (own)
            qCInfo(lcAdo).noquote() << "using the local clone" << QDir::toNativeSeparators(repoPath) << "remote" << remote;
        else
            qCInfo(lcAdo).noquote() << "no remote of" << QDir::toNativeSeparators(local->root()) << "is this repository:"
                                    << QString::fromUtf8(out).trimmed();
    } else {
        qCInfo(lcAdo).noquote() << QDir::toNativeSeparators(localDir) << "is not a git working tree";
    }
    if (repoPath.isEmpty()) {
        // No clone here: download just the changed files. Fetching the commits into an
        // empty repository would bring their whole history (millions of objects in a
        // large repository).
        auto r = download(ref, cred, pr->title, pr->sourceRef, pr->targetRef, error, progress);
        if (!r)
            return failed("downloading the changed files");
        return r;
    }
    auto repo = GitRepo::open(repoPath, error);
    if (!repo)
        return failed("opening the repository");

    if (!repo->hasCommit(pr->sourceCommit) || !repo->hasCommit(pr->targetCommit)) {
        say(QStringLiteral("Fetching %1!%2 from %3 ...").arg(ref.repo).arg(ref.id).arg(remote));
        QStringList base{QStringLiteral("fetch"), QStringLiteral("--no-tags"), QStringLiteral("--quiet")};
        if (repo->isBare())
            base << QStringLiteral("--filter=blob:none");
        base << remote;
        // Commits by id first (works for completed PRs whose branch is gone), then the branches.
        const QList<QStringList> attempts{{pr->sourceCommit, pr->targetCommit},
                                          {QStringLiteral("refs/pull/%1/merge").arg(ref.id), pr->targetRef},
                                          {pr->sourceRef, pr->targetRef}};
        QString lastError;
        for (const QStringList &specs : attempts) {
            // Own clone: the user's own git credentials first; the token if those fail.
            for (const AdoAuth::Credential *c : own ? QList<const AdoAuth::Credential *>{nullptr, &cred}
                                                    : QList<const AdoAuth::Credential *>{&cred}) {
                qCInfo(lcAdo).noquote() << "fetching" << specs.join(u' ') << "with"
                                        << (c ? QStringLiteral("the %1 credential").arg(c->source)
                                              : QStringLiteral("your own git credentials"));
                QString err;
                git(repoPath, base + specs, fetchEnv(c), &err);
                if (!err.isEmpty())
                    lastError = err;
                if (repo->hasCommit(pr->sourceCommit) && repo->hasCommit(pr->targetCommit))
                    break;
            }
            if (repo->hasCommit(pr->sourceCommit) && repo->hasCommit(pr->targetCommit))
                break;
        }
        if (!repo->hasCommit(pr->sourceCommit) || !repo->hasCommit(pr->targetCommit)) {
            if (error)
                *error = QStringLiteral("Could not fetch the pull request's commits: %1").arg(lastError);
            return failed("fetching");
        }
    }

    // Azure DevOps shows the source branch against its merge base with the target.
    const auto mergeBase = repo->mergeBase(pr->targetCommit, pr->sourceCommit, error);
    if (!mergeBase)
        return failed("finding the merge base");
    qCInfo(lcAdo).noquote() << "PR" << ref.id << "resolved: merge base" << *mergeBase << "head" << pr->sourceCommit;
    Resolved r;
    r.repoPath = repoPath;
    r.baseOid = *mergeBase;
    r.headOid = pr->sourceCommit;
    r.baseName = QStringLiteral("%1 (merge base %2)").arg(shortRef(pr->targetRef), mergeBase->left(10));
    r.headName = QStringLiteral("PR %1 %2 (%3)").arg(ref.id).arg(shortRef(pr->sourceRef), pr->sourceCommit.left(10));
    r.title = QStringLiteral("%1/%2!%3%4").arg(ref.project, ref.repo).arg(ref.id).arg(pr->title.isEmpty() ? QString() : QStringLiteral(": ") + pr->title);
    return r;
}

QVector<GitHubReview::Comment> parseThreads(const QByteArray &json, const QUrl &webUrl)
{
    QVector<GitHubReview::Comment> out;
    for (const QJsonValue &tv : QJsonDocument::fromJson(json).object().value(QStringLiteral("value")).toArray()) {
        const QJsonObject t = tv.toObject();
        if (t.value(QStringLiteral("isDeleted")).toBool())
            continue;
        const QJsonObject context = t.value(QStringLiteral("threadContext")).toObject();
        if (context.value(QStringLiteral("filePath")).toString().isEmpty())
            continue; // pull-request-level discussion, not attached to a line
        const qint64 threadId = qint64(t.value(QStringLiteral("id")).toDouble());
        for (const QJsonValue &cv : t.value(QStringLiteral("comments")).toArray()) {
            const QJsonObject c = cv.toObject();
            if (c.value(QStringLiteral("isDeleted")).toBool() || c.value(QStringLiteral("commentType")).toString() == QLatin1String("system"))
                continue;
            out.push_back(commentFrom(c, threadId, context, webUrl));
        }
    }
    return out;
}

QByteArray threadPayload(const QString &path, const QString &side, int line, int startLine, const QString &body)
{
    const bool left = side == QLatin1String("LEFT");
    const QJsonObject start{{QStringLiteral("line"), startLine > 0 ? startLine : line}, {QStringLiteral("offset"), 1}};
    const QJsonObject end{{QStringLiteral("line"), line}, {QStringLiteral("offset"), 1}};
    QJsonObject context{{QStringLiteral("filePath"), u'/' + path}};
    context.insert(left ? QStringLiteral("leftFileStart") : QStringLiteral("rightFileStart"), start);
    context.insert(left ? QStringLiteral("leftFileEnd") : QStringLiteral("rightFileEnd"), end);
    const QJsonObject comment{{QStringLiteral("parentCommentId"), 0}, {QStringLiteral("content"), body}, {QStringLiteral("commentType"), 1}};
    QJsonObject o{{QStringLiteral("comments"), QJsonArray{comment}}, {QStringLiteral("status"), 1}};
    if (!path.isEmpty())
        o.insert(QStringLiteral("threadContext"), context);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

std::optional<QVector<GitHubReview::Comment>> fetchComments(const Ref &ref, const AdoAuth::Credential &cred, QString *error)
{
    const Http::Response r = call("GET", apiUrl(ref, QStringLiteral("/threads")), cred, {}, error);
    if (!r.error.isEmpty())
        return std::nullopt;
    return parseThreads(r.body, ref.webUrl);
}

std::optional<GitHubReview::Comment> post(const Ref &ref, const AdoAuth::Credential &cred, const QString &path,
                                          const QString &side, int line, int startLine, const QString &body, QString *error)
{
    const Http::Response r = call("POST", apiUrl(ref, QStringLiteral("/threads")), cred,
                                  threadPayload(path, side, line, startLine, body), error);
    if (!r.error.isEmpty())
        return std::nullopt;
    const QByteArray wrapped = "{\"value\":[" + r.body + "]}";
    const auto parsed = parseThreads(wrapped, ref.webUrl);
    if (parsed.isEmpty()) {
        if (error)
            *error = QStringLiteral("Azure DevOps accepted the comment but returned something unexpected.");
        return std::nullopt;
    }
    return parsed.first();
}

std::optional<GitHubReview::Comment> reply(const Ref &ref, const AdoAuth::Credential &cred, qint64 threadId,
                                           const QString &body, QString *error)
{
    const QByteArray payload = QJsonDocument(QJsonObject{{QStringLiteral("content"), body},
                                                         {QStringLiteral("parentCommentId"), 1},
                                                         {QStringLiteral("commentType"), 1}})
                                   .toJson(QJsonDocument::Compact);
    const Http::Response r = call("POST", apiUrl(ref, QStringLiteral("/threads/%1/comments").arg(threadId)), cred, payload, error);
    if (!r.error.isEmpty())
        return std::nullopt;
    return commentFrom(QJsonDocument::fromJson(r.body).object(), threadId, {}, ref.webUrl);
}

namespace {

// Who the credential belongs to: one call per organisation and credential, not per vote.
std::optional<QString> myId(const Ref &ref, const AdoAuth::Credential &cred, QString *error)
{
    static QMutex mutex;
    static QHash<QString, QString> ids;
    const QString key = AdoAuth::organizationUrl(ref.collectionUrl).toString() + u'\n' + cred.source;
    {
        QMutexLocker lock(&mutex);
        if (const auto it = ids.constFind(key); it != ids.constEnd())
            return *it;
    }
    const auto id = AdoAuth::userId(ref.collectionUrl, cred, error);
    if (id) {
        QMutexLocker lock(&mutex);
        ids.insert(key, *id);
    }
    return id;
}

} // namespace

int parseVote(const QByteArray &reviewersJson, const QString &userId)
{
    for (const QJsonValue &v : QJsonDocument::fromJson(reviewersJson).object().value(QStringLiteral("value")).toArray()) {
        const QJsonObject r = v.toObject();
        if (r.value(QStringLiteral("id")).toString().compare(userId, Qt::CaseInsensitive) == 0)
            return r.value(QStringLiteral("vote")).toInt();
    }
    return 0;
}

std::optional<int> myVote(const Ref &ref, const AdoAuth::Credential &cred, QString *error)
{
    const auto me = myId(ref, cred, error);
    if (!me)
        return std::nullopt;
    const Http::Response r = call("GET", apiUrl(ref, QStringLiteral("/reviewers")), cred, {}, error);
    if (!r.error.isEmpty())
        return std::nullopt;
    const int vote = parseVote(r.body, *me);
    qCInfo(lcAdo).noquote() << "PR" << ref.id << ": your vote is" << vote;
    return vote;
}

bool setVote(const Ref &ref, const AdoAuth::Credential &cred, int vote, QString *error)
{
    const auto me = myId(ref, cred, error);
    if (!me)
        return false;
    qCInfo(lcAdo).noquote() << "PR" << ref.id << ": voting" << vote;
    const QByteArray payload = QJsonDocument(QJsonObject{{QStringLiteral("vote"), vote}}).toJson(QJsonDocument::Compact);
    return call("PUT", apiUrl(ref, QStringLiteral("/reviewers/%1").arg(*me)), cred, payload, error).error.isEmpty();
}

bool postMessage(const Ref &ref, const AdoAuth::Credential &cred, const QString &body, QString *error)
{
    return call("POST", apiUrl(ref, QStringLiteral("/threads")), cred, threadPayload({}, {}, 0, 0, body), error).error.isEmpty();
}

bool submit(const Ref &ref, const AdoAuth::Credential &cred, GitHubReview::Verdict verdict, const QString &body, QString *error)
{
    if (verdict != GitHubReview::Verdict::Comment
        && !setVote(ref, cred, verdict == GitHubReview::Verdict::Approve ? 10 : -5, error))
        return false;
    if (!body.trimmed().isEmpty())
        return postMessage(ref, cred, body, error);
    return true;
}

} // namespace AdoPr
