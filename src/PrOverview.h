#pragma once

#include "GitHubReview.h"

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QVector>

// What a pull request's overview page shows, in terms both hosts share: its description,
// who reviews it and how they voted, its checks (Azure DevOps policies and statuses,
// GitHub check runs), linked work items or issues, and the comments on the PR itself.
namespace PrOverview {

struct Reviewer {
    QString name;
    GitHubReview::Vote vote = GitHubReview::Vote::None;
    bool required = false;
    bool group = false;    // a team or group, not a person
    bool declined = false; // Azure DevOps: declined to review
};

enum class CheckState { Passed, Failed, Running, Queued, Skipped, Unknown };
struct Check {
    QString name, detail, url;
    CheckState state = CheckState::Unknown;
    bool required = false; // blocks completing the PR
};

struct WorkItem {
    QString id, type, title, state, url;
};

struct Overview {
    QString title, author, description, status, sourceBranch, targetBranch, created, url;
    bool draft = false;
    QVector<Reviewer> reviewers;
    QVector<Check> checks;
    QVector<WorkItem> workItems;
    QVector<GitHubReview::Thread> discussion; // comments on the PR, not on a line
    QStringList problems;                     // parts that could not be read
};

QString checkStateName(CheckState state);

// Azure DevOps (exposed for tests). `webBase` is the collection URL plus the project,
// for links.
Overview parseAdoPullRequest(const QByteArray &json);
QVector<Check> parseAdoPolicies(const QByteArray &json, const QString &webBase);
QVector<Check> parseAdoStatuses(const QByteArray &json);
QVector<WorkItem> parseAdoWorkItems(const QByteArray &json, const QString &webBase);
// Threads not attached to a file, without the ones Azure DevOps writes itself (votes,
// pushes, policy updates).
QVector<GitHubReview::Thread> parseAdoDiscussion(const QByteArray &threadsJson, const QUrl &webUrl);

// GitHub: `gh pr view --json` with the fields fetch() asks for.
Overview parseGitHubPullRequest(const QByteArray &json);

} // namespace PrOverview
