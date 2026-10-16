#pragma once

#include "CodeView.h"
#include "DiffFormat.h"
#include "DiffModel.h"
#include "GitHubReview.h"
#include "Search.h"

#include <QImage>
#include <QSet>
#include <QWidget>

class ImageDiffView;
class OverviewBar;
class QPushButton;
class QLabel;
class QStackedWidget;
class QTimer;

// Everything DiffPane needs to show one file.
struct FileView {
    FileDiff diff;
    QString title;
    QString leftLabel, rightLabel;
    DiffFormat::Paths paths;
    QString fileName;       // for syntax detection
    bool oldOnDisk = false; // the old side is a file that can be written
    bool newOnDisk = false; // the new side is a file that can be written (and edited)

    // Images: shown in the image view instead of "binary file differs".
    bool isImage = false;
    QImage oldImage, newImage;
    qint64 oldBytes = 0, newBytes = 0;

    // GitHub pull request review: comment threads for this file, and whether new
    // comments can be added.
    QVector<GitHubReview::Thread> threads;
    bool reviewEnabled = false;
};

// Right-hand side of the window: shows one file's diff inline or side by side and
// tracks the current change, search matches, folds and edits.
class DiffPane : public QWidget
{
    Q_OBJECT
public:
    enum class Mode { Inline, SideBySide };
    enum class SearchScope { Selection, File };

    explicit DiffPane(QWidget *parent = nullptr);

    void setFile(const FileView &file);
    void showMessage(const QString &title, const QString &message);
    // A diff too large to show straight away; "Show anyway" emits showLargeRequested().
    void showLargeNotice(const QString &title, int lineCount);
    // Replace the review threads of the current file, keeping the scroll position.
    void setThreads(const QVector<GitHubReview::Thread> &threads);
    bool hasDiff() const { return m_hasDiff; }
    const FileDiff &diff() const { return m_file.diff; }

    void setMode(Mode mode);
    Mode mode() const { return m_mode; }
    void applySettings();

    // View options.
    void setCollapseUnchanged(bool on);
    void expandAll();
    void setShowWhitespace(bool on);
    void setWordWrap(bool on);

    // Review threads start collapsed to one summary line; these open or close them all.
    void setCommentsExpanded(bool expanded);
    bool isThreadExpanded(qint64 threadId) const { return m_expandedThreads.contains(threadId); }

    // Syntax: empty `language` means auto-detect from the file name.
    void setLanguageOverride(const QString &language);
    QString detectedLanguage() const;
    QString effectiveLanguage() const;

    // Changes.
    int changeCount() const { return int(m_file.diff.chunks.size()); }
    int currentChange() const { return m_current; }
    bool nextChange();
    bool prevChange();
    void goToFirstChange();
    void goToLastChange();
    void goToChange(int index); // clamped to the available changes

    // Search within this file (or the current selection).
    void setSearch(const SearchQuery &query, SearchScope scope);
    void clearSearch();
    int matchCount() const { return int(m_matches.size()); }
    int currentMatch() const { return m_currentMatch; }
    bool nextMatch();
    bool prevMatch();
    void goToMatch(int index);
    bool hasSelection() const;

    // Editing the new (working-tree) side.
    bool canEdit() const { return m_hasDiff && m_file.newOnDisk && !m_file.diff.binary; }
    bool canCopyTo(bool newSide) const { return m_hasDiff && (newSide ? m_file.newOnDisk : m_file.oldOnDisk); }
    void setEditing(bool on);
    bool isEditing() const;
    bool isModified() const;
    QStringList editedLines() const;

signals:
    void currentChangeChanged(int index, int count);
    void modeChanged(Mode mode);
    void modifiedChanged(bool modified);
    void copyChangeRequested(int chunk, bool toNewSide);
    void searchResultsChanged(int count, int current);
    void showLargeRequested();
    // Review: add a comment on `line` (and up from `startLine` for a range) of `side` ("LEFT"/"RIGHT").
    void commentRequested(const QString &path, const QString &side, int line, int startLine);
    void replyRequested(qint64 threadId);

private:
    void rebuild();
    void showChange(int index);
    void jumpToMovePartner(CodeView *source, int row);
    void showContextMenu(CodeView *view, int row, const QPoint &globalPos);
    int chunkAtRow(CodeView *view, int row) const;
    ViewPalette makePalette() const;
    Row makeRow(int lineIndex) const;
    Row makeFoldRow(int firstHidden, int count) const;
    void expandFold(int foldKey);
    void updateOverview();
    void updateVisibleRange();
    void scheduleWrapPadding();
    void applyWrapPadding();
    void applySyntax();
    void recomputeMatches();
    void applySearchHighlights();
    void revealMatch(int index);
    CodeView *activeView() const;
    void indexThreads();
    Row makeCommentRow(const GitHubReview::Thread &t) const;
    QString threadHtml(const GitHubReview::Thread &t) const;
    void toggleThread(qint64 threadId);
    void relayout();
    void applyCommentHeights();
    Row makeSpacerRow(const Row &comment) const;
    bool nearChange(int lineIndex) const;

    FileView m_file;
    QString m_leftName, m_rightName;
    bool m_hasDiff = false;
    Mode m_mode = Mode::SideBySide;
    int m_current = -1;

    bool m_collapse = false;
    int m_context = 3;
    QSet<int> m_expanded; // fold keys (first hidden line) the user opened
    QVector<int> m_foldOfLine; // fold key for each hidden line, -1 when visible
    bool m_showWhitespace = false;
    bool m_wrap = false;
    QString m_languageOverride;

    SearchQuery m_query;
    SearchScope m_scope = SearchScope::File;
    CodeView *m_selView = nullptr;
    int m_selStart = -1, m_selEnd = -1; // document positions in m_selView
    QVector<LineMatch> m_matches;
    int m_currentMatch = -1;

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
    QPushButton *m_showAnyway = nullptr;
    QWidget *m_messagePage = nullptr;
    ImageDiffView *m_image = nullptr;
    // Review threads anchored to FileDiff lines: line index -> thread indices.
    QHash<int, QVector<int>> m_threadsAt;
    QSet<qint64> m_expandedThreads; // review threads shown in full
    OverviewBar *m_overview = nullptr;
    QTimer *m_wrapTimer = nullptr;

    // Row ranges of each chunk, per mode.
    QVector<QPair<int, int>> m_inlineChunkRows;
    QVector<QPair<int, int>> m_sideChunkRows;
    // Map from FileDiff line index to row in each view (-1 if absent or folded).
    QVector<int> m_inlineRowOfLine;
    QVector<int> m_leftRowOfLine;
    QVector<int> m_rightRowOfLine;
};
