#pragma once

#include "GitHubReview.h"
#include "GitRepo.h"
#include "PrOverview.h"

#include <functional>
#include <optional>

// Review operations for whichever host the pull request lives on (GitHub or Azure DevOps).
namespace PrReview {

bool enabled(const Comparison &cmp);
QString hostName(const Comparison &cmp); // "GitHub" or "Azure DevOps"
QString webUrl(const Comparison &cmp);

std::optional<QVector<GitHubReview::Comment>> fetch(const Comparison &cmp, QString *error);
std::optional<GitHubReview::Comment> post(const Comparison &cmp, const QString &path, const QString &side, int line,
                                          int startLine, const QString &body, QString *error);
std::optional<GitHubReview::Comment> reply(const Comparison &cmp, qint64 threadId, const QString &body, QString *error);
bool submit(const Comparison &cmp, GitHubReview::Verdict verdict, const QString &body, QString *error);

// Thread status. The states this host offers, in menu order (GitHub: unresolved and
// resolved; Azure DevOps: active, pending, resolved, won't fix, by design, closed).
using ThreadStatus = GitHubReview::ThreadStatus;
QVector<ThreadStatus> threadStatuses(const Comparison &cmp);
QString threadStatusName(const Comparison &cmp, ThreadStatus status); // GitHub's Active is "Unresolved"
// `root` is the thread's first comment (as fetched).
bool setThreadStatus(const Comparison &cmp, const GitHubReview::Comment &root, ThreadStatus status, QString *error);

// Voting. The votes this host offers, in menu order: GitHub approves or requests
// changes; Azure DevOps also approves with suggestions, waits for the author, rejects,
// and resets (Vote::None).
using Vote = GitHubReview::Vote;
QVector<Vote> votes(const Comparison &cmp);
QString voteState(const Comparison &cmp, Vote vote);  // "Approved", "Changes requested", "No vote", ...
QString voteAction(const Comparison &cmp, Vote vote); // "Approve", "Request changes", "Reset vote", ...
bool voteNeedsMessage(const Comparison &cmp, Vote vote); // GitHub won't request changes without one
std::optional<Vote> myVote(const Comparison &cmp, QString *error);
// Casts `vote`; a non-empty `message` goes with it (GitHub: the review's text; Azure
// DevOps: a comment on the pull request).
bool setVote(const Comparison &cmp, Vote vote, const QString &message, QString *error);
// The pull request's overview page (in the background: several requests).
std::optional<PrOverview::Overview> overview(const Comparison &cmp, QString *error);

// The pull request's versions: Azure DevOps pushes (iterations), GitHub commits.
struct Version {
    int number = 0;                 // 1, 2, ... (Azure DevOps: the iteration id)
    QString headCommit, baseCommit; // baseCommit: the merge base at the time (Azure DevOps)
    QString date, subject;
};
struct History {
    QVector<Version> versions;  // oldest first; the last one is the PR as it is now
    int lastReviewed = 0;       // the version the user last voted on or reviewed (0: none)
    QString lastReviewedCommit; // GitHub: that commit, even when it has been pushed over
    // A version's position in `versions` (-1 when absent).
    int indexOf(int number) const;
};
std::optional<History> history(const Comparison &cmp, QString *error);
QString versionName(const Comparison &cmp, const Version &v); // "Push 3", "commit 1a2b3c4"
// The comparison from version `from` (0: the merge base; -1: GitHub's last reviewed
// commit) to version `to`, made from the comparison of the whole PR (`whole`, shown from
// the repository at `wholeRoot`). Fetches commits or downloads files: slow, so call it
// in the background. The repository to show it from goes in *repoRoot (a folder of
// downloaded files when the whole PR was downloaded).
std::optional<Comparison> versionComparison(const Comparison &whole, const QString &wholeRoot, const History &h, int from,
                                            int to, QString *repoRoot, QString *error,
                                            const std::function<void(const QString &)> &progress = {});

// Azure DevOps' numbers for the votes, and back.
int adoVoteValue(Vote vote);
Vote adoVote(int value);

} // namespace PrReview
