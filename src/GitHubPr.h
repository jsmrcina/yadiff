#pragma once

#include "GitRepo.h"

#include <QString>

#include <functional>
#include <optional>

// Opening a GitHub pull request: resolves it with the `gh` CLI (falling back to
// GitHub's pull refs) and fetches it into a local repository.
namespace GitHubPr {

struct Ref {
    QString owner;
    QString repo;
    int number = 0;
};

struct Resolved {
    Ref ref;
    QString repoPath;   // repository to open (the user's clone, or the yadiff cache)
    QString baseOid;    // merge base of the PR's base branch and its head
    QString headOid;
    QString baseName;   // for display
    QString headName;
    QString title;
};

// Accepts https://github.com/OWNER/REPO/pull/N (with any trailing /files etc.),
// github.com/OWNER/REPO/pull/N and OWNER/REPO#N.
std::optional<Ref> parse(const QString &link);

// `localDir` is where yadiff was started; its repository is used when one of its
// remotes is the PR's repository. `progress` receives status lines.
std::optional<Resolved> resolve(const Ref &ref, const QString &localDir, QString *error,
                                const std::function<void(const QString &)> &progress = {});

// True when a git remote URL points at github.com/OWNER/REPO.
bool remoteMatches(const QString &url, const QString &owner, const QString &repo);

} // namespace GitHubPr
