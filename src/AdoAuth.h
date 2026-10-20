#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QUrl>

#include <optional>

class QJsonObject;

// Credentials for Azure DevOps (dev.azure.com, *.visualstudio.com or Azure DevOps Server).
//
// Looked up in this order:
//   1. Environment: YADIFF_ADO_TOKEN or SYSTEM_ACCESSTOKEN (Entra/pipeline bearer token),
//      AZURE_DEVOPS_EXT_PAT or YADIFF_ADO_PAT (personal access token).
//   2. A personal access token saved by `yadiff --ado-login` in the system keyring.
//   3. The Azure CLI (`az login`), when installed.
//   4. git's credential helper for the organisation (e.g. Git Credential Manager).
namespace AdoAuth {

struct Credential {
    enum class Kind { Pat, Bearer };
    Credential() = default;
    Credential(Kind k, QString s, QString from)
        : kind(k), secret(std::move(s)), source(std::move(from))
    {
    }
    Kind kind = Kind::Pat;
    QString secret;
    QString source; // where it came from, for messages ("keyring", "Azure CLI", ...)
    QDateTime expires; // when a token from the Azure CLI expires (invalid: unknown)
    QUrl collection;   // what find() found it for (to forget it when it's rejected)
    QByteArray authorizationHeader() const;
    QString kindName() const; // "personal access token" or "bearer token", for the log
};

// Canonical organisation URL: https://dev.azure.com/ORG for both cloud spellings,
// the collection URL for Azure DevOps Server. Used as the keyring key.
QUrl organizationUrl(const QUrl &collectionUrl);

// Finds a credential. One found through a program (keyring, az, git) is remembered per
// organisation until it is about to expire, rejected, or an hour has passed, so the PR,
// its comments and votes don't each wait for az.
std::optional<Credential> find(const QUrl &collectionUrl, QString *diagnostics = nullptr);
// The server refused `cred`: the next find() looks again instead of reusing it.
void rejected(const QUrl &collectionUrl, const Credential &cred);
void clearCache();
// When `az account get-access-token -o json`'s token expires (invalid when it doesn't say).
QDateTime parseAzureCliExpiry(const QJsonObject &o);

bool savePat(const QUrl &collectionUrl, const QString &pat, QString *error);
bool forget(const QUrl &collectionUrl, QString *error);

// Checks a credential against the server. Returns the signed-in user's display name.
std::optional<QString> validate(const QUrl &collectionUrl, const Credential &cred, QString *error);
// The signed-in user's id (needed to vote on a pull request).
std::optional<QString> userId(const QUrl &collectionUrl, const Credential &cred, QString *error);

// Turns an HTTP status from Azure DevOps into a sign-in problem description, or empty.
// (It answers 203 with an HTML sign-in page when the credential is not accepted.)
QString authProblem(int status);

bool azureCliAvailable();
std::optional<Credential> azureCliCredential();
// Runs `az login` (opens a browser). Returns false with *error when it fails.
bool azureCliLogin(QString *error);

} // namespace AdoAuth
