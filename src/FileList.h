#pragma once

#include "FileFilter.h"
#include "GitRepo.h"

#include <QTreeWidget>

// Left-hand file pane: changed files grouped by directory (paths relative to
// the repository root), showing bare file names.
class FileList : public QTreeWidget
{
    Q_OBJECT
public:
    explicit FileList(QWidget *parent = nullptr);

    // showGroups adds "Staged" / "Unstaged" top-level sections. `reviewed` holds the
    // initial check state per change (may be empty).
    void setChanges(const QVector<FileChange> &changes, bool showGroups, const QVector<bool> &reviewed = {});

    bool isReviewed(int index) const;
    void setReviewed(int index, bool reviewed);
    int reviewedCount() const;

    // Search across all files: bold the files that contain matches (empty map clears).
    void setMatchCounts(const QHash<int, int> &counts);
    QVector<int> displayOrder(bool includeHidden = false) const; // change indices in list order

    // Hides files the filter rejects (and directories left empty). Navigation skips them.
    void setFilter(const FileFilter &filter);
    int visibleCount() const;

    int currentIndex() const;          // index into the changes vector, or -1
    bool selectIndex(int index);
    bool selectFirst();
    bool selectNext();
    bool selectPrev();

signals:
    void fileActivated(int index);
    // The user ticked or unticked a file (directly, or via its directory/section).
    void reviewToggled(int index, bool reviewed);

private:
    QTreeWidgetItem *leafFor(int index) const;
    void styleLeaf(QTreeWidgetItem *item);
    bool m_populating = false;
    void applyFilter();
    QVector<QTreeWidgetItem *> m_leaves; // in display order
    QVector<QChar> m_status;             // status per change index
    FileFilter m_filter;
};
