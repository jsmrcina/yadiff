#pragma once

#include "AdoAuth.h"
#include "GitHubReview.h"

#include <QString>
#include <QUrl>
#include <QVector>

#include <functional>
#include <optional>

// Azure DevOps pull requests: link parsing, fetching the PR into a repository, and
// review threads (read, comment, reply, vote) through the REST API.
namespace AdoPr {

struct Ref {
    QUrl collectionUrl; // https://dev.azure.com/ORG, https://ORG.visualstudio.com, https://server/tfs/Collection
    QString project;
    QString repo;
    int id = 0;
    QUrl webUrl;        // the pull request page
};

// https://dev.azure.com/ORG/PROJECT/_git/REPO/pullrequest/ID (any trailing path or query),
// https://ORG.visualstudio.com/[DefaultCollection/]PROJECT/_git/REPO/pullrequest/ID,
// and Azure DevOps Server collection URLs.
std::optional<Ref> parse(const QString &link);

// True when a git remote URL is this repository (HTTPS or SSH spellings).
bool remoteMatches(const QString &remoteUrl, const Ref &ref);

struct Info {
    QString title, status;
    QString sourceRef, targetRef;       // refs/heads/...
    QString sourceCommit, targetCommit; // lastMergeSourceCommit / lastMergeTargetCommit
    QString remoteUrl;
};
std::optional<Info> info(const Ref &ref, const AdoAuth::Credential &cred, QString *error, int *httpStatus = nullptr);

struct Resolved {
    QString repoPath, baseOid, headOid, baseName, headName, title;
    // Without a clone of the repository: the PR's changed files, downloaded through the
    // REST API into yadiff's cache, as two folders (merge base and PR). repoPath is their parent.
    QString baseDir, headDir;
};

// The REST API's view of a pull request (exposed for tests).
struct Iteration {
    int id = 0;
    QString headCommit, baseCommit; // the PR's head and its merge base with the target
};
// The latest iteration in GET .../pullRequests/ID/iterations.
std::optional<Iteration> parseLatestIteration(const QByteArray &json);
struct Change {
    QString path, oldPath;          // repository-relative; path empty when deleted, oldPath when added
    QString objectId, oldObjectId;  // blob ids of the two versions
};
// GET .../iterations/N/changes?$compareTo=0: the files that differ from the merge base.
// `nextSkip` is where the next page starts (0: none).
QVector<Change> parseChanges(const QByteArray &json, int *nextSkip = nullptr);
// A path from the API ("/dir/file.txt") as one that is safe to write under a folder:
// relative, without ".." or a drive letter. Empty when it isn't.
QString safeRelativePath(const QString &apiPath);
// Downloads the files the PR changes (both versions) into yadiff's cache.
std::optional<Resolved> download(const Ref &ref, const AdoAuth::Credential &cred, const QString &title,
                                 const QString &sourceRef, const QString &targetRef, QString *error,
                                 const std::function<void(const QString &)> &progress = {});

// Fetches the PR into the user's clone (when a remote matches); without one, downloads
// its changed files through the REST API (a clone of a large repository's history is
// far too big to fetch).
std::optional<Resolved> resolve(const Ref &ref, const AdoAuth::Credential &cred, const QString &localDir, QString *error,
                                int *httpStatus = nullptr, const std::function<void(const QString &)> &progress = {});

// Review threads, in yadiff's common comment model. Thread roots use the thread id as
// their id; replies are numbered within it (see kReplyBase), so replies go to the thread.
constexpr qint64 kReplyBase = 100000;
QVector<GitHubReview::Comment> parseThreads(const QByteArray &json, const QUrl &webUrl);
QByteArray threadPayload(const QString &path, const QString &side, int line, int startLine, const QString &body);

// Thread status in the API's words ("active", "fixed", "wontFix", "closed", "byDesign",
// "pending"), and back.
GitHubReview::ThreadStatus parseThreadStatus(const QString &name);
QString threadStatusName(GitHubReview::ThreadStatus status);
bool setThreadStatus(const Ref &ref, const AdoAuth::Credential &cred, qint64 threadId, GitHubReview::ThreadStatus status,
                     QString *error);

std::optional<QVector<GitHubReview::Comment>> fetchComments(const Ref &ref, const AdoAuth::Credential &cred, QString *error);
std::optional<GitHubReview::Comment> post(const Ref &ref, const AdoAuth::Credential &cred, const QString &path,
                                          const QString &side, int line, int startLine, const QString &body, QString *error);
std::optional<GitHubReview::Comment> reply(const Ref &ref, const AdoAuth::Credential &cred, qint64 threadId,
                                           const QString &body, QString *error);
// Votes: 10 approved, 5 approved with suggestions, 0 no vote, -5 waiting for author,
// -10 rejected. `userId`'s vote in the reviewers JSON (0 when they aren't a reviewer).
int parseVote(const QByteArray &reviewersJson, const QString &userId);
std::optional<int> myVote(const Ref &ref, const AdoAuth::Credential &cred, QString *error);
bool setVote(const Ref &ref, const AdoAuth::Credential &cred, int vote, QString *error);
// A pull-request-level comment thread.
bool postMessage(const Ref &ref, const AdoAuth::Credential &cred, const QString &body, QString *error);

// Approve = vote 10; RequestChanges = vote -5 ("waiting for author"); a message becomes
// a pull-request-level comment thread.
bool submit(const Ref &ref, const AdoAuth::Credential &cred, GitHubReview::Verdict verdict, const QString &body, QString *error);

} // namespace AdoPr
