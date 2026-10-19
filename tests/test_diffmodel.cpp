#include "Comments.h"
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

    void stripsComments()
    {
        const Comments::Syntax c = Comments::forFile(QStringLiteral("src/x.cpp"));
        QVERIFY(c.isValid());
        const QStringList in{QStringLiteral("int a = 1; // one"), QStringLiteral("s = \"http://x\"; /* start"),
                             QStringLiteral("still comment"), QStringLiteral("end */ b = '/'; // c"),
                             QStringLiteral("q = \"esc \\\" // not\"; x")};
        const QStringList out = Comments::strip(in, c);
        QCOMPARE(out[0], QStringLiteral("int a = 1; "));
        QCOMPARE(out[1], QStringLiteral("s = \"http://x\"; "));
        QCOMPARE(out[2], QString());
        QCOMPARE(out[3], QStringLiteral(" b = '/'; "));
        QCOMPARE(out[4], in[4]); // the // is inside a string with an escaped quote
        QCOMPARE(Comments::strip({QStringLiteral("x = 1  # set x")}, Comments::forFile(QStringLiteral("a.py")))[0],
                 QStringLiteral("x = 1  "));
        QCOMPARE(Comments::strip({QStringLiteral("<a/><!-- c -->")}, Comments::forFile(QStringLiteral("a.xml")))[0],
                 QStringLiteral("<a/>"));
        QCOMPARE(Comments::strip({QStringLiteral("set(X 1) # c")}, Comments::forFile(QStringLiteral("CMakeLists.txt")))[0],
                 QStringLiteral("set(X 1) "));
        QVERIFY(!Comments::forFile(QStringLiteral("notes.unknownext")).isValid());
    }

    void unimportantLinesAreNotChunks()
    {
        FileDiff d;
        auto add = [&](LineKind k, bool unimportant) {
            DiffLine l;
            l.kind = k;
            l.text = QStringLiteral("x");
            l.unimportant = unimportant;
            d.lines.push_back(l);
        };
        add(LineKind::Added, true);
        add(LineKind::Equal, false);
        add(LineKind::Removed, true);
        add(LineKind::Added, false);
        DiffModel::computeChunks(d);
        QCOMPARE(d.chunks.size(), 1); // the first run is only unimportant
        QCOMPARE(d.chunks[0].first, 2);
        QCOMPARE(d.chunks[0].last, 3);
    }

    void alignsAroundAnchors()
    {
        const QStringList o{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("d")};
        const QStringList n{QStringLiteral("x"), QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c2"),
                            QStringLiteral("d")};
        auto lcs = [](const QStringList &a, const QStringList &b) {
            const auto [ia, ib] = LineDiff::lineIds(a, b);
            return LineDiff::myers(ia, ib);
        };
        // Force old "a" (0) onto new "x" (0), and c (2) onto c2 (3); a crossing anchor is dropped.
        LineDiff::Alignment kept;
        const LineDiff::Alignment al = LineDiff::alignAround(o, n, {{2, 3}, {0, 0}, {1, 0}}, lcs, &kept);
        QCOMPARE(kept, (LineDiff::Alignment{{0, 0}, {2, 3}}));
        // a~x and c~c2 differ, so only b and d match: b is old 1, which the lcs puts at new 1 or 2.
        QCOMPARE(al.size(), 2);
        QCOMPARE(al.last(), (LineDiff::Match{3, 4}));
        const FileDiff d = LineDiff::build(o, n, al, false, kept);
        int anchored = 0;
        for (const DiffLine &l : d.lines)
            anchored += l.anchored;
        QCOMPARE(anchored, 4);
        // The forced pairs point at each other, so side by side puts them on one row.
        for (int i = 0; i < d.lines.size(); ++i)
            if (d.lines[i].anchored) {
                QVERIFY(d.lines[i].pair >= 0);
                QCOMPARE(d.lines[d.lines[i].pair].pair, i);
            }

        // Partial changes keep line endings and add a newline where a line stops being last.
        const QList<QByteArray> from = LineDiff::splitKeepingEnds("a\r\nb\r\nc");
        QCOMPARE(from.size(), 3);
        const QList<QByteArray> to = LineDiff::splitKeepingEnds("a\r\nB\r\nc\r\nd\r\n");
        QCOMPARE(LineDiff::applyPart(from, to, {}, {4}).join(), QByteArray("a\r\nb\r\nc\r\nd\r\n"));
        QCOMPARE(LineDiff::applyPart(LineDiff::splitKeepingEnds("a\nb\nc"), LineDiff::splitKeepingEnds("a\nB\nc\n"), {2}, {2})
                     .join(),
                 QByteArray("a\nB\nc")); // a missing final newline alone doesn't make c a change
        QCOMPARE(LineDiff::applyPart(from, to, {2}, {}).join(), QByteArray("a\r\nc"));

        // An equal anchor just joins the alignment.
        const LineDiff::Alignment al2 = LineDiff::alignAround(o, o, {{1, 1}}, lcs, &kept);
        QCOMPARE(al2.size(), 4);
    }
};

QTEST_APPLESS_MAIN(TestDiffModel)
#include "test_diffmodel.moc"
