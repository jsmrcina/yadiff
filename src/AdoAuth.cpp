#include "AdoAuth.h"

#include "Http.h"
#include "Log.h"
#include "SecretStore.h"

#include <QDateTime>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimeZone>

namespace {

// The Azure DevOps resource id in Microsoft Entra ID.
const char *kAdoResource = "499b84ac-1321-427f-aa17-267ca6975798";

QString keyFor(const QUrl &collectionUrl)
{
    return QStringLiteral("ado:") + AdoAuth::organizationUrl(collectionUrl).toString(QUrl::StripTrailingSlash).toLower();
}

bool run(const QString &program, const QStringList &args, const QByteArray &input, QByteArray *out, int timeoutMs,
         const QProcessEnvironment &env = QProcessEnvironment::systemEnvironment())
{
    QProcess p;
    p.setProcessEnvironment(env);
    Log::ProcessTrace trace(lcAuth());
    p.start(program, args);
    if (!p.waitForStarted()) {
        trace.done(p);
        return false;
    }
    p.write(input);
    p.closeWriteChannel();
    if (!trace.wait(p, timeoutMs)) {
        trace.done(p);
        p.kill();
        return false;
    }
    const QByteArray stdOut = p.readAllStandardOutput();
    trace.done(p, stdOut.size(), p.readAllStandardError());
    if (out)
        *out = stdOut;
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

std::optional<AdoAuth::Credential> fromAzureCli()
{
    const QString az = QStandardPaths::findExecutable(QStringLiteral("az"));
    if (az.isEmpty())
        return std::nullopt;
    QByteArray out;
    if (!run(az,
             {QStringLiteral("account"), QStringLiteral("get-access-token"), QStringLiteral("--resource"),
              QString::fromLatin1(kAdoResource), QStringLiteral("-o"), QStringLiteral("json")},
             {}, &out, 60000))
        return std::nullopt;
    const QJsonObject o = QJsonDocument::fromJson(out).object();
    const QString token = o.value(QStringLiteral("accessToken")).toString().trimmed();
    if (token.isEmpty())
        return std::nullopt;
    Log::addSecret(token);
    AdoAuth::Credential c{AdoAuth::Credential::Kind::Bearer, token, QStringLiteral("Azure CLI")};
    c.expires = AdoAuth::parseAzureCliExpiry(o);
    return c;
}

std::optional<AdoAuth::Credential> fromGitCredential(const QUrl &org)
{
    // Asks git's configured helper (Git Credential Manager signs in interactively if needed).
    // Only when one is configured for this URL: otherwise git would fall back to an
    // askpass program and pop up an unexpected username prompt.
    QByteArray helper;
    run(QStringLiteral("git"), {QStringLiteral("config"), QStringLiteral("--get-urlmatch"), QStringLiteral("credential.helper"),
                                org.toString()},
        {}, &helper, 10000);
    if (helper.trimmed().isEmpty()) {
        qCInfo(lcAuth).noquote() << "no git credential helper is configured for" << Log::url(org);
        return std::nullopt;
    }
    qCInfo(lcAuth).noquote() << "asking git's credential helper" << QString::fromUtf8(helper).trimmed() << "for" << Log::url(org);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    env.remove(QStringLiteral("GIT_ASKPASS"));
    env.remove(QStringLiteral("SSH_ASKPASS"));
    const QByteArray input = "protocol=" + org.scheme().toUtf8() + "\nhost=" + org.host().toUtf8() + "\npath="
                           + org.path().mid(1).toUtf8() + "\n\n";
    QByteArray out;
    if (!run(QStringLiteral("git"), {QStringLiteral("credential"), QStringLiteral("fill")}, input, &out, 120000, env))
        return std::nullopt;
    for (const QByteArray &line : out.split('\n'))
        if (line.startsWith("password=") && line.size() > 9) {
            Log::addSecret(QString::fromUtf8(line.mid(9)));
            return AdoAuth::Credential{AdoAuth::Credential::Kind::Pat, QString::fromUtf8(line.mid(9)),
                                       QStringLiteral("git credential helper")};
        }
    return std::nullopt;
}

} // namespace

namespace AdoAuth {

QString Credential::kindName() const
{
    return kind == Kind::Bearer ? QStringLiteral("bearer token") : QStringLiteral("personal access token");
}

QByteArray Credential::authorizationHeader() const
{
    Log::addSecret(secret);
    if (kind == Kind::Bearer)
        return "Bearer " + secret.toUtf8();
    return "Basic " + (":" + secret.toUtf8()).toBase64(); // PAT: empty user name
}

QUrl organizationUrl(const QUrl &collectionUrl)
{
    QUrl u = collectionUrl.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::StripTrailingSlash | QUrl::RemoveUserInfo);
    const QString host = u.host().toLower();
    if (host.endsWith(QLatin1String(".visualstudio.com"))) {
        // ORG.visualstudio.com[/DefaultCollection] is the same organisation as dev.azure.com/ORG.
        QUrl out;
        out.setScheme(QStringLiteral("https"));
        out.setHost(QStringLiteral("dev.azure.com"));
        out.setPath(u'/' + host.section(u'.', 0, 0));
        return out;
    }
    if (host == QLatin1String("dev.azure.com")) {
        const QString org = u.path().section(u'/', 1, 1);
        u.setPath(u'/' + org);
    }
    return u;
}

namespace {
std::optional<Credential> findCredential(const QUrl &collectionUrl, QString *diagnostics);
}

namespace {

// Credentials that cost a program run to find (the keyring, az, git's helper), per
// organisation, so opening a PR, its comments and votes find one once. Environment
// variables are read every time: they are free, and tests change them.
struct Cached {
    Credential credential;
    QDateTime until;
};
QMutex g_cacheMutex;
QHash<QString, Cached> g_cache;

bool fromEnvironment(const Credential &c)
{
    return c.source.startsWith(QLatin1String("YADIFF_")) || c.source == QLatin1String("SYSTEM_ACCESSTOKEN")
        || c.source == QLatin1String("AZURE_DEVOPS_EXT_PAT");
}

std::optional<Credential> environmentCredential()
{
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const char *name : {"YADIFF_ADO_TOKEN", "SYSTEM_ACCESSTOKEN"}) {
        if (const QString v = env.value(QLatin1String(name)); !v.isEmpty())
            return Credential{Credential::Kind::Bearer, v, QLatin1String(name)};
    }
    for (const char *name : {"YADIFF_ADO_PAT", "AZURE_DEVOPS_EXT_PAT"}) {
        if (const QString v = env.value(QLatin1String(name)); !v.isEmpty())
            return Credential{Credential::Kind::Pat, v, QLatin1String(name)};
    }
    return std::nullopt;
}

std::optional<Credential> findCached(const QUrl &collectionUrl, QString *diagnostics);

} // namespace

std::optional<Credential> find(const QUrl &collectionUrl, QString *diagnostics)
{
    auto c = findCached(collectionUrl, diagnostics);
    if (c)
        c->collection = collectionUrl;
    return c;
}

namespace {
std::optional<Credential> findCached(const QUrl &collectionUrl, QString *diagnostics)
{
    if (auto env = environmentCredential()) {
        Log::addSecret(env->secret);
        qCDebug(lcAuth).noquote() << "credential for" << Log::url(organizationUrl(collectionUrl)) << ": " + env->kindName()
                                  << "from" << env->source;
        return env;
    }
    const QString key = keyFor(collectionUrl);
    {
        QMutexLocker lock(&g_cacheMutex);
        if (const auto it = g_cache.constFind(key); it != g_cache.constEnd()) {
            if (QDateTime::currentDateTimeUtc() < it->until) {
                qCDebug(lcAuth).noquote() << "credential for" << Log::url(organizationUrl(collectionUrl)) << ": the"
                                          << it->credential.kindName() << "from" << it->credential.source << "found earlier";
                return it->credential;
            }
            qCInfo(lcAuth).noquote() << "the" << it->credential.kindName() << "from" << it->credential.source << "for"
                                     << Log::url(organizationUrl(collectionUrl)) << "has expired; looking again";
            g_cache.erase(it);
        }
    }
    QString why;
    auto c = findCredential(collectionUrl, &why);
    if (c) {
        Log::addSecret(c->secret);
        qCInfo(lcAuth).noquote() << "credential for" << Log::url(organizationUrl(collectionUrl)) << ":" << c->kindName()
                                 << "from" << c->source
                                 << (c->expires.isValid() ? QStringLiteral("(valid until %1)").arg(c->expires.toLocalTime().toString(Qt::ISODate))
                                                          : QString());
        if (!fromEnvironment(*c)) {
            // Tokens are reused until five minutes before they expire; others for an hour
            // (or until rejected).
            const QDateTime now = QDateTime::currentDateTimeUtc();
            QDateTime until = c->expires.isValid() ? c->expires.addSecs(-300) : now.addSecs(3600);
            if (until > now.addSecs(3600))
                until = now.addSecs(3600);
            if (until > now) {
                QMutexLocker lock(&g_cacheMutex);
                g_cache.insert(key, {*c, until});
            }
        }
    } else {
        qCInfo(lcAuth).noquote() << "no credential for" << Log::url(organizationUrl(collectionUrl)) << ":" << why;
    }
    if (diagnostics)
        *diagnostics = why;
    return c;
}
} // namespace

void rejected(const QUrl &collectionUrl, const Credential &cred)
{
    QMutexLocker lock(&g_cacheMutex);
    const auto it = g_cache.find(keyFor(collectionUrl));
    if (it != g_cache.end() && it->credential.secret == cred.secret) {
        qCInfo(lcAuth).noquote() << "forgetting the rejected" << cred.kindName() << "from" << cred.source << "for"
                                 << Log::url(organizationUrl(collectionUrl));
        g_cache.erase(it);
    }
}

void clearCache()
{
    QMutexLocker lock(&g_cacheMutex);
    g_cache.clear();
}

QDateTime parseAzureCliExpiry(const QJsonObject &o)
{
    // Newer az: "expires_on" (seconds since the epoch). Older: "expiresOn", local time.
    const QJsonValue epoch = o.value(QStringLiteral("expires_on"));
    if (epoch.isDouble() && epoch.toDouble() > 0)
        return QDateTime::fromSecsSinceEpoch(qint64(epoch.toDouble()), QTimeZone::UTC);
    if (epoch.isString() && epoch.toString().toLongLong() > 0)
        return QDateTime::fromSecsSinceEpoch(epoch.toString().toLongLong(), QTimeZone::UTC);
    QString local = o.value(QStringLiteral("expiresOn")).toString();
    if (local.isEmpty())
        return {};
    local = local.section(u'.', 0, 0); // "2026-10-19 15:03:11.000000"
    QDateTime t = QDateTime::fromString(local, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    t.setTimeZone(QTimeZone::LocalTime);
    return t.isValid() ? t.toUTC() : QDateTime();
}

namespace {
std::optional<Credential> findCredential(const QUrl &collectionUrl, QString *diagnostics)
{
    QStringList tried;
    if (auto env = environmentCredential())
        return env;
    tried << QStringLiteral("environment variables");

    if (SecretStore::available()) {
        if (const auto pat = SecretStore::read(keyFor(collectionUrl)); pat && !pat->isEmpty())
            return Credential{Credential::Kind::Pat, *pat, SecretStore::backendName()};
        tried << SecretStore::backendName();
    }
    if (const auto c = fromAzureCli())
        return c;
    tried << (azureCliAvailable() ? QStringLiteral("Azure CLI (not signed in?)") : QStringLiteral("Azure CLI (not installed)"));
    if (const auto c = fromGitCredential(organizationUrl(collectionUrl)))
        return c;
    tried << QStringLiteral("git credential helper");
    if (diagnostics)
        *diagnostics = QStringLiteral("No Azure DevOps credential found. Tried: %1.").arg(tried.join(QStringLiteral(", ")));
    return std::nullopt;
}
} // namespace

bool savePat(const QUrl &collectionUrl, const QString &pat, QString *error)
{
    Log::addSecret(pat);
    clearCache();
    qCInfo(lcAuth).noquote() << "saving a token for" << Log::url(organizationUrl(collectionUrl)) << "in the" << SecretStore::backendName();
    return SecretStore::write(keyFor(collectionUrl),
                              QStringLiteral("yadiff: Azure DevOps token for %1")
                                  .arg(organizationUrl(collectionUrl).toString(QUrl::StripTrailingSlash)),
                              pat, error);
}

bool forget(const QUrl &collectionUrl, QString *error)
{
    qCInfo(lcAuth).noquote() << "forgetting the token for" << Log::url(organizationUrl(collectionUrl));
    clearCache();
    return SecretStore::remove(keyFor(collectionUrl), error);
}

QString authProblem(int status)
{
    if (status == 401 || status == 203)
        return QStringLiteral("Azure DevOps did not accept the credential (expired, revoked, or for another organisation).");
    if (status == 403)
        return QStringLiteral("The credential is valid but lacks permission (a token needs the Code (Read & write) scope).");
    return {};
}

namespace {
std::optional<QJsonObject> connectionData(const QUrl &collectionUrl, const Credential &cred, QString *error)
{
    const QUrl base = collectionUrl.adjusted(QUrl::StripTrailingSlash);
    if (!Http::safeForCredentials(base)) {
        if (error)
            *error = QStringLiteral("Refusing to send credentials over an unencrypted connection to %1.").arg(base.host());
        return std::nullopt;
    }
    QUrl url(base.toString() + QStringLiteral("/_apis/connectionData"));
    const Http::Response r = Http::request("GET", url, {{"Authorization", cred.authorizationHeader()}, {"Accept", "application/json"}});
    if (!r.error.isEmpty()) {
        if (error)
            *error = r.error;
        return std::nullopt;
    }
    if (!r.ok() || !QJsonDocument::fromJson(r.body).isObject()) {
        if (error) {
            *error = authProblem(r.status);
            if (error->isEmpty())
                *error = QStringLiteral("Azure DevOps answered HTTP %1.").arg(r.status);
        }
        return std::nullopt;
    }
    return QJsonDocument::fromJson(r.body).object();
}
} // namespace

std::optional<QString> validate(const QUrl &collectionUrl, const Credential &cred, QString *error)
{
    const auto o = connectionData(collectionUrl, cred, error);
    if (!o) {
        qCInfo(lcAuth).noquote() << "the" << cred.kindName() << "from" << cred.source << "was not accepted by"
                                 << Log::url(collectionUrl) << ":" << (error ? *error : QString());
        return std::nullopt;
    }
    const QJsonObject user = o->value(QStringLiteral("authenticatedUser")).toObject();
    QString name = user.value(QStringLiteral("providerDisplayName")).toString();
    if (name.isEmpty())
        name = user.value(QStringLiteral("customDisplayName")).toString();
    qCInfo(lcAuth).noquote() << "signed in to" << Log::url(collectionUrl) << "as" << name << "with the" << cred.kindName()
                             << "from" << cred.source;
    return name.isEmpty() ? QStringLiteral("(unknown user)") : name;
}

std::optional<QString> userId(const QUrl &collectionUrl, const Credential &cred, QString *error)
{
    const auto o = connectionData(collectionUrl, cred, error);
    if (!o)
        return std::nullopt;
    const QString id = o->value(QStringLiteral("authenticatedUser")).toObject().value(QStringLiteral("id")).toString();
    if (id.isEmpty() && error)
        *error = QStringLiteral("Azure DevOps did not say who is signed in.");
    return id.isEmpty() ? std::nullopt : std::optional<QString>(id);
}

std::optional<Credential> azureCliCredential()
{
    return fromAzureCli();
}

bool azureCliAvailable()
{
    return !QStandardPaths::findExecutable(QStringLiteral("az")).isEmpty();
}

bool azureCliLogin(QString *error)
{
    const QString az = QStandardPaths::findExecutable(QStringLiteral("az"));
    if (az.isEmpty()) {
        if (error)
            *error = QStringLiteral("The Azure CLI (az) is not installed.");
        return false;
    }
    if (!run(az, {QStringLiteral("login"), QStringLiteral("--allow-no-subscriptions")}, {}, nullptr, 600000)) {
        if (error)
            *error = QStringLiteral("az login did not complete.");
        return false;
    }
    return true;
}

} // namespace AdoAuth
