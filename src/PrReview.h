#pragma once

#include "GitHubReview.h"
#include "GitRepo.h"

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
// Azure DevOps' numbers for the votes, and back.
int adoVoteValue(Vote vote);
Vote adoVote(int value);

} // namespace PrReview
