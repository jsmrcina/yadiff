#include "Updater.h"

#include "Http.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QVersionNumber>

#include <cstdio>

namespace Updater {

namespace {

const char kWindowsAsset[] = "yadiff-windows-x64.zip";

bool runTool(const QString &program, const QStringList &args, QByteArray *out, QString *error,
             const QString &workDir = {}, int timeoutMs = 120000)
{
    QProcess p;
    if (!workDir.isEmpty())
        p.setWorkingDirectory(workDir);
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(program, args);
    if (!p.waitForStarted(15000)) {
        if (error)
            *error = QStringLiteral("Could not start %1").arg(program);
        return false;
    }
    p.closeWriteChannel();
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(5000);
        if (error)
            *error = QStringLiteral("%1 timed out").arg(program);
        return false;
    }
    if (out)
        *out = p.readAllStandardOutput();
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        if (error) {
            const QString err = QString::fromUtf8(p.readAllStandardError()).trimmed();
            *error = QStringLiteral("%1 %2 failed: %3")
                         .arg(QFileInfo(program).fileName(), args.value(0),
                              err.isEmpty() ? QStringLiteral("exit code %1").arg(p.exitCode()) : err.right(2000));
        }
        return false;
    }
    return true;
}

// gh, unless disabled (tests) or not installed.
QString ghProgram()
{
    if (qEnvironmentVariableIsSet("YADIFF_UPDATE_NO_GH"))
        return {};
    return QStandardPaths::findExecutable(QStringLiteral("gh"));
}

QString apiBase()
{
    const QString env = qEnvironmentVariable("YADIFF_UPDATE_API");
    return env.isEmpty() ? QStringLiteral("https://api.github.com") : env;
}

Http::Headers apiHeaders(const QUrl &url, const QByteArray &accept = "application/vnd.github+json")
{
    Http::Headers h{{"Accept", accept}, {"User-Agent", "yadiff"}, {"X-GitHub-Api-Version", "2022-11-28"}};
    QString token = qEnvironmentVariable("GH_TOKEN");
    if (token.isEmpty())
        token = qEnvironmentVariable("GITHUB_TOKEN");
    if (!token.isEmpty() && Http::safeForCredentials(url))
        h.append({"Authorization", "Bearer " + token.toUtf8()});
    return h;
}

std::optional<QByteArray> apiGet(const QString &path, QString *error, const QByteArray &accept = "application/vnd.github+json",
                                 int timeoutMs = 30000)
{
    const QUrl url(apiBase() + path);
    const Http::Response r = Http::request("GET", url, apiHeaders(url, accept), {}, timeoutMs);
    if (!r.ok()) {
        if (error)
            *error = !r.error.isEmpty() ? r.error
                     : r.status == 404  ? QStringLiteral("No release found (HTTP 404). For a private repository, "
                                                         "install the gh CLI and run `gh auth login`, or set GH_TOKEN.")
                                        : QStringLiteral("GitHub answered HTTP %1").arg(r.status);
        return std::nullopt;
    }
    return r.body;
}

Release fromJson(const QJsonObject &o)
{
    Release r;
    r.tag = o.value(QStringLiteral("tagName")).toString(o.value(QStringLiteral("tag_name")).toString());
    r.name = o.value(QStringLiteral("name")).toString();
    r.notes = o.value(QStringLiteral("body")).toString();
    r.url = o.value(QStringLiteral("url")).toString();
    if (const QString html = o.value(QStringLiteral("html_url")).toString(); !html.isEmpty())
        r.url = html; // the REST API's "url" is the API's own
    return r;
}

QString workRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/yadiff/update");
}

// Downloads to `dir`: the source archive (Linux) or the Windows zip. Returns the file.
std::optional<QString> download(const Release &release, bool source, const QString &dir, QString *error)
{
    const QString gh = ghProgram();
    if (!gh.isEmpty()) {
        QStringList args{QStringLiteral("release"), QStringLiteral("download"), release.tag,
                         QStringLiteral("--repo"),  repository(),               QStringLiteral("--dir"),
                         dir,                       QStringLiteral("--clobber")};
        if (source)
            args << QStringLiteral("--archive") << QStringLiteral("tar.gz");
        else
            args << QStringLiteral("--pattern") << QString::fromLatin1(kWindowsAsset);
        if (!runTool(gh, args, nullptr, error, {}, 600000))
            return std::nullopt;
        const QStringList found = QDir(dir).entryList({source ? QStringLiteral("*.tar.gz") : QString::fromLatin1(kWindowsAsset)},
                                                      QDir::Files);
        if (found.isEmpty()) {
            if (error)
                *error = QStringLiteral("gh downloaded nothing");
            return std::nullopt;
        }
        return dir + u'/' + found.first();
    }

    std::optional<QByteArray> data;
    if (source) {
        data = apiGet(QStringLiteral("/repos/%1/tarball/%2").arg(repository(), release.tag), error, "application/vnd.github+json",
                      600000);
    } else {
        const auto json = apiGet(QStringLiteral("/repos/%1/releases/tags/%2").arg(repository(), release.tag), error);
        if (!json)
            return std::nullopt;
        QString assetUrl;
        for (const QJsonValue &a : QJsonDocument::fromJson(*json).object().value(QStringLiteral("assets")).toArray())
            if (a.toObject().value(QStringLiteral("name")).toString() == QLatin1String(kWindowsAsset))
                assetUrl = a.toObject().value(QStringLiteral("url")).toString();
        if (assetUrl.isEmpty()) {
            if (error)
                *error = QStringLiteral("Release %1 has no %2").arg(release.tag, QString::fromLatin1(kWindowsAsset));
            return std::nullopt;
        }
        const QUrl url(assetUrl);
        const Http::Response r = Http::request("GET", url, apiHeaders(url, "application/octet-stream"), {}, 600000);
        if (!r.ok()) {
            if (error)
                *error = r.error.isEmpty() ? QStringLiteral("Download failed (HTTP %1)").arg(r.status) : r.error;
            return std::nullopt;
        }
        data = r.body;
    }
    if (!data)
        return std::nullopt;
    const QString path = dir + (source ? QStringLiteral("/source.tar.gz") : u'/' + QString::fromLatin1(kWindowsAsset));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(*data) != data->size()) {
        if (error)
            *error = f.errorString();
        return std::nullopt;
    }
    return path;
}

// The single top-level folder an archive extracted into, or `dir` itself.
QString extractedRoot(const QString &dir)
{
    const QStringList subdirs = QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    return subdirs.size() == 1 ? dir + u'/' + subdirs.first() : dir;
}

bool installLinux(const Release &release, const QString &work, const std::function<void(const QString &)> &progress,
                  QString *error)
{
    progress(QStringLiteral("Downloading yadiff %1 source…").arg(release.tag));
    const auto archive = download(release, true, work, error);
    if (!archive)
        return false;
    const QString src = work + QStringLiteral("/src");
    QDir().mkpath(src);
    if (!runTool(QStringLiteral("tar"), {QStringLiteral("-xzf"), *archive, QStringLiteral("-C"), src}, nullptr, error))
        return false;
    const QString root = extractedRoot(src);
    const QString build = work + QStringLiteral("/build");

    progress(QStringLiteral("Building yadiff %1…").arg(release.tag));
    QStringList configure{QStringLiteral("-S"), root, QStringLiteral("-B"), build,
                          QStringLiteral("-DCMAKE_BUILD_TYPE=RelWithDebInfo")};
    if (!QStandardPaths::findExecutable(QStringLiteral("ninja")).isEmpty())
        configure << QStringLiteral("-G") << QStringLiteral("Ninja");
    if (!runTool(QStringLiteral("cmake"), configure, nullptr, error, {}, 300000)
        || !runTool(QStringLiteral("cmake"), {QStringLiteral("--build"), build}, nullptr, error, {}, 1800000))
        return false;
    progress(QStringLiteral("Testing yadiff %1…").arg(release.tag));
    if (!runTool(QStringLiteral("ctest"), {QStringLiteral("--test-dir"), build, QStringLiteral("--timeout"), QStringLiteral("300")},
                 nullptr, error, {}, 900000))
        return false;

    progress(QStringLiteral("Installing yadiff %1…").arg(release.tag));
    const QString home = QDir::homePath();
    const QString prefix = home + QStringLiteral("/.local/opt/yadiff-") + release.tag;
    if (!runTool(QStringLiteral("cmake"), {QStringLiteral("--install"), build, QStringLiteral("--prefix"), prefix}, nullptr,
                 error, {}, 300000))
        return false;
    // Move the link in one step, so `yadiff` always starts some complete version.
    const QString link = home + QStringLiteral("/.local/bin/yadiff");
    const QString tmpLink = link + QStringLiteral(".new");
    QFile::remove(tmpLink);
    if (!QFile::link(prefix + QStringLiteral("/bin/yadiff"), tmpLink)
        || std::rename(QFile::encodeName(tmpLink).constData(), QFile::encodeName(link).constData()) != 0) {
        QFile::remove(tmpLink);
        if (error)
            *error = QStringLiteral("Could not point %1 at %2").arg(link, prefix);
        return false;
    }
    return true;
}

bool installWindows(const Release &release, const QString &work, const std::function<void(const QString &)> &progress,
                    QString *error)
{
    progress(QStringLiteral("Downloading yadiff %1…").arg(release.tag));
    const auto zip = download(release, false, work, error);
    if (!zip)
        return false;
    const QString out = work + QStringLiteral("/zip");
    QDir().mkpath(out);
    // Windows 10 and later ship bsdtar, which reads zip files.
    if (!runTool(QStringLiteral("tar"), {QStringLiteral("-xf"), *zip, QStringLiteral("-C"), out}, nullptr, error))
        return false;
    QString root = extractedRoot(out);
    if (!QFileInfo::exists(root + QStringLiteral("/yadiff.exe"))) {
        if (error)
            *error = QStringLiteral("The download has no yadiff.exe");
        return false;
    }
    progress(QStringLiteral("Installing yadiff %1…").arg(release.tag));
    return replaceFiles(root, QCoreApplication::applicationDirPath(), error);
}

} // namespace

QString repository()
{
    const QString env = qEnvironmentVariable("YADIFF_UPDATE_REPO");
    return env.isEmpty() ? QStringLiteral("jsmrcina/yadiff") : env;
}

std::optional<Release> latest(QString *error)
{
    if (const QString gh = ghProgram(); !gh.isEmpty()) {
        QByteArray out;
        QString ghError;
        if (runTool(gh,
                    {QStringLiteral("release"), QStringLiteral("view"), QStringLiteral("--repo"), repository(),
                     QStringLiteral("--json"), QStringLiteral("tagName,name,body,url")},
                    &out, &ghError, {}, 60000)) {
            Release r = fromJson(QJsonDocument::fromJson(out).object());
            if (!r.tag.isEmpty())
                return r;
        }
        // gh not logged in, or offline: the REST API may still work (public repo, token).
        const auto json = apiGet(QStringLiteral("/repos/%1/releases/latest").arg(repository()), error);
        if (!json) {
            if (error)
                *error = ghError.isEmpty() ? *error : ghError;
            return std::nullopt;
        }
        return fromJson(QJsonDocument::fromJson(*json).object());
    }
    const auto json = apiGet(QStringLiteral("/repos/%1/releases/latest").arg(repository()), error);
    if (!json)
        return std::nullopt;
    Release r = fromJson(QJsonDocument::fromJson(*json).object());
    if (r.tag.isEmpty()) {
        if (error)
            *error = QStringLiteral("GitHub's answer had no release tag");
        return std::nullopt;
    }
    return r;
}

bool isNewer(const QString &tag, const QString &current)
{
    auto parse = [](QString v) {
        if (v.startsWith(u'v') || v.startsWith(u'V'))
            v.remove(0, 1);
        return QVersionNumber::fromString(v);
    };
    const QVersionNumber t = parse(tag), c = parse(current);
    return !t.isNull() && t > c;
}

InstallKind installKind(const QString &exePath, const QString &home)
{
    const QString exe = QFileInfo(exePath).canonicalFilePath();
#ifdef Q_OS_WIN
    Q_UNUSED(home);
    // An unzipped folder the user can write to.
    QTemporaryFile probe(QFileInfo(exe).absolutePath() + QStringLiteral("/.yadiff-update-probe-XXXXXX"));
    if (!exe.isEmpty() && probe.open())
        return InstallKind::WindowsFolder;
    return InstallKind::Unsupported;
#else
    const QString optRoot = QFileInfo(home + QStringLiteral("/.local/opt")).canonicalFilePath();
    static const QRegularExpression snapshot(QStringLiteral("^/yadiff-[^/]+/bin/yadiff$"));
    if (exe.isEmpty() || optRoot.isEmpty() || !exe.startsWith(optRoot) || !snapshot.match(exe.mid(optRoot.size())).hasMatch())
        return InstallKind::Unsupported;
    if (!QFileInfo(home + QStringLiteral("/.local/bin/yadiff")).isSymLink())
        return InstallKind::Unsupported;
    return InstallKind::LinuxSnapshot;
#endif
}

InstallKind currentInstallKind()
{
    return installKind(QCoreApplication::applicationFilePath(), QDir::homePath());
}

QString unsupportedReason()
{
#ifdef Q_OS_WIN
    return QStringLiteral("yadiff can't write to its own folder (%1), so it can't update itself. "
                          "Download the new release and unzip it yourself.")
        .arg(QDir::toNativeSeparators(QCoreApplication::applicationDirPath()));
#else
    return QStringLiteral("This copy of yadiff (%1) wasn't installed with install.sh, so it can't update itself. "
                          "Update it the way it was installed.")
        .arg(QCoreApplication::applicationFilePath());
#endif
}

bool install(const Release &release, const std::function<void(const QString &)> &progress, QString *error)
{
    const InstallKind kind = currentInstallKind();
    if (kind == InstallKind::Unsupported) {
        if (error)
            *error = unsupportedReason();
        return false;
    }
    static const QRegularExpression safeTag(QStringLiteral("^v?[0-9A-Za-z._-]+$"));
    if (!safeTag.match(release.tag).hasMatch()) {
        if (error)
            *error = QStringLiteral("Unexpected release tag '%1'").arg(release.tag);
        return false;
    }
    QDir().mkpath(workRoot());
    QLockFile lock(workRoot() + QStringLiteral("/update.lock"));
    lock.setStaleLockTime(2 * 60 * 60 * 1000); // longer than any build; a crashed owner is detected by its pid
    if (!lock.tryLock(1000)) {
        if (error)
            *error = QStringLiteral("Another yadiff window is installing an update");
        return false;
    }
    const QString work = workRoot() + u'/' + release.tag;
    QDir(work).removeRecursively();
    QDir().mkpath(work);
    const bool ok = kind == InstallKind::LinuxSnapshot ? installLinux(release, work, progress, error)
                                                       : installWindows(release, work, progress, error);
    QDir(work).removeRecursively();
    return ok;
}

QString restartProgram()
{
    if (currentInstallKind() == InstallKind::LinuxSnapshot)
        return QDir::homePath() + QStringLiteral("/.local/bin/yadiff");
    return QCoreApplication::applicationFilePath();
}

void cleanUpOldFiles(const QString &dir)
{
    QDirIterator it(dir, {QStringLiteral("*.old")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        QFile::remove(it.next());
}

bool replaceFiles(const QString &from, const QString &to, QString *error)
{
    const QDir src(from);
    QDirIterator it(from, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        const QString rel = src.relativeFilePath(file);
        const QString dst = to + u'/' + rel;
        QDir().mkpath(QFileInfo(dst).absolutePath());
        const QString old = dst + QStringLiteral(".old");
        if (QFileInfo::exists(dst)) {
            QFile::remove(old);
            if (!QFile::rename(dst, old)) {
                if (error)
                    *error = QStringLiteral("Could not replace %1").arg(QDir::toNativeSeparators(dst));
                return false;
            }
        }
        if (!QFile::copy(file, dst)) {
            QFile::rename(old, dst); // put the old one back
            if (error)
                *error = QStringLiteral("Could not write %1").arg(QDir::toNativeSeparators(dst));
            return false;
        }
    }
    return true;
}

} // namespace Updater
