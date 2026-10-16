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

        // Path filter: only a.txt (both sections).
        Comparison filtered;
        filtered.paths = {QStringLiteral("a.txt")};
        auto only = repo->listChanges(filtered, &error);
        QCOMPARE(only.size(), 2);
        filtered.paths = {QStringLiteral("sub")}; // a directory
        only = repo->listChanges(filtered, &error);
        QCOMPARE(only.size(), 1);
        QCOMPARE(only[0].path, QStringLiteral("sub/dir/b.txt"));

        // All changes: one entry per file against HEAD, untracked included.
        Comparison all;
        all.allChanges = true;
        const auto allChanges = repo->listChanges(all, &error);
        QCOMPARE(allChanges.size(), 3); // a.txt, sub/dir/b.txt, untracked.txt
        const FileChange *aAll = find(allChanges, QStringLiteral("a.txt"), ChangeGroup::Revisions);
        QVERIFY(aAll);
        QCOMPARE(repo->loadDiff(*aAll, all, &error).chunks.size(), 2);

        // Fingerprints: the unstaged-vs-HEAD view and "all changes" compare the same
        // versions, so a review mark carries over; editing the file changes it.
        const QByteArray fpUnstaged = repo->fingerprint(*unstaged, Comparison{});
        QCOMPARE(repo->fingerprint(*aAll, all), fpUnstaged);
        QVERIFY(repo->fingerprint(*staged, Comparison{}) != fpUnstaged);
        Comparison patience;
        patience.options.algorithm = DiffAlgorithm::Patience;
        QCOMPARE(repo->fingerprint(*unstaged, patience), fpUnstaged);
        write(d + QStringLiteral("/a.txt"), "edited again\n");
        QVERIFY(repo->fingerprint(*unstaged, Comparison{}) != fpUnstaged);
        write(d + QStringLiteral("/a.txt"), "one\nTWO\nthree\nfour\n");

        // Diff options reach git.
        Comparison ws;
        ws.options.ignoreAllSpace = true;
        write(d + QStringLiteral("/sub/dir/b.txt"), "new   file\n");
        git(d, {QStringLiteral("add"), QStringLiteral("sub/dir/b.txt")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("b")});
        write(d + QStringLiteral("/sub/dir/b.txt"), "new file\n");
        // A whitespace-only change disappears from the list when whitespace is ignored...
        QVERIFY(!find(repo->listChanges(ws, &error), QStringLiteral("sub/dir/b.txt"), ChangeGroup::Unstaged));
        // ...and shows up, with one change, when it is not.
        const auto plainChanges = repo->listChanges(Comparison{}, &error);
        const FileChange *bws = find(plainChanges, QStringLiteral("sub/dir/b.txt"), ChangeGroup::Unstaged);
        QVERIFY(bws);
        QCOMPARE(repo->loadDiff(*bws, Comparison{}, &error).chunks.size(), 1);
        QVERIFY(repo->loadDiff(*bws, ws, &error).chunks.isEmpty());
        QCOMPARE(Comparison{}.options.algorithm, DiffAlgorithm::Patience); // the default
        git(d, {QStringLiteral("checkout"), QStringLiteral("--"), QStringLiteral("sub/dir/b.txt")});

        // Revisions mode.
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("second")});
        Comparison rc;
        rc.mode = Comparison::Mode::Revisions;
        rc.rev1 = *repo->resolveCommit(QStringLiteral("HEAD~1"), &error);
        rc.rev2 = *repo->resolveCommit(QStringLiteral("HEAD"), &error);
        const auto revChanges = repo->listChanges(rc, &error);
        QCOMPARE(revChanges.size(), 2); // a.txt edited, untracked.txt added
        QVERIFY(find(revChanges, QStringLiteral("a.txt"), ChangeGroup::Revisions));
        QVERIFY(!repo->resolveCommit(QStringLiteral("--output=x"), &error));
        QVERIFY(!repo->resolveCommit(QStringLiteral("does-not-exist"), &error));
    }

    void lineCountsAndPathsMode()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        write(d + QStringLiteral("/a.txt"), "1\n2\n3\n");
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("init")});
        write(d + QStringLiteral("/a.txt"), "1\nTWO\n3\n4\n");
        write(d + QStringLiteral("/u.txt"), "x\ny\n");
        QString error;
        auto repo = GitRepo::open(d, &error);
        Comparison all;
        all.allChanges = true;
        const auto changes = repo->listChanges(all, &error);
        const FileChange *a = find(changes, QStringLiteral("a.txt"), ChangeGroup::Revisions);
        const FileChange *u = find(changes, QStringLiteral("u.txt"), ChangeGroup::Revisions);
        QVERIFY(a && u);
        QCOMPARE(a->added, 2);
        QCOMPARE(a->removed, 1);
        QCOMPARE(u->added, 2); // untracked: counted from the file
        QCOMPARE(repo->diskPath(*a, all, true), d + QStringLiteral("/a.txt"));
        QVERIFY(repo->diskPath(*a, all, false).isEmpty()); // HEAD is not editable
        Comparison split;
        const auto splitChanges = repo->listChanges(split, &error);
        QVERIFY(!repo->log(10, 0, false, &error).isEmpty());

        // Directory compare outside any repository.
        QDir(d).mkpath(QStringLiteral("L/sub"));
        QDir(d).mkpath(QStringLiteral("R/sub"));
        write(d + QStringLiteral("/L/sub/mod.txt"), "x\ny\n");
        write(d + QStringLiteral("/R/sub/mod.txt"), "x\nY\n");
        write(d + QStringLiteral("/L/gone.txt"), "g\n");
        write(d + QStringLiteral("/R/new.txt"), "n\n");
        write(d + QStringLiteral("/L/same.txt"), "s\n");
        write(d + QStringLiteral("/R/same.txt"), "s\n");
        Comparison pc;
        pc.mode = Comparison::Mode::Paths;
        pc.leftPath = d + QStringLiteral("/L");
        pc.rightPath = d + QStringLiteral("/R");
        GitRepo none = GitRepo::forPaths(d);
        const auto pcs = none.listChanges(pc, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(pcs.size(), 3);
        const FileChange *mod = find(pcs, QStringLiteral("sub/mod.txt"), ChangeGroup::Revisions);
        QVERIFY(mod);
        QCOMPARE(mod->status, QChar(u'M'));
        QCOMPARE(mod->added, 1);
        QVERIFY(find(pcs, QStringLiteral("gone.txt"), ChangeGroup::Revisions)->status == u'D');
        QVERIFY(find(pcs, QStringLiteral("new.txt"), ChangeGroup::Revisions)->status == u'A');
        const FileDiff md = none.loadDiff(*mod, pc, &error);
        QCOMPARE(md.chunks.size(), 1);
        QCOMPARE(none.loadDiff(*find(pcs, QStringLiteral("new.txt"), ChangeGroup::Revisions), pc, &error).lines.size(), 1);
        QCOMPARE(none.diskPath(*mod, pc, false), d + QStringLiteral("/L/sub/mod.txt")); // both sides editable
        QCOMPARE(none.diskPath(*mod, pc, true), d + QStringLiteral("/R/sub/mod.txt"));

        // Two files, and git difftool's /dev/null for a new file.
        Comparison fc;
        fc.mode = Comparison::Mode::Paths;
        fc.pathsAreFiles = true;
        fc.leftPath = d + QStringLiteral("/L/sub/mod.txt");
        fc.rightPath = d + QStringLiteral("/R/sub/mod.txt");
        auto files = none.listChanges(fc, &error);
        QCOMPARE(files.size(), 1);
        QCOMPARE(files[0].status, QChar(u'M'));
        QCOMPARE(none.loadDiff(files[0], fc, &error).chunks.size(), 1);
        fc.leftPath = QStringLiteral("/dev/null");
        files = none.listChanges(fc, &error);
        QCOMPARE(files[0].status, QChar(u'A'));
        QCOMPARE(none.loadDiff(files[0], fc, &error).lines.size(), 2);
        fc.leftPath = d + QStringLiteral("/L/same.txt");
        fc.rightPath = d + QStringLiteral("/R/same.txt");
        files = none.listChanges(fc, &error);
        QCOMPARE(files[0].status, QChar(u'='));
    }

    void fileContentOfEachSide()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        write(d + QStringLiteral("/f.bin"), QByteArray("v1\0x", 4));
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("init")});
        write(d + QStringLiteral("/f.bin"), QByteArray("v2\0x", 4));
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        write(d + QStringLiteral("/f.bin"), QByteArray("v3\0x", 4));
        QString error;
        auto repo = GitRepo::open(d, &error);
        Comparison split;
        const auto changes = repo->listChanges(split, &error);
        const FileChange *staged = find(changes, QStringLiteral("f.bin"), ChangeGroup::Staged);
        const FileChange *unstaged = find(changes, QStringLiteral("f.bin"), ChangeGroup::Unstaged);
        QVERIFY(staged && unstaged);
        QCOMPARE(repo->fileContent(*staged, split, false)->left(2), QByteArray("v1"));   // HEAD
        QCOMPARE(repo->fileContent(*staged, split, true)->left(2), QByteArray("v2"));    // index
        QCOMPARE(repo->fileContent(*unstaged, split, true)->left(2), QByteArray("v3"));  // working tree
        Comparison vsIndex;
        vsIndex.unstagedAgainstHead = false;
        QCOMPARE(repo->fileContent(*unstaged, vsIndex, false)->left(2), QByteArray("v2"));
        QVERIFY(!repo->fileContent(*unstaged, split, true, 2)); // over the size cap
        FileChange added = *unstaged;
        added.status = u'A';
        QVERIFY(!repo->fileContent(added, split, false)); // no old side
    }
};

QTEST_APPLESS_MAIN(TestGitRepo)
#include "test_gitrepo.moc"
