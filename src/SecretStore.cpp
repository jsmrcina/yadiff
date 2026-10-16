#include "SecretStore.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincred.h>
#endif

namespace {

QString testFile()
{
    return qEnvironmentVariable("YADIFF_SECRET_STORE_FILE");
}

QJsonObject readTestFile()
{
    QFile f(testFile());
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

bool writeTestFile(const QJsonObject &o)
{
    QFile f(testFile());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(QJsonDocument(o).toJson());
    return true;
}

#ifndef Q_OS_WIN
bool secretTool(const QStringList &args, const QByteArray &input, QByteArray *out, QString *error)
{
    QProcess p;
    p.start(QStringLiteral("secret-tool"), args);
    if (!p.waitForStarted()) {
        if (error)
            *error = QStringLiteral("secret-tool is not installed (package libsecret).");
        return false;
    }
    p.write(input);
    p.closeWriteChannel();
    // The keyring may ask the user to unlock it, so allow time for that.
    if (!p.waitForFinished(120000)) {
        p.kill();
        if (error)
            *error = QStringLiteral("The keyring did not answer (is it locked?).");
        return false;
    }
    if (out)
        *out = p.readAllStandardOutput();
    if (p.exitCode() != 0) {
        if (error)
            *error = QString::fromUtf8(p.readAllStandardError()).trimmed();
        return false;
    }
    return true;
}
#else
std::wstring target(const QString &key)
{
    return (QStringLiteral("yadiff:") + key).toStdWString();
}
#endif

} // namespace

namespace SecretStore {

bool available()
{
    if (!testFile().isEmpty())
        return true;
#ifdef Q_OS_WIN
    return true;
#else
    return !QStandardPaths::findExecutable(QStringLiteral("secret-tool")).isEmpty();
#endif
}

QString backendName()
{
    if (!testFile().isEmpty())
        return QStringLiteral("test file");
#ifdef Q_OS_WIN
    return QStringLiteral("Windows Credential Manager");
#else
    return QStringLiteral("system keyring");
#endif
}

std::optional<QString> read(const QString &key, QString *error)
{
    if (!testFile().isEmpty()) {
        const QJsonObject o = readTestFile();
        if (!o.contains(key))
            return std::nullopt;
        return o.value(key).toString();
    }
#ifdef Q_OS_WIN
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(target(key).c_str(), CRED_TYPE_GENERIC, 0, &cred))
        return std::nullopt;
    const QString secret = QString::fromUtf8(reinterpret_cast<const char *>(cred->CredentialBlob), int(cred->CredentialBlobSize));
    CredFree(cred);
    return secret;
#else
    QByteArray out;
    if (!secretTool({QStringLiteral("lookup"), QStringLiteral("application"), QStringLiteral("yadiff"), QStringLiteral("key"), key},
                    {}, &out, error))
        return std::nullopt;
    return QString::fromUtf8(out);
#endif
}

bool write(const QString &key, const QString &label, const QString &secret, QString *error)
{
    if (!testFile().isEmpty()) {
        QJsonObject o = readTestFile();
        o.insert(key, secret);
        return writeTestFile(o);
    }
#ifdef Q_OS_WIN
    const std::wstring t = target(key);
    const std::wstring comment = label.toStdWString();
    const QByteArray blob = secret.toUtf8();
    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(t.c_str());
    cred.Comment = const_cast<LPWSTR>(comment.c_str());
    cred.CredentialBlobSize = DWORD(blob.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(blob.constData()));
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (!CredWriteW(&cred, 0)) {
        if (error)
            *error = QStringLiteral("Windows Credential Manager refused the secret (error %1).").arg(GetLastError());
        return false;
    }
    return true;
#else
    return secretTool({QStringLiteral("store"), QStringLiteral("--label=") + label, QStringLiteral("application"),
                       QStringLiteral("yadiff"), QStringLiteral("key"), key},
                      secret.toUtf8(), nullptr, error);
#endif
}

bool remove(const QString &key, QString *error)
{
    if (!testFile().isEmpty()) {
        QJsonObject o = readTestFile();
        o.remove(key);
        return writeTestFile(o);
    }
#ifdef Q_OS_WIN
    if (!CredDeleteW(target(key).c_str(), CRED_TYPE_GENERIC, 0) && GetLastError() != ERROR_NOT_FOUND) {
        if (error)
            *error = QStringLiteral("Could not remove the credential (error %1).").arg(GetLastError());
        return false;
    }
    return true;
#else
    return secretTool({QStringLiteral("clear"), QStringLiteral("application"), QStringLiteral("yadiff"), QStringLiteral("key"), key},
                      {}, nullptr, error);
#endif
}

} // namespace SecretStore
