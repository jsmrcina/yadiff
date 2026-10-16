#include "DiffModel.h"

#include <QTest>

class TestDiffModel : public QObject
{
    Q_OBJECT
private slots:
    void parsesLineNumbersAndKinds()
    {
        const QString diff = QStringLiteral(
            "diff --git a/f b/f\nindex 1..2 100644\n--- a/f\n+++ b/f\n"
            "@@ -1,3 +1,3 @@\n a\n-b\n+B\n c\n");
        const FileDiff d = DiffModel::parseUnified(diff);
        QCOMPARE(d.lines.size(), 4);
        QCOMPARE(d.lines[0].kind, LineKind::Equal);
        QCOMPARE(d.lines[1].kind, LineKind::Removed);
        QCOMPARE(d.lines[1].oldNo, 2);
        QCOMPARE(d.lines[2].kind, LineKind::Added);
        QCOMPARE(d.lines[2].newNo, 2);
        QCOMPARE(d.lines[3].oldNo, 3);
        QCOMPARE(d.lines[3].newNo, 3);
        QCOMPARE(d.chunks.size(), 1);
        QCOMPARE(d.chunks[0].first, 1);
        QCOMPARE(d.chunks[0].last, 2);
    }

    void ignoresNoNewlineMarkerAndCR()
    {
        const QString diff = QStringLiteral("@@ -1 +1 @@\n-old\r\n\\ No newline at end of file\n+new\n");
        const FileDiff d = DiffModel::parseUnified(diff);
        QCOMPARE(d.lines.size(), 2);
        QCOMPARE(d.lines[0].text, QStringLiteral("old"));
    }

    void detectsBinary()
    {
        const FileDiff d = DiffModel::parseUnified(QStringLiteral("diff --git a/x b/x\nBinary files a/x and b/x differ\n"));
        QVERIFY(d.binary);
        QVERIFY(d.lines.isEmpty());
    }

    void detectsMovedBlock()
    {
        // Block "void moved() ... }" moves from the top to the bottom.
        const QString diff = QStringLiteral(
            "@@ -1,8 +1,8 @@\n"
            "-void moved() {\n"
            "-    doSomethingImportant();\n"
            "-}\n"
            " int keep1;\n"
            " int keep2;\n"
            " int keep3;\n"
            " int keep4;\n"
            " int keep5;\n"
            "+void moved() {\n"
            "+    doSomethingImportant();\n"
            "+}\n");
        const FileDiff d = DiffModel::parseUnified(diff);
        QCOMPARE(d.moves.size(), 1);
        QCOMPARE(d.moves[0].count, 3);
        QCOMPARE(d.lines[0].moveId, 0);
        QCOMPARE(d.lines[0].partner, 8);
        QCOMPARE(d.lines[8].partner, 0);
        QVERIFY(d.lines[0].highlights.isEmpty());
    }

    void reindentedMoveStillCounts()
    {
        const QString diff = QStringLiteral(
            "@@ -1,4 +1,4 @@\n"
            "-call_a_fairly_long_function(x, y);\n"
            " separator line\n"
            " another separator\n"
            "+    call_a_fairly_long_function(x, y);\n");
        const FileDiff d = DiffModel::parseUnified(diff);
        QCOMPARE(d.moves.size(), 1);
    }

    void shortTrivialLinesAreNotMoves()
    {
        const QString diff = QStringLiteral("@@ -1,3 +1,3 @@\n-}\n mid\n+}\n");
        const FileDiff d = DiffModel::parseUnified(diff);
        QCOMPARE(d.moves.size(), 0);
        QCOMPARE(d.chunks.size(), 2);
    }

    void sameChunkReindentIsNotAMove()
    {
        const QString diff = QStringLiteral(
            "@@ -1,1 +1,1 @@\n-call_a_fairly_long_function(x, y);\n+    call_a_fairly_long_function(x, y);\n");
        const FileDiff d = DiffModel::parseUnified(diff);
        QCOMPARE(d.moves.size(), 0);
    }

    void intraLineHighlights()
    {
        const QString diff = QStringLiteral("@@ -1 +1 @@\n-int value = compute(alpha);\n+int value = compute(beta);\n");
        const FileDiff d = DiffModel::parseUnified(diff);
        QCOMPARE(d.lines[0].highlights.size(), 1);
        const Span a = d.lines[0].highlights[0];
        const Span b = d.lines[1].highlights[0];
        QCOMPARE(d.lines[0].text.mid(a.start, a.length), QStringLiteral("alpha"));
        QCOMPARE(d.lines[1].text.mid(b.start, b.length), QStringLiteral("beta"));
    }

    void pairsSimilarLinesNotPositions()
    {
        // The edited line is the second added line; it must pair with it, not the first.
        const QString diff = QStringLiteral(
            "@@ -1,2 +1,3 @@\n"
            "-    int y = 4;\n"
            "+#include <stdlib.h>\n"
            "+    int y = 5;\n"
            " end\n");
        const FileDiff d = DiffModel::parseUnified(diff);
        QCOMPARE(d.lines[0].pair, 2);
        QCOMPARE(d.lines[1].pair, -1);
        QCOMPARE(d.lines[2].pair, 0);
        QCOMPARE(d.lines[2].highlights.size(), 1);
    }

    void allAddedNumbersLines()
    {
        const FileDiff d = DiffModel::allAdded(QStringLiteral("one\ntwo\n"));
        QCOMPARE(d.lines.size(), 2);
        QCOMPARE(d.lines[1].newNo, 2);
        QCOMPARE(d.chunks.size(), 1);
    }
};

QTEST_APPLESS_MAIN(TestDiffModel)
#include "test_diffmodel.moc"
