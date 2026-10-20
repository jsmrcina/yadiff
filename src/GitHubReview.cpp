#include "GitHubReview.h"

#include "Log.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QMutex>
#include <QProcess>

#include <algorithm>

namespace {

bool gh(const QStringList &args, const QByteArray &input, QByteArray *out, QString *error)
{
    QProcess p;
    Log::ProcessTrace trace(lcGitHub());
    p.start(QStringLiteral("gh"), args);
    if (!p.waitForStarted()) {
        trace.done(p);
        if (error)
            *error = QStringLiteral("Could not start gh. Install the GitHub CLI and run `gh auth login`.");
        return false;
    }
    if (!input.isEmpty())
        p.write(input);
    p.closeWriteChannel();
    trace.wait(p);
    const QByteArray stdOut = p.readAllStandardOutput(), stdErr = p.readAllStandardError();
    trace.done(p, stdOut.size(), stdErr);
    if (out)
        *out = stdOut;
    if (p.exitCode() != 0) {
        if (error) {
            *error = QString::fromUtf8(stdErr).trimmed();
            // gh prints the API's JSON error on stdout for failed requests.
            const QJsonObject o = QJsonDocument::fromJson(out ? *out : QByteArray()).object();
            if (o.contains(QStringLiteral("message")))
                *error += QStringLiteral("\n") + o.value(QStringLiteral("message")).toString();
        }
        return false;
    }
    return true;
}

QString base(const GitHubReview::PrId &pr)
{
    return QStringLiteral("repos/%1/%2/pulls/%3").arg(pr.owner, pr.repo).arg(pr.number);
}

GitHubReview::Comment fromJson(const QJsonObject &o)
{
    GitHubReview::Comment c;
    c.id = qint64(o.value(QStringLiteral("id")).toDouble());
    c.inReplyTo = qint64(o.value(QStringLiteral("in_reply_to_id")).toDouble());
    c.path = o.value(QStringLiteral("path")).toString();
    c.side = o.value(QStringLiteral("side")).toString(QStringLiteral("RIGHT"));
    c.line = o.value(QStringLiteral("line")).toInt(); // null (outdated) -> 0
    c.startLine = o.value(QStringLiteral("start_line")).toInt();
    c.author = o.value(QStringLiteral("user")).toObject().value(QStringLiteral("login")).toString();
    c.body = o.value(QStringLiteral("body")).toString();
    c.createdAt = o.value(QStringLiteral("created_at")).toString();
    c.url = o.value(QStringLiteral("html_url")).toString();
    return c;
}

} // namespace

namespace GitHubReview {

bool isOpen(ThreadStatus status)
{
    return status == ThreadStatus::None || status == ThreadStatus::Active || status == ThreadStatus::Pending;
}

QString statusName(ThreadStatus status)
{
    switch (status) {
    case ThreadStatus::Active:
        return QStringLiteral("Active");
    case ThreadStatus::Pending:
        return QStringLiteral("Pending");
    case ThreadStatus::Resolved:
        return QStringLiteral("Resolved");
    case ThreadStatus::WontFix:
        return QStringLiteral("Won't fix");
    case ThreadStatus::ByDesign:
        return QStringLiteral("By design");
    case ThreadStatus::Closed:
        return QStringLiteral("Closed");
    case ThreadStatus::None:
        break;
    }
    return {};
}

QHash<qint64, ThreadState> parseThreadStates(const QByteArray &json, QString *nextCursor)
{
    QHash<qint64, ThreadState> out;
    const QJsonObject threads = QJsonDocument::fromJson(json)
                                    .object()
                                    .value(QStringLiteral("data"))
                                    .toObject()
                                    .value(QStringLiteral("repository"))
                                    .toObject()
                                    .value(QStringLiteral("pullRequest"))
                                    .toObject()
                                    .value(QStringLiteral("reviewThreads"))
                                    .toObject();
    for (const QJsonValue &v : threads.value(QStringLiteral("nodes")).toArray()) {
        const QJsonObject t = v.toObject();
        const QJsonArray first = t.value(QStringLiteral("comments")).toObject().value(QStringLiteral("nodes")).toArray();
        const qint64 id = first.isEmpty() ? 0 : qint64(first.first().toObject().value(QStringLiteral("databaseId")).toDouble());
        if (id > 0)
            out.insert(id, {t.value(QStringLiteral("id")).toString(), t.value(QStringLiteral("isResolved")).toBool()});
    }
    if (nextCursor) {
        const QJsonObject page = threads.value(QStringLiteral("pageInfo")).toObject();
        *nextCursor = page.value(QStringLiteral("hasNextPage")).toBool() ? page.value(QStringLiteral("endCursor")).toString() : QString();
    }
    return out;
}

QString displayTime(const QString &iso)
{
    QDateTime t = QDateTime::fromString(iso, Qt::ISODateWithMs);
    if (!t.isValid())
        t = QDateTime::fromString(iso, Qt::ISODate);
    if (!t.isValid())
        return iso;
    return t.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QVector<Comment> parseComments(const QByteArray &json)
{
    QVector<Comment> out;
    const QJsonArray top = QJsonDocument::fromJson(json).array();
    for (const QJsonValue &v : top) {
        if (v.isArray()) { // --slurp: one array per page
            for (const QJsonValue &c : v.toArray())
                out.push_back(fromJson(c.toObject()));
        } else {
            out.push_back(fromJson(v.toObject()));
        }
    }
    return out;
}

namespace {
// A list, or gh --slurp's list of pages, as one list.
QJsonArray flatten(const QByteArray &json)
{
    QJsonArray all;
    for (const QJsonValue &v : QJsonDocument::fromJson(json).array()) {
        if (v.isArray())
            for (const QJsonValue &x : v.toArray())
                all.append(x);
        else
            all.append(v);
    }
    return all;
}
} // namespace

QString lastReviewedCommit(const QByteArray &reviewsJson, const QString &login)
{
    QString commit;
    for (const QJsonValue &v : flatten(reviewsJson)) { // oldest first
        const QJsonObject r = v.toObject();
        if (r.value(QStringLiteral("user")).toObject().value(QStringLiteral("login")).toString().compare(login, Qt::CaseInsensitive) != 0
            || r.value(QStringLiteral("state")).toString() == QLatin1String("PENDING"))
            continue;
        if (const QString c = r.value(QStringLiteral("commit_id")).toString(); !c.isEmpty())
            commit = c;
    }
    return commit;
}

QVector<PrCommit> parseCommits(const QByteArray &json)
{
    QVector<PrCommit> out;
    for (const QJsonValue &v : flatten(json)) {
        const QJsonObject o = v.toObject();
        const QJsonObject commit = o.value(QStringLiteral("commit")).toObject();
        PrCommit c;
        c.sha = o.value(QStringLiteral("sha")).toString();
        c.subject = commit.value(QStringLiteral("message")).toString().section(u'\n', 0, 0);
        c.date = commit.value(QStringLiteral("committer")).toObject().value(QStringLiteral("date")).toString();
        if (!c.sha.isEmpty())
            out.push_back(c);
    }
    return out;
}

Vote latestVote(const QByteArray &reviewsJson, const QString &login)
{
    const QJsonArray reviews = flatten(reviewsJson);
    Vote vote = Vote::None;
    for (const QJsonValue &v : std::as_const(reviews)) { // oldest first
        const QJsonObject r = v.toObject();
        if (r.value(QStringLiteral("user")).toObject().value(QStringLiteral("login")).toString().compare(login, Qt::CaseInsensitive) != 0)
            continue;
        const QString state = r.value(QStringLiteral("state")).toString();
        if (state == QLatin1String("APPROVED"))
            vote = Vote::Approved;
        else if (state == QLatin1String("CHANGES_REQUESTED"))
            vote = Vote::Rejected;
        else if (state == QLatin1String("DISMISSED"))
            vote = Vote::None;
    }
    return vote;
}

QVector<Thread> threads(const QVector<Comment> &comments)
{
    QMap<qint64, int> rootIndex;
    QVector<Thread> out;
    QVector<Comment> sorted = comments;
    std::stable_sort(sorted.begin(), sorted.end(), [](const Comment &a, const Comment &b) { return a.createdAt < b.createdAt; });
    for (const Comment &c : std::as_const(sorted)) {
        if (c.inReplyTo == 0 || !rootIndex.contains(c.inReplyTo)) {
            rootIndex.insert(c.id, int(out.size()));
            out.push_back({c});
        } else {
            out[rootIndex.value(c.inReplyTo)].push_back(c);
            rootIndex.insert(c.id, rootIndex.value(c.inReplyTo)); // replies to replies
        }
    }
    return out;
}

QByteArray commentPayload(const QString &commitId, const QString &path, const QString &side, int line, int startLine,
                          const QString &body)
{
    QJsonObject o{{QStringLiteral("body"), body},
                  {QStringLiteral("commit_id"), commitId},
                  {QStringLiteral("path"), path},
                  {QStringLiteral("line"), line},
                  {QStringLiteral("side"), side}};
    if (startLine > 0 && startLine < line) {
        o.insert(QStringLiteral("start_line"), startLine);
        o.insert(QStringLiteral("start_side"), side);
    }
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QByteArray reviewPayload(Verdict verdict, const QString &body)
{
    const char *event = verdict == Verdict::Approve ? "APPROVE" : verdict == Verdict::RequestChanges ? "REQUEST_CHANGES" : "COMMENT";
    QJsonObject o{{QStringLiteral("event"), QString::fromLatin1(event)}};
    if (!body.isEmpty())
        o.insert(QStringLiteral("body"), body);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

namespace {

// Who gh is signed in as doesn't change while yadiff runs: ask once.
std::optional<QString> myLogin(QString *error)
{
    static QMutex mutex;
    static QString login;
    QMutexLocker lock(&mutex);
    if (login.isEmpty()) {
        QByteArray out;
        if (!gh({QStringLiteral("api"), QStringLiteral("user"), QStringLiteral("--jq"), QStringLiteral(".login")}, {}, &out, error))
            return std::nullopt;
        login = QString::fromUtf8(out).trimmed();
    }
    return login;
}

std::optional<QByteArray> reviews(const PrId &pr, QString *error)
{
    QByteArray out;
    if (!gh({QStringLiteral("api"), QStringLiteral("--paginate"), QStringLiteral("--slurp"), base(pr) + QStringLiteral("/reviews")},
            {}, &out, error))
        return std::nullopt;
    return out;
}

} // namespace

std::optional<Vote> myVote(const PrId &pr, QString *error)
{
    const auto login = myLogin(error);
    const auto json = login ? reviews(pr, error) : std::nullopt;
    return json ? std::optional<Vote>(latestVote(*json, *login)) : std::nullopt;
}

std::optional<QString> myLastReviewedCommit(const PrId &pr, QString *error)
{
    const auto login = myLogin(error);
    const auto json = login ? reviews(pr, error) : std::nullopt;
    return json ? std::optional<QString>(lastReviewedCommit(*json, *login)) : std::nullopt;
}

std::optional<QByteArray> overviewJson(const PrId &pr, QString *error)
{
    const QString url = QStringLiteral("https://github.com/%1/%2/pull/%3").arg(pr.owner, pr.repo).arg(pr.number);
    QString fields = QStringLiteral("title,body,author,state,isDraft,baseRefName,headRefName,createdAt,url,reviewRequests,"
                                    "latestReviews,statusCheckRollup,comments,closingIssuesReferences");
    QByteArray out;
    QString err;
    if (gh({QStringLiteral("pr"), QStringLiteral("view"), url, QStringLiteral("--json"), fields}, {}, &out, &err))
        return out;
    // Older gh doesn't know closingIssuesReferences.
    if (!err.contains(QLatin1String("closingIssuesReferences"))) {
        if (error)
            *error = err;
        return std::nullopt;
    }
    fields.remove(QStringLiteral(",closingIssuesReferences"));
    if (!gh({QStringLiteral("pr"), QStringLiteral("view"), url, QStringLiteral("--json"), fields}, {}, &out, error))
        return std::nullopt;
    return out;
}

std::optional<QVector<PrCommit>> commits(const PrId &pr, QString *error)
{
    QByteArray out;
    if (!gh({QStringLiteral("api"), QStringLiteral("--paginate"), QStringLiteral("--slurp"), base(pr) + QStringLiteral("/commits")},
            {}, &out, error))
        return std::nullopt;
    return parseCommits(out);
}

namespace {

// Whether each thread is resolved: only GitHub's GraphQL API says. Empty when it fails
// (the comments still show, without their state).
QHash<qint64, ThreadState> fetchThreadStates(const PrId &pr)
{
    static const QString query = QStringLiteral(
        "query($owner:String!,$repo:String!,$number:Int!,$after:String){repository(owner:$owner,name:$repo){"
        "pullRequest(number:$number){reviewThreads(first:100,after:$after){pageInfo{hasNextPage endCursor}"
        "nodes{id isResolved comments(first:1){nodes{databaseId}}}}}}}");
    QHash<qint64, ThreadState> all;
    QString cursor;
    for (int page = 0; page < 50; ++page) {
        QStringList args{QStringLiteral("api"), QStringLiteral("graphql"), QStringLiteral("-f"), QStringLiteral("query=") + query,
                         QStringLiteral("-f"), QStringLiteral("owner=") + pr.owner, QStringLiteral("-f"),
                         QStringLiteral("repo=") + pr.repo, QStringLiteral("-F"), QStringLiteral("number=%1").arg(pr.number)};
        if (!cursor.isEmpty())
            args << QStringLiteral("-f") << QStringLiteral("after=") + cursor;
        QByteArray out;
        QString error;
        if (!gh(args, {}, &out, &error)) {
            qCWarning(lcGitHub).noquote() << "could not read which review threads are resolved:" << error;
            return {};
        }
        all.insert(parseThreadStates(out, &cursor));
        if (cursor.isEmpty())
            break;
    }
    return all;
}

} // namespace

std::optional<QVector<Comment>> fetch(const PrId &pr, QString *error)
{
    QByteArray out;
    if (!gh({QStringLiteral("api"), QStringLiteral("--paginate"), QStringLiteral("--slurp"), base(pr) + QStringLiteral("/comments")},
            {}, &out, error))
        return std::nullopt;
    QVector<Comment> comments = parseComments(out);
    const QHash<qint64, ThreadState> states = fetchThreadStates(pr);
    for (Comment &c : comments) {
        if (const auto it = states.constFind(c.id); it != states.constEnd() && c.inReplyTo == 0) {
            c.threadNode = it->node;
            c.status = it->resolved ? ThreadStatus::Resolved : ThreadStatus::Active;
        }
    }
    return comments;
}

bool setResolved(const QString &threadNode, bool resolved, QString *error)
{
    if (threadNode.isEmpty()) {
        if (error)
            *error = QStringLiteral("GitHub did not say which thread this is; reload the comments and try again.");
        return false;
    }
    const QString mutation = resolved
                                 ? QStringLiteral("mutation($id:ID!){resolveReviewThread(input:{threadId:$id}){thread{isResolved}}}")
                                 : QStringLiteral("mutation($id:ID!){unresolveReviewThread(input:{threadId:$id}){thread{isResolved}}}");
    QByteArray out;
    if (!gh({QStringLiteral("api"), QStringLiteral("graphql"), QStringLiteral("-f"), QStringLiteral("query=") + mutation,
             QStringLiteral("-f"), QStringLiteral("id=") + threadNode},
            {}, &out, error))
        return false;
    const QJsonArray errors = QJsonDocument::fromJson(out).object().value(QStringLiteral("errors")).toArray();
    if (!errors.isEmpty()) {
        if (error)
            *error = errors.first().toObject().value(QStringLiteral("message")).toString();
        return false;
    }
    return true;
}

std::optional<Comment> post(const PrId &pr, const QString &commitId, const QString &path, const QString &side, int line,
                            int startLine, const QString &body, QString *error)
{
    QByteArray out;
    if (!gh({QStringLiteral("api"), QStringLiteral("-X"), QStringLiteral("POST"), base(pr) + QStringLiteral("/comments"),
             QStringLiteral("--input"), QStringLiteral("-")},
            commentPayload(commitId, path, side, line, startLine, body), &out, error))
        return std::nullopt;
    return fromJson(QJsonDocument::fromJson(out).object());
}

std::optional<Comment> reply(const PrId &pr, qint64 threadId, const QString &body, QString *error)
{
    QByteArray out;
    const QByteArray payload = QJsonDocument(QJsonObject{{QStringLiteral("body"), body}}).toJson(QJsonDocument::Compact);
    if (!gh({QStringLiteral("api"), QStringLiteral("-X"), QStringLiteral("POST"),
             base(pr) + QStringLiteral("/comments/%1/replies").arg(threadId), QStringLiteral("--input"), QStringLiteral("-")},
            payload, &out, error))
        return std::nullopt;
    return fromJson(QJsonDocument::fromJson(out).object());
}

bool submitReview(const PrId &pr, Verdict verdict, const QString &body, QString *error)
{
    return gh({QStringLiteral("api"), QStringLiteral("-X"), QStringLiteral("POST"), base(pr) + QStringLiteral("/reviews"),
               QStringLiteral("--input"), QStringLiteral("-")},
              reviewPayload(verdict, body), nullptr, error);
}

} // namespace GitHubReview
