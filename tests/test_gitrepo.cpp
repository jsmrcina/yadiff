#include "GitRepo.h"

#include <QDir>
#include <QFileInfo>
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

    void swapIdenticalBlankAndGlobs()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        write(d + QStringLiteral("/same.txt"), "s\n");
        write(d + QStringLiteral("/blank.txt"), "a\nb\n");
        write(d + QStringLiteral("/real.txt"), "a\nb\n");
        write(d + QStringLiteral("/gone.txt"), "g\n");
        write(d + QStringLiteral("/Cargo.lock"), "v1\n");
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("init")});
        write(d + QStringLiteral("/blank.txt"), "a\n\n\nb\n");
        write(d + QStringLiteral("/real.txt"), "a\nB\nc\n");
        write(d + QStringLiteral("/Cargo.lock"), "v2\n");
        QFile::remove(d + QStringLiteral("/gone.txt"));
        write(d + QStringLiteral("/new.txt"), "n\n");
        QString error;
        auto repo = GitRepo::open(d, &error);
        Comparison cmp;
        cmp.allChanges = true;
        auto changes = repo->listChanges(cmp, &error);
        QCOMPARE(changes.size(), 5);
        QVERIFY(find(changes, QStringLiteral("blank.txt"), ChangeGroup::Revisions)->blankOnly);
        QVERIFY(!find(changes, QStringLiteral("real.txt"), ChangeGroup::Revisions)->blankOnly);
        QCOMPARE(categoryOf(*find(changes, QStringLiteral("gone.txt"), ChangeGroup::Revisions)), FileCategory::LeftOnly);
        QCOMPARE(categoryOf(*find(changes, QStringLiteral("new.txt"), ChangeGroup::Revisions)), FileCategory::RightOnly);
        QCOMPARE(categoryOf(*find(changes, QStringLiteral("real.txt"), ChangeGroup::Revisions)), FileCategory::Different);

        // Identical files on request.
        cmp.includeIdentical = true;
        changes = repo->listChanges(cmp, &error);
        const FileChange *same = find(changes, QStringLiteral("same.txt"), ChangeGroup::Revisions);
        QVERIFY(same);
        QCOMPARE(same->status, QChar(u'='));
        QCOMPARE(categoryOf(*same), FileCategory::Identical);
        const FileDiff sd = repo->loadDiff(*same, cmp, &error);
        QCOMPARE(sd.lines.size(), 1);
        QVERIFY(sd.chunks.isEmpty());
        cmp.includeIdentical = false;

        // Ignore globs.
        cmp.ignoreGlobs = {QStringLiteral("*.lock"), QStringLiteral("# a comment")};
        cmp.useIgnoreGlobs = true;
        changes = repo->listChanges(cmp, &error);
        QVERIFY(!find(changes, QStringLiteral("Cargo.lock"), ChangeGroup::Revisions));
        cmp.useIgnoreGlobs = false;
        QVERIFY(find(repo->listChanges(cmp, &error), QStringLiteral("Cargo.lock"), ChangeGroup::Revisions));

        // Swapped sides: statuses and counts flip, the working tree is on the left.
        Comparison rev = cmp;
        rev.reversed = true;
        const auto swapped = repo->listChanges(rev, &error);
        const FileChange *gone = find(swapped, QStringLiteral("gone.txt"), ChangeGroup::Revisions);
        QVERIFY(gone);
        QCOMPARE(gone->status, QChar(u'A')); // on the right (HEAD) only
        const FileChange *realR = find(swapped, QStringLiteral("real.txt"), ChangeGroup::Revisions);
        QCOMPARE(realR->added, 1);   // "b"
        QCOMPARE(realR->removed, 2); // "B", "c"
        QCOMPARE(categoryOf(*find(swapped, QStringLiteral("new.txt"), ChangeGroup::Revisions), true), FileCategory::LeftOnly);
        QCOMPARE(repo->diskPath(*realR, rev, false), d + QStringLiteral("/real.txt"));
        QVERIFY(repo->diskPath(*realR, rev, true).isEmpty());
        const FileDiff rd = repo->loadDiff(*realR, rev, &error);
        QCOMPARE(rd.lines[1].kind, LineKind::Removed); // the working tree's "B" is now the old side
        QCOMPARE(rd.lines[1].text, QStringLiteral("B"));
        QCOMPARE(repo->fileContent(*realR, rev, true)->left(3), QByteArray("a\nb")); // HEAD on the right
        // Review marks follow the files, whichever way round.
        QCOMPARE(repo->fingerprint(*realR, rev), repo->fingerprint(*find(repo->listChanges(cmp, &error), QStringLiteral("real.txt"), ChangeGroup::Revisions), cmp));
        const FileDiff nd = repo->loadDiff(*find(swapped, QStringLiteral("new.txt"), ChangeGroup::Revisions), rev, &error);
        QCOMPARE(nd.lines[0].kind, LineKind::Removed);

        QVERIFY(!repo->lastCommit(QStringLiteral("real.txt")).isEmpty());
        QVERIFY(repo->lastCommit(QStringLiteral("new.txt")).isEmpty());
    }

    void stageUnstageRevert()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        write(d + QStringLiteral("/a.txt"), "a\n");
        write(d + QStringLiteral("/b.txt"), "b\n");
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("init")});
        write(d + QStringLiteral("/a.txt"), "A\n");
        write(d + QStringLiteral("/b.txt"), "B\n");
        write(d + QStringLiteral("/u.txt"), "untracked\n");
        QString error;
        auto repo = GitRepo::open(d, &error);
        Comparison split;
        auto changes = repo->listChanges(split, &error);
        const FileChange a = *find(changes, QStringLiteral("a.txt"), ChangeGroup::Unstaged);
        QVERIFY2(repo->stage({a}, &error), qPrintable(error));
        changes = repo->listChanges(split, &error);
        QVERIFY(find(changes, QStringLiteral("a.txt"), ChangeGroup::Staged));
        QVERIFY2(repo->unstage({*find(changes, QStringLiteral("a.txt"), ChangeGroup::Staged)}, &error), qPrintable(error));
        QVERIFY(!find(repo->listChanges(split, &error), QStringLiteral("a.txt"), ChangeGroup::Staged));

        // Revert: unstaged edit goes back to HEAD, untracked file is removed.
        changes = repo->listChanges(split, &error);
        QVERIFY2(repo->revert({*find(changes, QStringLiteral("b.txt"), ChangeGroup::Unstaged),
                               *find(changes, QStringLiteral("u.txt"), ChangeGroup::Unstaged)},
                              split, &error),
                 qPrintable(error));
        QFile b(d + QStringLiteral("/b.txt"));
        QVERIFY(b.open(QIODevice::ReadOnly));
        QCOMPARE(b.readAll(), QByteArray("b\n"));
        QVERIFY(!QFileInfo::exists(d + QStringLiteral("/u.txt")));

        // "All changes": a staged edit is reverted too.
        git(d, {QStringLiteral("add"), QStringLiteral("a.txt")});
        Comparison all;
        all.allChanges = true;
        changes = repo->listChanges(all, &error);
        QVERIFY2(repo->revert({*find(changes, QStringLiteral("a.txt"), ChangeGroup::Revisions)}, all, &error), qPrintable(error));
        QVERIFY(repo->listChanges(all, &error).isEmpty());

        // Swapped sides refuse to write.
        write(d + QStringLiteral("/a.txt"), "again\n");
        all.reversed = true;
        changes = repo->listChanges(all, &error);
        QVERIFY(!repo->revert(changes, all, &error));
        QVERIFY(error.contains(QStringLiteral("Swap")));

        // Directory comparison: the right file becomes a copy of the left one.
        QDir(d).mkpath(QStringLiteral("L"));
        QDir(d).mkpath(QStringLiteral("R"));
        write(d + QStringLiteral("/L/x.txt"), "left\n");
        write(d + QStringLiteral("/R/x.txt"), "right\n");
        Comparison pc;
        pc.mode = Comparison::Mode::Paths;
        pc.leftPath = d + QStringLiteral("/L");
        pc.rightPath = d + QStringLiteral("/R");
        GitRepo none = GitRepo::forPaths(d);
        QVERIFY2(none.revert(none.listChanges(pc, &error), pc, &error), qPrintable(error));
        QFile r(d + QStringLiteral("/R/x.txt"));
        QVERIFY(r.open(QIODevice::ReadOnly));
        QCOMPARE(r.readAll(), QByteArray("left\n"));
    }
    void blameEachSide()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        write(d + QStringLiteral("/f.txt"), "one\ntwo\n");
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("first")});
        write(d + QStringLiteral("/f.txt"), "one\nTWO\nthree\n");
        git(d, {QStringLiteral("commit"), QStringLiteral("-qam"), QStringLiteral("second")});
        write(d + QStringLiteral("/f.txt"), "one\nTWO\nthree\nstaged\n");
        git(d, {QStringLiteral("add"), QStringLiteral("f.txt")});
        write(d + QStringLiteral("/f.txt"), "zero\none\nTWO\nthree\nstaged\n");
        QString error;
        auto repo = GitRepo::open(d, &error);
        QVERIFY(repo);

        // Working tree against HEAD: the old side is HEAD, the new side the working tree.
        Comparison all;
        all.allChanges = true;
        const auto changes = repo->listChanges(all, &error);
        QCOMPARE(changes.size(), 1);
        const QVector<BlameLine> head = repo->blame(changes[0], all, false);
        QCOMPARE(head.size(), 3);
        QCOMPARE(head[0].summary, QStringLiteral("first"));
        QCOMPARE(head[1].summary, QStringLiteral("second"));
        QCOMPARE(head[1].author, QStringLiteral("t"));
        QVERIFY(head[1].time > 0);
        QVERIFY(head[0].hash != head[1].hash);
        const QVector<BlameLine> work = repo->blame(changes[0], all, true);
        QCOMPARE(work.size(), 5);
        QVERIFY(work[0].hash.isEmpty()); // not committed yet
        QCOMPARE(work[1].summary, QStringLiteral("first"));
        QVERIFY(work[4].hash.isEmpty());

        // Staged: the new side is the index (which lacks "zero").
        Comparison split;
        const auto splitChanges = repo->listChanges(split, &error);
        const FileChange *staged = find(splitChanges, QStringLiteral("f.txt"), ChangeGroup::Staged);
        QVERIFY(staged);
        const QVector<BlameLine> index = repo->blame(*staged, split, true);
        QCOMPARE(index.size(), 4);
        QCOMPARE(index[0].summary, QStringLiteral("first"));
        QVERIFY(index[3].hash.isEmpty());

        // Revisions, also swapped.
        Comparison rc;
        rc.mode = Comparison::Mode::Revisions;
        rc.rev1 = *repo->resolveCommit(QStringLiteral("HEAD~1"), &error);
        rc.rev2 = *repo->resolveCommit(QStringLiteral("HEAD"), &error);
        const auto revChanges = repo->listChanges(rc, &error);
        QCOMPARE(repo->blame(revChanges[0], rc, false).size(), 2);
        QCOMPARE(repo->blame(revChanges[0], rc, true).size(), 3);
        rc.reversed = true;
        const auto swapped = repo->listChanges(rc, &error);
        QCOMPARE(repo->blame(swapped[0], rc, false).size(), 3);

        // The empty tree resolves, so a root commit can be compared with "its parent".
        QVERIFY(repo->resolveCommit(repo->emptyTree(), &error));
        QVERIFY(!repo->resolveCommit(repo->emptyTree().left(8) + QStringLiteral("zz"), &error));
    }
    void inProcessDiffOptions()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        write(d + QStringLiteral("/f.txt"), "Alpha\n  beta\ngamma\ndelta\n");
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("init")});
        write(d + QStringLiteral("/f.txt"), "alpha\nbeta  \ngamma\nDELTA\nepsilon\n");
        QString error;
        auto repo = GitRepo::open(d, &error);
        Comparison cmp;
        cmp.allChanges = true;
        const FileChange fc = repo->listChanges(cmp, &error).value(0);
        QCOMPARE(fc.path, QStringLiteral("f.txt"));
        auto changed = [](const FileDiff &fd) {
            int n = 0;
            for (const DiffLine &l : fd.lines)
                n += l.kind != LineKind::Equal;
            return n;
        };

        for (DiffAlgorithm algo : allAlgorithms()) {
            cmp.options = DiffOptions();
            cmp.options.algorithm = algo;
            FileDiff fd = repo->loadDiff(fc, cmp, &error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(changed(fd), 7); // 3 removed + 4 added (gamma is unchanged)

            cmp.options.ignoreCase = true;
            fd = repo->loadDiff(fc, cmp, &error);
            QCOMPARE(changed(fd), 3); // beta's whitespace (2 lines) + epsilon
            QCOMPARE(fd.lines[0].kind, LineKind::Equal);
            QCOMPARE(fd.lines[0].text, QStringLiteral("alpha"));
            QCOMPARE(fd.lines[0].oldText, QStringLiteral("Alpha"));

            cmp.options.ignoreTrim = true;
            fd = repo->loadDiff(fc, cmp, &error);
            QCOMPARE(changed(fd), 1);
            QCOMPARE(fd.lines.last().text, QStringLiteral("epsilon"));
            QCOMPARE(fd.lines.last().newNo, 5);
        }

        // Nothing left once everything that differs is ignored.
        write(d + QStringLiteral("/f.txt"), "ALPHA\n  beta\ngamma\ndelta\n");
        cmp.options = DiffOptions();
        cmp.options.ignoreCase = true;
        const FileDiff none = repo->loadDiff(fc, cmp, &error);
        QVERIFY(none.lines.isEmpty());
        QVERIFY(none.message.contains(QStringLiteral("current options")));

        // Swapped sides work too.
        cmp.reversed = true;
        cmp.options.algorithm = DiffAlgorithm::Heckel;
        cmp.options.ignoreCase = false;
        const FileDiff rev = repo->loadDiff(repo->listChanges(cmp, &error).value(0), cmp, &error);
        QCOMPARE(rev.lines[0].kind, LineKind::Removed);
        QCOMPARE(rev.lines[0].text, QStringLiteral("ALPHA"));
    }

    void commentsUnimportantTextAndAnchors()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        write(d + QStringLiteral("/a.cpp"), "int a; // old\nint b;\n// built 2024-01-01 10:00\nint c;\n");
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("init")});
        write(d + QStringLiteral("/a.cpp"),
              "int a; // new comment\nint b;\n// built 2025-06-30 18:45\n// an added comment\nint c;\nint d;\n");
        QString error;
        auto repo = GitRepo::open(d, &error);
        Comparison cmp;
        cmp.allChanges = true;
        const FileChange fc = repo->listChanges(cmp, &error).value(0);
        auto counted = [](const FileDiff &fd) {
            int n = 0;
            for (const DiffLine &l : fd.lines)
                n += l.kind != LineKind::Equal && !l.unimportant;
            return n;
        };
        FileDiff fd = repo->loadDiff(fc, cmp, &error);
        QCOMPARE(counted(fd), 6); // 2 removed + 4 added

        for (DiffAlgorithm algo : {DiffAlgorithm::Patience, DiffAlgorithm::MyersLcs}) {
            cmp.options = DiffOptions();
            cmp.options.algorithm = algo;
            cmp.options.ignoreComments = true;
            fd = repo->loadDiff(fc, cmp, &error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(counted(fd), 1); // only "int d;"
            QCOMPARE(fd.chunks.size(), 1);
            QCOMPARE(fd.lines[0].kind, LineKind::Equal); // the comment changed: equal, greyed
            QVERIFY(fd.lines[0].unimportant);
            QCOMPARE(fd.lines[0].oldText, QStringLiteral("int a; // old"));
            bool addedComment = false;
            for (const DiffLine &l : fd.lines)
                addedComment |= l.kind == LineKind::Added && l.unimportant && l.text.contains(QStringLiteral("added"));
            QVERIFY(addedComment);
        }

        // Unimportant text: only the timestamps ignored.
        cmp.options = DiffOptions();
        cmp.options.unimportantPatterns = {QStringLiteral("\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}"), QStringLiteral("([")};
        fd = repo->loadDiff(fc, cmp, &error); // the invalid pattern is skipped
        QCOMPARE(counted(fd), 4); // int a's comment (2) + the added comment + int d
        bool stamp = false;
        for (const DiffLine &l : fd.lines)
            stamp |= l.kind == LineKind::Equal && l.unimportant && l.oldText.contains(QStringLiteral("2024"));
        QVERIFY(stamp);

        // Manual alignment: line 4 "int c;" lined up with the added comment (new line 4).
        cmp.options = DiffOptions();
        FileChange anchored = fc;
        anchored.anchors = {{4, 4}};
        fd = repo->loadDiff(anchored, cmp, &error);
        int r = -1, s = -1;
        for (int i = 0; i < fd.lines.size(); ++i) {
            if (fd.lines[i].kind == LineKind::Removed && fd.lines[i].oldNo == 4)
                r = i;
            if (fd.lines[i].kind == LineKind::Added && fd.lines[i].newNo == 4)
                s = i;
        }
        QVERIFY(r >= 0 && s >= 0);
        QCOMPARE(fd.lines[r].pair, s);
        QVERIFY(fd.lines[r].anchored && fd.lines[s].anchored);
    }
};

QTEST_APPLESS_MAIN(TestGitRepo)
#include "test_gitrepo.moc"
