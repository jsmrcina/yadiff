#include "DiffModel.h"
#include "LineDiff.h"

#include <QRandomGenerator>

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
    void myersFindsALongestCommonSubsequence()
    {
        auto lcsLength = [](const QVector<int> &a, const QVector<int> &b) {
            QVector<QVector<int>> t(a.size() + 1, QVector<int>(b.size() + 1, 0));
            for (int i = int(a.size()) - 1; i >= 0; --i)
                for (int j = int(b.size()) - 1; j >= 0; --j)
                    t[i][j] = a[i] == b[j] ? t[i + 1][j + 1] + 1 : std::max(t[i + 1][j], t[i][j + 1]);
            return t[0][0];
        };
        QRandomGenerator rng(42);
        for (int round = 0; round < 500; ++round) {
            QVector<int> a, b;
            const int alphabet = 1 + int(rng.bounded(6));
            for (int i = int(rng.bounded(25)); i > 0; --i)
                a << int(rng.bounded(alphabet));
            for (int i = int(rng.bounded(25)); i > 0; --i)
                b << int(rng.bounded(alphabet));
            for (const auto &align : {LineDiff::myers(a, b), LineDiff::heckel(a, b)}) {
                int pa = -1, pb = -1;
                for (const LineDiff::Match &m : align) {
                    QVERIFY(m.first > pa && m.second > pb); // strictly increasing
                    QCOMPARE(a[m.first], b[m.second]);
                    pa = m.first;
                    pb = m.second;
                }
            }
            QCOMPARE(int(LineDiff::myers(a, b).size()), lcsLength(a, b));
        }
    }

    void heckelAnchorsOnUniqueLines()
    {
        // a b c X d e  ->  d e X a b c : the unique lines anchor; crossing blocks can't
        // all stay, so the longest in-order run does and the rest are removed and added.
        const QStringList oldLines{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("X"),
                                   QStringLiteral("d"), QStringLiteral("e")};
        const QStringList newLines{QStringLiteral("d"), QStringLiteral("e"), QStringLiteral("X"), QStringLiteral("a"),
                                   QStringLiteral("b"), QStringLiteral("c")};
        const auto [a, b] = LineDiff::lineIds(oldLines, newLines);
        const LineDiff::Alignment h = LineDiff::heckel(a, b);
        QCOMPARE(h.size(), 3);
        QCOMPARE(h.first(), LineDiff::Match(0, 3)); // a b c kept
        const FileDiff d = LineDiff::build(oldLines, newLines, h, true);
        QCOMPARE(d.lines.size(), 9);

        // Repeated lines next to an anchor match through it; isolated repeats don't.
        const QStringList o2{QStringLiteral("}"), QStringLiteral("unique1"), QStringLiteral("}"), QStringLiteral("}")};
        const QStringList n2{QStringLiteral("}"), QStringLiteral("unique1"), QStringLiteral("}"), QStringLiteral("new"),
                             QStringLiteral("}")};
        const auto [a2, b2] = LineDiff::lineIds(o2, n2);
        QCOMPARE(LineDiff::heckel(a2, b2).size(), 4);
    }

    void fromUnifiedZeroAndBuild()
    {
        const QString diff = QStringLiteral("@@ -2 +2 @@\n-b\n+B\n@@ -3,0 +4,2 @@\n+x\n+y\n@@ -5,1 +7,0 @@\n-e\n");
        const LineDiff::Alignment al = LineDiff::fromUnifiedZero(diff, 5, 6);
        QCOMPARE(al, (LineDiff::Alignment{{0, 0}, {2, 2}, {3, 5}}));
        const QStringList o{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("C"), QStringLiteral("d"), QStringLiteral("e")};
        const QStringList n{QStringLiteral("a"), QStringLiteral("B"), QStringLiteral("c"), QStringLiteral("x"),
                            QStringLiteral("y"), QStringLiteral("d")};
        const FileDiff d = LineDiff::build(o, n, al, false);
        QCOMPARE(d.chunks.size(), 3);
        QCOMPARE(d.lines[3].kind, LineKind::Equal); // C ~ c
        QCOMPARE(d.lines[3].text, QStringLiteral("c"));
        QCOMPARE(d.lines[3].oldText, QStringLiteral("C"));
        QVERIFY(d.lines[0].oldText.isNull());
        QCOMPARE(LineDiff::splitLines(QStringLiteral("a\nb\n")).size(), 2);
        QCOMPARE(LineDiff::splitLines(QStringLiteral("a\nb")).size(), 2);
    }
};

QTEST_APPLESS_MAIN(TestDiffModel)
#include "test_diffmodel.moc"
