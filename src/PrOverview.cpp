#include "PrOverview.h"

#include "AdoPr.h"
#include "PrReview.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace PrOverview {

namespace {

QString shortRef(const QString &ref)
{
    return ref.startsWith(QLatin1String("refs/heads/")) ? ref.mid(11) : ref;
}

QJsonArray values(const QByteArray &json)
{
    return QJsonDocument::fromJson(json).object().value(QStringLiteral("value")).toArray();
}

} // namespace

QString checkStateName(CheckState state)
{
    switch (state) {
    case CheckState::Passed:
        return QStringLiteral("Passed");
    case CheckState::Failed:
        return QStringLiteral("Failed");
    case CheckState::Running:
        return QStringLiteral("Running");
    case CheckState::Queued:
        return QStringLiteral("Queued");
    case CheckState::Skipped:
        return QStringLiteral("Not applicable");
    case CheckState::Unknown:
        break;
    }
    return QStringLiteral("Unknown");
}

Overview parseAdoPullRequest(const QByteArray &json)
{
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    Overview out;
    out.title = o.value(QStringLiteral("title")).toString();
    out.description = o.value(QStringLiteral("description")).toString();
    out.author = o.value(QStringLiteral("createdBy")).toObject().value(QStringLiteral("displayName")).toString();
    out.created = o.value(QStringLiteral("creationDate")).toString();
    out.status = o.value(QStringLiteral("status")).toString();
    out.draft = o.value(QStringLiteral("isDraft")).toBool();
    out.sourceBranch = shortRef(o.value(QStringLiteral("sourceRefName")).toString());
    out.targetBranch = shortRef(o.value(QStringLiteral("targetRefName")).toString());
    for (const QJsonValue &v : o.value(QStringLiteral("reviewers")).toArray()) {
        const QJsonObject r = v.toObject();
        Reviewer rv;
        rv.name = r.value(QStringLiteral("displayName")).toString();
        rv.vote = PrReview::adoVote(r.value(QStringLiteral("vote")).toInt());
        rv.required = r.value(QStringLiteral("isRequired")).toBool();
        rv.group = r.value(QStringLiteral("isContainer")).toBool();
        rv.declined = r.value(QStringLiteral("hasDeclined")).toBool();
        out.reviewers.push_back(rv);
    }
    // Required first, then those who voted, then by name.
    std::stable_sort(out.reviewers.begin(), out.reviewers.end(), [](const Reviewer &a, const Reviewer &b) {
        if (a.required != b.required)
            return a.required;
        if ((a.vote != GitHubReview::Vote::None) != (b.vote != GitHubReview::Vote::None))
            return a.vote != GitHubReview::Vote::None;
        return a.name.localeAwareCompare(b.name) < 0;
    });
    return out;
}

QVector<Check> parseAdoPolicies(const QByteArray &json, const QString &webBase)
{
    QVector<Check> out;
    for (const QJsonValue &v : values(json)) {
        const QJsonObject e = v.toObject();
        const QJsonObject config = e.value(QStringLiteral("configuration")).toObject();
        if (config.contains(QStringLiteral("isEnabled")) && !config.value(QStringLiteral("isEnabled")).toBool())
            continue;
        const QJsonObject settings = config.value(QStringLiteral("settings")).toObject();
        const QJsonObject context = e.value(QStringLiteral("context")).toObject();
        Check c;
        c.name = settings.value(QStringLiteral("displayName")).toString();
        if (c.name.isEmpty())
            c.name = context.value(QStringLiteral("buildDefinitionName")).toString();
        const QString type = config.value(QStringLiteral("type")).toObject().value(QStringLiteral("displayName")).toString();
        if (c.name.isEmpty())
            c.name = type;
        else if (!type.isEmpty() && type != c.name)
            c.detail = type;
        c.required = config.value(QStringLiteral("isBlocking")).toBool();
        const QString status = e.value(QStringLiteral("status")).toString().toLower();
        c.state = status == QLatin1String("approved")        ? CheckState::Passed
                : status == QLatin1String("rejected")        ? CheckState::Failed
                : status == QLatin1String("broken")          ? CheckState::Failed
                : status == QLatin1String("running")         ? CheckState::Running
                : status == QLatin1String("queued")          ? CheckState::Queued
                : status == QLatin1String("notapplicable")   ? CheckState::Skipped
                                                             : CheckState::Unknown;
        if (context.value(QStringLiteral("isExpired")).toBool() && c.state == CheckState::Passed) {
            c.state = CheckState::Queued;
            c.detail = QStringLiteral("expired");
        }
        if (const int build = context.value(QStringLiteral("buildId")).toInt(); build > 0 && !webBase.isEmpty())
            c.url = QStringLiteral("%1/_build/results?buildId=%2").arg(webBase).arg(build);
        out.push_back(c);
    }
    std::stable_sort(out.begin(), out.end(), [](const Check &a, const Check &b) { return a.required && !b.required; });
    return out;
}

QVector<Check> parseAdoStatuses(const QByteArray &json)
{
    // Every update is a new entry: keep the latest per context (the list is oldest first).
    QVector<Check> out;
    QHash<QString, int> at;
    for (const QJsonValue &v : values(json)) {
        const QJsonObject s = v.toObject();
        const QJsonObject context = s.value(QStringLiteral("context")).toObject();
        const QString genre = context.value(QStringLiteral("genre")).toString();
        const QString name = context.value(QStringLiteral("name")).toString();
        Check c;
        c.name = genre.isEmpty() ? name : genre + u'/' + name;
        c.detail = s.value(QStringLiteral("description")).toString();
        c.url = s.value(QStringLiteral("targetUrl")).toString();
        const QString state = s.value(QStringLiteral("state")).toString().toLower();
        c.state = state == QLatin1String("succeeded")       ? CheckState::Passed
                : state == QLatin1String("failed")          ? CheckState::Failed
                : state == QLatin1String("error")           ? CheckState::Failed
                : state == QLatin1String("pending")         ? CheckState::Running
                : state == QLatin1String("notapplicable")   ? CheckState::Skipped
                                                            : CheckState::Unknown;
        if (const auto it = at.constFind(c.name); it != at.constEnd())
            out[*it] = c;
        else {
            at.insert(c.name, int(out.size()));
            out.push_back(c);
        }
    }
    return out;
}

QVector<WorkItem> parseAdoWorkItems(const QByteArray &json, const QString &webBase)
{
    QVector<WorkItem> out;
    for (const QJsonValue &v : values(json)) {
        const QJsonObject o = v.toObject();
        const QJsonObject f = o.value(QStringLiteral("fields")).toObject();
        WorkItem w;
        w.id = QString::number(o.value(QStringLiteral("id")).toInteger());
        w.type = f.value(QStringLiteral("System.WorkItemType")).toString();
        w.title = f.value(QStringLiteral("System.Title")).toString();
        w.state = f.value(QStringLiteral("System.State")).toString();
        if (!webBase.isEmpty())
            w.url = QStringLiteral("%1/_workitems/edit/%2").arg(webBase, w.id);
        out.push_back(w);
    }
    return out;
}

QVector<GitHubReview::Thread> parseAdoDiscussion(const QByteArray &threadsJson, const QUrl &webUrl)
{
    // The thread parser's model, for the threads it leaves out: no file, people talking.
    QJsonArray general;
    for (const QJsonValue &tv : values(threadsJson)) {
        QJsonObject t = tv.toObject();
        if (t.value(QStringLiteral("isDeleted")).toBool()
            || !t.value(QStringLiteral("threadContext")).toObject().value(QStringLiteral("filePath")).toString().isEmpty())
            continue;
        bool people = false;
        for (const QJsonValue &cv : t.value(QStringLiteral("comments")).toArray())
            people |= cv.toObject().value(QStringLiteral("commentType")).toString() != QLatin1String("system")
                   && !cv.toObject().value(QStringLiteral("isDeleted")).toBool();
        if (!people)
            continue;
        // parseThreads skips threads without a file: give it one, then take it away.
        t.insert(QStringLiteral("threadContext"), QJsonObject{{QStringLiteral("filePath"), QStringLiteral("/")}});
        general.append(t);
    }
    QVector<GitHubReview::Comment> comments =
        AdoPr::parseThreads(QJsonDocument(QJsonObject{{QStringLiteral("value"), general}}).toJson(QJsonDocument::Compact), webUrl);
    for (GitHubReview::Comment &c : comments) {
        c.path.clear();
        c.line = 0;
        c.startLine = 0;
    }
    QVector<GitHubReview::Thread> threads = GitHubReview::threads(comments);
    std::reverse(threads.begin(), threads.end()); // newest first, as the overview lists them
    return threads;
}

Overview parseGitHubPullRequest(const QByteArray &json)
{
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    Overview out;
    out.title = o.value(QStringLiteral("title")).toString();
    out.description = o.value(QStringLiteral("body")).toString();
    out.author = o.value(QStringLiteral("author")).toObject().value(QStringLiteral("login")).toString();
    out.created = o.value(QStringLiteral("createdAt")).toString();
    out.status = o.value(QStringLiteral("state")).toString().toLower();
    out.draft = o.value(QStringLiteral("isDraft")).toBool();
    out.sourceBranch = o.value(QStringLiteral("headRefName")).toString();
    out.targetBranch = o.value(QStringLiteral("baseRefName")).toString();
    out.url = o.value(QStringLiteral("url")).toString();

    // Reviewers: those who reviewed (their latest decision), then those still asked.
    QHash<QString, int> seen;
    for (const QJsonValue &v : o.value(QStringLiteral("latestReviews")).toArray()) {
        const QJsonObject r = v.toObject();
        Reviewer rv;
        rv.name = r.value(QStringLiteral("author")).toObject().value(QStringLiteral("login")).toString();
        const QString state = r.value(QStringLiteral("state")).toString();
        rv.vote = state == QLatin1String("APPROVED")            ? GitHubReview::Vote::Approved
                : state == QLatin1String("CHANGES_REQUESTED")   ? GitHubReview::Vote::Rejected
                                                                : GitHubReview::Vote::None;
        if (rv.name.isEmpty() || seen.contains(rv.name))
            continue;
        seen.insert(rv.name, int(out.reviewers.size()));
        out.reviewers.push_back(rv);
    }
    for (const QJsonValue &v : o.value(QStringLiteral("reviewRequests")).toArray()) {
        const QJsonObject r = v.toObject();
        Reviewer rv;
        rv.group = r.value(QStringLiteral("__typename")).toString() == QLatin1String("Team");
        rv.name = rv.group ? r.value(QStringLiteral("name")).toString() : r.value(QStringLiteral("login")).toString();
        if (rv.name.isEmpty())
            rv.name = r.value(QStringLiteral("slug")).toString();
        if (!rv.name.isEmpty() && !seen.contains(rv.name)) {
            seen.insert(rv.name, int(out.reviewers.size()));
            out.reviewers.push_back(rv);
        }
    }

    for (const QJsonValue &v : o.value(QStringLiteral("statusCheckRollup")).toArray()) {
        const QJsonObject c = v.toObject();
        Check ch;
        if (c.value(QStringLiteral("__typename")).toString() == QLatin1String("StatusContext")) {
            ch.name = c.value(QStringLiteral("context")).toString();
            ch.url = c.value(QStringLiteral("targetUrl")).toString();
            ch.detail = c.value(QStringLiteral("description")).toString();
            const QString state = c.value(QStringLiteral("state")).toString();
            ch.state = state == QLatin1String("SUCCESS")                                       ? CheckState::Passed
                     : state == QLatin1String("FAILURE") || state == QLatin1String("ERROR")    ? CheckState::Failed
                     : state == QLatin1String("PENDING") || state == QLatin1String("EXPECTED") ? CheckState::Running
                                                                                               : CheckState::Unknown;
        } else {
            ch.name = c.value(QStringLiteral("name")).toString();
            const QString workflow = c.value(QStringLiteral("workflowName")).toString();
            if (!workflow.isEmpty() && workflow != ch.name)
                ch.name = workflow + QStringLiteral(" / ") + ch.name;
            ch.url = c.value(QStringLiteral("detailsUrl")).toString();
            const QString status = c.value(QStringLiteral("status")).toString();
            const QString conclusion = c.value(QStringLiteral("conclusion")).toString();
            if (status == QLatin1String("IN_PROGRESS"))
                ch.state = CheckState::Running;
            else if (status != QLatin1String("COMPLETED"))
                ch.state = CheckState::Queued;
            else if (conclusion == QLatin1String("SUCCESS"))
                ch.state = CheckState::Passed;
            else if (conclusion == QLatin1String("SKIPPED") || conclusion == QLatin1String("NEUTRAL"))
                ch.state = CheckState::Skipped;
            else if (conclusion == QLatin1String("FAILURE") || conclusion == QLatin1String("TIMED_OUT")
                     || conclusion == QLatin1String("CANCELLED") || conclusion == QLatin1String("ACTION_REQUIRED")
                     || conclusion == QLatin1String("STARTUP_FAILURE"))
                ch.state = CheckState::Failed;
            if (ch.state == CheckState::Failed && !conclusion.isEmpty() && conclusion != QLatin1String("FAILURE"))
                ch.detail = conclusion.toLower().replace(u'_', u' ');
        }
        if (!ch.name.isEmpty())
            out.checks.push_back(ch);
    }

    for (const QJsonValue &v : o.value(QStringLiteral("closingIssuesReferences")).toArray()) {
        const QJsonObject i = v.toObject();
        WorkItem w;
        w.id = QString::number(i.value(QStringLiteral("number")).toInt());
        w.type = QStringLiteral("Issue");
        w.title = i.value(QStringLiteral("title")).toString();
        w.url = i.value(QStringLiteral("url")).toString();
        out.workItems.push_back(w);
    }

    // PR-level comments: one-comment threads, newest first.
    qint64 id = 1;
    for (const QJsonValue &v : o.value(QStringLiteral("comments")).toArray()) {
        const QJsonObject c = v.toObject();
        GitHubReview::Comment cm;
        cm.id = id++;
        cm.author = c.value(QStringLiteral("author")).toObject().value(QStringLiteral("login")).toString();
        cm.body = c.value(QStringLiteral("body")).toString();
        cm.createdAt = c.value(QStringLiteral("createdAt")).toString();
        cm.url = c.value(QStringLiteral("url")).toString();
        out.discussion.prepend({cm});
    }
    return out;
}

} // namespace PrOverview
