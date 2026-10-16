#include "DiffPane.h"
#include "FileList.h"
#include "MainWindow.h"

#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

namespace {

void git(const QString &dir, const QStringList &args)
{
    QProcess p;
    p.setWorkingDirectory(dir);
    p.start(QStringLiteral("git"), QStringList{QStringLiteral("-c"), QStringLiteral("user.name=t"), QStringLiteral("-c"),
                                               QStringLiteral("user.email=t@example.com"), QStringLiteral("-c"),
                                               QStringLiteral("commit.gpgsign=false")}
                                       + args);
    QVERIFY(p.waitForFinished());
    QVERIFY2(p.exitCode() == 0, p.readAllStandardError().constData());
}

void write(const QString &path, const QByteArray &data)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

QByteArray lines(int n, int changeEvery = 0)
{
    QByteArray out;
    for (int i = 1; i <= n; ++i)
        out += (changeEvery && i % changeEvery == 0 ? "changed " : "line ") + QByteArray::number(i) + '\n';
    return out;
}

} // namespace

class TestNavigation : public QObject
{
    Q_OBJECT
private slots:
    void f3WalksChangesThenFiles()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        write(d + QStringLiteral("/a.txt"), lines(100));
        write(d + QStringLiteral("/b.txt"), lines(10));
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("init")});
        write(d + QStringLiteral("/a.txt"), lines(100, 30)); // changes at lines 30, 60, 90
        write(d + QStringLiteral("/b.txt"), lines(10, 5));   // changes at lines 5, 10

        QString error;
        auto repo = GitRepo::open(d, &error);
        QVERIFY2(repo, qPrintable(error));
        MainWindow w(std::move(*repo), Comparison{});
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        auto *files = w.findChild<FileList *>();
        auto *pane = w.findChild<DiffPane *>();
        QVERIFY(files && pane);

        // Opens on a.txt (index 0), first change.
        QCOMPARE(files->currentIndex(), 0);
        QCOMPARE(pane->changeCount(), 3);
        QCOMPARE(pane->currentChange(), 0);

        QTest::keyClick(&w, Qt::Key_F3);
        QCOMPARE(pane->currentChange(), 1);
        QTest::keyClick(&w, Qt::Key_F3);
        QCOMPARE(pane->currentChange(), 2);

        // No more changes in a.txt: F3 moves to b.txt's first change.
        QTest::keyClick(&w, Qt::Key_F3);
        QCOMPARE(files->currentIndex(), 1);
        QCOMPARE(pane->changeCount(), 2);
        QCOMPARE(pane->currentChange(), 0);

        // Shift+F3 at the first change goes back to a.txt's last change.
        QTest::keyClick(&w, Qt::Key_F3, Qt::ShiftModifier);
        QCOMPARE(files->currentIndex(), 0);
        QCOMPARE(pane->currentChange(), 2);

        // Switching view mode keeps the current change.
        pane->setMode(DiffPane::Mode::Inline);
        QCOMPARE(pane->currentChange(), 2);
        pane->setMode(DiffPane::Mode::SideBySide);
        QCOMPARE(pane->currentChange(), 2);
    }
};

QTEST_MAIN(TestNavigation)
#include "test_navigation.moc"
