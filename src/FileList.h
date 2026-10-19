#pragma once

#include "FileFilter.h"
#include "GitRepo.h"

#include <QTreeWidget>

// Left-hand file pane: changed files grouped by folder, showing bare file names.
// Files can be marked (shown with a highlight colour) and several can be selected.
class FileList : public QTreeWidget
{
    Q_OBJECT
public:
    explicit FileList(QWidget *parent = nullptr);

    // showGroups adds "Staged" / "Unstaged" (and "Unchanged") sections. `marked` holds
    // the initial mark per change (may be empty).
    void setChanges(const QVector<FileChange> &changes, bool showGroups, const QVector<bool> &marked = {});

    // Folder names: `root` is prefixed to make them fully qualified (empty: as listed);
    // with `trimCommonPrefix` the part all folders share is left off.
    void setFolderDisplay(const QString &root, bool trimCommonPrefix);

    bool isMarked(int index) const;
    void setMarked(int index, bool marked);
    int markedCount() const;
    QVector<int> markedIndices() const;
    void setMarkColor(const QColor &color);

    // Change indices of the selected files (the current one when nothing is selected).
    QVector<int> selectedIndices() const;
    void selectIndices(const QVector<int> &indices);

    // Background loading of each file's diff: waiting (a hollow dot), loading (a spinner),
    // loaded (its line counts, once known) or failed (a red mark). Not-yet-loaded names
    // are greyed.
    enum class LoadState { None, Queued, Loading, Loaded, Failed };
    void setLoadState(int index, LoadState state);
    LoadState loadState(int index) const;
    void clearLoadStates();
    // Line counts that arrive after the list (from the loaded diff).
    void setStats(int index, int added, int removed, bool blankOnly);
    // While the list itself is loading: a single greyed row with `text` (empty removes it).
    void setPlaceholder(const QString &text);

    // Search across all files: bold the files that contain matches (empty map clears).
    void setMatchCounts(const QHash<int, int> &counts);
    QVector<int> displayOrder(bool includeHidden = false) const; // change indices in list order

    // Hides files the filter rejects (and folders left empty). Navigation skips them.
    void setFilter(const FileFilter &filter);
    int visibleCount() const;

    int currentIndex() const; // index into the changes vector, or -1
    bool selectIndex(int index);
    bool selectFirst();
    bool selectNext();
    bool selectPrev();

signals:
    void fileActivated(int index);           // the current file changed
    void viewRequested(int index);           // Enter or double-click: look at its diff
    void markToggled(int index, bool marked);
    void contextMenuRequested(const QPoint &globalPos);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    QTreeWidgetItem *leafFor(int index) const;
    void updateSpinner();
    void styleLeaf(QTreeWidgetItem *item);
    void applyFilter();
    void updateFolderNames();

    QVector<QTreeWidgetItem *> m_leaves; // in display order
    QVector<QTreeWidgetItem *> m_folders;
    QVector<FileChange> m_changes;
    FileFilter m_filter;
    QColor m_markColor;
    QString m_root;
    bool m_trimPrefix = false;
    class QTimer *m_spinTimer = nullptr; // repaints while any file is loading
};
