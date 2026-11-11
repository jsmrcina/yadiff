#include "SecretStore.h"

#include "Log.h"

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
    Log::ProcessTrace trace(lcAuth());
    p.start(QStringLiteral("secret-tool"), args);
    if (!p.waitForStarted()) {
        trace.done(p);
        if (error)
            *error = QStringLiteral("secret-tool is not installed (package libsecret).");
        return false;
    }
    p.write(input);
    p.closeWriteChannel();
    // The keyring may ask the user to unlock it, so allow time for that.
    if (!trace.wait(p, 120000)) {
        trace.done(p);
        p.kill();
        if (error)
            *error = QStringLiteral("The keyring did not answer (is it locked?).");
        return false;
    }
    const QByteArray stdOut = p.readAllStandardOutput(), stdErr = p.readAllStandardError();
    trace.done(p, stdOut.size(), stdErr);
    if (out)
        *out = stdOut;
    if (p.exitCode() != 0) {
        if (error)
            *error = QString::fromUtf8(stdErr).trimmed();
        return false;
    }
    return true;
}
#else
std::wstring target(const QString &key)
{
    return (QStringLiteral("yadiff:") + key).toStdWString();
}

// A credential holds at most CRED_MAX_CREDENTIAL_BLOB_SIZE (2560) bytes, less than an Entra
// access token can take. A longer secret is split: the key's own credential says how many
// parts there are, and the parts are "<key>#1", "<key>#2", ...
const QByteArray kPartsPrefix = "yadiff-parts:";
const int kPartSize = CRED_MAX_CREDENTIAL_BLOB_SIZE;

std::optional<QByteArray> readBlob(const std::wstring &t)
{
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(t.c_str(), CRED_TYPE_GENERIC, 0, &cred))
        return std::nullopt;
    QByteArray blob(reinterpret_cast<const char *>(cred->CredentialBlob), int(cred->CredentialBlobSize));
    CredFree(cred);
    return blob;
}

bool writeBlob(const std::wstring &t, const QString &label, const QByteArray &blob)
{
    const std::wstring comment = label.toStdWString();
    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(t.c_str());
    cred.Comment = const_cast<LPWSTR>(comment.c_str());
    cred.CredentialBlobSize = DWORD(blob.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(blob.constData()));
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&cred, 0);
}

// How many parts the secret under `key` is split into (0: not split).
int partCount(const QString &key)
{
    const auto head = readBlob(target(key));
    return head && head->startsWith(kPartsPrefix) ? head->mid(kPartsPrefix.size()).toInt() : 0;
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
    qCInfo(lcAuth).noquote() << "reading" << key << "from the" << backendName();
    if (!testFile().isEmpty()) {
        const QJsonObject o = readTestFile();
        if (!o.contains(key))
            return std::nullopt;
        Log::addSecret(o.value(key).toString());
        return o.value(key).toString();
    }
#ifdef Q_OS_WIN
    auto blob = readBlob(target(key));
    if (!blob) {
        qCInfo(lcAuth).noquote() << "Credential Manager has no" << key << "(error" << GetLastError() << ")";
        return std::nullopt; // a missing credential isn't an error
    }
    if (blob->startsWith(kPartsPrefix)) {
        const int parts = blob->mid(kPartsPrefix.size()).toInt();
        blob->clear();
        for (int i = 1; i <= parts; ++i) {
            const auto part = readBlob(target(key + u'#' + QString::number(i)));
            if (!part) {
                if (error)
                    *error = QStringLiteral("Part %1 of %2 is missing from the Credential Manager.").arg(i).arg(key);
                return std::nullopt;
            }
            blob->append(*part);
        }
    }
    const QString secret = QString::fromUtf8(*blob);
    Log::addSecret(secret);
    return secret;
#else
    QByteArray out;
    if (!secretTool({QStringLiteral("lookup"), QStringLiteral("application"), QStringLiteral("yadiff"), QStringLiteral("key"), key},
                    {}, &out, error))
        return std::nullopt;
    Log::addSecret(QString::fromUtf8(out));
    return QString::fromUtf8(out);
#endif
}

bool write(const QString &key, const QString &label, const QString &secret, QString *error)
{
    Log::addSecret(secret);
    qCInfo(lcAuth).noquote() << "writing" << key << "to the" << backendName();
    if (!testFile().isEmpty()) {
        QJsonObject o = readTestFile();
        o.insert(key, secret);
        return writeTestFile(o);
    }
#ifdef Q_OS_WIN
    const QByteArray blob = secret.toUtf8();
    const int oldParts = partCount(key);
    int parts = 0;
    bool ok = true;
    if (blob.size() <= kPartSize && !blob.startsWith(kPartsPrefix)) {
        ok = writeBlob(target(key), label, blob);
    } else {
        parts = int((blob.size() + kPartSize - 1) / kPartSize);
        for (int i = 1; ok && i <= parts; ++i)
            ok = writeBlob(target(key + u'#' + QString::number(i)), label, blob.mid(qsizetype(i - 1) * kPartSize, kPartSize));
        ok = ok && writeBlob(target(key), label, kPartsPrefix + QByteArray::number(parts));
    }
    if (!ok) {
        if (error)
            *error = QStringLiteral("Windows Credential Manager refused the secret (error %1).").arg(GetLastError());
        return false;
    }
    for (int i = parts + 1; i <= oldParts; ++i) // left from a longer secret
        CredDeleteW(target(key + u'#' + QString::number(i)).c_str(), CRED_TYPE_GENERIC, 0);
    return true;
#else
    return secretTool({QStringLiteral("store"), QStringLiteral("--label=") + label, QStringLiteral("application"),
                       QStringLiteral("yadiff"), QStringLiteral("key"), key},
                      secret.toUtf8(), nullptr, error);
#endif
}

bool remove(const QString &key, QString *error)
{
    qCInfo(lcAuth).noquote() << "removing" << key << "from the" << backendName();
    if (!testFile().isEmpty()) {
        QJsonObject o = readTestFile();
        o.remove(key);
        return writeTestFile(o);
    }
#ifdef Q_OS_WIN
    const int parts = partCount(key);
    for (int i = 1; i <= parts; ++i)
        CredDeleteW(target(key + u'#' + QString::number(i)).c_str(), CRED_TYPE_GENERIC, 0);
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
