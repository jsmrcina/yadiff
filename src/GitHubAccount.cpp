#include "GitHubAccount.h"

#include "Log.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QProcess>
#include <QSettings>

#include <algorithm>

namespace GitHubAccount {

namespace {

const QString kHost = QStringLiteral("github.com");

QString ownerKey(const QString &owner)
{
    return QStringLiteral("github/accountFor/") + owner.toLower();
}

// Runs gh with the user's own environment (no GH_TOKEN of ours).
bool runGh(const QStringList &args, QByteArray *out, QString *error)
{
    QProcess p;
    Log::ProcessTrace trace(lcGitHub());
    p.start(QStringLiteral("gh"), args);
    if (!p.waitForStarted()) {
        trace.done(p);
        if (error)
            *error = QStringLiteral("Could not start gh. Install the GitHub CLI and run `gh auth login`.");
        return false;
    }
    p.closeWriteChannel();
    trace.wait(p);
    const QByteArray stdErr = p.readAllStandardError();
    *out = p.readAllStandardOutput();
    trace.done(p, out->size(), stdErr);
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        if (error)
            *error = QString::fromUtf8(stdErr).trimmed();
        return false;
    }
    return true;
}

} // namespace

QString forOwner(const QString &owner)
{
    return owner.isEmpty() ? QString() : QSettings().value(ownerKey(owner)).toString();
}

void setForOwner(const QString &owner, const QString &login)
{
    QSettings s;
    if (login.isEmpty())
        s.remove(ownerKey(owner));
    else
        s.setValue(ownerKey(owner), login);
    remember(login);
    qCInfo(lcGitHub).noquote() << "using" << (login.isEmpty() ? QStringLiteral("gh's active account") : login) << "for" << owner;
}

QStringList used()
{
    return QSettings().value(QStringLiteral("github/usedAccounts")).toStringList();
}

void remember(const QString &login)
{
    if (login.isEmpty())
        return;
    QStringList list = used();
    if (list.value(0).compare(login, Qt::CaseInsensitive) == 0)
        return;
    list.erase(std::remove_if(list.begin(), list.end(), [&](const QString &l) { return l.compare(login, Qt::CaseInsensitive) == 0; }),
               list.end());
    list.prepend(login);
    QSettings().setValue(QStringLiteral("github/usedAccounts"), list.mid(0, 20));
}

SignedIn parseStatus(const QByteArray &json)
{
    SignedIn s;
    const QJsonArray accounts = QJsonDocument::fromJson(json).object().value(QStringLiteral("hosts")).toObject().value(kHost).toArray();
    for (const QJsonValue &v : accounts) {
        const QJsonObject o = v.toObject();
        const QString login = o.value(QStringLiteral("login")).toString();
        if (login.isEmpty())
            continue;
        s.logins << login;
        if (o.value(QStringLiteral("active")).toBool())
            s.active = login;
    }
    return s;
}

std::optional<SignedIn> signedIn(QString *error)
{
    QByteArray out;
    if (!runGh({QStringLiteral("auth"), QStringLiteral("status"), QStringLiteral("--hostname"), kHost, QStringLiteral("--json"),
                QStringLiteral("hosts")},
               &out, error))
        return std::nullopt;
    return parseStatus(out);
}

bool applyTo(QProcessEnvironment &env, const QString &login, QString *error)
{
    if (login.isEmpty())
        return true;
    // gh keeps tokens in the system keyring; reading one costs a process, so once per run.
    static QMutex mutex;
    static QHash<QString, QString> tokens;
    QMutexLocker lock(&mutex);
    QString token = tokens.value(login.toLower());
    if (token.isEmpty()) {
        QByteArray out;
        QString err;
        if (!runGh({QStringLiteral("auth"), QStringLiteral("token"), QStringLiteral("--hostname"), kHost, QStringLiteral("--user"), login},
                   &out, &err)) {
            if (error)
                *error = QStringLiteral("gh is not signed in to GitHub as %1 (%2). Run `gh auth login` to add the account.")
                             .arg(login, err.section(u'\n', 0, 0));
            return false;
        }
        token = QString::fromUtf8(out).trimmed();
        Log::addSecret(token);
        tokens.insert(login.toLower(), token);
    }
    env.insert(QStringLiteral("GH_TOKEN"), token);
    return true;
}

} // namespace GitHubAccount
