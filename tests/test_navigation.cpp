#include "AdoLoginDialog.h"
#include "ComparisonArgs.h"
#include "CodeView.h"
#include "DiffPane.h"
#include "PrInbox.h"
#include "PrListDialog.h"
#include "PrOverviewPanel.h"
#include "FileList.h"
#include "HistoryDialog.h"
#include "ImageDiffView.h"
#include "MainWindow.h"
#include "MergeWindow.h"
#include "NewComparisonDialog.h"
#include "PrReview.h"
#include "SearchBar.h"
#include "SettingsDialog.h"
#include "Shortcuts.h"
#include "TableDiffView.h"
#include "Session.h"
#include "Settings.h"
#include "Theme.h"

#include <QApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFile>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QMimeData>
#include <QTimer>
#include <QRadioButton>
#include <QCheckBox>
#include <QLineEdit>
#include <QTabWidget>
#include <QTreeWidget>
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
#include <QComboBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QStatusBar>
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

    void nextChangeStartsAtTheClickedLine()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        write(d + QStringLiteral("/a.txt"), lines(100));
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("init")});
        write(d + QStringLiteral("/a.txt"), lines(100, 30)); // changes at lines 30, 60, 90

        QString error;
        auto repo = GitRepo::open(d, &error);
        QVERIFY2(repo, qPrintable(error));
        MainWindow w(std::move(*repo), Comparison{});
        w.resize(1100, 700);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto *pane = w.findChild<DiffPane *>();
        QVERIFY(pane);
        QCOMPARE(pane->changeCount(), 3);
        auto *changeLabel = w.findChild<QLabel *>(QStringLiteral("changeLabel"));
        QVERIFY(changeLabel);
        auto label = [changeLabel] { return changeLabel->text().trimmed(); };
        QCOMPARE(label(), QStringLiteral("Change 1 of 3"));

        for (const auto mode : {DiffPane::Mode::SideBySide, DiffPane::Mode::Inline}) {
            pane->setMode(mode);
            CodeView *view = nullptr;
            for (CodeView *v : pane->findChildren<CodeView *>())
                if (v->isVisible() && (!view || v->x() > view->x()))
                    view = v; // the right side, or the inline view
            QVERIFY(view);
            auto clickLine = [view](int newLine) {
                int row = -1;
                for (int i = 0; i < view->rows().size(); ++i)
                    if (view->rows()[i].newNo == newLine && view->rows()[i].kind != LineKind::Removed)
                        row = i;
                QVERIFY(row >= 0);
                view->scrollToRow(row);
                QTextCursor c(view->document()->findBlockByNumber(row));
                view->setTextCursor(c);
                QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, view->cursorRect().center());
                QCOMPARE(view->cursorRow(), row);
            };

            // Between the second and third change: Next is the third, Previous the second.
            clickLine(75);
            QCOMPARE(pane->currentChange(), -1);
            QCOMPARE(label(), QStringLiteral("Between changes 2 and 3 of 3"));
            QVERIFY(pane->nextChange());
            QCOMPARE(pane->currentChange(), 2);
            clickLine(75);
            QVERIFY(pane->prevChange());
            QCOMPARE(pane->currentChange(), 1);

            // On a change: it becomes the current one.
            clickLine(60);
            QCOMPARE(pane->currentChange(), 1);
            QCOMPARE(label(), QStringLiteral("Change 2 of 3"));

            // Above the first: Previous has nowhere to go, Next is the first.
            clickLine(5);
            QCOMPARE(label(), QStringLiteral("3 changes"));
            QVERIFY(!pane->prevChange());
            QVERIFY(pane->nextChange());
            QCOMPARE(pane->currentChange(), 0);

            // Below the last.
            clickLine(95);
            QCOMPARE(label(), QStringLiteral("After change 3 of 3"));
            QVERIFY(!pane->nextChange());
            QVERIFY(pane->prevChange());
            QCOMPARE(pane->currentChange(), 2);

            // A cursor key moves the place too.
            clickLine(59);
            QCOMPARE(pane->currentChange(), -1);
            QTest::keyClick(view, Qt::Key_Down);
            QCOMPARE(pane->currentChange(), 1);
        }
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
        // Wrapping round from the last match to the first, and back.
        pane->goToMatch(0);
        QVERIFY(pane->prevMatch(true));
        QCOMPARE(pane->currentMatch(), 1);
        QVERIFY(pane->nextMatch(true));
        QCOMPARE(pane->currentMatch(), 0);
        pane->clearSearch();

        // Find with the last "needle" selected searches the whole file for it, starting there;
        // Find next goes round to the first.
        CodeView *right = nullptr;
        for (CodeView *v : pane->findChildren<CodeView *>())
            if (v->isVisible() && v->numbers() == CodeView::Numbers::New)
                right = v;
        QVERIFY(right);
        int lastRow = -1;
        for (int row = 0; row < right->rows().size(); ++row)
            if (right->rows()[row].newNo == 3)
                lastRow = row;
        QVERIFY(lastRow >= 0);
        right->setFocus();
        QTextCursor sel(right->document()->findBlockByNumber(lastRow));
        sel.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 6);
        right->setTextCursor(sel);
        QCOMPARE(pane->selectedWord(), QStringLiteral("needle"));
        action(w, QStringLiteral("Find…"))->trigger();
        QCOMPARE(bar->scope(), SearchBar::Scope::File);
        QCOMPARE(bar->query().text(), QStringLiteral("needle"));
        QCOMPARE(pane->matchCount(), 2);
        QCOMPARE(pane->currentMatch(), 1);
        action(w, QStringLiteral("Find next"))->trigger();
        QCOMPARE(pane->currentMatch(), 0);
        QTest::qWait(300); // the find bar's delayed re-search keeps the current match
        QCOMPARE(pane->currentMatch(), 0);
        // Lines selected: search in them. Closed and opened again without a selection:
        // this file.
        QTextCursor lines(right->document()->firstBlock());
        lines.movePosition(QTextCursor::Down, QTextCursor::KeepAnchor, 2);
        right->setTextCursor(lines);
        action(w, QStringLiteral("Find…"))->trigger();
        QCOMPARE(bar->scope(), SearchBar::Scope::Selection);
        QTest::keyClick(bar, Qt::Key_Escape);
        QVERIFY(bar->isHidden());
        right->setTextCursor(QTextCursor(right->document()));
        action(w, QStringLiteral("Find…"))->trigger();
        QCOMPARE(bar->scope(), SearchBar::Scope::File);
        QTest::keyClick(bar, Qt::Key_Escape);

        // All changes through the search bar: Enter past the last match moves to b.txt.
        QTest::keyClick(&w, Qt::Key_F, Qt::ControlModifier);
        QVERIFY(bar->isVisible());
        bar->setScope(SearchBar::Scope::AllChanges);
        auto *edit = bar->findChild<QLineEdit *>();
        edit->clear();
        QTRY_COMPARE(pane->matchCount(), 0);
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
        {
            // For real, the list loads in the background: the dialog is up at once with a
            // "Loading…" row, and closing it while git runs is fine.
            qunsetenv("YADIFF_SYNCHRONOUS");
            HistoryDialog async(*repo);
            auto *asyncList = async.findChild<QTreeWidget *>();
            QVERIFY(async.isLoading());
            QCOMPARE(asyncList->topLevelItemCount(), 1);
            QVERIFY(asyncList->topLevelItem(0)->text(1).startsWith(QStringLiteral("Loading")));
            QTRY_VERIFY(!async.isLoading());
            QCOMPARE(asyncList->topLevelItemCount(), 3);
            auto *closedEarly = new HistoryDialog(*repo);
            delete closedEarly;
            qputenv("YADIFF_SYNCHRONOUS", "1");
        }
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
        QVERIFY(browser->parentWidget()->height() >= lines * right->fontMetrics().lineSpacing() - 2); // the overlay fills the row
        QVERIFY(browser->height() >= browser->document()->size().height()); // and shows all of the thread
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

        // A resolved thread says so first, greyed; its expanded header names the state.
        root.status = GitHubReview::ThreadStatus::Resolved;
        v.threads = GitHubReview::threads({root, reply});
        pane.setCommentsExpanded(false);
        pane.setFile(v);
        const Row &resolved = right->rows()[2];
        QVERIFY(resolved.comment && resolved.commentDone);
        QVERIFY2(resolved.text.startsWith(QStringLiteral("\u2713 Resolved · ")), qPrintable(resolved.text));
        pane.setCommentsExpanded(true);
        QVERIFY(right->findChild<QTextBrowser *>()->toPlainText().contains(QStringLiteral("Resolved")));
        QVERIFY(right->findChild<QTextBrowser *>()->toPlainText().contains(QStringLiteral("Reply"))); // no states here: no Resolve
    }

    void movedLinesKeepTheirSidesColour()
    {
        const FileDiff diff = DiffModel::parseUnified(QStringLiteral("@@ -1,8 +1,8 @@\n-void moved() {\n-    doSomethingImportant();\n-}\n"
                                                                     " int keep1;\n int keep2;\n int keep3;\n int keep4;\n int keep5;\n"
                                                                     "+void moved() {\n+    doSomethingImportant();\n+}\n"));
        QCOMPARE(diff.moves.size(), 1);
        FileView v;
        v.diff = diff;
        v.title = v.fileName = QStringLiteral("f.cpp");
        DiffPane pane;
        pane.resize(900, 400);
        pane.setMode(DiffPane::Mode::Inline);
        pane.show();
        QVERIFY(QTest::qWaitForWindowExposed(&pane));
        pane.setFile(v);
        CodeView *view = nullptr;
        for (CodeView *cv : pane.findChildren<CodeView *>())
            if (!cv->rows().isEmpty())
                view = cv;
        QVERIFY(view);
        const ViewPalette pal = diffPalette(&pane);
        int out = 0, in = 0;
        for (int i = 0; i < view->rows().size(); ++i) {
            const Row &r = view->rows()[i];
            if (!r.moved)
                continue;
            const QColor bg = view->document()->findBlockByNumber(i).blockFormat().background().color();
            QCOMPARE(bg, r.kind == LineKind::Removed ? pal.removed : pal.added);
            ++(r.kind == LineKind::Removed ? out : in);
        }
        QCOMPARE(out, 3);
        QCOMPARE(in, 3);
        if (const QString dir = qEnvironmentVariable("YADIFF_TEST_GRAB"); !dir.isEmpty()) {
            QApplication::processEvents();
            pane.grab().save(dir + QStringLiteral("/moved.png"));
        }
    }

    void reviewCommentsAreWrittenInline()
    {
        const FileDiff diff = DiffModel::parseUnified(QStringLiteral("@@ -1,3 +1,3 @@\n a\n-b\n+B\n c\n"));
        GitHubReview::Comment root;
        root.id = 7;
        root.path = QStringLiteral("f.txt");
        root.side = QStringLiteral("RIGHT");
        root.line = 2;
        root.author = QStringLiteral("alice");
        root.body = QStringLiteral("Why?");
        root.status = GitHubReview::ThreadStatus::Active;
        FileView v;
        v.diff = diff;
        v.title = v.fileName = QStringLiteral("f.txt");
        v.paths = {QStringLiteral("f.txt"), QStringLiteral("f.txt")};
        v.threads = GitHubReview::threads({root});
        v.reviewEnabled = true;
        v.threadStatuses = {{GitHubReview::ThreadStatus::Active, QStringLiteral("Active")},
                            {GitHubReview::ThreadStatus::Resolved, QStringLiteral("Resolved")}};
        DiffPane pane;
        pane.resize(1000, 500);
        pane.setMode(DiffPane::Mode::Inline);
        pane.show();
        QVERIFY(QTest::qWaitForWindowExposed(&pane));
        pane.setFile(v);
        CodeView *view = nullptr;
        for (CodeView *cv : pane.findChildren<CodeView *>())
            if (!cv->rows().isEmpty())
                view = cv;
        QVERIFY(view);
        auto editor = [view] { return view->findChild<QPlainTextEdit *>(); };

        // Reply: the thread opens with an editor under it, which has the focus.
        QVector<std::pair<qint64, QString>> replies;
        connect(&pane, &DiffPane::replySubmitted, [&](qint64 id, const QString &body) { replies.push_back({id, body}); });
        QVector<GitHubReview::ThreadStatus> statuses;
        connect(&pane, &DiffPane::threadStatusRequested, [&](qint64, GitHubReview::ThreadStatus s) { statuses << s; });
        pane.startReply(7);
        QVERIFY(pane.isThreadExpanded(7));
        QVERIFY(editor());
        QTRY_VERIFY(editor()->hasFocus());
        QTest::keyClicks(editor(), QStringLiteral("Because"));
        // A rebuild (new threads arrive, say) keeps the text and the focus.
        pane.setThreads(v.threads);
        QCOMPARE(editor()->toPlainText(), QStringLiteral("Because"));
        QTRY_VERIFY(editor()->hasFocus());
        if (const QString out = qEnvironmentVariable("YADIFF_TEST_GRAB"); !out.isEmpty()) {
            QApplication::processEvents();
            pane.grab().save(out + QStringLiteral("/reply.png"));
        }
        QTest::keyClick(editor(), Qt::Key_Return, Qt::ControlModifier);
        QTest::keyRelease(editor(), Qt::Key_Control); // or Qt still thinks Ctrl is held, for later tests
        QCOMPARE(QGuiApplication::keyboardModifiers(), Qt::NoModifier);
        QTRY_COMPARE(replies.size(), 1);
        QCOMPARE(replies[0], std::make_pair(qint64(7), QStringLiteral("Because")));
        QVERIFY(editor()->isReadOnly()); // while posting
        // Refused: the text stays to try again.
        pane.draftFailed();
        QVERIFY(!editor()->isReadOnly());
        QCOMPARE(editor()->toPlainText(), QStringLiteral("Because"));
        pane.draftPosted();
        pane.setThreads(v.threads);
        QVERIFY(!editor());
        QVERIFY(pane.isThreadExpanded(7));

        // The thread's own links reply and resolve.
        auto *browser = view->findChild<QTextBrowser *>();
        QVERIFY(browser);
        emit browser->anchorClicked(QUrl(QStringLiteral("yadiff:resolve")));
        QTRY_COMPARE(statuses.size(), 1);
        QCOMPARE(statuses[0], GitHubReview::ThreadStatus::Resolved);
        emit browser->anchorClicked(QUrl(QStringLiteral("yadiff:reply")));
        QTRY_VERIFY(editor());
        QTest::keyClick(editor(), Qt::Key_Escape); // empty: cancelled without asking
        QTRY_VERIFY(!editor());

        // A new thread: an editor under its line, alone.
        QVector<QStringList> posted;
        connect(&pane, &DiffPane::commentSubmitted, [&](const QString &path, const QString &side, int line, int startLine, const QString &body) {
            posted.push_back({path, side, QString::number(line), QString::number(startLine), body});
        });
        pane.startComment(QStringLiteral("f.txt"), QStringLiteral("LEFT"), 2, 0);
        int row = -1;
        for (int i = 0; i < view->rows().size(); ++i)
            if (view->rows()[i].comment && view->rows()[i].threadId == DiffPane::kNewThread)
                row = i;
        QVERIFY(row > 0);
        QCOMPARE(view->rows()[row - 1].kind, LineKind::Removed); // under the old line 2
        QVERIFY(view->rows()[row].compose);
        QVERIFY(view->document()->findBlockByNumber(row).text().size() >= 4); // room for the editor
        QTRY_VERIFY(editor() && editor()->hasFocus());
        QTest::keyClicks(editor(), QStringLiteral("Gone?"));
        QTest::keyClick(editor(), Qt::Key_Enter, Qt::ControlModifier);
        QTest::keyRelease(editor(), Qt::Key_Control);
        QTRY_COMPARE(posted.size(), 1);
        QCOMPARE(posted[0], (QStringList{QStringLiteral("f.txt"), QStringLiteral("LEFT"), QStringLiteral("2"), QStringLiteral("0"), QStringLiteral("Gone?")}));
        pane.draftPosted();
        pane.setThreads(v.threads);
        QVERIFY(!editor());
        QVERIFY(!pane.hasDraft());
    }

    void prOverviewShowsHowThePrStands()
    {
        PrOverview::Overview o;
        o.title = QStringLiteral("Make two loud");
        o.author = QStringLiteral("Alice");
        o.description = QStringLiteral("Makes **two** loud.\n\n<!-- bot-marker -->\n\n```\ncode here\n```");
        o.sourceBranch = QStringLiteral("feature");
        o.targetBranch = QStringLiteral("main");
        o.reviewers = {{QStringLiteral("Team X"), GitHubReview::Vote::Approved, true, true, false},
                       {QStringLiteral("Bob"), GitHubReview::Vote::WaitingForAuthor, false, false, false}};
        PrOverview::Check failed;
        failed.name = QStringLiteral("CI build");
        failed.state = PrOverview::CheckState::Failed;
        failed.required = true;
        o.checks = {failed};
        o.workItems = {{QStringLiteral("1234"), QStringLiteral("Bug"), QStringLiteral("Loud numbers"), QStringLiteral("Active"), {}}};
        GitHubReview::Comment c;
        c.id = 6;
        c.author = QStringLiteral("Carol");
        c.body = QStringLiteral("Overall fine");
        c.status = GitHubReview::ThreadStatus::Active;
        o.discussion = {{c}};
        PrOverviewPanel panel;
        panel.resize(420, 600);
        panel.setOverview(o, QStringLiteral("Azure DevOps"), true);
        const QString text = panel.browser()->toPlainText();
        for (const char *expected : {"Make two loud", "by Alice", "feature \u2192 main", "Team X", "(required)", "approved",
                                     "waiting for author", "CI build", "failed", "Bug 1234: Loud numbers", "Overall fine", "Active",
                                     "code here"})
            QVERIFY2(text.contains(QString::fromUtf8(expected)), expected);
        QVERIFY(!text.contains(QStringLiteral("bot-marker")));
        QVERIFY(!text.contains(QStringLiteral("**")));
        QVERIFY2(panel.summary().contains(QStringLiteral("1 approved")) && panel.summary().contains(QStringLiteral("1 check failed")),
                 qPrintable(panel.summary()));
        // Its links reply to and resolve the PR's threads, and add a comment.
        qint64 replied = 0, resolved = 0;
        bool commented = false;
        connect(&panel, &PrOverviewPanel::replyRequested, [&](qint64 id) { replied = id; });
        connect(&panel, &PrOverviewPanel::resolveRequested, [&](qint64 id, bool on) { resolved = on ? id : -id; });
        connect(&panel, &PrOverviewPanel::commentRequested, [&] { commented = true; });
        emit panel.browser()->anchorClicked(QUrl(QStringLiteral("yadiff:reply:6")));
        emit panel.browser()->anchorClicked(QUrl(QStringLiteral("yadiff:resolve:6")));
        emit panel.browser()->anchorClicked(QUrl(QStringLiteral("yadiff:comment")));
        QCOMPARE(replied, qint64(6));
        QCOMPARE(resolved, qint64(6));
        QVERIFY(commented);
        QVERIFY(panel.browser()->toHtml().contains(QStringLiteral("yadiff:resolve:6")));
        // Folding keeps the header's summary.
        panel.setFolded(true);
        QVERIFY(panel.isFolded() && !panel.browser()->isVisibleTo(&panel));
        panel.setFolded(false);
        if (const QString out = qEnvironmentVariable("YADIFF_TEST_GRAB"); !out.isEmpty()) {
            panel.show();
            QApplication::processEvents();
            panel.grab().save(out + QStringLiteral("/overview.png"));
        }
    }

    void comparesAPullRequestsPushes()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        auto commit = [&](const char *file, const QString &msg) {
            write(d + u'/' + QLatin1String(file), "x\n");
            git(d, {QStringLiteral("add"), QStringLiteral("-A")});
            git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), msg});
        };
        commit("base.txt", QStringLiteral("base"));
        commit("a.txt", QStringLiteral("push one"));
        commit("b.txt", QStringLiteral("push two"));
        commit("c.txt", QStringLiteral("push three"));
        QString error;
        auto repo = GitRepo::open(d, &error);
        auto at = [&](const char *rev) { return *repo->resolveCommit(QLatin1String(rev), &error); };
        // An Azure DevOps PR with no server behind it (comments and votes fail quickly);
        // its pushes come from the test.
        Comparison pr;
        pr.mode = Comparison::Mode::Revisions;
        pr.rev1 = at("HEAD~3");
        pr.rev1Name = QStringLiteral("main (merge base)");
        pr.rev2 = at("HEAD");
        pr.rev2Name = QStringLiteral("PR 9 feature");
        pr.prHost = QStringLiteral("ado");
        pr.prNumber = 9;
        pr.prCollectionUrl = QStringLiteral("http://127.0.0.1:9");
        pr.prProject = QStringLiteral("P");
        pr.prRepo = QStringLiteral("R");
        pr.prWebUrl = QStringLiteral("http://127.0.0.1:9/P/_git/R/pullrequest/9");
        pr.prHead = pr.rev2;
        PrReview::History h;
        h.versions = {{1, at("HEAD~2"), pr.rev1, {}, {}}, {2, at("HEAD~1"), pr.rev1, {}, {}}, {3, at("HEAD"), pr.rev1, {}, {}}};
        h.lastReviewed = 1;
        MainWindow w(std::move(*repo), pr);
        w.resize(1300, 700);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto *files = w.findChild<FileList *>();
        QTRY_COMPARE(files->visibleCount(), 3);
        QComboBox *versions = nullptr;
        for (QComboBox *c : w.findChildren<QComboBox *>())
            if (c->toolTip().contains(QStringLiteral("since an earlier push")))
                versions = c;
        QVERIFY(versions);

        // The pushes arrive: it opens at the changes since the user's last review (push 1).
        w.showPrHistory(h);
        QTRY_VERIFY(!w.isBusy());
        QCOMPARE(files->visibleCount(), 2);
        QVERIFY2(versions->currentText().startsWith(QStringLiteral("Since your last review (Push 1)")), qPrintable(versions->currentText()));
        QVERIFY(versions->isVisibleTo(&w));
        QVERIFY(w.windowTitle().size() > 0);
        // All changes, then just the latest push.
        auto pick = [&](const QString &prefix) {
            for (int i = 0; i < versions->count(); ++i)
                if (versions->itemText(i).startsWith(prefix)) {
                    versions->setCurrentIndex(i);
                    emit versions->activated(i);
                    return true;
                }
            return false;
        };
        QVERIFY(pick(QStringLiteral("All changes (3 pushes)")));
        QTRY_VERIFY(!w.isBusy());
        QCOMPARE(files->visibleCount(), 3);
        QVERIFY(pick(QStringLiteral("Since Push 2")));
        QTRY_VERIFY(!w.isBusy());
        QCOMPARE(files->visibleCount(), 1);
        QVERIFY(versions->currentText().startsWith(QStringLiteral("Since Push 2")));
        // The pushes are still known after switching: the picker keeps all its entries.
        QVERIFY(versions->findText(QStringLiteral("Compare two pushes…")) >= 0);
        if (const QString out = qEnvironmentVariable("YADIFF_TEST_GRAB"); !out.isEmpty()) {
            QApplication::processEvents();
            w.grab().save(out + QStringLiteral("/pushes.png"));
        }
    }

    void pullRequestListShowsWhatsNew()
    {
        // What was seen: the same however the link is spelled.
        PrInbox::markSeen(QStringLiteral("https://dev.azure.com/contoso/My%20Project/_git/web/pullrequest/4"), QStringLiteral("abc"));
        QCOMPARE(PrInbox::lastSeen(QStringLiteral("https://dev.azure.com/Contoso/My Project/_git/web/pullrequest/4/")), QStringLiteral("abc"));
        QVERIFY(PrInbox::lastSeen(QStringLiteral("https://dev.azure.com/contoso/My%20Project/_git/web/pullrequest/5")).isEmpty());
        PrInbox::markSeen(QStringLiteral("https://dev.azure.com/contoso/My%20Project/_git/web/pullrequest/6"), QStringLiteral("old"));

        PrInbox::Listing listing;
        auto item = [](int n, const QString &head) {
            PrInbox::Item it;
            it.host = QStringLiteral("ado");
            it.number = n;
            it.link = QStringLiteral("https://dev.azure.com/contoso/My%20Project/_git/web/pullrequest/%1").arg(n);
            it.repo = QStringLiteral("My Project/web");
            it.title = QStringLiteral("Change %1").arg(n);
            it.author = QStringLiteral("Alice");
            it.headCommit = head;
            return it;
        };
        listing.items = {item(4, QStringLiteral("abc")), item(5, QStringLiteral("x")), item(6, QStringLiteral("new"))};
        listing.items[2].via = QStringLiteral("Core");
        listing.items[2].myVote = GitHubReview::Vote::WaitingForAuthor;
        listing.problems << QStringLiteral("GitHub: the GitHub CLI (gh) is not installed");
        PrListDialog dlg(nullptr, false);
        dlg.showListing(listing);
        QTreeWidget *tree = dlg.tree();
        QCOMPARE(tree->topLevelItemCount(), 3);
        QCOMPARE(tree->topLevelItem(0)->text(5), QString()); // seen at its head
        QCOMPARE(tree->topLevelItem(1)->text(5), QStringLiteral("new"));
        QCOMPARE(tree->topLevelItem(2)->text(5), QStringLiteral("new push, via Core"));
        QCOMPARE(tree->topLevelItem(2)->text(3), QStringLiteral("Waiting for author"));
        QVERIFY(tree->topLevelItem(2)->font(0).bold() && !tree->topLevelItem(0)->font(0).bold());
        QVERIFY(dlg.findChildren<QLabel *>().size() > 0);
        bool problemShown = false;
        for (QLabel *l : dlg.findChildren<QLabel *>())
            problemShown |= l->text().contains(QStringLiteral("gh) is not installed"));
        QVERIFY(problemShown);
        // The filter, then opening a row.
        auto *filter = dlg.findChild<QLineEdit *>();
        filter->setText(QStringLiteral("change 5"));
        QVERIFY(tree->topLevelItem(0)->isHidden() && !tree->topLevelItem(1)->isHidden());
        filter->clear();
        tree->setCurrentItem(tree->topLevelItem(2));
        emit tree->itemActivated(tree->topLevelItem(2), 0);
        QCOMPARE(dlg.result(), int(QDialog::Accepted));
        QCOMPARE(dlg.host(), QStringLiteral("ado"));
        QVERIFY(dlg.link().endsWith(QStringLiteral("/pullrequest/6")));
        if (const QString out = qEnvironmentVariable("YADIFF_TEST_GRAB"); !out.isEmpty()) {
            PrListDialog shot(nullptr, false);
            shot.showListing(listing);
            shot.show();
            QApplication::processEvents();
            shot.grab().save(out + QStringLiteral("/pr-list.png"));
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

    void settingsChangeShortcutsAndOptions()
    {
        QTemporaryDir tmp;
        git(tmp.path(), {QStringLiteral("init"), QStringLiteral("-q")});
        QString error;
        Shortcuts::setOverrides({});
        auto repo = GitRepo::open(tmp.path(), &error);
        MainWindow w(std::move(*repo), Comparison{});
        w.show();
        QAction *find = action(w, QStringLiteral("Find…"));
        QAction *next = action(w, QStringLiteral("Next change"));
        QVERIFY(find && next);
        QCOMPARE(Shortcuts::id(find), QStringLiteral("Edit/Find"));
        QCOMPARE(Shortcuts::defaults(find).size(), 2); // F3 and Ctrl+F

        {
            SettingsDialog dlg(&w, {});
            if (const QString grab = qEnvironmentVariable("YADIFF_TEST_GRAB"); !grab.isEmpty()) {
                dlg.show();
                dlg.grab().save(grab + QStringLiteral("-options.png"));
                dlg.tabs()->setCurrentIndex(1);
                dlg.grab().save(grab + QStringLiteral("-shortcuts.png"));
            }
            // Every command with a shortcut is listed, under its menu.
            QStringList listed;
            for (int i = 0; i < dlg.shortcutTree()->topLevelItemCount(); ++i)
                for (int c = 0; c < dlg.shortcutTree()->topLevelItem(i)->childCount(); ++c)
                    listed << dlg.shortcutTree()->topLevelItem(i)->text(0) + u'/' + dlg.shortcutTree()->topLevelItem(i)->child(c)->text(0);
            QVERIFY(listed.contains(QStringLiteral("Edit/Find")));
            QVERIFY(listed.contains(QStringLiteral("View/Zoom › Zoom in")));
            QVERIFY(listed.contains(QStringLiteral("Window/Filter files")));

            // A new shortcut; taking one another command has moves it (once confirmed).
            QVERIFY(dlg.assign(find, {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F)}));
            QVERIFY(dlg.assign(find, {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), QKeySequence(Qt::ALT | Qt::Key_Down)}, false));
            QVERIFY(dlg.pending(next).isEmpty());
            QCOMPARE(find->shortcuts(), Shortcuts::defaults(find)); // not until Apply
            dlg.applyShortcuts();
            QCOMPARE(find->shortcut(), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
            QVERIFY(next->shortcuts().isEmpty());
            QVERIFY(!next->toolTip().contains(QStringLiteral("Alt+Down"))); // the tooltip follows

            // Options are the menus' own: ticking one here toggles the action.
            QCheckBox *numbers = nullptr;
            for (QCheckBox *c : dlg.findChildren<QCheckBox *>())
                if (c->text() == QStringLiteral("Show line numbers"))
                    numbers = c;
            QVERIFY(numbers);
            QAction *numbersAct = action(w, QStringLiteral("Show line numbers"));
            const bool was = numbersAct->isChecked();
            QCOMPARE(numbers->isChecked(), was);
            numbers->click();
            QCOMPARE(numbersAct->isChecked(), !was);
            numbersAct->trigger();
            QCOMPARE(numbers->isChecked(), was);
            QRadioButton *oneDark = nullptr;
            for (QRadioButton *r : dlg.findChildren<QRadioButton *>())
                if (r->text() == QStringLiteral("One Dark"))
                    oneDark = r;
            QVERIFY(oneDark);
            oneDark->click();
            QCOMPARE(Theme::current(), QStringLiteral("one-dark"));
            action(w, QStringLiteral("System"))->trigger();
            QVERIFY(!oneDark->isChecked());
            // Votes are not options.
            for (QRadioButton *r : dlg.findChildren<QRadioButton *>())
                QVERIFY(!r->text().contains(QStringLiteral("Approve")));

            // Finding: only what matches stays.
            dlg.filter()->setText(QStringLiteral("line numbers"));
            QVERIFY(!numbers->isHidden());
            QVERIFY(oneDark->isHidden());
            int shown = 0;
            for (int i = 0; i < dlg.shortcutTree()->topLevelItemCount(); ++i)
                for (int c = 0; c < dlg.shortcutTree()->topLevelItem(i)->childCount(); ++c)
                    shown += !dlg.shortcutTree()->topLevelItem(i)->child(c)->isHidden();
            QCOMPARE(shown, 1);
        }

        // Saved: the next window has them too; back to the defaults clears them.
        QCOMPARE(Shortcuts::overrides().value(QStringLiteral("Edit/Find")).size(), 2);
        {
            auto repo2 = GitRepo::open(tmp.path(), &error);
            MainWindow w2(std::move(*repo2), Comparison{});
            QCOMPARE(action(w2, QStringLiteral("Find…"))->shortcut(), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
            QVERIFY(action(w2, QStringLiteral("Next change"))->shortcuts().isEmpty());
        }
        {
            SettingsDialog dlg(&w, {});
            QVERIFY(dlg.assign(find, Shortcuts::defaults(find)));
            QVERIFY(dlg.assign(next, Shortcuts::defaults(next)));
            dlg.applyShortcuts();
        }
        QVERIFY(Shortcuts::overrides().isEmpty());
        QCOMPARE(next->shortcut(), QKeySequence(Qt::ALT | Qt::Key_Down));
        QVERIFY(next->toolTip().contains(QStringLiteral("Alt+Down")));
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
        // The line numbers follow the theme too (they kept the previous one's colours).
        auto gutterLightness = [pane] {
            QCoreApplication::processEvents();
            CodeView *v = pane->findChildren<CodeView *>().constLast();
            for (CodeView *c : pane->findChildren<CodeView *>())
                if (c->isVisible())
                    v = c;
            return v->grab().toImage().pixelColor(4, 4).lightness();
        };
        QVERIFY2(gutterLightness() < 128, qPrintable(QString::number(gutterLightness())));
        action(w, QStringLiteral("One Light"))->trigger();
        QVERIFY2(gutterLightness() > 128, qPrintable(QString::number(gutterLightness())));
        action(w, QStringLiteral("One Dark"))->trigger();
        QVERIFY2(gutterLightness() < 128, qPrintable(QString::number(gutterLightness())));
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
    void unimportantTextAndManualAlignment()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        write(d + QStringLiteral("/f.cpp"), "int a; // one\nint b;\nint c;\n");
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("first")});
        write(d + QStringLiteral("/f.cpp"), "int a; // two\nint x;\nint b;\nint c;\n");
        QString error;
        auto repo = GitRepo::open(d, &error);
        Comparison cmp;
        cmp.allChanges = true;
        MainWindow w(std::move(*repo), cmp);
        w.resize(1200, 700);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto *pane = w.findChild<DiffPane *>();
        QVERIFY(pane->hasDiff());
        QCOMPARE(pane->changeCount(), 1); // git: lines 1-2 (one chunk)

        action(w, QStringLiteral("Ignore comments"))->trigger();
        QCOMPARE(pane->changeCount(), 1); // only "int x;" is left
        QVERIFY(pane->diff().lines[0].unimportant);
        CodeView *right = nullptr;
        for (CodeView *v : pane->findChildren<CodeView *>())
            if (v->numbers() == CodeView::Numbers::New)
                right = v;
        QVERIFY(right->rows()[0].unimportant);
        action(w, QStringLiteral("Ignore comments"))->trigger();

        // Line up old "int b;" (2) with new "int x;" (2): they show as an edit on one row.
        QMetaObject::invokeMethod(pane, "alignRequested", Q_ARG(int, 2), Q_ARG(int, 2));
        int pairs = 0;
        for (const DiffLine &l : pane->diff().lines)
            pairs += l.anchored;
        QCOMPARE(pairs, 2);
        int row = -1;
        for (int r = 0; r < right->rows().size(); ++r)
            if (right->rows()[r].newNo == 2)
                row = r;
        QVERIFY(right->rows()[row].anchored);
        QMetaObject::invokeMethod(pane, "clearAlignmentsRequested");
        for (const DiffLine &l : pane->diff().lines)
            QVERIFY(!l.anchored);
        Settings::setDiffOptions(DiffOptions());
    }
    void stagesTheCurrentChange()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        write(d + QStringLiteral("/f.txt"), "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n");
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("first")});
        write(d + QStringLiteral("/f.txt"), "one\n2\n3\n4\n5\n6\n7\n8\n9\nten\n");
        QString error;
        auto repo = GitRepo::open(d, &error);
        MainWindow w(std::move(*repo), Comparison());
        w.resize(1200, 700);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto *pane = w.findChild<DiffPane *>();
        QVERIFY(pane->hasDiff());
        QCOMPARE(pane->changeCount(), 2);
        QAction *stage = action(w, QStringLiteral("Stage current change or selected lines"));
        QVERIFY(stage->isEnabled());
        QVERIFY(!action(w, QStringLiteral("Unstage current change or selected lines"))->isEnabled());
        pane->goToChange(1);
        stage->trigger();
        QProcess p;
        p.setWorkingDirectory(d);
        p.start(QStringLiteral("git"), {QStringLiteral("show"), QStringLiteral(":0:f.txt")});
        QVERIFY(p.waitForFinished());
        QCOMPARE(p.readAllStandardOutput(), QByteArray("1\n2\n3\n4\n5\n6\n7\n8\n9\nten\n"));
        // The file is now in both sections; the staged one can be unstaged.
        auto *files = w.findChild<FileList *>();
        QVERIFY(files);
        bool unstageWorked = false;
        for (int i = 0; i < 4 && !unstageWorked; ++i) {
            files->selectIndex(i);
            if (action(w, QStringLiteral("Unstage current change or selected lines"))->isEnabled()) {
                QCOMPARE(pane->changeCount(), 1);
                pane->goToChange(0);
                action(w, QStringLiteral("Unstage current change or selected lines"))->trigger();
                unstageWorked = true;
            }
        }
        QVERIFY(unstageWorked);
        p.start(QStringLiteral("git"), {QStringLiteral("diff"), QStringLiteral("--cached"), QStringLiteral("--quiet")});
        QVERIFY(p.waitForFinished());
        QCOMPARE(p.exitCode(), 0); // nothing staged any more
    }
    void stepsThroughCommits()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        auto commit = [&](const char *file, const char *text, const QString &msg) {
            write(d + u'/' + QLatin1String(file), text);
            git(d, {QStringLiteral("add"), QStringLiteral("-A")});
            git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), msg});
        };
        commit("a.txt", "a\n", QStringLiteral("one"));
        commit("b.txt", "b\n", QStringLiteral("two"));
        commit("c.txt", "c\n", QStringLiteral("three"));
        QString error;
        auto repo = GitRepo::open(d, &error);
        Comparison cmp;
        cmp.mode = Comparison::Mode::Revisions;
        cmp.rev1 = *repo->resolveCommit(QStringLiteral("HEAD~2"), &error);
        cmp.rev1Name = QStringLiteral("HEAD~2");
        cmp.rev2 = *repo->resolveCommit(QStringLiteral("HEAD"), &error);
        cmp.rev2Name = QStringLiteral("HEAD");
        MainWindow w(std::move(*repo), cmp);
        w.resize(1200, 700);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto *files = w.findChild<FileList *>();
        auto count = [&] {
            int n = 0;
            while (files->selectIndex(n))
                ++n;
            files->selectIndex(0);
            return n;
        };
        QCOMPARE(count(), 2); // b.txt and c.txt
        QAction *next = action(w, QStringLiteral("Next commit"));
        QAction *prev = action(w, QStringLiteral("Previous commit"));
        QVERIFY(next->isEnabled());
        QVERIFY(!prev->isEnabled()); // already showing the whole range
        next->trigger();
        QCOMPARE(count(), 1);
        QVERIFY(w.windowTitle().contains(QStringLiteral("two")));
        next->trigger();
        QVERIFY(w.windowTitle().contains(QStringLiteral("three")));
        QVERIFY(!next->isEnabled());
        prev->trigger();
        prev->trigger();
        QCOMPARE(count(), 2); // back to the whole range
        QVERIFY(!prev->isEnabled());

        // A single commit steps through its neighbours, down to the root commit.
        auto repo2 = GitRepo::open(d, &error);
        Comparison one;
        one.mode = Comparison::Mode::Revisions;
        one.rev1 = *repo2->resolveCommit(QStringLiteral("HEAD~2"), &error);
        one.rev2 = *repo2->resolveCommit(QStringLiteral("HEAD~1"), &error);
        MainWindow w2(std::move(*repo2), one);
        w2.show();
        QAction *prev2 = action(w2, QStringLiteral("Previous commit"));
        QAction *next2 = action(w2, QStringLiteral("Next commit"));
        QVERIFY(prev2->isEnabled() && next2->isEnabled());
        prev2->trigger();
        QVERIFY(w2.windowTitle().contains(QStringLiteral("one")));
        QVERIFY(!prev2->isEnabled());
        next2->trigger();
        next2->trigger();
        QVERIFY(w2.windowTitle().contains(QStringLiteral("three")));
        QVERIFY(!next2->isEnabled());

        // A one-commit pull request has no neighbours to step through: those would be the
        // target branch's history, which can take minutes to walk in a large repository.
        auto repo3 = GitRepo::open(d, &error);
        Comparison pr = one;
        pr.prHost = QStringLiteral("ado");
        pr.prNumber = 7;
        MainWindow w3(std::move(*repo3), pr);
        w3.show();
        QVERIFY(!action(w3, QStringLiteral("Previous commit"))->isEnabled());
        QVERIFY(!action(w3, QStringLiteral("Next commit"))->isEnabled());

        // A PR has a vote button in the status bar, with the host's votes; here there is
        // no server, so the vote stays unknown.
        QToolButton *vote = nullptr;
        for (auto *b : w3.statusBar()->findChildren<QToolButton *>())
            if (b->text().startsWith(QStringLiteral("Your vote")))
                vote = b;
        QVERIFY(vote && vote->isVisible());
        QTRY_VERIFY(vote->text() == QStringLiteral("Your vote: unknown"));
        QStringList labels;
        for (QAction *a : vote->menu()->actions())
            if (!a->isSeparator())
                labels << a->text();
        QCOMPARE(labels.mid(0, 5), (QStringList{QStringLiteral("Approve…"), QStringLiteral("Approve with suggestions…"),
                                                QStringLiteral("Wait for author…"), QStringLiteral("Reject…"),
                                                QStringLiteral("Reset vote…")}));
        QVERIFY(action(w3, QStringLiteral("Approve with suggestions…")));
        // A GitHub PR offers GitHub's two (checked without a window, which would call gh).
        Comparison ghPr = one;
        ghPr.prHost = QStringLiteral("github");
        ghPr.prNumber = 3;
        QCOMPARE(PrReview::votes(ghPr), (QVector<GitHubReview::Vote>{GitHubReview::Vote::Approved, GitHubReview::Vote::Rejected}));
        QCOMPARE(PrReview::voteAction(ghPr, GitHubReview::Vote::Rejected), QStringLiteral("Request changes"));
        QVERIFY(PrReview::voteNeedsMessage(ghPr, GitHubReview::Vote::Rejected));
    }
    void loadsEveryDiffInTheBackground()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        for (int i = 0; i < 9; ++i)
            write(d + QStringLiteral("/f%1.txt").arg(i), "a\nb\nc\n");
        write(d + QStringLiteral("/blank.txt"), "a\nb\n");
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("one")});
        for (int i = 0; i < 9; ++i)
            write(d + QStringLiteral("/f%1.txt").arg(i), QByteArray("a\nB\nc\n") + QByteArray(i, 'x') + "\n");
        write(d + QStringLiteral("/blank.txt"), "a\n\nb\n");
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("two")});
        QString error;
        auto repo = GitRepo::open(d, &error);
        Comparison cmp;
        cmp.mode = Comparison::Mode::Revisions;
        cmp.rev1 = *repo->resolveCommit(QStringLiteral("HEAD~1"), &error);
        cmp.rev2 = *repo->resolveCommit(QStringLiteral("HEAD"), &error);
        cmp.rev1Name = QStringLiteral("HEAD~1");
        cmp.rev2Name = QStringLiteral("HEAD");
        const QVector<FileChange> fromGit = repo->listChanges(cmp, &error);

        // Really in the background here, not the tests' synchronous mode.
        qunsetenv("YADIFF_SYNCHRONOUS");
        MainWindow w(std::move(*repo), cmp);
        qputenv("YADIFF_SYNCHRONOUS", "1");
        w.show();
        auto *files = w.findChild<FileList *>();
        auto *bar = w.findChild<QProgressBar *>();
        QVERIFY(bar);
        QTRY_VERIFY_WITH_TIMEOUT(!w.isBusy(), 30000);
        QVERIFY(bar->isHidden()); // nothing left to load

        // Every file is loaded, and its line counts (from the diff) match git's.
        const QVector<int> order = files->displayOrder(true);
        QCOMPARE(order.size(), 10);
        for (int idx : order)
            QCOMPARE(files->loadState(idx), FileList::LoadState::Loaded);
        for (const FileChange &g : fromGit) {
            bool found = false;
            for (int idx : order) {
                files->selectIndex(idx);
                const QString tip = files->currentItem()->toolTip(0);
                if (!tip.startsWith(g.path + u'\n'))
                    continue;
                found = true;
                QVERIFY2(tip.contains(QStringLiteral("%1 added, %2 removed lines").arg(g.added).arg(g.removed)), qPrintable(tip));
                QCOMPARE(tip.contains(QStringLiteral("Only blank lines differ")), g.blankOnly);
            }
            QVERIFY2(found, qPrintable(g.path));
        }
        QVERIFY(order.size() > 0);

        // Loaded files show at once, from what was loaded.
        files->selectIndex(order.last());
        QTRY_VERIFY_WITH_TIMEOUT(!w.isBusy(), 10000);
        QVERIFY(w.windowTitle().contains(QStringLiteral("HEAD")));
        auto *pane = w.findChild<DiffPane *>();
        QVERIFY(pane->currentChange() >= 0);
    }

    void hexAndTables()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        QByteArray bin(300, '\x01');
        bin[0] = '\0';
        write(d + QStringLiteral("/a.bin"), bin);
        write(d + QStringLiteral("/b.csv"), "id,v\n1,a\n2,b\n3,c\n");
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("first")});
        bin[150] = '\x02';
        write(d + QStringLiteral("/a.bin"), bin);
        write(d + QStringLiteral("/b.csv"), "id,v\n1,a\n2,B\n4,d\n");
        QString error;
        auto repo = GitRepo::open(d, &error);
        Comparison cmp;
        cmp.allChanges = true;
        Settings::setHexBinary(true);
        Settings::setTableView(true);
        MainWindow w(std::move(*repo), cmp);
        w.resize(1200, 700);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto *pane = w.findChild<DiffPane *>();
        auto *files = w.findChild<FileList *>();
        QVERIFY(files->selectIndex(0)); // a.bin
        QVERIFY(pane->diff().hexDump);
        QCOMPARE(pane->changeCount(), 1);
        QVERIFY(!pane->canEdit());
        QVERIFY(!pane->canCopyTo(true));

        QVERIFY(files->selectIndex(1)); // b.csv
        auto *table = pane->findChild<TableDiffView *>();
        QVERIFY(pane->showsTable());
        QVERIFY(table->isVisible());
        QCOMPARE(table->result().changed, 1);
        QCOMPARE(table->changeCount(), 3); // 2 changed, 3 removed, 4 added (by order)
        table->setKeyColumn(0);
        QCOMPARE(table->result().changed, 1);
        QCOMPARE(table->result().removed, 1);
        QCOMPARE(table->result().added, 1);
        QVERIFY(pane->nextChange());
        QVERIFY(pane->nextChange());
        QVERIFY(!pane->nextChange()); // the last of three
        QMetaObject::invokeMethod(table, "textViewRequested");
        QVERIFY(!pane->showsTable());
        QVERIFY(!action(w, QStringLiteral("Show CSV and TSV files as tables"))->isChecked());
        Settings::setTableView(true);
    }
    void mergeWindowResolves()
    {
        QTemporaryDir tmp;
        MergeWindow::Input in;
        in.base = "a\nb\nc\nd\ne\nf\n";
        in.ours = "a\nB\nc\nd\ne\nX\n";   // b changed (only here), f conflicts
        in.theirs = "a\nb\nc\nD\ne\nY\n"; // d changed (only here), f conflicts
        in.outputPath = tmp.filePath(QStringLiteral("out.txt"));
        in.fileName = QStringLiteral("out.txt");
        MergeWindow w(in);
        w.show();
        QCOMPARE(w.conflictCount(), 1);
        QCOMPARE(w.unresolvedCount(), 1);
        QCOMPARE(w.resultText(), QStringLiteral("a\nB\nc\nD\ne\n<<<<<<< Ours\nX\n||||||| base\nf\n=======\nY\n>>>>>>> Theirs\n"));
        QVERIFY(w.currentRegion() >= 0); // opens on the conflict
        w.take(Merge3::Pick::TheirsThenOurs);
        QCOMPARE(w.unresolvedCount(), 0);
        QCOMPARE(w.resultText(), QStringLiteral("a\nB\nc\nD\ne\nY\nX\n"));
        w.take(Merge3::Pick::Theirs);
        QCOMPARE(w.resultText(), QStringLiteral("a\nB\nc\nD\ne\nY\n"));
        QVERIFY(w.save());
        QVERIFY(w.resolvedAndSaved());
        QFile f(in.outputPath);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("a\nB\nc\nD\ne\nY\n"));
        f.close();
        // Back to markers, then a hand edit resolves it too.
        w.take(Merge3::Pick::Markers);
        QCOMPARE(w.unresolvedCount(), 1);
        auto *edit = w.findChild<QPlainTextEdit *>();
        QTextCursor c(edit->document());
        c.setPosition(int(w.resultText().indexOf(QStringLiteral("X"))));
        c.insertText(QStringLiteral("hand "));
        QCOMPARE(w.unresolvedCount(), 0);
        // CRLF files keep their line endings.
        MergeWindow::Input crlf = in;
        crlf.base = "a\r\nb\r\n";
        crlf.ours = "a\r\nB\r\n";
        crlf.theirs = "a\r\nb\r\n";
        MergeWindow w2(crlf);
        QCOMPARE(w2.conflictCount(), 0);
        QVERIFY(w2.save());
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("a\r\nB\r\n"));
        w.findChild<QPlainTextEdit *>()->document()->setModified(false);
    }

    void resolvesARealConflict()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("-b"), QStringLiteral("main")});
        write(d + QStringLiteral("/f.txt"), "one\ntwo\nthree\n");
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("base")});
        git(d, {QStringLiteral("checkout"), QStringLiteral("-qb"), QStringLiteral("other")});
        write(d + QStringLiteral("/f.txt"), "one\nTWO (theirs)\nthree\n");
        git(d, {QStringLiteral("commit"), QStringLiteral("-qam"), QStringLiteral("theirs")});
        git(d, {QStringLiteral("checkout"), QStringLiteral("-q"), QStringLiteral("main")});
        write(d + QStringLiteral("/f.txt"), "one\nTWO (ours)\nthree\n");
        git(d, {QStringLiteral("commit"), QStringLiteral("-qam"), QStringLiteral("ours")});
        QProcess merge;
        merge.setWorkingDirectory(d);
        merge.start(QStringLiteral("git"), {QStringLiteral("merge"), QStringLiteral("-q"), QStringLiteral("other")});
        QVERIFY(merge.waitForFinished());
        QVERIFY(merge.exitCode() != 0); // conflict
        QString error;
        auto repo = GitRepo::open(d, &error);
        MainWindow w(std::move(*repo), Comparison());
        w.show();
        QAction *resolve = action(w, QStringLiteral("Resolve conflict\u2026"));
        QVERIFY(resolve->isEnabled());
        resolve->trigger();
        MergeWindow *mw = nullptr;
        for (QWidget *top : QApplication::topLevelWidgets())
            if (auto *m = qobject_cast<MergeWindow *>(top); m && m->isVisible())
                mw = m;
        QVERIFY(mw);
        QCOMPARE(mw->conflictCount(), 1);
        mw->take(Merge3::Pick::OursThenTheirs);
        QVERIFY(mw->save());
        mw->close();
        QFile f(d + QStringLiteral("/f.txt"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("one\nTWO (ours)\nTWO (theirs)\nthree\n"));
        QProcess p;
        p.setWorkingDirectory(d);
        p.start(QStringLiteral("git"), {QStringLiteral("diff"), QStringLiteral("--name-only"), QStringLiteral("--diff-filter=U")});
        QVERIFY(p.waitForFinished());
        QVERIFY(p.readAllStandardOutput().isEmpty()); // marked resolved
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
    void emptyWindowOutsideARepository()
    {
        // Plain `yadiff` outside a repository opens a window with nothing to compare.
        QTemporaryDir tmp;
        Comparison cmp;
        cmp.empty = true;
        MainWindow w(GitRepo::forPaths(tmp.path()), cmp);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QCOMPARE(w.findChild<FileList *>()->visibleCount(), 0);
        QVERIFY(!w.findChild<DiffPane *>()->hasDiff());
        QCOMPARE(w.windowTitle(), QStringLiteral("yadiff"));
        bool hint = false;
        for (QLabel *l : w.findChildren<QLabel *>())
            hint = hint || (l->isVisible() && l->text().contains(QStringLiteral("New comparison")));
        QVERIFY(hint);
        action(w, QStringLiteral("Refresh list"))->trigger(); // still nothing, and no git error
        QCOMPARE(w.findChild<FileList *>()->visibleCount(), 0);

        // File > New comparison then shows the new comparison in this same window.
        QDir(tmp.path()).mkpath(QStringLiteral("L"));
        QDir(tmp.path()).mkpath(QStringLiteral("R"));
        write(tmp.filePath(QStringLiteral("L/a.txt")), "one\n");
        write(tmp.filePath(QStringLiteral("R/a.txt")), "two\n");
        bool filled = false;
        QTimer poll;
        poll.setInterval(20);
        connect(&poll, &QTimer::timeout, &w, [&] {
            auto *dlg = w.findChild<NewComparisonDialog *>();
            if (!dlg || !dlg->isVisible())
                return;
            poll.stop();
            dlg->findChild<QLineEdit *>(QStringLiteral("leftDir"))->setText(tmp.filePath(QStringLiteral("L")));
            dlg->findChild<QLineEdit *>(QStringLiteral("rightDir"))->setText(tmp.filePath(QStringLiteral("R")));
            for (QPushButton *b : dlg->findChildren<QPushButton *>())
                if (b->text() == QStringLiteral("Compare"))
                    b->click();
            filled = true;
        });
        poll.start();
        QTimer guard;
        guard.setSingleShot(true);
        connect(&guard, &QTimer::timeout, &w, [] {
            if (QWidget *m = QApplication::activeModalWidget())
                m->close();
        });
        guard.start(10000);
        action(w, QStringLiteral("New comparison…"))->trigger();
        QVERIFY(filled);
        QVERIFY(w.isVisible());
        QCOMPARE(w.findChild<FileList *>()->visibleCount(), 1);
        QVERIFY(w.findChild<DiffPane *>()->hasDiff());
        QVERIFY2(w.windowTitle().contains(QStringLiteral("compare")), qPrintable(w.windowTitle()));
    }
    void newComparisonArgumentsResolve()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        git(d, {QStringLiteral("init"), QStringLiteral("-q")});
        write(d + QStringLiteral("/f.txt"), "a\n");
        git(d, {QStringLiteral("add"), QStringLiteral("-A")});
        git(d, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("one")});
        QString error;
        // A repository: its working tree.
        auto req = ComparisonArgs::fromArguments({QStringLiteral("-C"), d}, QDir::tempPath(), &error);
        QVERIFY(req);
        auto r = ComparisonArgs::resolve(*req, &error);
        QVERIFY2(r, qPrintable(error));
        QCOMPARE(r->cmp.mode, Comparison::Mode::WorkingTree);
        QVERIFY(!r->cmp.empty);
        // Revisions in it.
        req = ComparisonArgs::fromArguments({QStringLiteral("-C"), d, QStringLiteral("HEAD")}, QDir::tempPath(), &error);
        r = ComparisonArgs::resolve(*req, &error);
        QVERIFY2(r, qPrintable(error));
        QCOMPARE(r->cmp.mode, Comparison::Mode::Revisions);
        QVERIFY(r->cmp.rev2.isEmpty()); // against the working tree
        // Mistakes come back as errors, not exits.
        req = ComparisonArgs::fromArguments({QStringLiteral("-C"), d, QStringLiteral("nosuchrev")}, QDir::tempPath(), &error);
        QVERIFY(!ComparisonArgs::resolve(*req, &error));
        QVERIFY(error.contains(QStringLiteral("nosuchrev")));
        QVERIFY(!ComparisonArgs::fromArguments({QStringLiteral("--bogus")}, QDir::tempPath(), &error));

        // toArguments() gives fromArguments() back the same request.
        ComparisonArgs::Request q;
        q.cwd = d;
        q.positional = {QStringLiteral("HEAD"), QStringLiteral("f.txt")};
        q.forcedPaths = 1;
        q.title = QStringLiteral("T");
        q.algorithm = QStringLiteral("heckel");
        const auto back = ComparisonArgs::fromArguments(ComparisonArgs::toArguments(q), QDir::tempPath(), &error);
        QVERIFY(back);
        QCOMPARE(back->positional, q.positional);
        QCOMPARE(back->forcedPaths, 1);
        QCOMPARE(back->title, QStringLiteral("T"));
        QCOMPARE(back->algorithm, QStringLiteral("heckel"));
        QCOMPARE(back->cwd, d);
    }

    void dropsRecentsSessionsAndPatches()
    {
        QTemporaryDir tmp;
        const QString d = tmp.path();
        write(d + QStringLiteral("/a.txt"), "one\ntwo\n");
        write(d + QStringLiteral("/b.txt"), "one\nTWO\n");
        write(d + QStringLiteral("/fix.patch"), "--- a/a.txt\n+++ b/a.txt\n@@ -1,2 +1,2 @@\n one\n-two\n+2\n");
        Settings::clearRecentComparisons();
        Comparison empty;
        empty.empty = true;
        auto open = [&](const QStringList &dropped) {
            auto w = std::make_unique<MainWindow>(GitRepo::forPaths(d), empty);
            w->show();
            w->openDropped(dropped);
            return w;
        };
        // Two files dropped on an empty window are compared right there.
        auto w = open({d + QStringLiteral("/a.txt"), d + QStringLiteral("/b.txt")});
        auto *pane = w->findChild<DiffPane *>();
        QVERIFY(pane->hasDiff());
        QCOMPARE(pane->changeCount(), 1);
        QVERIFY(!Settings::recentComparisons().isEmpty());
        QVERIFY(Settings::recentComparisons().first().arguments.contains(QStringLiteral("--no-index")));

        // One at a time works too.
        w = open({d + QStringLiteral("/a.txt")});
        QVERIFY(!w->findChild<DiffPane *>()->hasDiff());
        w->openDropped({d + QStringLiteral("/b.txt")});
        QCOMPARE(w->findChild<DiffPane *>()->changeCount(), 1);

        // A patch: it fits a.txt, so the whole file shows with the change.
        w = open({d + QStringLiteral("/fix.patch")});
        pane = w->findChild<DiffPane *>();
        QVERIFY(pane->hasDiff());
        QCOMPARE(pane->changeCount(), 1);
        QVERIFY(w->windowTitle().contains(QStringLiteral("fix.patch")));
        QVERIFY(action(*w, QStringLiteral("Apply patch\u2026"))->isEnabled());

        // A session reopens its comparison with its options and view.
        Session s;
        s.arguments = {QStringLiteral("-C"), d, QStringLiteral("--no-index"), d + QStringLiteral("/a.txt"),
                       d + QStringLiteral("/b.txt")};
        s.title = QStringLiteral("Saved one");
        s.sideBySide = 0;
        s.hasOptions = true;
        s.options.ignoreCase = true;
        QString error;
        QVERIFY(s.write(d + QStringLiteral("/s.yadiff"), &error));
        w = open({d + QStringLiteral("/s.yadiff")});
        pane = w->findChild<DiffPane *>();
        QCOMPARE(w->windowTitle(), QStringLiteral("Saved one"));
        QCOMPARE(pane->mode(), DiffPane::Mode::Inline);
        QCOMPARE(pane->changeCount(), 0); // two vs TWO: equal when ignoring case
        QVERIFY(action(*w, QStringLiteral("Ignore case"))->isChecked());
        Settings::setSideBySide(true);
        Settings::clearRecentComparisons();
    }
};

QTEST_MAIN(TestNavigation)
#include "test_navigation.moc"
