#pragma once

#include "AdoAuth.h"
#include "GitHubReview.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include <optional>

// Pull requests waiting for the user's review: on Azure DevOps, the active PRs in the
// projects yadiff has seen where they (or one of their teams) are a reviewer; on GitHub,
// open PRs that ask for their review or that they reviewed before.
namespace PrInbox {

struct Item {
    QString host;   // "ado" or "github"
    QString link;   // the PR's web page (what yadiff -ado / -ghpr opens)
    QString repo;   // "PROJECT/REPO" or "owner/repo"
    int number = 0;
    QString title, author, created;
    bool draft = false;
    GitHubReview::Vote myVote = GitHubReview::Vote::None;
    bool reviewed = false; // GitHub: the user has reviewed it before
    bool required = false; // Azure DevOps: the user's review is required
    QString via;           // a team the review was asked of, when not the user directly
    QString headCommit;    // the PR's head now (to tell whether it has new pushes)
};

// An Azure DevOps project to look in.
struct AdoSource {
    QUrl collection;
    QString project;
    bool operator==(const AdoSource &o) const { return collection == o.collection && project == o.project; }
};
// https://dev.azure.com/ORG/PROJECT[/...], https://ORG.visualstudio.com/PROJECT[/...]
// (and Azure DevOps Server collection/project URLs when they contain /_git/ or a PR).
std::optional<AdoSource> parseSource(const QString &url);
// Projects to look in: every one a PR was opened from in yadiff, and any added by hand.
QVector<AdoSource> sources();
void addSource(const AdoSource &s);
void removeSource(const AdoSource &s);

// Remembers the head commit last seen of a PR (shown in yadiff), and reads it back.
void markSeen(const QString &link, const QString &headCommit);
QString lastSeen(const QString &link);

// Parsers (exposed for tests). `reviewerIds`: the user's id first, then their teams'
// (id -> team name).
QVector<Item> parseAdoPullRequests(const QByteArray &json, const AdoSource &source, const QString &userId,
                                   const QHash<QString, QString> &teams);
QHash<QString, QString> parseAdoTeams(const QByteArray &json);
QVector<Item> parseGitHubSearch(const QByteArray &json);

struct Listing {
    QVector<Item> items;
    QStringList problems; // sources that could not be read, and why
};
// Asks every source (slow: in the background). Without a credential for an Azure DevOps
// organisation, that source is skipped with a problem saying how to sign in; without
// the GitHub CLI, GitHub is skipped.
Listing fetchAll(const QVector<AdoSource> &ado, bool github);

} // namespace PrInbox
