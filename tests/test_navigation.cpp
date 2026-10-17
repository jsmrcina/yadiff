#include "AdoLoginDialog.h"
#include "CodeView.h"
#include "DiffPane.h"
#include "FileList.h"
#include "HistoryDialog.h"
#include "ImageDiffView.h"
#include "MainWindow.h"
#include "NewComparisonDialog.h"
#include "SearchBar.h"
#include "Settings.h"
#include "Theme.h"

#include <QApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFile>
#include <QMenu>
#include <QMimeData>
#include <QTimer>
#include <QRadioButton>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QAbstractButton>
#include <QMessageBox>
#include <QTextBrowser>
#include <QLabel>
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


// A window action by its menu text (without the & accelerator).
QAction *action(QWidget &w, const QString &text)
{
    for (QAction *a : w.findChildren<QAction *>())
        if (a->text().remove(u'&') == text)
            return a;
    return nullptr;
}

} // namespace

class TestNavigation : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        // Without these, QSettings can't read or write at all on Windows (Linux falls back).
        QCoreApplication::setOrganizationName(QStringLiteral("yadiff-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_navigation"));
    }

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

        QTest::keyClick(&w, Qt::Key_Down, Qt::AltModifier);
        QCOMPARE(pane->currentChange(), 1);
        QTest::keyClick(&w, Qt::Key_Down, Qt::AltModifier);
        QCOMPARE(pane->currentChange(), 2);

        // No more changes in a.txt: F3 moves to b.txt's first change.
        QTest::keyClick(&w, Qt::Key_Down, Qt::AltModifier);
        QCOMPARE(files->currentIndex(), 1);
        QCOMPARE(pane->changeCount(), 2);
        QCOMPARE(pane->currentChange(), 0);

        // Shift+F3 at the first change goes back to a.txt's last change.
        QTest::keyClick(&w, Qt::Key_Up, Qt::AltModifier);
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
        QCOMPARE(files->markedCount(), 0);

        files->setMarked(0, true);
        QVERIFY(files->isMarked(0));
        w.reload();
        QVERIFY(files->isMarked(0));   // survives a reload
        QVERIFY(!files->isMarked(1));

        write(d + QStringLiteral("/a.txt"), lines(5, 4)); // a.txt changes again
        w.reload();
        QVERIFY(!files->isMarked(0));  // so its mark is dropped
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
        QVERIFY(seen.value(QStringLiteral("F3")).contains(QStringLiteral("Find")));
        QVERIFY(seen.contains(QStringLiteral("Alt+Down")));
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
        QTest::keyClick(&w, Qt::Key_Up, Qt::AltModifier);
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
        files->setMarked(0, true);
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
        root.body = QStringLiteral("<span style=\"font-weight:600\">PR Assistant</span>\n\nWhy **uppercase**?\n\n"
                                   "Second paragraph with `code`.\n<!-- hidden-agent-marker -->");
        root.createdAt = QStringLiteral("2026-10-12T14:03:00Z");
        root.url = QStringLiteral("https://dev.azure.com/o/p/_git/r/pullrequest/1?discussionId=1");
        GitHubReview::Comment reply = root;
        reply.id = 2;
        reply.inReplyTo = 1;
        reply.author = QStringLiteral("bob");
        reply.body = QStringLiteral("Style guide");
        reply.createdAt = QStringLiteral("2026-10-12T15:30:00Z");
        FileView v;
        v.diff = diff;
        v.title = v.fileName = QStringLiteral("f.txt");
        v.paths = {QStringLiteral("f.txt"), QStringLiteral("f.txt")};
        v.threads = GitHubReview::threads({root, reply});
        v.reviewEnabled = true;
        DiffPane pane;
        pane.resize(1000, 400);
        pane.setMode(DiffPane::Mode::SideBySide);
        pane.show();
        QVERIFY(QTest::qWaitForWindowExposed(&pane));
        pane.setFile(v);
        QList<CodeView *> views;
        for (CodeView *cv : pane.findChildren<CodeView *>())
            if (!cv->rows().isEmpty()) // the inline view stays empty in side-by-side mode
                views << cv;
        QCOMPARE(views.size(), 2);
        CodeView *left = views[0]->rows()[1].kind == LineKind::Removed ? views[0] : views[1];
        CodeView *right = left == views[0] ? views[1] : views[0];
        QCOMPARE(right->rows().size(), left->rows().size());

        // Collapsed by default: one summary line under the B row, with the time and the gist.
        const Row &summary = right->rows()[2];
        QVERIFY(summary.comment);
        QVERIFY(!summary.commentExpanded);
        QVERIFY(summary.text.contains(QStringLiteral("alice")));
        QVERIFY(summary.text.contains(GitHubReview::displayTime(root.createdAt)));
        QVERIFY(summary.text.contains(QStringLiteral(":"))); // a time, not just a date
        QVERIFY(summary.text.contains(QStringLiteral("1 reply")));
        QVERIFY(summary.text.contains(QStringLiteral("PR Assistant"))); // markup stripped
        QVERIFY(!summary.text.contains(QStringLiteral("<span")));
        QVERIFY(!summary.text.contains(QChar(0x2028)));
        QVERIFY(left->rows()[2].filler);
        QVERIFY(!right->findChild<QTextBrowser *>());

        // Expanding renders the thread over the row, and both sides reserve its height.
        emit right->commentToggled(summary.threadId);
        QVERIFY(pane.isThreadExpanded(1));
        auto *browser = right->findChild<QTextBrowser *>();
        QVERIFY(browser);
        const QString html = browser->toHtml();
        QVERIFY(html.contains(root.url.toHtmlEscaped())); // author links to the comment
        QVERIFY(html.contains(QStringLiteral("bob")));
        QVERIFY(html.contains(GitHubReview::displayTime(reply.createdAt)));
        QVERIFY(!html.contains(QStringLiteral("hidden-agent-marker")));
        QVERIFY(!browser->toPlainText().contains(QStringLiteral("**")));
        bool bold = false;
        for (QTextBlock b = browser->document()->begin(); b.isValid(); b = b.next())
            for (auto it = b.begin(); !it.atEnd(); ++it)
                bold |= it.fragment().text().contains(QStringLiteral("uppercase")) && it.fragment().charFormat().fontWeight() >= QFont::Bold;
        QVERIFY(bold); // Markdown rendered, not shown raw
        const int lines = right->document()->findBlockByNumber(2).text().size() + 1;
        QVERIFY(lines > 3);
        QCOMPARE(left->document()->findBlockByNumber(2).text().size(), lines - 1); // spacer matches
        QVERIFY(browser->height() >= lines * right->fontMetrics().lineSpacing() - 2);
        if (const QString out = qEnvironmentVariable("YADIFF_TEST_GRAB"); !out.isEmpty()) {
            QApplication::processEvents();
            pane.grab().save(out + QStringLiteral("/comments.png"));
        }

        // The ▾ link in the thread collapses it again.
        emit browser->anchorClicked(QUrl(QStringLiteral("yadiff:collapse")));
        QVERIFY(!pane.isThreadExpanded(1));
        QVERIFY(!right->findChild<QTextBrowser *>());
        pane.setCommentsExpanded(true);
        QVERIFY(right->findChild<QTextBrowser *>());
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

    void adoLoginDialogLooksRight()
    {
        AdoLoginDialog dlg(QUrl(QStringLiteral("https://contoso.visualstudio.com/DefaultCollection")),
                           QStringLiteral("No Azure DevOps credential found."));
        QLineEdit *org = nullptr;
        for (QLineEdit *e : dlg.findChildren<QLineEdit *>())
            if (e->echoMode() == QLineEdit::Normal)
                org = e;
        QVERIFY(org);
        QCOMPARE(org->text(), QStringLiteral("https://dev.azure.com/contoso")); // normalised, read-only
        QVERIFY(org->isReadOnly());
        bool linksToTokens = false;
        for (QLabel *l : dlg.findChildren<QLabel *>())
            linksToTokens |= l->text().contains(QStringLiteral("https://dev.azure.com/contoso/_usersSettings/tokens"));
        QVERIFY(linksToTokens);
        // Signing in with an empty token asks for one instead of contacting the server.
        for (QPushButton *b : dlg.findChildren<QPushButton *>())
            if (b->text() == QStringLiteral("Sign in"))
                b->click();
        QVERIFY(!dlg.credential());
        if (const QString out = qEnvironmentVariable("YADIFF_TEST_GRAB"); !out.isEmpty()) {
            dlg.show();
            QApplication::processEvents();
            dlg.grab().save(out + QStringLiteral("/ado-login.png"));
        }
    }

    void reviewCommentsDontBreakHighlighting()
    {
        // Regression: a comment containing "/*" opened a C++ block comment that ran to
        // the end of the file, so everything after it was coloured as a comment.
        const FileDiff diff = DiffModel::parseUnified(
            QStringLiteral("@@ -1,3 +1,3 @@\n-int a = 1;\n+int a = 2;\n int b = 3;\n int c = 4;\n"));
        GitHubReview::Comment c;
        c.id = 1;
        c.path = QStringLiteral("main.cpp");
        c.side = QStringLiteral("RIGHT");
        c.line = 1;
        c.author = QStringLiteral("bot");
        c.body = QStringLiteral("Applies to **/*.{cpp,h} and \"unterminated 'quotes\n```\nint x;");
        c.createdAt = QStringLiteral("2026-10-12T14:03:00Z");
        auto formatsOf = [&](bool withComment, bool expand) {
            FileView v;
            v.diff = diff;
            v.title = v.fileName = QStringLiteral("main.cpp");
            v.paths = {QStringLiteral("main.cpp"), QStringLiteral("main.cpp")};
            if (withComment)
                v.threads = GitHubReview::threads({c});
            v.reviewEnabled = true;
            auto *pane = new DiffPane;
            pane->setMode(DiffPane::Mode::Inline);
            pane->resize(900, 400);
            pane->show();
            pane->setFile(v);
            if (expand)
                pane->setCommentsExpanded(true);
            QApplication::processEvents();
            CodeView *view = nullptr;
            for (CodeView *cv : pane->findChildren<CodeView *>())
                if (!cv->rows().isEmpty())
                    view = cv;
            // Formats on the last code line ("int c = 4;").
            QTextBlock last = view->document()->lastBlock();
            QStringList out;
            for (const QTextLayout::FormatRange &r : last.layout()->formats())
                out << QStringLiteral("%1+%2:%3").arg(r.start).arg(r.length).arg(r.format.foreground().color().name());
            delete pane;
            return out;
        };
        const QStringList plain = formatsOf(false, false);
        QVERIFY(!plain.isEmpty()); // "int" is highlighted
        QCOMPARE(formatsOf(true, false), plain);
        QCOMPARE(formatsOf(true, true), plain);
    }

    void botCommentsRenderReadably()
    {
        // Shaped like the review bots on Azure DevOps: Markdown mixed with styled HTML spans,
        // tables of links, <details> and hidden HTML comments.
        const QString body = QStringLiteral(
            "<span style=\"font-family:'Segoe UI'; font-size:14px; font-weight:600;\">Review Bot</span>\n\n"
            "<small class=\"flex-row\"><span style=\"border: 1px solid rgba(var(--palette-tint), 1);\">Severity</span>"
            "<span>High</span></small>\n\n"
            "The buffer length is not written back to `BlockInfo.dwCurrentSize`. Applies to `**/*.{cpp,h}`.\n"
            "Here is the suggested code:\n```\nif (!buffer)\n{\n    return ERROR_INVALID_DATA;\n}\n```\n"
            "<table><tr><td><p>Rate this:</p></td><td><a href=\"https://example.com/useful\">Useful</a></td></tr></table>\n\n"
            "<details><summary>About the bot</summary>\n\n#### About\nIt reviews code.\n\n</details>\n"
            "<!-- BotUserAgent=example -->\n");
        GitHubReview::Comment c;
        c.id = 9;
        c.path = QStringLiteral("main.cpp");
        c.side = QStringLiteral("RIGHT");
        c.line = 1;
        c.author = QStringLiteral("Review Bot");
        c.body = body;
        c.createdAt = QStringLiteral("2026-10-12T14:03:00Z");
        c.url = QStringLiteral("https://dev.azure.com/o/p/_git/r/pullrequest/1?discussionId=9");
        FileView v;
        v.diff = DiffModel::parseUnified(QStringLiteral("@@ -1,2 +1,2 @@\n-int a;\n+int b;\n int c;\n"));
        v.title = v.fileName = QStringLiteral("main.cpp");
        v.paths = {QStringLiteral("main.cpp"), QStringLiteral("main.cpp")};
        v.threads = GitHubReview::threads({c});
        v.reviewEnabled = true;
        DiffPane pane;
        pane.resize(1000, 700);
        pane.setMode(DiffPane::Mode::Inline);
        pane.show();
        QVERIFY(QTest::qWaitForWindowExposed(&pane));
        pane.setFile(v);
        CodeView *view = nullptr;
        for (CodeView *cv : pane.findChildren<CodeView *>())
            if (!cv->rows().isEmpty())
                view = cv;
        const Row &summary = view->rows()[2];
        QVERIFY(summary.comment);
        QVERIFY(summary.text.startsWith(QStringLiteral("\U0001F4AC Review Bot")));
        QVERIFY(summary.text.contains(QStringLiteral("Review Bot · 2026-10-12")));
        QVERIFY(!summary.text.contains(u'<'));
        QVERIFY(summary.text.contains(QStringLiteral("The buffer length is not written back"))); // gist skips short title and badge lines
        pane.setCommentsExpanded(true);
        auto *browser = view->findChild<QTextBrowser *>();
        QVERIFY(browser);
        const QString text = browser->toPlainText();
        QVERIFY(!text.contains(QStringLiteral("<span")));
        QVERIFY(!text.contains(QStringLiteral("BotUserAgent")));
        QVERIFY(!text.contains(QStringLiteral("```")));
        QVERIFY(text.contains(QStringLiteral("return ERROR_INVALID_DATA;")));
        QVERIFY(text.contains(QStringLiteral("Useful")));
        QVERIFY(text.contains(QStringLiteral("Severity High"))); // badges stay apart
        QVERIFY(text.contains(QStringLiteral("\u25B8 About the bot"))); // <details> folded to its summary
        QVERIFY(!text.contains(QStringLiteral("It reviews code")));
        // The fenced code is monospace on a shaded band.
        bool monoCode = false;
        for (QTextBlock b = browser->document()->begin(); b.isValid(); b = b.next())
            if (b.text().contains(QStringLiteral("ERROR_INVALID_DATA")))
                monoCode = b.begin().fragment().charFormat().fontFamilies().toStringList().contains(Settings::font().family());
        QVERIFY(monoCode);
        if (const QString out = qEnvironmentVariable("YADIFF_TEST_GRAB"); !out.isEmpty()) {
            QApplication::processEvents();
            pane.grab().save(out + QStringLiteral("/bot-comment.png"));
        }
    }

    void markingOptionsAndFileOperations()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        QDir(d).mkpath(QStringLiteral("sub"));
        write(d + QStringLiteral("/sub/a.cpp"), "int a;\nint b;\nint c;\nint d;\nint e;\nint f;\n");
        write(d + QStringLiteral("/blank.txt"), "x\ny\n");
        write(d + QStringLiteral("/gone.txt"), "g\n");
        write(d + QStringLiteral("/same.txt"), "s\n");
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("init")});
        write(d + QStringLiteral("/sub/a.cpp"), "int a;\nint B;\nint c;\nint d;\nint E;\nint f;\n");
        write(d + QStringLiteral("/blank.txt"), "x\n\n\ny\n");
        QFile::remove(d + QStringLiteral("/gone.txt"));
        write(d + QStringLiteral("/new.txt"), "n\n");
        QString error;
        auto repo = GitRepo::open(d, &error);
        Comparison cmp;
        cmp.allChanges = true;
        MainWindow w(std::move(*repo), cmp);
        w.resize(1200, 700);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.activateWindow();
        auto *files = w.findChild<FileList *>();
        auto *pane = w.findChild<DiffPane *>();
        QCOMPARE(files->visibleCount(), 4); // a.cpp, blank.txt, gone.txt, new.txt
        auto indexOf = [&](const QString &name) {
            for (int i : files->displayOrder(true))
                if (files->topLevelItemCount() && files->findItems(name, Qt::MatchExactly | Qt::MatchRecursive).value(0)
                    && files->findItems(name, Qt::MatchExactly | Qt::MatchRecursive).first()->data(0, Qt::UserRole + 1).toInt() == i)
                    return i;
            return -1;
        };

        // Fully qualified folder names, or trimmed to what they don't share.
        auto folderText = [&] {
            QStringList out;
            for (int i = 0; i < files->topLevelItemCount(); ++i)
                out << files->topLevelItem(i)->text(0);
            return out;
        };
        const QString root = QFileInfo(d).canonicalFilePath();
        QVERIFY2(folderText().contains(root + QStringLiteral("/sub")), qPrintable(folderText().join(u',')));
        action(w, QStringLiteral("Trim common prefix from long folder names"))->setChecked(true);
        QVERIFY(folderText().contains(QStringLiteral("sub")));
        action(w, QStringLiteral("Trim common prefix from long folder names"))->setChecked(false);

        // Marking: a highlight, not a checkbox.
        const int a = indexOf(QStringLiteral("a.cpp"));
        QVERIFY(a >= 0);
        files->selectIndex(a);
        action(w, QStringLiteral("Mark / unmark selected files"))->trigger();
        QVERIFY(files->isMarked(a));
        QTreeWidgetItem *item = files->findItems(QStringLiteral("a.cpp"), Qt::MatchExactly | Qt::MatchRecursive).first();
        QCOMPARE(item->background(0).color(), Settings::markColor());
        QVERIFY(!(item->flags() & Qt::ItemIsUserCheckable));
        action(w, QStringLiteral("Hide marked files"))->setChecked(true);
        QCOMPARE(files->visibleCount(), 3);
        action(w, QStringLiteral("Hide marked files"))->setChecked(false);
        action(w, QStringLiteral("Unmark all files"))->trigger();
        QCOMPARE(files->markedCount(), 0);

        // Options > Show ... files.
        action(w, QStringLiteral("Show left-only files"))->setChecked(false);
        QCOMPARE(files->visibleCount(), 3); // gone.txt hidden
        action(w, QStringLiteral("Show blanks-only files"))->setChecked(false);
        QCOMPARE(files->visibleCount(), 2); // blank.txt hidden too
        action(w, QStringLiteral("Show no files"))->trigger();
        QCOMPARE(files->visibleCount(), 0);
        action(w, QStringLiteral("Show all files"))->trigger(); // includes identical: same.txt listed
        QCOMPARE(files->visibleCount(), 5);
        action(w, QStringLiteral("Mark identical files"))->trigger();
        QCOMPARE(files->markedCount(), 1);
        QVERIFY(files->isMarked(indexOf(QStringLiteral("same.txt"))));
        action(w, QStringLiteral("Select marked files"))->trigger();
        QCOMPARE(files->selectedIndices(), QVector<int>{indexOf(QStringLiteral("same.txt"))});
        action(w, QStringLiteral("Show identical files"))->setChecked(false);
        QCOMPARE(files->visibleCount(), 4);

        // Context menu: Copy and Copy commit hash.
        files->selectIndex(indexOf(QStringLiteral("a.cpp")));
        auto runMenu = [&](const QString &actionText) {
            bool found = false;
            QTimer::singleShot(0, [&] {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (!menu)
                    return;
                for (QAction *act : menu->actions())
                    if (act->text().remove(u'&') == actionText && act->isEnabled()) {
                        act->trigger();
                        found = true;
                    }
                menu->close();
            });
            emit files->contextMenuRequested(files->mapToGlobal(QPoint(10, 10)));
            return found;
        };
        QVERIFY(runMenu(QStringLiteral("Copy")));
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("sub/a.cpp"));
        QVERIFY(runMenu(QStringLiteral("Copy commit hash")));
        QByteArray head;
        {
            QProcess p;
            p.setWorkingDirectory(d);
            p.start(QStringLiteral("git"), {QStringLiteral("rev-parse"), QStringLiteral("HEAD")});
            p.waitForFinished();
            head = p.readAllStandardOutput().trimmed();
        }
        QCOMPARE(QApplication::clipboard()->text().toLatin1(), head);

        // Edit > Copy as Markdown / for code review, on a selection in the diff.
        files->selectIndex(indexOf(QStringLiteral("a.cpp")));
        QVERIFY(pane->goToLine(2, true));
        QVERIFY(action(w, QStringLiteral("Copy as Markdown"))->isEnabled());
        action(w, QStringLiteral("Copy as Markdown"))->trigger();
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("```cpp\nint B;\n```\n"));
        action(w, QStringLiteral("Copy for code review"))->trigger();
        QVERIFY(QApplication::clipboard()->text().startsWith(QStringLiteral("`sub/a.cpp:2`\n```cpp\nint B;")));

        // View options.
        CodeView *right = nullptr, *left = nullptr;
        for (CodeView *v : pane->findChildren<CodeView *>())
            if (v->isVisible())
                (v->rows().value(1).kind == LineKind::Removed ? left : right) = v;
        QVERIFY(left && right);
        action(w, QStringLiteral("Show right only"))->trigger();
        QVERIFY(!left->isVisible() && right->isVisible());
        action(w, QStringLiteral("Show both"))->trigger();
        QVERIFY(left->isVisible());
        const int gutter = right->gutterWidth();
        action(w, QStringLiteral("Show line numbers"))->setChecked(false);
        QVERIFY(right->gutterWidth() < gutter);
        action(w, QStringLiteral("Show line numbers"))->setChecked(true);
        const qreal size = right->font().pointSizeF();
        action(w, QStringLiteral("Zoom in"))->trigger();
        QVERIFY(right->font().pointSizeF() > size);
        action(w, QStringLiteral("100%"))->trigger();
        QCOMPARE(right->font().pointSizeF(), size);
        emit right->wordClicked(QStringLiteral("int"));
        QCOMPARE(right->extraSelections().size(), 6); // every "int" on the right
        emit right->wordClicked(QString());
        QVERIFY(right->extraSelections().isEmpty());

        // Remembered scroll positions: back on a.cpp at its second change.
        action(w, QStringLiteral("Remember scroll positions for files"))->setChecked(true);
        pane->goToChange(1);
        files->selectIndex(indexOf(QStringLiteral("blank.txt")));
        files->selectIndex(indexOf(QStringLiteral("sub/a.cpp")) >= 0 ? indexOf(QStringLiteral("sub/a.cpp")) : indexOf(QStringLiteral("a.cpp")));
        QCOMPARE(pane->currentChange(), 1);
        action(w, QStringLiteral("Remember scroll positions for files"))->setChecked(false);

        // Stage the selected file (File > Stage files): it moves to the index.
        files->selectIndex(indexOf(QStringLiteral("new.txt")));
        action(w, QStringLiteral("Stage files"))->trigger();
        {
            QProcess p;
            p.setWorkingDirectory(d);
            p.start(QStringLiteral("git"), {QStringLiteral("diff"), QStringLiteral("--cached"), QStringLiteral("--name-only")});
            p.waitForFinished();
            QCOMPARE(p.readAllStandardOutput().trimmed(), QByteArray("new.txt"));
        }

        // Revert (confirmed in the dialog): a.cpp is back to HEAD.
        files->selectIndex(indexOf(QStringLiteral("a.cpp")));
        QTimer::singleShot(0, [&] {
            for (QMessageBox *box : w.findChildren<QMessageBox *>())
                for (QAbstractButton *b : box->buttons())
                    if (b->text() == QStringLiteral("Revert"))
                        b->click();
        });
        action(w, QStringLiteral("Revert files…"))->trigger();
        QFile af(d + QStringLiteral("/sub/a.cpp"));
        QVERIFY(af.open(QIODevice::ReadOnly));
        QCOMPARE(af.readAll(), QByteArray("int a;\nint b;\nint c;\nint d;\nint e;\nint f;\n"));

        // Swap: the working tree goes to the left, so the deleted file is now right-only.
        action(w, QStringLiteral("Swap right and left"))->trigger();
        const int gone = indexOf(QStringLiteral("gone.txt"));
        QVERIFY(gone >= 0);
        files->selectIndex(gone);
        QVERIFY(pane->findChild<QLabel *>() != nullptr);
        bool leftIsWorkingTree = false;
        for (QLabel *l : pane->findChildren<QLabel *>())
            leftIsWorkingTree |= l->text().startsWith(QStringLiteral("Working tree")) && l->text().contains(QStringLiteral("does not exist"));
        QVERIFY(leftIsWorkingTree);
        QVERIFY(!action(w, QStringLiteral("Revert files…"))->isEnabled());
        action(w, QStringLiteral("Swap right and left"))->trigger();

        // Globbing ignore patterns.
        Settings::setIgnoreGlobs({QStringLiteral("*.txt")});
        w.findChild<QAction *>(); // keep the window busy-free
        action(w, QStringLiteral("Use globbing ignore patterns"))->setChecked(false);
        action(w, QStringLiteral("Use globbing ignore patterns"))->setChecked(true);
        QCOMPARE(files->visibleCount(), 0); // a.cpp was reverted; the rest are .txt
        Settings::setIgnoreGlobs({});
        action(w, QStringLiteral("Use globbing ignore patterns"))->setChecked(false);
    }

    void newComparisonDialogBuildsArguments()
    {
        QTemporaryDir tmp;
        QDir(tmp.path()).mkpath(QStringLiteral("L"));
        QDir(tmp.path()).mkpath(QStringLiteral("R"));
        // A failed check would open a modal warning; close any so the test fails instead of hanging.
        QTimer guard;
        connect(&guard, &QTimer::timeout, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->reject();
        });
        guard.start(200);
        NewComparisonDialog dlg{QString()};
        auto *leftDir = dlg.findChild<QLineEdit *>(QStringLiteral("leftDir"));
        auto *rightDir = dlg.findChild<QLineEdit *>(QStringLiteral("rightDir"));
        QVERIFY(leftDir && rightDir);
        leftDir->setText(tmp.filePath(QStringLiteral("L")));
        rightDir->setText(tmp.filePath(QStringLiteral("R")));
        QPushButton *compare = nullptr;
        for (QPushButton *b : dlg.findChildren<QPushButton *>())
            if (b->text() == QStringLiteral("Compare"))
                compare = b;
        QVERIFY(compare);
        compare->click();
        QCOMPARE(dlg.result(), int(QDialog::Accepted));
        QCOMPARE(dlg.arguments(), (QStringList{QStringLiteral("--no-index"), tmp.filePath(QStringLiteral("L")), tmp.filePath(QStringLiteral("R"))}));

        NewComparisonDialog pr{QString()};
        for (QRadioButton *r : pr.findChildren<QRadioButton *>())
            if (r->text().startsWith(QStringLiteral("A pull request")))
                r->click();
        pr.findChild<QLineEdit *>(QStringLiteral("prUrl"))->setText(QStringLiteral("https://dev.azure.com/o/p/_git/r/pullrequest/5"));
        for (QPushButton *b : pr.findChildren<QPushButton *>())
            if (b->text() == QStringLiteral("Compare"))
                b->click();
        QCOMPARE(pr.arguments(), (QStringList{QStringLiteral("-ado"), QStringLiteral("https://dev.azure.com/o/p/_git/r/pullrequest/5")}));
    }
    void themesAndBlame()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        write(d + QStringLiteral("/f.txt"), "one\ntwo\n");
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("first")});
        write(d + QStringLiteral("/f.txt"), "one\nTWO\n");
        QString error;
        auto repo = GitRepo::open(d, &error);
        Comparison cmp;
        cmp.allChanges = true;
        Settings::setShowBlame(false);
        MainWindow w(std::move(*repo), cmp);
        w.resize(1200, 700);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto *pane = w.findChild<DiffPane *>();
        QVERIFY(pane->hasDiff());

        // Themes: the palette follows, and light / dark switches to the counterpart.
        action(w, QStringLiteral("Vibrant Ink"))->trigger();
        QCOMPARE(Theme::current(), QStringLiteral("vibrant-ink"));
        QVERIFY(Theme::isDark());
        QCOMPARE(QApplication::palette().color(QPalette::Base), Theme::currentDef().base);
        action(w, QStringLiteral("One Dark"))->trigger();
        action(w, QStringLiteral("Switch light / dark"))->trigger();
        QCOMPARE(Theme::current(), QStringLiteral("one-light"));
        QVERIFY(!Theme::isDark());
        QVERIFY(action(w, QStringLiteral("One Light"))->isChecked());
        action(w, QStringLiteral("System"))->trigger();

        // Blame: off by default, then one entry per line of each side.
        CodeView *left = nullptr, *right = nullptr;
        for (CodeView *v : pane->findChildren<CodeView *>()) {
            if (v->numbers() == CodeView::Numbers::Old)
                left = v;
            else if (v->numbers() == CodeView::Numbers::New)
                right = v;
        }
        QVERIFY(left && right);
        const int plainGutter = right->viewport()->geometry().left();
        QVERIFY(!right->blameAt(0));
        action(w, QStringLiteral("Git blame"))->setChecked(true);
        QVERIFY(right->viewport()->geometry().left() > plainGutter);
        QVERIFY(right->blameAt(0));
        QCOMPARE(right->blameAt(0)->summary, QStringLiteral("first"));
        int changed = -1;
        for (int row = 0; row < right->rows().size(); ++row)
            if (right->rows()[row].newNo == 2)
                changed = row;
        QVERIFY(right->blameAt(changed) && right->blameAt(changed)->hash.isEmpty()); // not committed yet
        for (int row = 0; row < left->rows().size(); ++row)
            if (left->rows()[row].oldNo == 2)
                QCOMPARE(left->blameAt(row)->summary, QStringLiteral("first"));
        action(w, QStringLiteral("Git blame"))->setChecked(false);
        QCOMPARE(right->viewport()->geometry().left(), plainGutter);
    }
    void checkForUpdateAsksFirst()
    {
        // A fake GitHub API with a newer release.
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QByteArray body = R"({"tag_name":"v99.0.0","name":"yadiff 99","body":"Big release","html_url":"https://example.invalid/r"})";
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (QTcpSocket *s = server.nextPendingConnection())
                connect(s, &QTcpSocket::readyRead, s, [s, body] {
                    s->readAll();
                    s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size())
                             + "\r\nConnection: close\r\n\r\n" + body);
                    s->disconnectFromHost();
                });
        });
        qputenv("YADIFF_UPDATE_NO_GH", "1");
        qputenv("YADIFF_UPDATE_API", QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()).toUtf8());

        QTemporaryDir tmp;
        git(tmp.path(), {QStringLiteral("init"), QStringLiteral("-q")});
        QString error;
        auto repo = GitRepo::open(tmp.path(), &error);
        MainWindow w(std::move(*repo), Comparison());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        // The dialog names the release, shows its notes, and installs nothing unless asked.
        QString text, notes;
        QStringList buttons;
        QTimer poll;
        poll.setInterval(20);
        connect(&poll, &QTimer::timeout, &w, [&] {
            for (QMessageBox *box : w.findChildren<QMessageBox *>()) {
                if (!box->isVisible())
                    continue;
                text = box->text();
                notes = box->detailedText();
                for (QAbstractButton *b : box->buttons())
                    buttons << b->text();
                for (QAbstractButton *b : box->buttons())
                    if (b->text() == QStringLiteral("Later") || b->text() == QStringLiteral("Close"))
                        b->click();
                poll.stop();
            }
        });
        poll.start();
        QTimer guard;
        guard.setSingleShot(true);
        connect(&guard, &QTimer::timeout, &w, [] {
            if (QWidget *m = QApplication::activeModalWidget())
                m->close();
        });
        guard.start(10000);
        action(w, QStringLiteral("Check for update now…"))->trigger();
        qunsetenv("YADIFF_UPDATE_NO_GH");
        qunsetenv("YADIFF_UPDATE_API");
        QVERIFY2(text.contains(QStringLiteral("v99.0.0")), qPrintable(text));
        QCOMPARE(notes, QStringLiteral("Big release"));
        QVERIFY(buttons.contains(QStringLiteral("Open release page")));
        QVERIFY(!Settings::autoUpdate());
    }
};

QTEST_MAIN(TestNavigation)
#include "test_navigation.moc"
