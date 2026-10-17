#include "Updater.h"

#include <QDir>
#include <QFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

namespace {

void write(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

QByteArray read(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// Answers every request with `body`, remembering the request headers.
class FakeGitHub : public QObject
{
public:
    QByteArray body;
    int status = 200;
    QList<QByteArray> requests;

    FakeGitHub()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = m_server.nextPendingConnection())
                connect(s, &QTcpSocket::readyRead, this, [this, s] {
                    m_buffer += s->readAll();
                    if (!m_buffer.contains("\r\n\r\n"))
                        return;
                    requests << m_buffer;
                    m_buffer.clear();
                    s->write("HTTP/1.1 " + QByteArray::number(status) + " X\r\nContent-Type: application/json\r\nContent-Length: "
                             + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    s->disconnectFromHost();
                });
        });
        m_server.listen(QHostAddress::LocalHost);
    }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }

private:
    QTcpServer m_server;
    QByteArray m_buffer;
};

} // namespace

class TestUpdate : public QObject
{
    Q_OBJECT
private slots:
    void versions()
    {
        QVERIFY(Updater::isNewer(QStringLiteral("v0.7.0"), QStringLiteral("0.6.0")));
        QVERIFY(Updater::isNewer(QStringLiteral("v0.10.0"), QStringLiteral("0.9.1")));
        QVERIFY(Updater::isNewer(QStringLiteral("v1.0"), QStringLiteral("0.99.0")));
        QVERIFY(!Updater::isNewer(QStringLiteral("v0.6.0"), QStringLiteral("0.6.0")));
        QVERIFY(!Updater::isNewer(QStringLiteral("v0.5.2"), QStringLiteral("0.6.0")));
        QVERIFY(!Updater::isNewer(QStringLiteral("nightly"), QStringLiteral("0.6.0")));
    }

    void installKinds()
    {
#ifdef Q_OS_WIN
        QSKIP("Linux install layout");
#else
        QTemporaryDir tmp;
        const QString home = tmp.path();
        const QString exe = home + QStringLiteral("/.local/opt/yadiff-v0.6.0/bin/yadiff");
        write(exe, "binary");
        QCOMPARE(Updater::installKind(exe, home), Updater::InstallKind::Unsupported); // no ~/.local/bin link
        QDir().mkpath(home + QStringLiteral("/.local/bin"));
        QVERIFY(QFile::link(exe, home + QStringLiteral("/.local/bin/yadiff")));
        QCOMPARE(Updater::installKind(exe, home), Updater::InstallKind::LinuxSnapshot);
        // A build tree or a system install is left alone.
        write(home + QStringLiteral("/src/yadiff/build/yadiff"), "binary");
        QCOMPARE(Updater::installKind(home + QStringLiteral("/src/yadiff/build/yadiff"), home), Updater::InstallKind::Unsupported);
        QCOMPARE(Updater::installKind(QStringLiteral("/usr/bin/yadiff"), home), Updater::InstallKind::Unsupported);
#endif
    }

    void replacesFilesInPlace()
    {
        QTemporaryDir tmp;
        const QString from = tmp.path() + QStringLiteral("/new"), to = tmp.path() + QStringLiteral("/app");
        write(from + QStringLiteral("/yadiff.exe"), "new exe");
        write(from + QStringLiteral("/platforms/qwindows.dll"), "new dll");
        write(to + QStringLiteral("/yadiff.exe"), "old exe");
        write(to + QStringLiteral("/keep.txt"), "untouched");
        QString error;
        QVERIFY2(Updater::replaceFiles(from, to, &error), qPrintable(error));
        QCOMPARE(read(to + QStringLiteral("/yadiff.exe")), QByteArray("new exe"));
        QCOMPARE(read(to + QStringLiteral("/yadiff.exe.old")), QByteArray("old exe"));
        QCOMPARE(read(to + QStringLiteral("/platforms/qwindows.dll")), QByteArray("new dll"));
        // A second update replaces the leftover .old file too.
        write(from + QStringLiteral("/yadiff.exe"), "newer exe");
        QVERIFY2(Updater::replaceFiles(from, to, &error), qPrintable(error));
        QCOMPARE(read(to + QStringLiteral("/yadiff.exe.old")), QByteArray("new exe"));
        Updater::cleanUpOldFiles(to);
        QVERIFY(!QFile::exists(to + QStringLiteral("/yadiff.exe.old")));
        QVERIFY(QFile::exists(to + QStringLiteral("/platforms/qwindows.dll")));
        QCOMPARE(read(to + QStringLiteral("/keep.txt")), QByteArray("untouched"));
    }

    void latestReleaseFromTheApi()
    {
        FakeGitHub gh;
        gh.body = R"({"tag_name":"v9.1.0","name":"yadiff 9.1.0","body":"Notes","html_url":"https://github.com/o/r/releases/tag/v9.1.0","url":"https://api.github.com/x"})";
        qputenv("YADIFF_UPDATE_NO_GH", "1");
        qputenv("YADIFF_UPDATE_API", gh.base().toUtf8());
        qputenv("YADIFF_UPDATE_REPO", "o/r");
        qputenv("GH_TOKEN", "test-token");
        QString error;
        const auto rel = Updater::latest(&error);
        QVERIFY2(rel, qPrintable(error));
        QCOMPARE(rel->tag, QStringLiteral("v9.1.0"));
        QCOMPARE(rel->notes, QStringLiteral("Notes"));
        QCOMPARE(rel->url, QStringLiteral("https://github.com/o/r/releases/tag/v9.1.0"));
        QCOMPARE(gh.requests.size(), 1);
        QVERIFY(gh.requests[0].startsWith("GET /repos/o/r/releases/latest "));
        QVERIFY(gh.requests[0].contains("Bearer test-token"));

        gh.status = 404;
        gh.body = R"({"message":"Not Found"})";
        QVERIFY(!Updater::latest(&error));
        QVERIFY2(error.contains(QStringLiteral("gh auth login")), qPrintable(error));
        qunsetenv("GH_TOKEN");
        qunsetenv("YADIFF_UPDATE_API");
        qunsetenv("YADIFF_UPDATE_REPO");
        qunsetenv("YADIFF_UPDATE_NO_GH");
    }
};

QTEST_GUILESS_MAIN(TestUpdate)
#include "test_update.moc"
