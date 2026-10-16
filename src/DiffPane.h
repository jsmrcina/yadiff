#pragma once

#include "CodeView.h"
#include "DiffModel.h"

#include <QWidget>

class QLabel;
class QStackedWidget;

// Right-hand side of the window: shows one file's diff inline or side by side
// and tracks the "current change" for navigation.
class DiffPane : public QWidget
{
    Q_OBJECT
public:
    enum class Mode { Inline, SideBySide };

    explicit DiffPane(QWidget *parent = nullptr);

    void setDiff(const FileDiff &diff, const QString &title, const QString &leftLabel, const QString &rightLabel);
    void showMessage(const QString &title, const QString &message);

    void setMode(Mode mode);
    Mode mode() const { return m_mode; }
    void applySettings();

    int changeCount() const { return int(m_diff.chunks.size()); }
    int currentChange() const { return m_current; }

    bool nextChange();
    bool prevChange();
    void goToFirstChange();
    void goToLastChange();

signals:
    void currentChangeChanged(int index, int count);

private:
    void rebuild();
    void showChange(int index);
    void jumpToMovePartner(CodeView *source, int row);
    ViewPalette makePalette() const;
    Row makeRow(int lineIndex) const;

    FileDiff m_diff;
    bool m_hasDiff = false;
    Mode m_mode = Mode::SideBySide;
    int m_current = -1;

    QLabel *m_title = nullptr;
    QStackedWidget *m_stack = nullptr;
    QWidget *m_sidePage = nullptr;
    CodeView *m_inline = nullptr;
    CodeView *m_left = nullptr;
    CodeView *m_right = nullptr;
    QLabel *m_leftLabel = nullptr;
    QLabel *m_rightLabel = nullptr;
    QLabel *m_inlineLabel = nullptr;
    QLabel *m_message = nullptr;

    // Row ranges of each chunk, per mode.
    QVector<QPair<int, int>> m_inlineChunkRows;
    QVector<QPair<int, int>> m_sideChunkRows;
    // Map from FileDiff line index to row in each side-by-side view (-1 if absent).
    QVector<int> m_leftRowOfLine;
    QVector<int> m_rightRowOfLine;
};
