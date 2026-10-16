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
};
// Fetches the PR into the user's clone (when a remote matches) or yadiff's cache.
std::optional<Resolved> resolve(const Ref &ref, const AdoAuth::Credential &cred, const QString &localDir, QString *error,
                                int *httpStatus = nullptr, const std::function<void(const QString &)> &progress = {});

// Review threads, in yadiff's common comment model. Thread roots use the thread id as
// their id; replies are numbered within it (see kReplyBase), so replies go to the thread.
constexpr qint64 kReplyBase = 100000;
QVector<GitHubReview::Comment> parseThreads(const QByteArray &json, const QUrl &webUrl);
QByteArray threadPayload(const QString &path, const QString &side, int line, int startLine, const QString &body);

std::optional<QVector<GitHubReview::Comment>> fetchComments(const Ref &ref, const AdoAuth::Credential &cred, QString *error);
std::optional<GitHubReview::Comment> post(const Ref &ref, const AdoAuth::Credential &cred, const QString &path,
                                          const QString &side, int line, int startLine, const QString &body, QString *error);
std::optional<GitHubReview::Comment> reply(const Ref &ref, const AdoAuth::Credential &cred, qint64 threadId,
                                           const QString &body, QString *error);
// Approve = vote 10; RequestChanges = vote -5 ("waiting for author"); a message becomes
// a pull-request-level comment thread.
bool submit(const Ref &ref, const AdoAuth::Credential &cred, GitHubReview::Verdict verdict, const QString &body, QString *error);

} // namespace AdoPr
