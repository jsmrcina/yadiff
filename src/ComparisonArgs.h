#pragma once

#include "AdoAuth.h"
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
    QString title;            // shown instead of the default description
    QString patch;            // a patch file to show (Comparison::Mode::Patch)
    bool allowEmpty = false;  // no arguments outside a repository: an empty comparison
    std::function<void(const QString &)> progress;
    // Azure DevOps sign-in when no credential works: returns the credential, or nullopt
    // when the user cancels. Default: AdoLoginDialog, which needs the UI thread; a
    // resolve on a worker thread passes one that asks the UI thread.
    std::function<std::optional<AdoAuth::Credential>(const QUrl &collection, const QString &reason)> signIn;
};

// The default sign-in: AdoLoginDialog (UI thread only). A credential the user chose not
// to save is put in this process's environment, so the window can still use it.
std::optional<AdoAuth::Credential> signInWithDialog(const QUrl &collection, const QString &reason);

struct Result {
    GitRepo repo;
    Comparison cmp;
};

// Resolves `req`; may fetch a pull request and ask the user to sign in (Azure DevOps).
std::optional<Result> resolve(const Request &req, QString *error);

// Parses the arguments File > New comparison builds (-C DIR, revisions, --no-index A B,
// -ghpr URL, -ado URL, -a, --algorithm NAME, --title TEXT, --patch FILE, --).
std::optional<Request> fromArguments(const QStringList &args, const QString &cwd, QString *error);
// The other way round: arguments that fromArguments() turns back into `req` (for recent
// comparisons and sessions).
QStringList toArguments(const Request &req);

} // namespace ComparisonArgs
