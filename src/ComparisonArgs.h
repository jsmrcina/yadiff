#pragma once

#include "GitRepo.h"

#include <QJsonObject>
#include <QStringList>

#include <functional>
#include <optional>

// Turns yadiff's command line (revisions, paths, --no-index, -ghpr, -ado) into the
// repository and comparison to show. Shared by main() and File > New comparison,
// which switches an empty window to the new comparison in place.
namespace ComparisonArgs {

struct Request {
    QStringList positional;   // revisions and paths
    qsizetype forcedPaths = 0; // how many trailing positionals came after "--"
    QString cwd;
    bool all = false;
    bool noIndex = false;
    QString ghpr, ado;        // pull request links
    QString algorithm;
    bool allowEmpty = false;  // no arguments outside a repository: an empty comparison
    QJsonObject handed;       // a pull request already resolved (by the parent process)
    std::function<void(const QString &)> progress;
};

struct Result {
    GitRepo repo;
    Comparison cmp;
    QJsonObject handed; // the resolved pull request, if any
};

// Resolves `req`; may fetch a pull request and ask the user to sign in (Azure DevOps).
std::optional<Result> resolve(const Request &req, QString *error);

// Parses the arguments File > New comparison builds (-C DIR, revisions, --no-index A B,
// -ghpr URL, -ado URL, -a, --algorithm NAME, --).
std::optional<Request> fromArguments(const QStringList &args, const QString &cwd, QString *error);

} // namespace ComparisonArgs
