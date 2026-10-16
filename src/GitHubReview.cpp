#include "GitHubReview.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QProcess>

#include <algorithm>

namespace {

bool gh(const QStringList &args, const QByteArray &input, QByteArray *out, QString *error)
{
    QProcess p;
    p.start(QStringLiteral("gh"), args);
    if (!p.waitForStarted()) {
        if (error)
            *error = QStringLiteral("Could not start gh. Install the GitHub CLI and run `gh auth login`.");
        return false;
    }
    if (!input.isEmpty())
        p.write(input);
    p.closeWriteChannel();
    p.waitForFinished(-1);
    if (out)
        *out = p.readAllStandardOutput();
    if (p.exitCode() != 0) {
        if (error) {
            *error = QString::fromUtf8(p.readAllStandardError()).trimmed();
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

std::optional<QVector<Comment>> fetch(const PrId &pr, QString *error)
{
    QByteArray out;
    if (!gh({QStringLiteral("api"), QStringLiteral("--paginate"), QStringLiteral("--slurp"), base(pr) + QStringLiteral("/comments")},
            {}, &out, error))
        return std::nullopt;
    return parseComments(out);
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
