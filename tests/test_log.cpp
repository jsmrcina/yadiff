// The log: secrets scrubbed, command lines, levels, and what reaches the file.
#include "Log.h"

#include <QFile>
#include <QElapsedTimer>
#include <QProcess>
#include <QtConcurrent/QtConcurrentRun>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QUrl>

class TestLog : public QObject
{
    Q_OBJECT

private slots:
    void parsesLevels()
    {
        QCOMPARE(Log::parseLevel(QStringLiteral("debug")), Log::Level::Debug);
        QCOMPARE(Log::parseLevel(QStringLiteral(" Detailed ")), Log::Level::Debug);
        QCOMPARE(Log::parseLevel(QStringLiteral("INFO")), Log::Level::Info);
        QCOMPARE(Log::parseLevel(QStringLiteral("warn")), Log::Level::Warning);
        QCOMPARE(Log::parseLevel(QStringLiteral("off")), Log::Level::Off);
        QVERIFY(!Log::parseLevel(QStringLiteral("loud")));
        for (Log::Level l : {Log::Level::Off, Log::Level::Error, Log::Level::Warning, Log::Level::Info, Log::Level::Debug})
            QCOMPARE(Log::parseLevel(Log::levelName(l)), l);
    }

    void redactsSecrets_data()
    {
        QTest::addColumn<QString>("in");
        QTest::addColumn<QString>("out");
        QTest::newRow("url password") << "fetch https://me:hunter2@dev.azure.com/org/p/_git/r"
                                      << "fetch https://me:***@dev.azure.com/org/p/_git/r";
        QTest::newRow("url user only") << "https://org@dev.azure.com/org" << "https://org@dev.azure.com/org";
        QTest::newRow("bearer") << "Authorization: Bearer abc.def-ghi_123456" << "Authorization: Bearer ***";
        QTest::newRow("basic") << "Basic OnNlY3JldC1wYXQ=" << "Basic ***";
        QTest::newRow("github token header") << "Authorization: token 0123456789abcdef" << "Authorization: token ***";
        QTest::newRow("github pat") << "token ghp_0123456789abcdefghijABCDEFGHIJ used"
                                    << "token *** used";
        QTest::newRow("fine-grained pat") << "github_pat_11ABCDEFG0123456789_abcdefghijklmnopqrstuvwxyz" << "***";
        QTest::newRow("jwt") << "got eyJhbGciOiJSUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.c2lnbmF0dXJl ok" << "got *** ok";
        QTest::newRow("ado pat") << "pat abcdefghijklmnopqrstuvwxyz234567abcdefghijklmnopqrst end" << "pat *** end";
        QTest::newRow("query") << "GET https://x/api?api-version=7.1&access_token=s3cr3t&x=1"
                               << "GET https://x/api?api-version=7.1&access_token=***&x=1";
        QTest::newRow("credential fill") << "password=hunter2" << "password=***";
        QTest::newRow("env") << "YADIFF_ADO_PAT=whatever GH_TOKEN=x" << "YADIFF_ADO_PAT=*** GH_TOKEN=***";
        QTest::newRow("commit ids stay") << "merge base 0123456789abcdef0123456789abcdef01234567"
                                         << "merge base 0123456789abcdef0123456789abcdef01234567";
        QTest::newRow("ordinary text stays") << "Basic usage: run git fetch" << "Basic usage: run git fetch";
    }

    void redactsSecrets()
    {
        QFETCH(QString, in);
        QFETCH(QString, out);
        QCOMPARE(Log::redact(in), out);
    }

    void redactsRegisteredSecrets()
    {
        Log::addSecret(QStringLiteral("plain-looking-value"));
        Log::addSecret(QStringLiteral("abc")); // too short to scrub safely: ignored
        QCOMPARE(Log::redact(QStringLiteral("x plain-looking-value y abc")), QStringLiteral("x *** y abc"));
        QCOMPARE(Log::url(QUrl(QStringLiteral("https://u:pw@host/p?token=t"))), QStringLiteral("https://u@host/p?token=***"));
    }

    void quotesCommandLines()
    {
        QCOMPARE(Log::commandLine(QStringLiteral("git"), {QStringLiteral("diff"), QStringLiteral("--"), QStringLiteral("a b.txt")}),
                 QStringLiteral("git diff -- 'a b.txt'"));
        QCOMPARE(Log::commandLine(QStringLiteral("git"), {QStringLiteral("it's"), QString()}),
                 QStringLiteral("git 'it'\\''s' ''"));
    }

    void writesTheFileAtTheChosenLevel()
    {
        QTemporaryDir tmp;
        const QString path = tmp.filePath(QStringLiteral("sub/yadiff.log"));
        Log::init(path, QStringLiteral("info"));
        QCOMPARE(Log::filePath(), path);
        qCInfo(lcApp) << "visible info";
        qCDebug(lcApp) << "hidden debug";
        Log::addSecret(QStringLiteral("very-secret-value"));
        qCInfo(lcAuth).noquote() << "token very-secret-value\nsecond line";

        // A failing command: its error output is logged; a quiet successful one is not.
        {
            QProcess p;
            Log::ProcessTrace trace(lcGit());
            p.start(QStringLiteral("git"), {QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("no-such-ref-xyz")});
            p.waitForFinished(30000);
            trace.done(p, p.readAllStandardOutput().size(), "fatal: Needed a single revision");
        }
        {
            const Log::Quiet quiet;
            QProcess p;
            Log::ProcessTrace trace(lcGit());
            p.start(QStringLiteral("git"), {QStringLiteral("--version")});
            p.waitForFinished(30000);
            trace.done(p, p.readAllStandardOutput().size());
        }
        {
            // A command that never ends (it waits for input): wait() gives up at its timeout.
            QProcess p;
            Log::ProcessTrace trace(lcGit());
            p.start(QStringLiteral("git"), {QStringLiteral("hash-object"), QStringLiteral("--stdin")});
            QVERIFY(p.waitForStarted());
            QVERIFY(!trace.wait(p, 300));
            trace.done(p);
            p.kill();
            p.waitForFinished();
        }
        {
            // A command waited on under a CancelScope is killed once it is cancelled.
            auto cancel = std::make_shared<std::atomic_bool>(false);
            QElapsedTimer timer;
            timer.start();
            auto future = QtConcurrent::run([cancel] {
                const Log::CancelScope scope(cancel);
                QProcess p;
                Log::ProcessTrace trace(lcGit());
                p.start(QStringLiteral("git"), {QStringLiteral("hash-object"), QStringLiteral("--stdin")});
                p.waitForStarted();
                const bool finished = trace.wait(p);
                trace.done(p);
                return finished;
            });
            QTest::qWait(300);
            *cancel = true;
            future.waitForFinished();
            QVERIFY(!future.result());
            QVERIFY(timer.elapsed() < 5000);
        }
        {
            // The watchdog: the UI thread blocked (waiting for a command) is logged, and so
            // is its recovery.
            Log::startWatchdog(300);
            Log::noteAction(QStringLiteral("menu Compare commits"));
            {
                const Log::UiWait busy(QStringLiteral("git log --all"));
                QThread::msleep(900);
            }
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < 500)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            Log::stopWatchdog();
        }
        Log::setLevel(Log::Level::Debug);
        qCDebug(lcDiff) << "now debug shows";
        Log::shutdown();

        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString log = QString::fromUtf8(f.readAll());
        QVERIFY(log.contains(QStringLiteral("INFO  yadiff.app    visible info")));
        QVERIFY(!log.contains(QStringLiteral("hidden debug")));
        QVERIFY(log.contains(QStringLiteral("token ***\n    | second line")));
        QVERIFY(!log.contains(QStringLiteral("very-secret-value")));
        QVERIFY(log.contains(QStringLiteral("git rev-parse --verify no-such-ref-xyz -> FAILED: exit 128")));
        QVERIFY(log.contains(QStringLiteral("stderr: fatal: Needed a single revision")));
        QVERIFY(!log.contains(QStringLiteral("git --version")));
        QVERIFY(log.contains(QStringLiteral("git hash-object --stdin -> FAILED: still running (timed out)")));
        QVERIFY(log.contains(QStringLiteral("stopping (no longer wanted): git hash-object --stdin")));
        QVERIFY(log.contains(QStringLiteral("DEBUG yadiff.diff   now debug shows")));
        QVERIFY2(log.contains(QStringLiteral("FROZEN: the window has not responded for")), qPrintable(log));
        QVERIFY(log.contains(QStringLiteral("waiting for: git log --all; last action: menu Compare commits")));
        QVERIFY(log.contains(QStringLiteral("the window responds again (it was waiting for: git log --all)")));
        // Back to quiet, as in the other tests.
        Log::init(QStringLiteral("off"));
    }
};

QTEST_GUILESS_MAIN(TestLog)
#include "test_log.moc"
