#include "DiffFormat.h"
#include "GitHubPr.h"
#include "Difftool.h"
#include "FileFilter.h"
#include "GitHubReview.h"
#include "Report.h"
#include "ReviewStore.h"
#include "Search.h"
#include "Session.h"
#include "ShellMenu.h"
#include "TextFile.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

class TestFormat : public QObject
{
    Q_OBJECT
private slots:
    void singleChunkWithContext()
    {
        // 10 lines, line 5 edited, line 9 edited (a second chunk that must not leak in).
        QString diff = QStringLiteral("@@ -1,10 +1,10 @@\n");
        for (int i = 1; i <= 10; ++i) {
            if (i == 5 || i == 9)
                diff += QStringLiteral("-old %1\n+new %1\n").arg(i);
            else
                diff += QStringLiteral(" line %1\n").arg(i);
        }
        const FileDiff d = DiffModel::parseUnified(diff, false);
        QCOMPARE(d.chunks.size(), 2);

        const auto out = DiffFormat::unified(d, {QStringLiteral("a.txt"), QStringLiteral("a.txt")}, {0});
        const QStringList lines = out.text.split(u'\n', Qt::SkipEmptyParts);
        QCOMPARE(lines[0], QStringLiteral("diff --git a/a.txt b/a.txt"));
        QCOMPARE(lines[1], QStringLiteral("--- a/a.txt"));
        QCOMPARE(lines[2], QStringLiteral("+++ b/a.txt"));
        QCOMPARE(lines[3], QStringLiteral("@@ -2,7 +2,7 @@")); // 3 before, -/+, 3 after (stops at line 8)
        QCOMPARE(lines[4], QStringLiteral(" line 2"));
        QVERIFY(lines.contains(QStringLiteral("-old 5")));
        QVERIFY(lines.contains(QStringLiteral("+new 5")));
        QVERIFY(!lines.contains(QStringLiteral("-old 9")));
        QVERIFY(out.html.contains(QStringLiteral("background:#ffebe9")));
        QVERIFY(out.html.contains(QStringLiteral("background:#dafbe1")));
    }

    void wholeFileMergesNearbyHunks()
    {
        QString diff = QStringLiteral("@@ -1,10 +1,10 @@\n");
        for (int i = 1; i <= 10; ++i)
            diff += (i == 5 || i == 9) ? QStringLiteral("-old %1\n+new %1\n").arg(i) : QStringLiteral(" line %1\n").arg(i);
        const FileDiff d = DiffModel::parseUnified(diff, false);
        const auto out = DiffFormat::unifiedFile(d, {QStringLiteral("a.txt"), QStringLiteral("a.txt")});
        QCOMPARE(out.text.count(QStringLiteral("@@ -")), 1);
        QVERIFY(out.text.contains(QStringLiteral("@@ -2,9 +2,9 @@")));
    }

    void newFileHeaderAndCounts()
    {
        const FileDiff d = DiffModel::allAdded(QStringLiteral("one\n"));
        const auto out = DiffFormat::unifiedFile(d, {QString(), QStringLiteral("n.txt")});
        QVERIFY(out.text.contains(QStringLiteral("--- /dev/null\n+++ b/n.txt\n@@ -0,0 +1 @@\n+one\n")));
    }

    void sideText()
    {
        const FileDiff d = DiffModel::parseUnified(QStringLiteral("@@ -1,2 +1,2 @@\n-a\n-b\n+c\n+d\n"), false);
        QCOMPARE(DiffFormat::sideText(d, 0, LineKind::Removed), QStringLiteral("a\nb\n"));
        QCOMPARE(DiffFormat::sideText(d, 0, LineKind::Added), QStringLiteral("c\nd\n"));
    }

    void parsesPrLinks()
    {
        auto r = GitHubPr::parse(QStringLiteral("https://github.com/jsmrcina/yadiff/pull/12/files?diff=split#x"));
        QVERIFY(r);
        QCOMPARE(r->owner, QStringLiteral("jsmrcina"));
        QCOMPARE(r->repo, QStringLiteral("yadiff"));
        QCOMPARE(r->number, 12);
        QVERIFY(GitHubPr::parse(QStringLiteral("github.com/a/b.c/pull/3")));
        QCOMPARE(GitHubPr::parse(QStringLiteral("a/b#7"))->number, 7);
        QVERIFY(!GitHubPr::parse(QStringLiteral("https://github.com/a/b/issues/3")));
        QVERIFY(!GitHubPr::parse(QStringLiteral("https://gitlab.com/a/b/pull/3")));
    }

    void matchesRemotes()
    {
        QVERIFY(GitHubPr::remoteMatches(QStringLiteral("https://github.com/Jsmrcina/yadiff.git"), QStringLiteral("jsmrcina"),
                                        QStringLiteral("yadiff")));
        QVERIFY(GitHubPr::remoteMatches(QStringLiteral("git@github.com:jsmrcina/yadiff.git"), QStringLiteral("jsmrcina"),
                                        QStringLiteral("yadiff")));
        QVERIFY(GitHubPr::remoteMatches(QStringLiteral("ssh://git@github.com/jsmrcina/yadiff"), QStringLiteral("jsmrcina"),
                                        QStringLiteral("yadiff")));
        QVERIFY(!GitHubPr::remoteMatches(QStringLiteral("https://github.com/jsmrcina/yadiff-other.git"),
                                         QStringLiteral("jsmrcina"), QStringLiteral("yadiff")));
    }

    void reviewStore()
    {
        QTemporaryDir tmp;
        const QString ini = tmp.filePath(QStringLiteral("r.ini"));
        {
            ReviewStore s(QStringLiteral("/repo"), ini);
            QVERIFY(!s.hasAny(QStringLiteral("a/b.txt")));
            s.setReviewed(QStringLiteral("a/b.txt"), "fp1", true);
        }
        ReviewStore s(QStringLiteral("/repo"), ini);
        QVERIFY(s.isReviewed(QStringLiteral("a/b.txt"), "fp1"));
        QVERIFY(!s.isReviewed(QStringLiteral("a/b.txt"), "fp2"));
        QVERIFY(!ReviewStore(QStringLiteral("/other"), ini).hasAny(QStringLiteral("a/b.txt")));
        s.setReviewed(QStringLiteral("a/b.txt"), "fp1", false);
        QVERIFY(!s.hasAny(QStringLiteral("a/b.txt")));
    }

    void searchSubstringRegexCase()
    {
        const SearchQuery plain(QStringLiteral("ab"), false, false);
        QCOMPARE(plain.find(QStringLiteral("xAbyabab")).size(), 3);
        const SearchQuery cased(QStringLiteral("ab"), false, true);
        QCOMPARE(cased.find(QStringLiteral("xAbyabab")).size(), 2);
        const SearchQuery re(QStringLiteral("a+b"), true, false);
        const auto m = re.find(QStringLiteral("aab b AB"));
        QCOMPARE(m.size(), 2);
        QCOMPARE(m[0].start, 0);
        QCOMPARE(m[0].length, 3);
        QVERIFY(!SearchQuery(QStringLiteral("(unclosed"), true, false).isValid());
        QVERIFY(SearchQuery(QStringLiteral("x*"), true, false).find(QStringLiteral("abc")).isEmpty()); // empty matches skipped
    }

    void applyChunkBothDirections()
    {
        // old: a b c d   new: a B c X d   (two chunks: b->B, insertion of X)
        const FileDiff d = DiffModel::parseUnified(QStringLiteral("@@ -1,4 +1,5 @@\n a\n-b\n+B\n c\n+X\n d\n"), false);
        QCOMPARE(d.chunks.size(), 2);
        const QStringList newSide{QStringLiteral("a"), QStringLiteral("B"), QStringLiteral("c"), QStringLiteral("X"), QStringLiteral("d")};
        const QStringList oldSide{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("d")};
        QString error;
        // Revert the first change in the new file.
        auto r = TextFile::applyChunk(newSide, d, 0, true, &error);
        QVERIFY2(r, qPrintable(error));
        QCOMPARE(*r, (QStringList{"a", "b", "c", "X", "d"}));
        // Revert the insertion in the new file (a deletion there).
        r = TextFile::applyChunk(newSide, d, 1, true, &error);
        QCOMPARE(*r, (QStringList{"a", "B", "c", "d"}));
        // Copy the insertion into the old file.
        r = TextFile::applyChunk(oldSide, d, 1, false, &error);
        QCOMPARE(*r, (QStringList{"a", "b", "c", "X", "d"}));
        // A file that changed since the diff is refused.
        QVERIFY(!TextFile::applyChunk(QStringList{"a", "zzz", "c", "X", "d"}, d, 0, true, &error));
        QVERIFY(error.contains(QStringLiteral("changed on disk")));
    }

    void textFileKeepsLineEndings()
    {
        QTemporaryDir tmp;
        const QString path = tmp.filePath(QStringLiteral("crlf.txt"));
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("one\r\ntwo");
        }
        QString error;
        auto c = TextFile::read(path, &error);
        QVERIFY(c);
        QCOMPARE(c->lines, (QStringList{"one", "two"}));
        QCOMPARE(c->eol, QByteArray("\r\n"));
        QVERIFY(!c->trailingNewline);
        c->lines << QStringLiteral("three");
        QVERIFY(TextFile::write(path, *c, &error));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("one\r\ntwo\r\nthree"));
    }

    void difftoolRegistration()
    {
        QTemporaryDir tmp;
        const QString cfg = tmp.filePath(QStringLiteral("gitconfig"));
        qputenv("GIT_CONFIG_GLOBAL", cfg.toUtf8()); // never touch the real ~/.gitconfig
        QString summary, error;
        QVERIFY(!Difftool::isRegistered());
        QVERIFY2(Difftool::registerTool(QStringLiteral("/opt/my tools/yadiff"), true, &summary, &error), qPrintable(error));
        QVERIFY(Difftool::isRegistered());
        QProcess p;
        p.start(QStringLiteral("git"), {QStringLiteral("config"), QStringLiteral("--global"), QStringLiteral("difftool.yadiff.cmd")});
        p.waitForFinished();
        QCOMPARE(QString::fromUtf8(p.readAllStandardOutput()).trimmed(),
                 QStringLiteral("'/opt/my tools/yadiff' --wait --no-index \"$LOCAL\" \"$REMOTE\""));
        QVERIFY(Difftool::unregisterTool(&summary, &error));
        QVERIFY(!Difftool::isRegistered());
        qunsetenv("GIT_CONFIG_GLOBAL");
    }

    void fileFilterTerms()
    {
        FileFilter none;
        QVERIFY(!none.isActive());
        QVERIFY(none.matches(QStringLiteral("a/b.cpp"), u'M', false));

        FileFilter sub(QStringLiteral("core"));
        QVERIFY(sub.matches(QStringLiteral("src/core/x.cpp"), u'M', false));
        QVERIFY(!sub.matches(QStringLiteral("src/ui/x.cpp"), u'M', false));

        FileFilter glob(QStringLiteral("*.cpp *.h"));
        QVERIFY(glob.matches(QStringLiteral("src/a.cpp"), u'M', false));
        QVERIFY(glob.matches(QStringLiteral("a.h"), u'M', false));
        QVERIFY(!glob.matches(QStringLiteral("README.md"), u'M', false));

        FileFilter excl(QStringLiteral("!*.lock !vendor"));
        QVERIFY(excl.matches(QStringLiteral("src/a.cpp"), u'M', false));
        QVERIFY(!excl.matches(QStringLiteral("Cargo.lock"), u'M', false));
        QVERIFY(!excl.matches(QStringLiteral("third/vendor/x.c"), u'M', false));

        FileFilter status;
        status.hiddenStatuses = {u'D'};
        status.hideReviewed = true;
        QVERIFY(status.isActive());
        QVERIFY(!status.matches(QStringLiteral("x"), u'D', false));
        QVERIFY(!status.matches(QStringLiteral("x"), u'M', true));
        QVERIFY(status.matches(QStringLiteral("x"), u'M', false));
    }

    void reportHtml()
    {
        Report::Input in;
        in.title = QStringLiteral("yadiff — repo <x>");
        in.description = QStringLiteral("100% %1 safe");
        Report::File f;
        f.change.path = f.change.oldPath = QStringLiteral("src/a&b.cpp");
        f.change.status = u'M';
        f.change.added = 1;
        f.change.removed = 1;
        f.reviewed = true;
        QString text = QStringLiteral("@@ -1,12 +1,12 @@\n");
        for (int i = 1; i <= 12; ++i)
            text += i == 10 ? QStringLiteral("-int x = <old>;\n+int x = <new>;\n") : QStringLiteral(" ctx %1\n").arg(i);
        f.diff = DiffModel::parseUnified(text);
        Report::File img;
        img.change.path = img.change.oldPath = QStringLiteral("logo.png");
        img.change.status = u'M';
        img.diff.binary = true;
        img.newImage = QByteArray("PNGDATA");
        in.files = {f, img};
        const QString h = Report::html(in);
        QVERIFY(h.startsWith(QStringLiteral("<!doctype html>")));
        QVERIFY(h.contains(QStringLiteral("repo &lt;x&gt;")));
        QVERIFY(h.contains(QStringLiteral("100% %1 safe"))); // no QString::arg re-substitution
        QVERIFY(h.contains(QStringLiteral("src/a&amp;b.cpp")));
        QVERIFY(h.contains(QStringLiteral("&lt;<mark>old</mark>&gt;")));
        QVERIFY(h.contains(QStringLiteral("<tr class=\"add\">")));
        QVERIFY(h.contains(QStringLiteral("6 unchanged lines"))); // lines 1-6 folded (3 kept before line 10)
        QVERIFY(h.contains(QStringLiteral("&#10003; reviewed")));
        QVERIFY(h.contains(QStringLiteral("data:image/png;base64,") + QString::fromLatin1(QByteArray("PNGDATA").toBase64())));
        QVERIFY(!h.contains(QStringLiteral("ctx 1<"))); // folded away
    }

    void reviewCommentsParseAndThread()
    {
        // Two pages, as `gh api --paginate --slurp` returns them.
        const QByteArray json =
            "[[{\"id\": 3972146180, \"in_reply_to_id\": null, \"path\": \"README.md\", \"line\": 71, \"start_line\": 60,"
            " \"side\": \"RIGHT\", \"user\": {\"login\": \"alice\"}, \"body\": \"Reorder\","
            " \"created_at\": \"2026-01-01T10:00:00Z\", \"html_url\": \"https://github.com/o/r/pull/1\"},"
            " {\"id\": 3972196522, \"in_reply_to_id\": 3972146180, \"path\": \"README.md\", \"line\": 71, \"side\": \"RIGHT\","
            " \"user\": {\"login\": \"bob\"}, \"body\": \"Done\", \"created_at\": \"2026-01-01T11:00:00Z\"}],"
            " [{\"id\": 5, \"path\": \"a.cpp\", \"line\": null, \"side\": \"LEFT\", \"user\": {\"login\": \"carol\"},"
            " \"body\": \"old\", \"created_at\": \"2025-12-31T00:00:00Z\"},"
            " {\"id\": 6, \"in_reply_to_id\": 3972196522, \"path\": \"README.md\", \"line\": 71, \"side\": \"RIGHT\","
            " \"user\": {\"login\": \"dave\"}, \"body\": \"reply to reply\", \"created_at\": \"2026-01-02T00:00:00Z\"}]]";
        const auto comments = GitHubReview::parseComments(json);
        QCOMPARE(comments.size(), 4);
        QCOMPARE(comments[0].id, qint64(3972146180));
        QCOMPARE(comments[0].startLine, 60);
        QVERIFY(comments[2].outdated());
        const auto threads = GitHubReview::threads(comments);
        QCOMPARE(threads.size(), 2);
        QCOMPARE(threads[1].size(), 3); // alice, bob, dave (reply to bob's reply)
        QCOMPARE(threads[1][2].author, QStringLiteral("dave"));

        const QJsonObject c = QJsonDocument::fromJson(GitHubReview::commentPayload(
            QStringLiteral("abc"), QStringLiteral("a.cpp"), QStringLiteral("RIGHT"), 12, 10, QStringLiteral("hi"))).object();
        QCOMPARE(c.value(QStringLiteral("line")).toInt(), 12);
        QCOMPARE(c.value(QStringLiteral("start_line")).toInt(), 10);
        QCOMPARE(c.value(QStringLiteral("start_side")).toString(), QStringLiteral("RIGHT"));
        QCOMPARE(c.value(QStringLiteral("commit_id")).toString(), QStringLiteral("abc"));
        const QJsonObject single = QJsonDocument::fromJson(GitHubReview::commentPayload(
            QStringLiteral("abc"), QStringLiteral("a.cpp"), QStringLiteral("LEFT"), 3, 0, QStringLiteral("x"))).object();
        QVERIFY(!single.contains(QStringLiteral("start_line")));
        const QJsonObject review = QJsonDocument::fromJson(
            GitHubReview::reviewPayload(GitHubReview::Verdict::RequestChanges, QStringLiteral("fix"))).object();
        QCOMPARE(review.value(QStringLiteral("event")).toString(), QStringLiteral("REQUEST_CHANGES"));
        QCOMPARE(GitHubReview::reviewPayload(GitHubReview::Verdict::Approve, {}), QByteArray("{\"event\":\"APPROVE\"}"));
    }

    void globsFollowGitignoreRules()
    {
        QVERIFY(globMatches(QStringLiteral("*.lock"), QStringLiteral("Cargo.lock")));
        QVERIFY(globMatches(QStringLiteral("*.lock"), QStringLiteral("a/b/yarn.lock")));     // no slash: any folder
        QVERIFY(!globMatches(QStringLiteral("*.lock"), QStringLiteral("a/lock.txt")));
        QVERIFY(globMatches(QStringLiteral("build/"), QStringLiteral("build/x/y.o")));       // folder, anywhere
        QVERIFY(globMatches(QStringLiteral("build/"), QStringLiteral("src/build/y.o")));
        QVERIFY(!globMatches(QStringLiteral("build/"), QStringLiteral("src/build")));          // a file named build
        QVERIFY(globMatches(QStringLiteral("src/*.cpp"), QStringLiteral("src/a.cpp")));       // slash: from the top
        QVERIFY(!globMatches(QStringLiteral("src/*.cpp"), QStringLiteral("src/x/a.cpp")));    // * stays in a folder
        QVERIFY(!globMatches(QStringLiteral("src/*.cpp"), QStringLiteral("lib/src/a.cpp")));
        QVERIFY(globMatches(QStringLiteral("src/**/*.cpp"), QStringLiteral("src/x/y/a.cpp")));
        QVERIFY(globMatches(QStringLiteral("src/**/*.cpp"), QStringLiteral("src/a.cpp")));
        QVERIFY(globMatches(QStringLiteral("**/gen/*"), QStringLiteral("a/gen/b.h")));
        QVERIFY(globMatches(QStringLiteral("/docs/"), QStringLiteral("docs/a.md")));
        QVERIFY(globMatches(QStringLiteral("v?.txt"), QStringLiteral("v1.txt")));
        QVERIFY(globMatches(QStringLiteral("[ab].txt"), QStringLiteral("b.txt")));
        QVERIFY(!globMatches(QStringLiteral("# comment"), QStringLiteral("# comment")));
        QVERIFY(!globMatches(QString(), QStringLiteral("x")));
    }

    void diffHelpers()
    {
        const FileDiff eq = DiffModel::allEqual(QStringLiteral("a\nb\n"));
        QCOMPARE(eq.lines.size(), 2);
        QVERIFY(eq.chunks.isEmpty());
        QCOMPARE(eq.lines[1].oldNo, 2);
        const FileDiff d = DiffModel::parseUnified(QStringLiteral("@@ -1,2 +1,2 @@\n a\n-b\n+c\n"));
        const FileDiff r = DiffModel::reversed(d);
        QCOMPARE(r.lines[1].kind, LineKind::Added);
        QCOMPARE(r.lines[1].text, QStringLiteral("b"));
        QCOMPARE(r.lines[2].kind, LineKind::Removed);
        QCOMPARE(r.chunks.size(), 1);
    }

    void sessionsRoundTrip()
    {
        QTemporaryDir tmp;
        Session s;
        s.arguments = {QStringLiteral("-C"), QStringLiteral("/x y"), QStringLiteral("main"), QStringLiteral("--"),
                       QStringLiteral("src")};
        s.title = QStringLiteral("Review");
        s.currentFile = QStringLiteral("src/a.cpp");
        s.sideBySide = 0;
        s.hasOptions = true;
        s.options.algorithm = DiffAlgorithm::Heckel;
        s.options.ignoreComments = true;
        s.options.unimportantPatterns = {QStringLiteral("\\d+")};
        QString error;
        const QString path = tmp.filePath(QStringLiteral("s.yadiff"));
        QVERIFY(s.write(path, &error));
        const auto back = Session::read(path, &error);
        QVERIFY2(back, qPrintable(error));
        QCOMPARE(back->arguments, s.arguments);
        QCOMPARE(back->title, s.title);
        QCOMPARE(back->currentFile, s.currentFile);
        QCOMPARE(back->sideBySide, 0);
        QCOMPARE(back->options.algorithm, DiffAlgorithm::Heckel);
        QVERIFY(back->options.ignoreComments);
        QCOMPARE(back->options.unimportantPatterns, s.options.unimportantPatterns);
        QFile junk(tmp.filePath(QStringLiteral("junk.yadiff")));
        QVERIFY(junk.open(QIODevice::WriteOnly));
        junk.write("{}");
        junk.close();
        QVERIFY(!Session::read(junk.fileName(), &error));
    }

    void fileManagerMenu()
    {
#ifndef Q_OS_WIN
        QTemporaryDir tmp;
        qputenv("XDG_DATA_HOME", tmp.path().toUtf8());
        QString summary, error;
        QVERIFY(!ShellMenu::isInstalled());
        QVERIFY2(ShellMenu::install(QStringLiteral("/opt/my apps/yadiff"), &summary, &error), qPrintable(error));
        QVERIFY(ShellMenu::isInstalled());
        QFile pair(tmp.path() + QStringLiteral("/kio/servicemenus/yadiff-compare.desktop"));
        QVERIFY(pair.permissions() & QFile::ExeOwner); // Plasma 6 needs this
        QVERIFY(pair.open(QIODevice::ReadOnly));
        const QByteArray text = pair.readAll();
        QVERIFY(text.contains("Exec=\"/opt/my apps/yadiff\" --no-index %F"));
        QVERIFY(text.contains("X-KDE-MinNumberOfUrls=2"));
        QVERIFY(ShellMenu::uninstall(&summary, &error));
        QVERIFY(!ShellMenu::isInstalled());
        qunsetenv("XDG_DATA_HOME");
#endif
    }
};

QTEST_APPLESS_MAIN(TestFormat)
#include "test_format.moc"
