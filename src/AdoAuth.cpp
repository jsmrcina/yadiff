#include "AdoAuth.h"

#include "Http.h"
#include "SecretStore.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

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
    p.start(program, args);
    if (!p.waitForStarted())
        return false;
    p.write(input);
    p.closeWriteChannel();
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        return false;
    }
    if (out)
        *out = p.readAllStandardOutput();
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
              QString::fromLatin1(kAdoResource), QStringLiteral("--query"), QStringLiteral("accessToken"), QStringLiteral("-o"),
              QStringLiteral("tsv")},
             {}, &out, 60000))
        return std::nullopt;
    const QString token = QString::fromUtf8(out).trimmed();
    if (token.isEmpty())
        return std::nullopt;
    return AdoAuth::Credential{AdoAuth::Credential::Kind::Bearer, token, QStringLiteral("Azure CLI")};
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
    if (helper.trimmed().isEmpty())
        return std::nullopt;
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
        if (line.startsWith("password=") && line.size() > 9)
            return AdoAuth::Credential{AdoAuth::Credential::Kind::Pat, QString::fromUtf8(line.mid(9)),
                                       QStringLiteral("git credential helper")};
    return std::nullopt;
}

} // namespace

namespace AdoAuth {

QByteArray Credential::authorizationHeader() const
{
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

std::optional<Credential> find(const QUrl &collectionUrl, QString *diagnostics)
{
    QStringList tried;
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const char *name : {"YADIFF_ADO_TOKEN", "SYSTEM_ACCESSTOKEN"}) {
        if (const QString v = env.value(QLatin1String(name)); !v.isEmpty())
            return Credential{Credential::Kind::Bearer, v, QLatin1String(name)};
    }
    for (const char *name : {"YADIFF_ADO_PAT", "AZURE_DEVOPS_EXT_PAT"}) {
        if (const QString v = env.value(QLatin1String(name)); !v.isEmpty())
            return Credential{Credential::Kind::Pat, v, QLatin1String(name)};
    }
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

bool savePat(const QUrl &collectionUrl, const QString &pat, QString *error)
{
    return SecretStore::write(keyFor(collectionUrl),
                              QStringLiteral("yadiff: Azure DevOps token for %1")
                                  .arg(organizationUrl(collectionUrl).toString(QUrl::StripTrailingSlash)),
                              pat, error);
}

bool forget(const QUrl &collectionUrl, QString *error)
{
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
    if (!o)
        return std::nullopt;
    const QJsonObject user = o->value(QStringLiteral("authenticatedUser")).toObject();
    QString name = user.value(QStringLiteral("providerDisplayName")).toString();
    if (name.isEmpty())
        name = user.value(QStringLiteral("customDisplayName")).toString();
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
