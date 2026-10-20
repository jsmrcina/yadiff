#include "PrInbox.h"

#include "Http.h"
#include "Log.h"
#include "PrReview.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentMap>

#include <algorithm>

namespace PrInbox {

namespace {

QString seg(const QString &s)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(s));
}

QString sourceKey(const AdoSource &s)
{
    return s.collection.toString(QUrl::StripTrailingSlash) + u'\t' + s.project;
}

QString seenKey(const QString &link)
{
    // QSettings keys can't hold every character a URL can: hash it.
    // Spelled the same however the link was written (encoding, case).
    const QString canonical = QUrl(link).adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::StripTrailingSlash)
                                  .toString(QUrl::FullyDecoded)
                                  .toLower();
    return QStringLiteral("prSeen/") + QString::fromLatin1(QCryptographicHash::hash(canonical.toUtf8(), QCryptographicHash::Sha1).toHex());
}

Http::Response get(const QUrl &url, const AdoAuth::Credential &cred, QString *error)
{
    if (!Http::safeForCredentials(url)) {
        if (error)
            *error = QStringLiteral("Refusing to send credentials over an unencrypted connection to %1.").arg(url.host());
        return {};
    }
    Http::Response r = Http::request("GET", url, {{"Authorization", cred.authorizationHeader()}, {"Accept", "application/json"}});
    if (r.error.isEmpty() && (!r.ok() || r.status == 203)) {
        r.error = AdoAuth::authProblem(r.status);
        if (r.error.isEmpty())
            r.error = QStringLiteral("Azure DevOps answered HTTP %1").arg(r.status);
        if (r.status == 401 || r.status == 203)
            AdoAuth::rejected(cred.collection, cred);
    }
    if (error && !r.error.isEmpty())
        *error = r.error;
    return r;
}

bool gh(const QStringList &args, QByteArray *out, QString *error)
{
    if (QStandardPaths::findExecutable(QStringLiteral("gh")).isEmpty()) {
        if (error)
            *error = QStringLiteral("the GitHub CLI (gh) is not installed");
        return false;
    }
    QProcess p;
    Log::ProcessTrace trace(lcGitHub());
    p.start(QStringLiteral("gh"), args);
    if (!p.waitForStarted()) {
        trace.done(p);
        if (error)
            *error = QStringLiteral("could not start gh");
        return false;
    }
    p.closeWriteChannel();
    trace.wait(p, 120000);
    const QByteArray stdErr = p.readAllStandardError();
    *out = p.readAllStandardOutput();
    trace.done(p, out->size(), stdErr);
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        if (error)
            *error = QString::fromUtf8(stdErr).trimmed().section(u'\n', 0, 0);
        return false;
    }
    return true;
}

std::optional<QVector<Item>> adoList(const AdoSource &source, QString *error)
{
    QString diag;
    const auto cred = AdoAuth::find(source.collection, &diag);
    if (!cred) {
        if (error)
            *error = QStringLiteral("not signed in (open one of its pull requests, or run yadiff --ado-login)");
        return std::nullopt;
    }
    const QString collection = source.collection.toString(QUrl::StripTrailingSlash);
    const auto me = AdoAuth::userId(source.collection, *cred, error);
    if (!me)
        return std::nullopt;
    // The user's teams in the project: reviews asked of a team are asked of them too.
    QString err;
    const Http::Response teamsReply = get(
        QUrl(QStringLiteral("%1/_apis/projects/%2/teams?$mine=true&$top=200&api-version=7.1-preview.3").arg(collection, seg(source.project)),
             QUrl::StrictMode),
        *cred, &err);
    QHash<QString, QString> teams;
    if (teamsReply.error.isEmpty())
        teams = parseAdoTeams(teamsReply.body);
    else
        qCInfo(lcAdo).noquote() << "could not list your teams in" << source.project << ":" << err;

    QStringList reviewers{*me};
    for (auto it = teams.constBegin(); it != teams.constEnd(); ++it)
        reviewers << it.key();
    QVector<Item> out;
    QHash<QString, int> seen;
    for (const QString &id : std::as_const(reviewers)) {
        const Http::Response r = get(
            QUrl(QStringLiteral("%1/%2/_apis/git/pullrequests?searchCriteria.reviewerId=%3&searchCriteria.status=active&$top=200&api-version=7.1")
                     .arg(collection, seg(source.project), seg(id)),
                 QUrl::StrictMode),
            *cred, error);
        if (!r.error.isEmpty())
            return std::nullopt;
        for (const Item &item : parseAdoPullRequests(r.body, source, *me, teams)) {
            if (seen.contains(item.link))
                continue;
            seen.insert(item.link, int(out.size()));
            out.push_back(item);
        }
    }
    qCInfo(lcAdo).noquote() << "pull requests to review in" << Log::url(source.collection) << source.project << ":" << out.size()
                            << "(you and" << teams.size() << "teams)";
    return out;
}

std::optional<QVector<Item>> gitHubList(QString *error)
{
    if (QStandardPaths::findExecutable(QStringLiteral("gh")).isEmpty())
        return QVector<Item>(); // no GitHub CLI: nothing to look in, not a problem
    static const QString fields = QStringLiteral(
        "nodes{... on PullRequest{number title url isDraft createdAt author{login} repository{nameWithOwner} headRefOid "
        "viewerLatestReview{state}}}");
    const QString query = QStringLiteral("query{asked:search(query:\"is:pr is:open archived:false review-requested:@me\",type:ISSUE,first:50){%1}"
                                         "reviewed:search(query:\"is:pr is:open archived:false reviewed-by:@me -author:@me\",type:ISSUE,first:50){%1}}")
                              .arg(fields);
    QByteArray out;
    if (!gh({QStringLiteral("api"), QStringLiteral("graphql"), QStringLiteral("-f"), QStringLiteral("query=") + query}, &out, error))
        return std::nullopt;
    return parseGitHubSearch(out);
}

} // namespace

std::optional<AdoSource> parseSource(const QString &text)
{
    QUrl url(text.trimmed());
    if (!url.isValid() || url.host().isEmpty() || (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http")))
        return std::nullopt;
    const QString path = url.path(QUrl::FullyDecoded);
    const QStringList parts = path.split(u'/', Qt::SkipEmptyParts);
    AdoSource s;
    QUrl base = url.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::RemoveUserInfo);
    // Anything with /_git/ or /pullrequest/: the project is the part before _git.
    if (const int git = int(parts.indexOf(QStringLiteral("_git"))); git >= 1) {
        base.setPath(u'/' + parts.mid(0, git - 1).join(u'/'));
        s.collection = base.adjusted(QUrl::StripTrailingSlash);
        s.project = parts[git - 1];
        return s;
    }
    const QString host = url.host().toLower();
    if (host == QLatin1String("dev.azure.com") && parts.size() >= 2) {
        base.setPath(u'/' + parts[0]);
        s.project = parts[1];
    } else if (host.endsWith(QLatin1String(".visualstudio.com")) && !parts.isEmpty()) {
        const bool dc = parts[0].compare(QLatin1String("DefaultCollection"), Qt::CaseInsensitive) == 0;
        if (dc && parts.size() < 2)
            return std::nullopt;
        base.setPath(dc ? QStringLiteral("/DefaultCollection") : QString());
        s.project = parts[dc ? 1 : 0];
    } else {
        return std::nullopt;
    }
    if (s.project.startsWith(u'_'))
        return std::nullopt;
    s.collection = base.adjusted(QUrl::StripTrailingSlash);
    return s;
}

QVector<AdoSource> sources()
{
    QVector<AdoSource> out;
    for (const QString &line : QSettings().value(QStringLiteral("review/adoSources")).toStringList()) {
        const AdoSource s{QUrl(line.section(u'\t', 0, 0)), line.section(u'\t', 1)};
        if (s.collection.isValid() && !s.project.isEmpty() && !out.contains(s))
            out.push_back(s);
    }
    return out;
}

void addSource(const AdoSource &s)
{
    QStringList list = QSettings().value(QStringLiteral("review/adoSources")).toStringList();
    const QString key = sourceKey(s);
    for (const QString &k : std::as_const(list))
        if (k.compare(key, Qt::CaseInsensitive) == 0)
            return;
    list << key;
    QSettings().setValue(QStringLiteral("review/adoSources"), list);
}

void removeSource(const AdoSource &s)
{
    QStringList list = QSettings().value(QStringLiteral("review/adoSources")).toStringList();
    const QString key = sourceKey(s);
    list.removeIf([&](const QString &k) { return k.compare(key, Qt::CaseInsensitive) == 0; });
    QSettings().setValue(QStringLiteral("review/adoSources"), list);
}

void markSeen(const QString &link, const QString &headCommit)
{
    if (!link.isEmpty() && !headCommit.isEmpty())
        QSettings().setValue(seenKey(link), headCommit);
}

QString lastSeen(const QString &link)
{
    return QSettings().value(seenKey(link)).toString();
}

QHash<QString, QString> parseAdoTeams(const QByteArray &json)
{
    QHash<QString, QString> out;
    for (const QJsonValue &v : QJsonDocument::fromJson(json).object().value(QStringLiteral("value")).toArray()) {
        const QJsonObject t = v.toObject();
        const QString id = t.value(QStringLiteral("id")).toString();
        if (!id.isEmpty())
            out.insert(id, t.value(QStringLiteral("name")).toString());
    }
    return out;
}

QVector<Item> parseAdoPullRequests(const QByteArray &json, const AdoSource &source, const QString &userId,
                                   const QHash<QString, QString> &teams)
{
    QVector<Item> out;
    const QString collection = source.collection.toString(QUrl::StripTrailingSlash);
    for (const QJsonValue &v : QJsonDocument::fromJson(json).object().value(QStringLiteral("value")).toArray()) {
        const QJsonObject pr = v.toObject();
        const QJsonObject repo = pr.value(QStringLiteral("repository")).toObject();
        const QString project = repo.value(QStringLiteral("project")).toObject().value(QStringLiteral("name")).toString(source.project);
        Item it;
        it.host = QStringLiteral("ado");
        it.number = pr.value(QStringLiteral("pullRequestId")).toInt();
        it.repo = project + u'/' + repo.value(QStringLiteral("name")).toString();
        it.link = QStringLiteral("%1/%2/_git/%3/pullrequest/%4")
                      .arg(collection, seg(project), seg(repo.value(QStringLiteral("name")).toString()))
                      .arg(it.number);
        it.title = pr.value(QStringLiteral("title")).toString();
        it.author = pr.value(QStringLiteral("createdBy")).toObject().value(QStringLiteral("displayName")).toString();
        it.created = pr.value(QStringLiteral("creationDate")).toString();
        it.draft = pr.value(QStringLiteral("isDraft")).toBool();
        it.headCommit = pr.value(QStringLiteral("lastMergeSourceCommit")).toObject().value(QStringLiteral("commitId")).toString();
        bool direct = false;
        QString team;
        for (const QJsonValue &rv : pr.value(QStringLiteral("reviewers")).toArray()) {
            const QJsonObject r = rv.toObject();
            const QString id = r.value(QStringLiteral("id")).toString();
            if (id.compare(userId, Qt::CaseInsensitive) == 0) {
                direct = true;
                it.myVote = PrReview::adoVote(r.value(QStringLiteral("vote")).toInt());
                it.required = it.required || r.value(QStringLiteral("isRequired")).toBool();
            } else if (teams.contains(id) || teams.contains(id.toLower())) {
                if (team.isEmpty())
                    team = r.value(QStringLiteral("displayName")).toString();
                it.required = it.required || r.value(QStringLiteral("isRequired")).toBool();
            }
        }
        if (!direct)
            it.via = team;
        if (it.number > 0)
            out.push_back(it);
    }
    return out;
}

QVector<Item> parseGitHubSearch(const QByteArray &json)
{
    QVector<Item> out;
    QHash<QString, int> at;
    const QJsonObject data = QJsonDocument::fromJson(json).object().value(QStringLiteral("data")).toObject();
    for (const char *list : {"asked", "reviewed"}) {
        for (const QJsonValue &v : data.value(QLatin1String(list)).toObject().value(QStringLiteral("nodes")).toArray()) {
            const QJsonObject pr = v.toObject();
            Item it;
            it.host = QStringLiteral("github");
            it.number = pr.value(QStringLiteral("number")).toInt();
            it.link = pr.value(QStringLiteral("url")).toString();
            it.repo = pr.value(QStringLiteral("repository")).toObject().value(QStringLiteral("nameWithOwner")).toString();
            it.title = pr.value(QStringLiteral("title")).toString();
            it.author = pr.value(QStringLiteral("author")).toObject().value(QStringLiteral("login")).toString();
            it.created = pr.value(QStringLiteral("createdAt")).toString();
            it.draft = pr.value(QStringLiteral("isDraft")).toBool();
            it.headCommit = pr.value(QStringLiteral("headRefOid")).toString();
            const QString state = pr.value(QStringLiteral("viewerLatestReview")).toObject().value(QStringLiteral("state")).toString();
            it.reviewed = !state.isEmpty();
            it.myVote = state == QLatin1String("APPROVED")            ? GitHubReview::Vote::Approved
                      : state == QLatin1String("CHANGES_REQUESTED")   ? GitHubReview::Vote::Rejected
                                                                      : GitHubReview::Vote::None;
            if (it.number <= 0 || it.link.isEmpty() || at.contains(it.link))
                continue;
            at.insert(it.link, int(out.size()));
            out.push_back(it);
        }
    }
    return out;
}

Listing fetchAll(const QVector<AdoSource> &ado, bool github)
{
    struct Part {
        std::optional<AdoSource> source; // none: GitHub
        std::optional<QVector<Item>> items;
        QString error;
    };
    QVector<Part> parts;
    for (const AdoSource &s : ado)
        parts.push_back({s, std::nullopt, {}});
    if (github)
        parts.push_back({std::nullopt, std::nullopt, {}});
    QThreadPool pool;
    pool.setMaxThreadCount(4);
    QtConcurrent::blockingMap(&pool, parts, [](Part &p) {
        p.items = p.source ? adoList(*p.source, &p.error) : gitHubList(&p.error);
    });
    Listing out;
    for (const Part &p : std::as_const(parts)) {
        if (p.items)
            out.items += *p.items;
        else
            out.problems << QStringLiteral("%1: %2").arg(p.source ? AdoAuth::organizationUrl(p.source->collection).toString() + u'/' + p.source->project
                                                                  : QStringLiteral("GitHub"),
                                                         p.error);
    }
    // Newest first.
    std::stable_sort(out.items.begin(), out.items.end(), [](const Item &a, const Item &b) { return a.created > b.created; });
    return out;
}

} // namespace PrInbox
