#pragma once

#include "Merge3.h"

#include <QMainWindow>
#include <QTextCursor>

class CodeView;
class QLabel;
class QPlainTextEdit;

// Resolving a merge conflict: ours, the base and theirs side by side above an editable
// result. Changes from one side are taken automatically; each conflict starts as
// git-style markers and is resolved by taking a side (or both, or the base), or by
// editing the result.
class MergeWindow : public QMainWindow
{
    Q_OBJECT
public:
    struct Input {
        QByteArray base, ours, theirs;
        QString baseLabel = QStringLiteral("Base"), oursLabel = QStringLiteral("Ours"),
                theirsLabel = QStringLiteral("Theirs");
        QString outputPath; // Save writes the result here
        QString fileName;   // for syntax highlighting and the title
    };

    explicit MergeWindow(const Input &input, QWidget *parent = nullptr);

    int conflictCount() const;
    int unresolvedCount() const;
    // The result was saved with nothing unresolved (git mergetool's exit status).
    bool resolvedAndSaved() const { return m_savedResolved && !isModified(); }
    QString resultText() const;
    bool isModified() const;

    // The current conflict (an index into the regions), and taking a side for it.
    int currentRegion() const { return m_current; }
    void goToConflict(int delta); // +1 next, -1 previous
    void take(Merge3::Pick pick);
    void takeForAll(Merge3::Pick pick); // every conflict not resolved yet
    bool save();

signals:
    void saved(bool resolved);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void buildViews();
    void buildResult();
    void replaceRegion(int index, const QStringList &lines);
    int regionAt(int position) const;
    int regionStart(int index) const;
    int regionEnd(int index) const;
    void setCurrent(int index, bool scroll);
    void updateMarks();
    void onEdited(int position, int removed, int added);

    Input m_in;
    QStringList m_base, m_ours, m_theirs;
    QByteArray m_eol = "\n";
    bool m_finalNewline = true;
    QVector<Merge3::Region> m_regions;
    QVector<Merge3::Pick> m_picks;
    QVector<bool> m_edited;          // changed by hand in the result
    QVector<QTextCursor> m_starts;   // where each region starts in the result
    QVector<int> m_rowOfRegion;      // first row of each region in the three views
    int m_current = -1;
    bool m_applying = false;
    bool m_savedResolved = false;

    CodeView *m_oursView = nullptr, *m_baseView = nullptr, *m_theirsView = nullptr;
    QPlainTextEdit *m_result = nullptr;
    QObject *m_highlighter = nullptr;
    QLabel *m_status = nullptr;
};
