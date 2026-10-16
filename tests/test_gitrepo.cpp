#include "GitRepo.h"

#include <QDir>
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

const FileChange *find(const QVector<FileChange> &v, const QString &path, ChangeGroup g)
{
    for (const FileChange &fc : v)
        if (fc.path == path && fc.group == g)
            return &fc;
    return nullptr;
}

} // namespace

class TestGitRepo : public QObject
{
    Q_OBJECT
private slots:
    void workingTreeAndRevisions()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});

        QString error;
        {
            // No commits yet: staged files are compared against the empty tree.
            write(d + QStringLiteral("/a.txt"), "one\ntwo\nthree\n");
            git(d, {QStringLiteral("add"), QStringLiteral("a.txt")});
            auto repo = GitRepo::open(d, &error);
            QVERIFY2(repo, qPrintable(error));
            Comparison cmp;
            const auto changes = repo->listChanges(cmp, &error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            const FileChange *a = find(changes, QStringLiteral("a.txt"), ChangeGroup::Staged);
            QVERIFY(a);
            QCOMPARE(a->status, QChar(u'A'));
            const FileDiff fd = repo->loadDiff(*a, cmp, &error);
            QCOMPARE(fd.lines.size(), 3);
        }

        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("first")});
        QDir(d).mkpath(QStringLiteral("sub/dir"));
        write(d + QStringLiteral("/a.txt"), "one\nTWO\nthree\n");
        write(d + QStringLiteral("/sub/dir/b.txt"), "new file\n");
        git(d, {QStringLiteral("add"), QStringLiteral("a.txt")});
        write(d + QStringLiteral("/a.txt"), "one\nTWO\nthree\nfour\n");
        write(d + QStringLiteral("/untracked.txt"), "u\n");

        auto repo = GitRepo::open(d + QStringLiteral("/sub"), &error); // from a subdirectory
        QVERIFY2(repo, qPrintable(error));
        Comparison cmp;
        const auto changes = repo->listChanges(cmp, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));

        const FileChange *staged = find(changes, QStringLiteral("a.txt"), ChangeGroup::Staged);
        const FileChange *unstaged = find(changes, QStringLiteral("a.txt"), ChangeGroup::Unstaged);
        QVERIFY(staged && unstaged);
        QVERIFY(find(changes, QStringLiteral("untracked.txt"), ChangeGroup::Unstaged));
        QVERIFY(find(changes, QStringLiteral("sub/dir/b.txt"), ChangeGroup::Unstaged)); // untracked

        // Staged: HEAD vs index -> one changed line.
        FileDiff fd = repo->loadDiff(*staged, cmp, &error);
        QCOMPARE(fd.chunks.size(), 1);
        QCOMPARE(fd.lines.size(), 4); // one, -two, +TWO, three

        // Unstaged vs HEAD includes both the staged edit and the new line.
        fd = repo->loadDiff(*unstaged, cmp, &error);
        QCOMPARE(fd.chunks.size(), 2);

        // Unstaged vs index only has the new line.
        cmp.unstagedAgainstHead = false;
        fd = repo->loadDiff(*unstaged, cmp, &error);
        QCOMPARE(fd.chunks.size(), 1);

        // Revisions mode.
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("second")});
        Comparison rc;
        rc.mode = Comparison::Mode::Revisions;
        rc.rev1 = *repo->resolveCommit(QStringLiteral("HEAD~1"), &error);
        rc.rev2 = *repo->resolveCommit(QStringLiteral("HEAD"), &error);
        const auto revChanges = repo->listChanges(rc, &error);
        QCOMPARE(revChanges.size(), 3);
        QVERIFY(find(revChanges, QStringLiteral("sub/dir/b.txt"), ChangeGroup::Revisions));
        QVERIFY(!repo->resolveCommit(QStringLiteral("--output=x"), &error));
        QVERIFY(!repo->resolveCommit(QStringLiteral("does-not-exist"), &error));
    }
};

QTEST_APPLESS_MAIN(TestGitRepo)
#include "test_gitrepo.moc"
