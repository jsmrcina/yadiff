#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVector>

#include <optional>

// Pull request review comments through the `gh` CLI (which handles authentication).
namespace GitHubReview {

struct PrId {
    QString owner, repo;
    int number = 0;
    bool isValid() const { return number > 0 && !owner.isEmpty() && !repo.isEmpty(); }
};

// A review thread's state. GitHub only resolves threads (Active or Resolved); Azure
// DevOps has them all. None: the host didn't say.
enum class ThreadStatus { None, Active, Pending, Resolved, WontFix, ByDesign, Closed };
// Still to be dealt with: no status, active or pending.
bool isOpen(ThreadStatus status);
QString statusName(ThreadStatus status); // "Active", "Resolved", "Won't fix", ...

struct Comment {
    qint64 id = 0;
    qint64 inReplyTo = 0;  // 0 for the first comment of a thread
    QString path;
    QString side;          // "RIGHT" (new side) or "LEFT" (old side)
    int line = 0;          // last line the comment covers; 0 when outdated
    int startLine = 0;     // first line of a multi-line comment, 0 when single-line
    QString author, body, createdAt, url;
    // The thread's state, on its first comment; threadNode is GitHub's id for the thread
    // (needed to resolve it).
    ThreadStatus status = ThreadStatus::None;
    QString threadNode;
    bool outdated() const { return line <= 0; }
};

using Thread = QVector<Comment>; // first comment, then replies in order

// "2026-10-12T14:03:21Z" -> "2026-10-12 16:03" in local time (falls back to the input).
QString displayTime(const QString &iso);

enum class Verdict { Comment, Approve, RequestChanges };

// A reviewer's vote, in the terms both hosts share. GitHub knows None, Approved and
// Rejected ("changes requested"); Azure DevOps has all five.
enum class Vote { None, Approved, ApprovedWithSuggestions, WaitingForAuthor, Rejected };

// `login`'s vote from GitHub's reviews JSON (a list, or gh --slurp's list of pages): their
// latest approval, change request or dismissal decides; plain comments don't change it.
Vote latestVote(const QByteArray &reviewsJson, const QString &login);

// The commit `login` last reviewed (their latest submitted review's commit_id), or empty.
QString lastReviewedCommit(const QByteArray &reviewsJson, const QString &login);

// A commit of the pull request (GET .../pulls/N/commits), oldest first.
struct PrCommit {
    QString sha, subject, date;
};
QVector<PrCommit> parseCommits(const QByteArray &json);

// Parses GitHub's JSON (a list, or gh --slurp's list of pages).
QVector<Comment> parseComments(const QByteArray &json);
// Groups replies under their first comment, threads in creation order.
QVector<Thread> threads(const QVector<Comment> &comments);

// GitHub's GraphQL reviewThreads page: the first comment's id -> (thread id, resolved).
// `nextCursor` is set when there's another page.
struct ThreadState {
    QString node;
    bool resolved = false;
};
QHash<qint64, ThreadState> parseThreadStates(const QByteArray &json, QString *nextCursor = nullptr);

// Request bodies (exposed for tests).
QByteArray commentPayload(const QString &commitId, const QString &path, const QString &side, int line, int startLine,
                          const QString &body);
QByteArray reviewPayload(Verdict verdict, const QString &body);

std::optional<QVector<Comment>> fetch(const PrId &pr, QString *error);
std::optional<Comment> post(const PrId &pr, const QString &commitId, const QString &path, const QString &side, int line,
                            int startLine, const QString &body, QString *error);
std::optional<Comment> reply(const PrId &pr, qint64 threadId, const QString &body, QString *error);
// Resolves or unresolves a thread (`threadNode` from the fetched comments).
bool setResolved(const QString &threadNode, bool resolved, QString *error);
bool submitReview(const PrId &pr, Verdict verdict, const QString &body, QString *error);
// The signed-in gh user's current vote on the PR.
std::optional<Vote> myVote(const PrId &pr, QString *error);
// The PR's overview (gh pr view), as PrOverview::parseGitHubPullRequest reads it.
std::optional<QByteArray> overviewJson(const PrId &pr, QString *error);
// The PR's commits, and the one the signed-in user last reviewed (empty when none).
std::optional<QVector<PrCommit>> commits(const PrId &pr, QString *error);
std::optional<QString> myLastReviewedCommit(const PrId &pr, QString *error);

} // namespace GitHubReview
