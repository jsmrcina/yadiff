#pragma once

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <optional>

// Which GitHub account yadiff's gh commands run as. gh can be signed in to several accounts
// on github.com; yadiff picks one per repository owner (a user or an organization), so an
// account that can only read one owner's pull requests needn't be used to comment on them.
// The choice is yadiff's own: gh's active account (gh auth switch) is left alone.
namespace GitHubAccount {

// The account chosen for `owner`'s repositories, or empty for gh's active account.
QString forOwner(const QString &owner);
// Chooses `login` for `owner` (empty: gh's active account) and remembers it as used.
void setForOwner(const QString &owner, const QString &login);

// Logins yadiff has used, most recent first.
QStringList used();
void remember(const QString &login);

// The accounts gh is signed in to on github.com (gh auth status), and its active one.
struct SignedIn {
    QStringList logins;
    QString active;
};
std::optional<SignedIn> signedIn(QString *error);
SignedIn parseStatus(const QByteArray &json); // gh auth status --json hosts (exposed for tests)

// Sets up `env` so gh runs as `login`: GH_TOKEN, from gh auth token --user. An empty login
// leaves gh to its active account. False, with `error`, when gh has no token for it.
bool applyTo(QProcessEnvironment &env, const QString &login, QString *error);

} // namespace GitHubAccount
