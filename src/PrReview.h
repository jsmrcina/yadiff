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

} // namespace PrReview
