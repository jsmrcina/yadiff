#include "Comments.h"
#include "DiffModel.h"
#include "LineDiff.h"
#include "HexDiff.h"
#include "PatchFile.h"
#include "StructuralDiff.h"
#include "TableDiff.h"

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

    void readsAndAppliesPatches()
    {
        const QString patch = QStringLiteral(
            "From abc Mon Sep 17 00:00:00 2001\nSubject: [PATCH] x\n\n---\n f | 2 +-\n\n"
            "diff --git a/f.txt b/f.txt\nindex 1..2 100644\n--- a/f.txt\n+++ b/f.txt\n"
            "@@ -2,3 +2,3 @@\n b\n-c\n+C\n d\n@@ -9,2 +9,3 @@\n i\n j\n+k\n"
            "diff --git a/new.txt b/new.txt\nnew file mode 100644\n--- /dev/null\n+++ b/new.txt\n@@ -0,0 +1 @@\n+hello\n"
            "diff --git a/old name b/new name\nsimilarity index 100%\nrename from old name\nrename to new name\n"
            "-- \n2.40.0\n");
        const QVector<PatchFile::File> files = PatchFile::parse(patch);
        QCOMPARE(files.size(), 3);
        QCOMPARE(files[0].path(), QStringLiteral("f.txt"));
        QCOMPARE(files[0].status, QChar(u'M'));
        QCOMPARE(files[0].hunks.size(), 2);
        QCOMPARE(files[0].added, 2);
        QCOMPARE(files[0].removed, 1);
        QCOMPARE(files[1].status, QChar(u'A'));
        QVERIFY(files[1].oldPath.isEmpty());
        QCOMPARE(files[2].status, QChar(u'R'));
        QCOMPARE(files[2].oldPath, QStringLiteral("old name"));
        QCOMPARE(files[2].newPath, QStringLiteral("new name"));

        // Applies where it says, and two lines further down when the file grew at the top.
        QStringList base;
        for (char c = 'a'; c <= 'j'; ++c)
            base << QString(QChar(c));
        auto applied = PatchFile::apply(base, files[0]);
        QVERIFY(applied);
        QCOMPARE(applied->value(2), QStringLiteral("C"));
        QCOMPARE(applied->last(), QStringLiteral("k"));
        applied = PatchFile::apply(QStringList{QStringLiteral("0"), QStringLiteral("00")} + base, files[0]);
        QVERIFY(applied);
        QCOMPARE(applied->value(4), QStringLiteral("C"));
        QVERIFY(!PatchFile::apply({QStringLiteral("x")}, files[0]));
        QCOMPARE(*PatchFile::apply({}, files[1]), QStringList{QStringLiteral("hello")});

        // A plain diff -u, and the gaps between hunks when shown as they are.
        const auto plain = PatchFile::parse(QStringLiteral("--- f.txt\t2024-01-01\n+++ f.txt\t2024-01-02\n@@ -1 +1 @@\n-a\n+b\n"));
        QCOMPARE(plain.size(), 1);
        QCOMPARE(plain[0].path(), QStringLiteral("f.txt"));
        const FileDiff d = DiffModel::parseUnified(files[0].text);
        QVERIFY(d.lines[0].gap); // line 1 isn't in the patch
        int gaps = 0;
        for (const DiffLine &l : d.lines)
            gaps += l.gap;
        QCOMPARE(gaps, 2);
        QCOMPARE(d.chunks.size(), 2);
    }

    void hexDumps()
    {
        QCOMPARE(HexDiff::row("Hello, world!\n\x01\xff", 0, 16),
                 QStringLiteral("00000000  48 65 6c 6c 6f 2c 20 77  6f 72 6c 64 21 0a 01 ff  |Hello, world!...|"));
        QByteArray a(4096, '\0');
        for (int i = 0; i < a.size(); ++i)
            a[i] = char((i * 7919) % 251);
        // Same size, one byte changed: one row pair, two highlights (hex and ASCII).
        QByteArray b = a;
        b[100] = char(b[100] ^ 0x55);
        FileDiff d = HexDiff::build(a, b);
        QVERIFY(d.hexDump);
        QCOMPARE(d.chunks.size(), 1);
        int removed = 0;
        for (const DiffLine &l : d.lines)
            if (l.kind == LineKind::Removed) {
                ++removed;
                QCOMPARE(l.highlights.size(), 2);
                QVERIFY(l.pair >= 0);
            }
        QCOMPARE(removed, 1);
        // Three bytes inserted: the rest lines up again, with each side's own offsets.
        QByteArray c = a;
        c.insert(1000, "XYZ");
        d = HexDiff::build(a, c);
        QCOMPARE(d.chunks.size(), 1);
        const DiffLine &last = d.lines.last();
        QCOMPARE(last.kind, LineKind::Equal);
        QCOMPARE(last.text.left(8).toInt(nullptr, 16) - last.oldText.left(8).toInt(nullptr, 16), 3);
        QCOMPARE(last.text.mid(8), last.oldText.mid(8)); // same bytes
        QVERIFY(!last.unimportant);
        // Added file.
        d = HexDiff::build({}, QByteArray("abc"));
        QCOMPARE(d.lines.size(), 1);
        QCOMPARE(d.lines[0].kind, LineKind::Added);
    }

    void tables()
    {
        const TableDiff::Table t = TableDiff::parse(QStringLiteral("id,name\r\n1,\"Smith, J\"\n2,\"say \"\"hi\"\"\nthere\"\n\n"), u',');
        QCOMPARE(t.size(), 3);
        QCOMPARE(t[1][1], QStringLiteral("Smith, J"));
        QCOMPARE(t[2][1], QStringLiteral("say \"hi\"\nthere"));
        QCOMPARE(TableDiff::delimiterFor(QStringLiteral("a.csv"), QStringLiteral("a;b;c")), QChar(u';'));
        QCOMPARE(TableDiff::delimiterFor(QStringLiteral("a.tsv"), QString()), QChar(u'\t'));

        // By key: row 2 changed its price, 3 was removed, 4 added, and the columns moved.
        const QStringList oh{QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("price")};
        const TableDiff::Table od{{QStringLiteral("1"), QStringLiteral("apple"), QStringLiteral("5")},
                                  {QStringLiteral("2"), QStringLiteral("pear"), QStringLiteral("6")},
                                  {QStringLiteral("3"), QStringLiteral("plum"), QStringLiteral("7")}};
        const QStringList nh{QStringLiteral("name"), QStringLiteral("id"), QStringLiteral("price")};
        const TableDiff::Table nd{{QStringLiteral("pear"), QStringLiteral("2"), QStringLiteral("9")},
                                  {QStringLiteral("apple"), QStringLiteral("1"), QStringLiteral("5")},
                                  {QStringLiteral("fig"), QStringLiteral("4"), QStringLiteral("1")}};
        TableDiff::Result r = TableDiff::compare(oh, od, nh, nd, 1); // key: "id" (second new column)
        QCOMPARE(r.columns, nh);
        QCOMPARE(r.changed, 1);
        QCOMPARE(r.added, 1);
        QCOMPARE(r.removed, 1);
        bool sawChange = false;
        for (const TableDiff::Row &row : r.rows)
            if (row.kind == TableDiff::Row::Kind::Changed) {
                QCOMPARE(row.changedColumns, QVector<int>{2});
                QCOMPARE(r.oldCell(od, row.oldRow, 2), QStringLiteral("6"));
                QCOMPARE(r.newCell(nd, row.newRow, 2), QStringLiteral("9"));
                sawChange = true;
            }
        QVERIFY(sawChange);
        // By order, the moved rows don't match up the same way.
        r = TableDiff::compare(oh, od, nh, nd, -1);
        QVERIFY(r.added + r.removed + r.changed > 0);
    }

    void structuralDiff()
    {
        const Comments::Syntax cpp = Comments::forFile(QStringLiteral("a.cpp"));
        const auto toks = StructuralDiff::tokenize({QStringLiteral("if (a->b == \"x y\") // note it")}, cpp, false);
        QStringList keys;
        for (const auto &t : toks)
            keys << t.key;
        QCOMPARE(keys, (QStringList{QStringLiteral("if"), QStringLiteral("("), QStringLiteral("a"), QStringLiteral("->"),
                                    QStringLiteral("b"), QStringLiteral("=="), QStringLiteral("\"x y\""), QStringLiteral(")"),
                                    QStringLiteral("/"), QStringLiteral("/"), QStringLiteral("note"), QStringLiteral("it")}));
        QVERIFY(toks.last().comment);

        // A call re-wrapped over three lines, re-indented code, and one real change.
        const QStringList o{QStringLiteral("int f() {"), QStringLiteral("  call(alpha, beta, gamma);"),
                            QStringLiteral("  return x+1;"), QStringLiteral("}")};
        const QStringList n{QStringLiteral("int f() {"), QStringLiteral("    call(alpha,"), QStringLiteral("         beta,"),
                            QStringLiteral("         gamma);"), QStringLiteral("    return x + 2;"), QStringLiteral("}")};
        StructuralDiff::Options opt;
        opt.syntax = cpp;
        const FileDiff d = StructuralDiff::build(o, n, opt);
        QCOMPARE(d.chunks.size(), 1);
        int important = 0;
        for (const DiffLine &l : d.lines) {
            if (l.kind == LineKind::Equal || l.unimportant)
                continue;
            ++important;
            // Only "1" and "2" are highlighted.
            QCOMPARE(l.highlights.size(), 1);
            QCOMPARE(l.highlights[0].length, 1);
            QVERIFY(l.pair >= 0);
        }
        QCOMPARE(important, 2); // return x+1 / return x + 2
        int unimportant = 0;
        for (const DiffLine &l : d.lines)
            unimportant += l.unimportant && l.kind != LineKind::Equal;
        QCOMPARE(unimportant, 4); // the call on one line, and on three

        // Only whitespace: nothing that counts.
        const FileDiff same = StructuralDiff::build({QStringLiteral("a=b;")}, {QStringLiteral("a = b ;")}, opt);
        QVERIFY(same.chunks.isEmpty());
        QVERIFY(same.lines[0].unimportant);
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
