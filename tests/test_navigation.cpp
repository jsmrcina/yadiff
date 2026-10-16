#include "CodeView.h"
#include "DiffPane.h"
#include "FileList.h"
#include "HistoryDialog.h"
#include "ImageDiffView.h"
#include "MainWindow.h"
#include "SearchBar.h"
#include "Settings.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFile>
#include <QMenu>
#include <QMimeData>
#include <QTimer>
#include <QImage>
#include <QStackedWidget>
#include <QPushButton>
#include <QTreeWidget>
#include <QLineEdit>
#include <QScrollBar>
#include <QSet>
#include <QTextLayout>
#include <QAbstractTextDocumentLayout>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QToolButton>

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


// Shows a diff in a pane the way MainWindow does.
void showDiff(DiffPane &pane, const FileDiff &d, const QString &title, const QString &left, const QString &right,
              const DiffFormat::Paths &paths = {})
{
    FileView v;
    v.diff = d;
    v.title = title;
    v.leftLabel = left;
    v.rightLabel = right;
    v.paths = paths;
    v.fileName = title;
    pane.setFile(v);
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

    void equalLinesHaveNoBackgroundAfterDeletedFile()
    {
        // Regression: QPlainTextEdit::setPlainText kept the previous first block's
        // format, so after a fully-removed file every new line looked "removed".
        FileDiff deleted = DiffModel::parseUnified(QStringLiteral("@@ -1,2 +0,0 @@\n-gone 1\n-gone 2\n"));
        // A word-highlighted first line: its character format must not leak either.
        FileDiff highlighted = DiffModel::parseUnified(QStringLiteral("@@ -1 +1 @@\n-int value = alpha;\n+int value = beta;\n"));
        FileDiff edited = DiffModel::parseUnified(QStringLiteral("@@ -1,3 +1,3 @@\n same 1\n-old\n+new\n same 2\n"));
        for (const auto mode : {DiffPane::Mode::Inline, DiffPane::Mode::SideBySide}) {
            DiffPane pane;
            pane.setMode(mode);
            showDiff(pane, highlighted, QStringLiteral("w"), QStringLiteral("a"), QStringLiteral("b"));
            for (CodeView *view : pane.findChildren<CodeView *>()) {
                // Put the text cursor inside the highlighted word, as clicking there would.
                QTextCursor c(view->document());
                c.setPosition(view->document()->characterCount() > 14 ? 14 : 0);
                view->setTextCursor(c);
            }
            showDiff(pane, deleted, QStringLiteral("x"), QStringLiteral("a"), QStringLiteral("b"));
            showDiff(pane, edited, QStringLiteral("y"), QStringLiteral("a"), QStringLiteral("b"));
            for (CodeView *view : pane.findChildren<CodeView *>()) {
                if (view->rows().isEmpty())
                    continue;
                QTextBlock b = view->document()->firstBlock();
                for (const Row &r : view->rows()) {
                    const bool plain = r.kind == LineKind::Equal && !r.filler;
                    if (plain) {
                        QCOMPARE(b.blockFormat().background().style(), Qt::NoBrush);
                        for (auto it = b.begin(); !it.atEnd(); ++it)
                            QCOMPARE(it.fragment().charFormat().background().style(), Qt::NoBrush);
                    }
                    b = b.next();
                }
            }
        }
    }

    void reviewMarksPersistUntilFileChanges()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        write(d + QStringLiteral("/a.txt"), lines(5));
        write(d + QStringLiteral("/b.txt"), lines(5));
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("init")});
        write(d + QStringLiteral("/a.txt"), lines(5, 2));
        write(d + QStringLiteral("/b.txt"), lines(5, 3));

        QString error;
        auto repo = GitRepo::open(d, &error);
        QVERIFY(repo);
        MainWindow w(std::move(*repo), Comparison{});
        auto *files = w.findChild<FileList *>();
        QCOMPARE(files->reviewedCount(), 0);

        files->setReviewed(0, true);
        QVERIFY(files->isReviewed(0));
        w.reload();
        QVERIFY(files->isReviewed(0));   // survives a reload
        QVERIFY(!files->isReviewed(1));

        write(d + QStringLiteral("/a.txt"), lines(5, 4)); // a.txt changes again
        w.reload();
        QVERIFY(!files->isReviewed(0));  // so its mark is dropped
    }

    void viewToggleButtonSwitchesModes()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        write(d + QStringLiteral("/a.txt"), lines(5));
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("init")});
        write(d + QStringLiteral("/a.txt"), lines(5, 2));
        QString error;
        auto repo = GitRepo::open(d, &error);
        MainWindow w(std::move(*repo), Comparison{});
        auto *pane = w.findChild<DiffPane *>();
        QToolButton *toggle = nullptr;
        for (QToolButton *b : w.findChildren<QToolButton *>())
            if (b->text() == QStringLiteral("Side by side") || b->text() == QStringLiteral("Inline"))
                toggle = b;
        QVERIFY(toggle);
        const auto before = pane->mode();
        toggle->click();
        QVERIFY(pane->mode() != before);
        QCOMPARE(toggle->text(), pane->mode() == DiffPane::Mode::Inline ? QStringLiteral("Inline") : QStringLiteral("Side by side"));
        toggle->click();
        QCOMPARE(pane->mode(), before);
    }

    void contextMenuCopiesChangeAsDiff()
    {
        QString text = QStringLiteral("@@ -1,5 +1,5 @@\n a\n b\n-old line\n+new line\n c\n d\n");
        const FileDiff diff = DiffModel::parseUnified(text);
        for (const auto mode : {DiffPane::Mode::Inline, DiffPane::Mode::SideBySide}) {
            DiffPane pane;
            pane.setMode(mode);
            pane.resize(800, 400);
            pane.show();
            showDiff(pane, diff, QStringLiteral("f.txt"), QStringLiteral("HEAD: f.txt"), QStringLiteral("Working tree: f.txt"),
                         {QStringLiteral("f.txt"), QStringLiteral("f.txt")});
            QVERIFY(QTest::qWaitForWindowExposed(&pane));
            CodeView *view = nullptr;
            for (CodeView *v : pane.findChildren<CodeView *>())
                if (v->isVisible())
                    view = v; // inline view, or the right-hand pane
            QVERIFY(view);
            QApplication::clipboard()->clear();

            bool triggered = false;
            QTimer::singleShot(0, [&] {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (!menu)
                    return;
                for (QAction *a : menu->actions())
                    if (a->text().startsWith(QStringLiteral("Copy this change as")) && a->isEnabled()) {
                        a->trigger();
                        triggered = true;
                    }
                menu->close();
            });
            // Row 2 or 3 is inside the change in both layouts.
            const int lineHeight = view->fontMetrics().lineSpacing();
            const QPoint pos(20, int(lineHeight * 2.5));
            QContextMenuEvent ev(QContextMenuEvent::Mouse, pos, view->viewport()->mapToGlobal(pos));
            QApplication::sendEvent(view->viewport(), &ev);

            QVERIFY(triggered);
            const QMimeData *mime = QApplication::clipboard()->mimeData();
            QVERIFY(mime->text().contains(QStringLiteral("--- a/f.txt\n+++ b/f.txt\n@@ -1,5 +1,5 @@\n a\n b\n-old line\n+new line\n")));
            QVERIFY(mime->hasHtml());
            QVERIFY(mime->html().contains(QStringLiteral("-old line")));
        }
    }

    void whitespaceButtonHidesWhitespaceOnlyFiles()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        write(d + QStringLiteral("/code.txt"), "a b\nc\n");
        write(d + QStringLiteral("/real.txt"), "x\n");
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("init")});
        write(d + QStringLiteral("/code.txt"), "a    b\nc\n"); // whitespace only
        write(d + QStringLiteral("/real.txt"), "y\n");

        QString error;
        auto repo = GitRepo::open(d, &error);
        Comparison cmp;
        cmp.allChanges = true;
        MainWindow w(std::move(*repo), cmp);
        auto *files = w.findChild<FileList *>();
        QToolButton *ws = nullptr;
        for (QToolButton *b : w.findChildren<QToolButton *>())
            if (b->text() == QStringLiteral("Ignore whitespace"))
                ws = b;
        QVERIFY(ws);
        QVERIFY(!ws->isChecked());
        auto fileCount = [&] {
            int n = 0;
            for (int i = 0; i < 10; ++i)
                n += files->selectIndex(i);
            return n;
        };
        QCOMPARE(fileCount(), 2);
        ws->click();
        QVERIFY(ws->isChecked());
        QCOMPARE(fileCount(), 1);
        ws->click();
        QCOMPARE(fileCount(), 2);
    }

    void shortcutsAreUnique()
    {
        QTemporaryDir tmp;
        git(tmp.path(), {QStringLiteral("init"), QStringLiteral("-q")});
        QString error;
        auto repo = GitRepo::open(tmp.path(), &error);
        MainWindow w(std::move(*repo), Comparison{});
        QHash<QString, QString> seen;
        for (QAction *a : w.findChildren<QAction *>()) {
            for (const QKeySequence &k : a->shortcuts()) {
                const QString key = k.toString();
                QVERIFY2(!seen.contains(key), qPrintable(QStringLiteral("%1 used by '%2' and '%3'").arg(key, seen.value(key), a->text())));
                seen.insert(key, a->text());
            }
        }
        QVERIFY(seen.contains(QStringLiteral("F3")));
    }

    void collapseAndExpandUnchanged()
    {
        QString text = QStringLiteral("@@ -1,40 +1,40 @@\n");
        for (int i = 1; i <= 40; ++i)
            text += i == 20 ? QStringLiteral("-old\n+new\n") : QStringLiteral(" line %1\n").arg(i);
        const FileDiff diff = DiffModel::parseUnified(text);
        DiffPane pane;
        pane.setMode(DiffPane::Mode::Inline);
        pane.setCollapseUnchanged(true);
        showDiff(pane, diff, QStringLiteral("f.txt"), QStringLiteral("a"), QStringLiteral("b"));
        CodeView *view = nullptr;
        for (CodeView *v : pane.findChildren<CodeView *>())
            if (!v->rows().isEmpty())
                view = v;
        QVERIFY(view);
        int folds = 0;
        for (const Row &r : view->rows())
            folds += r.fold;
        QCOMPARE(folds, 2);                    // before and after the change
        QCOMPARE(view->rows().size(), 2 + 3 + 2 + 3); // fold, 3 context, -/+, 3 context, fold
        int foldRow = 0;
        while (!view->rows()[foldRow].fold)
            ++foldRow;
        emit view->foldClicked(foldRow);
        folds = 0;
        for (const Row &r : view->rows())
            folds += r.fold;
        QCOMPARE(folds, 1);
        pane.expandAll();
        QCOMPARE(view->rows().size(), 41);
        pane.setCollapseUnchanged(false);
        QCOMPARE(view->rows().size(), 41);
    }

    void searchThisFileAndAllChanges()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        write(d + QStringLiteral("/a.txt"), lines(5));
        write(d + QStringLiteral("/b.txt"), lines(5));
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("init")});
        write(d + QStringLiteral("/a.txt"), "needle one\nline 2\nneedle two\n");
        write(d + QStringLiteral("/b.txt"), "line 1\nNEEDLE three\n");
        QString error;
        auto repo = GitRepo::open(d, &error);
        Comparison cmp;
        cmp.allChanges = true;
        MainWindow w(std::move(*repo), cmp);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.activateWindow();
        auto *files = w.findChild<FileList *>();
        auto *pane = w.findChild<DiffPane *>();
        auto *bar = w.findChild<SearchBar *>();
        QVERIFY(files && pane && bar);
        QCOMPARE(files->currentIndex(), 0);

        // This file, substring, case-insensitive.
        pane->setSearch(SearchQuery(QStringLiteral("needle"), false, false), DiffPane::SearchScope::File);
        QCOMPARE(pane->matchCount(), 2);
        QVERIFY(pane->nextMatch());
        QVERIFY(!pane->nextMatch());
        // Regex.
        pane->setSearch(SearchQuery(QStringLiteral("needle (one|two)"), true, true), DiffPane::SearchScope::File);
        QCOMPARE(pane->matchCount(), 2);

        // All changes through the search bar: Enter past the last match moves to b.txt.
        QTest::keyClick(&w, Qt::Key_F, Qt::ControlModifier);
        QVERIFY(bar->isVisible());
        bar->setScope(SearchBar::Scope::AllChanges);
        auto *edit = bar->findChild<QLineEdit *>();
        edit->setText(QStringLiteral("needle"));
        QTRY_COMPARE(pane->matchCount(), 2);
        QTest::keyClick(edit, Qt::Key_Return);
        QTest::keyClick(edit, Qt::Key_Return);
        QCOMPARE(files->currentIndex(), 1);
        QCOMPARE(pane->matchCount(), 1);
        QTest::keyClick(edit, Qt::Key_Return, Qt::ShiftModifier); // back to a.txt's last match
        QCOMPARE(files->currentIndex(), 0);
        QCOMPARE(pane->currentMatch(), 1);
    }

    void editAndSaveWorkingTreeFile()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        write(d + QStringLiteral("/a.txt"), "one\ntwo\nthree\n");
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("init")});
        write(d + QStringLiteral("/a.txt"), "one\nTWO\nthree\n");
        QString error;
        auto repo = GitRepo::open(d, &error);
        Comparison cmp;
        cmp.allChanges = true;
        MainWindow w(std::move(*repo), cmp);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.activateWindow();
        auto *pane = w.findChild<DiffPane *>();
        QVERIFY(pane->canEdit());
        pane->setEditing(true);
        QVERIFY(pane->isEditing());
        CodeView *right = nullptr;
        for (CodeView *v : pane->findChildren<CodeView *>())
            if (v->isEditing())
                right = v;
        QVERIFY(right);
        QTextCursor c(right->document());
        c.movePosition(QTextCursor::End);
        c.insertText(QStringLiteral("\nfour"));
        QVERIFY(pane->isModified());
        QTest::keyClick(&w, Qt::Key_S, Qt::ControlModifier);
        QVERIFY(!pane->isModified());
        QVERIFY(pane->isEditing()); // stays in edit mode after saving
        QFile f(d + QStringLiteral("/a.txt"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("one\nTWO\nthree\nfour\n"));
        QCOMPARE(pane->changeCount(), 2); // TWO, and the new line
    }

    void applyChangeToWorkingTree()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        write(d + QStringLiteral("/a.txt"), "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\n");
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("init")});
        write(d + QStringLiteral("/a.txt"), "one\nTWO\nthree\nfour\nfive\nsix\nSEVEN\neight\n");
        QString error;
        auto repo = GitRepo::open(d, &error);
        Comparison cmp;
        cmp.allChanges = true;
        MainWindow w(std::move(*repo), cmp);
        auto *pane = w.findChild<DiffPane *>();
        QCOMPARE(pane->changeCount(), 2);
        QVERIFY(pane->canCopyTo(true));
        QVERIFY(!pane->canCopyTo(false)); // HEAD can't be written
        emit pane->copyChangeRequested(0, true); // put "two" back
        QFile f(d + QStringLiteral("/a.txt"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("one\ntwo\nthree\nfour\nfive\nsix\nSEVEN\neight\n"));
        QCOMPARE(pane->changeCount(), 1);
    }

    void wordWrapKeepsSidesAligned()
    {
        const QString longLine = QString(400, u'x');
        const FileDiff diff = DiffModel::parseUnified(
            QStringLiteral("@@ -1,3 +1,3 @@\n a\n-short\n+%1\n b\n").arg(longLine));
        DiffPane pane;
        pane.resize(900, 400);
        pane.setMode(DiffPane::Mode::SideBySide);
        pane.show();
        QVERIFY(QTest::qWaitForWindowExposed(&pane));
        pane.setWordWrap(true);
        showDiff(pane, diff, QStringLiteral("f.txt"), QStringLiteral("a"), QStringLiteral("b"));
        QList<CodeView *> views;
        for (CodeView *v : pane.findChildren<CodeView *>())
            if (v->isVisible())
                views << v;
        QCOMPARE(views.size(), 2);
        // After padding, every row occupies the same number of visual lines on both sides.
        // The short left line gets padded to the wrapped height of the long right one.
        auto padded = [&] {
            for (CodeView *v : views)
                if (v->document()->findBlockByNumber(1).text().endsWith(QChar(0x2028)))
                    return true;
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(padded(), 2000);
        for (int row = 0; row < views[0]->rows().size(); ++row) {
            auto lines = [&](CodeView *v) {
                const QTextBlock b = v->document()->findBlockByNumber(row);
                v->document()->documentLayout()->blockBoundingRect(b); // force layout
                return b.layout()->lineCount();
            };
            QCOMPARE(lines(views[0]), lines(views[1]));
        }
    }

    void syntaxHighlightingIsApplied()
    {
        const FileDiff diff = DiffModel::parseUnified(QStringLiteral("@@ -1 +1 @@\n-int a = 1;\n+int a = 2;\n"));
        DiffPane pane;
        pane.setMode(DiffPane::Mode::Inline);
        showDiff(pane, diff, QStringLiteral("main.cpp"), QStringLiteral("a"), QStringLiteral("b"));
        QCOMPARE(pane.detectedLanguage(), QStringLiteral("C++"));
        CodeView *view = nullptr;
        for (CodeView *v : pane.findChildren<CodeView *>())
            if (!v->rows().isEmpty())
                view = v;
        QApplication::processEvents();
        const QTextBlock b = view->document()->firstBlock();
        bool coloured = false;
        for (const QTextLayout::FormatRange &r : b.layout()->formats())
            coloured |= r.format.hasProperty(QTextFormat::ForegroundBrush);
        QVERIFY(coloured); // "int" gets a keyword/type colour
        pane.setLanguageOverride(QStringLiteral("-")); // None
        QVERIFY(pane.effectiveLanguage().isEmpty());
    }

    void autoReloadPicksUpEdits()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        write(d + QStringLiteral("/a.txt"), "one\n");
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("init")});
        write(d + QStringLiteral("/a.txt"), "one\ntwo\n");
        Settings::setAutoReload(true);
        QString error;
        auto repo = GitRepo::open(d, &error);
        Comparison cmp;
        cmp.allChanges = true;
        MainWindow w(std::move(*repo), cmp);
        w.show();
        auto *pane = w.findChild<DiffPane *>();
        QCOMPARE(pane->changeCount(), 1);
        write(d + QStringLiteral("/a.txt"), "zero\none\ntwo\n");
        QTRY_COMPARE_WITH_TIMEOUT(pane->changeCount(), 2, 5000);
        Settings::setAutoReload(false);
    }

    void savingHighlightedFileSkipsFillerRows()
    {
        // Regression: the syntax highlighter owns QTextBlock::userState(), which yadiff
        // once used to mark filler rows, so saving a highlighted file wrote them out.
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        write(d + QStringLiteral("/main.cpp"), "int a;\nint b;\nint c;\n");
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("init")});
        write(d + QStringLiteral("/main.cpp"), "int a;\nint c;\n"); // a removed line -> filler on the right
        QString error;
        auto repo = GitRepo::open(d, &error);
        Comparison cmp;
        cmp.allChanges = true;
        MainWindow w(std::move(*repo), cmp);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.activateWindow();
        auto *pane = w.findChild<DiffPane *>();
        QCOMPARE(pane->effectiveLanguage(), QStringLiteral("C++"));
        pane->setEditing(true);
        CodeView *right = nullptr;
        for (CodeView *v : pane->findChildren<CodeView *>())
            if (v->isEditing())
                right = v;
        QVERIFY(right);
        QApplication::processEvents(); // let the highlighter run
        QTextCursor c(right->document());
        c.movePosition(QTextCursor::End);
        c.insertText(QStringLiteral("\nint d;"));
        QApplication::processEvents();
        QTest::keyClick(&w, Qt::Key_S, Qt::ControlModifier);
        QFile f(d + QStringLiteral("/main.cpp"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("int a;\nint c;\nint d;\n"));
    }

    void historyDialogPicksCommits()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        for (int i = 1; i <= 3; ++i) {
            write(d + QStringLiteral("/a.txt"), lines(i));
            git(d, {QStringLiteral("add"), QStringLiteral("-A")});
            git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("commit %1").arg(i)});
        }
        QString error;
        auto repo = GitRepo::open(d, &error);
        HistoryDialog dlg(*repo);
        dlg.show();
        auto *list = dlg.findChild<QTreeWidget *>();
        QCOMPARE(list->topLevelItemCount(), 3);
        if (const QString out = qEnvironmentVariable("YADIFF_TEST_GRAB"); !out.isEmpty())
            dlg.grab().save(out + QStringLiteral("/history.png"));
        QPushButton *show = nullptr, *compare = nullptr;
        for (QPushButton *b : dlg.findChildren<QPushButton *>()) {
            if (b->text() == QStringLiteral("Show commit's changes"))
                show = b;
            if (b->text() == QStringLiteral("Compare the two"))
                compare = b;
        }
        QVERIFY(show && compare);
        list->topLevelItem(0)->setSelected(true);
        QVERIFY(show->isEnabled());
        QVERIFY(!compare->isEnabled());
        list->topLevelItem(2)->setSelected(true);
        QVERIFY(compare->isEnabled());
        compare->click();
        QCOMPARE(dlg.choice(), HistoryDialog::Choice::CompareTwo);
        QCOMPARE(dlg.first().subject, QStringLiteral("commit 1"));  // older
        QCOMPARE(dlg.second().subject, QStringLiteral("commit 3")); // newer

        // And the window switches to that comparison.
        Comparison c;
        c.mode = Comparison::Mode::Revisions;
        c.rev1 = dlg.first().oid;
        c.rev2 = dlg.second().oid;
        MainWindow w(std::move(*repo), Comparison{});
        w.setComparison(c);
        auto *pane = w.findChild<DiffPane *>();
        QCOMPARE(pane->changeCount(), 1);
    }

    void loadsInTheBackground()
    {
        // Everything else runs synchronously (YADIFF_SYNCHRONOUS); this checks the real path.
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        for (int i = 0; i < 6; ++i)
            write(d + QStringLiteral("/f%1.txt").arg(i), lines(50));
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("init")});
        for (int i = 0; i < 6; ++i)
            write(d + QStringLiteral("/f%1.txt").arg(i), lines(50, 10 + i));
        QString error;
        auto repo = GitRepo::open(d, &error);
        Comparison cmp;
        cmp.allChanges = true;
        qunsetenv("YADIFF_SYNCHRONOUS");
        MainWindow w(std::move(*repo), cmp);
        qputenv("YADIFF_SYNCHRONOUS", "1");
        w.show();
        auto *files = w.findChild<FileList *>();
        auto *pane = w.findChild<DiffPane *>();
        QTRY_VERIFY_WITH_TIMEOUT(!w.isBusy() && pane->changeCount() > 0, 10000);
        // Switch files quickly: only the last one's diff may land in the pane.
        for (int i = 1; i < 6; ++i)
            files->selectIndex(i);
        QTRY_VERIFY_WITH_TIMEOUT(!w.isBusy(), 10000);
        QCOMPARE(files->currentIndex(), 5);
        QCOMPARE(pane->changeCount(), 50 / 15); // f5 changed every 15th line
        // Shift+F3 into the previous file lands on its last change after it loads.
        w.activateWindow();
        pane->goToFirstChange();
        QTest::keyClick(&w, Qt::Key_F3, Qt::ShiftModifier);
        QTRY_VERIFY_WITH_TIMEOUT(!w.isBusy() && files->currentIndex() == 4, 10000);
        QCOMPARE(pane->currentChange(), pane->changeCount() - 1);
    }

    void filterHidesFilesAndNavigationSkipsThem()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        for (const char *n : {"a.cpp", "b.lock", "c.cpp"})
            write(d + u'/' + QLatin1String(n), lines(3));
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("init")});
        for (const char *n : {"a.cpp", "b.lock", "c.cpp"})
            write(d + u'/' + QLatin1String(n), lines(3, 2));
        QString error;
        auto repo = GitRepo::open(d, &error);
        Comparison cmp;
        cmp.allChanges = true;
        MainWindow w(std::move(*repo), cmp);
        auto *files = w.findChild<FileList *>();
        QCOMPARE(files->visibleCount(), 3);
        QLineEdit *filter = nullptr;
        for (QLineEdit *e : w.findChildren<QLineEdit *>())
            if (e->placeholderText().startsWith(QStringLiteral("Filter files")))
                filter = e;
        QVERIFY(filter);
        filter->setText(QStringLiteral("!*.lock"));
        QCOMPARE(files->visibleCount(), 2);
        files->selectIndex(0); // a.cpp
        QVERIFY(files->selectNext());
        QCOMPARE(files->currentIndex(), 2); // c.cpp: b.lock is skipped
        filter->setText(QStringLiteral("*.lock"));
        QCOMPARE(files->visibleCount(), 1);
        filter->clear();
        QCOMPARE(files->visibleCount(), 3);
        // Hide reviewed files.
        files->setReviewed(0, true);
        FileFilter hide;
        hide.hideReviewed = true;
        files->setFilter(hide);
        QCOMPARE(files->visibleCount(), 2);
    }

    void imagesOpenInTheImageView()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        QImage img(20, 10, QImage::Format_ARGB32);
        img.fill(Qt::white);
        QVERIFY(img.save(d + QStringLiteral("/pic.png")));
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("init")});
        for (int x = 0; x < 5; ++x)
            img.setPixelColor(x, 0, Qt::red);
        QVERIFY(img.save(d + QStringLiteral("/pic.png")));
        QString error;
        auto repo = GitRepo::open(d, &error);
        Comparison cmp;
        cmp.allChanges = true;
        MainWindow w(std::move(*repo), cmp);
        auto *view = w.findChild<ImageDiffView *>();
        QVERIFY(view);
        QVERIFY(view->isVisibleTo(&w));
        QCOMPARE(view->differingPixels(), 5);
        view->setMode(ImageDiffView::Mode::Swipe);
        view->setMode(ImageDiffView::Mode::Difference);
        if (const QString out = qEnvironmentVariable("YADIFF_TEST_GRAB"); !out.isEmpty()) {
            w.resize(1000, 500);
            w.show();
            QApplication::processEvents();
            w.grab().save(out + QStringLiteral("/image.png"));
        }
    }

    void reviewThreadsSitUnderTheirLine()
    {
        const FileDiff diff = DiffModel::parseUnified(QStringLiteral("@@ -1,3 +1,3 @@\n a\n-b\n+B\n c\n"));
        GitHubReview::Comment root;
        root.id = 1;
        root.path = QStringLiteral("f.txt");
        root.side = QStringLiteral("RIGHT");
        root.line = 2;
        root.author = QStringLiteral("alice");
        root.body = QStringLiteral("Why uppercase?\nSecond line");
        root.createdAt = QStringLiteral("2026-01-01");
        GitHubReview::Comment reply = root;
        reply.id = 2;
        reply.inReplyTo = 1;
        reply.author = QStringLiteral("bob");
        reply.body = QStringLiteral("Style guide");
        FileView v;
        v.diff = diff;
        v.title = v.fileName = QStringLiteral("f.txt");
        v.paths = {QStringLiteral("f.txt"), QStringLiteral("f.txt")};
        v.threads = GitHubReview::threads({root, reply});
        v.reviewEnabled = true;
        DiffPane pane;
        pane.setMode(DiffPane::Mode::SideBySide);
        pane.setFile(v);
        QList<CodeView *> views;
        for (CodeView *cv : pane.findChildren<CodeView *>())
            if (!cv->rows().isEmpty()) // the inline view stays empty in side-by-side mode
                views << cv;
        QCOMPARE(views.size(), 2);
        CodeView *left = views[0]->rows()[1].kind == LineKind::Removed ? views[0] : views[1];
        CodeView *right = left == views[0] ? views[1] : views[0];
        QCOMPARE(right->rows().size(), left->rows().size());
        // Row 1 is the b/B pair; row 2 is the thread on the right and a spacer on the left.
        QVERIFY(right->rows()[2].comment);
        QVERIFY(right->rows()[2].text.contains(QStringLiteral("alice")));
        QVERIFY(right->rows()[2].text.contains(QStringLiteral("bob")));
        QVERIFY(left->rows()[2].filler);
        QCOMPARE(left->document()->findBlockByNumber(2).text().size(),
                 right->rows()[2].text.count(QChar(0x2028))); // same number of visual lines
        if (const QString out = qEnvironmentVariable("YADIFF_TEST_GRAB"); !out.isEmpty()) {
            pane.resize(1000, 300);
            pane.show();
            QApplication::processEvents();
            pane.grab().save(out + QStringLiteral("/comments.png"));
        }
    }

    void largeDiffsAskFirst()
    {
        DiffPane pane;
        bool asked = false;
        connect(&pane, &DiffPane::showLargeRequested, [&] { asked = true; });
        pane.showLargeNotice(QStringLiteral("big.txt"), 250000);
        QPushButton *show = nullptr;
        for (QPushButton *b : pane.findChildren<QPushButton *>())
            if (b->text() == QStringLiteral("Show anyway"))
                show = b;
        QVERIFY(show && show->isVisibleTo(&pane));
        show->click();
        QVERIFY(asked);
    }
};

QTEST_MAIN(TestNavigation)
#include "test_navigation.moc"
