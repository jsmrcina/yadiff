#pragma once

#include "GitRepo.h"

#include <QTreeWidget>

// Left-hand file pane: changed files grouped by directory (paths relative to
// the repository root), showing bare file names.
class FileList : public QTreeWidget
{
    Q_OBJECT
public:
    explicit FileList(QWidget *parent = nullptr);

    // showGroups adds "Staged" / "Unstaged" top-level sections.
    void setChanges(const QVector<FileChange> &changes, bool showGroups);

    int currentIndex() const;          // index into the changes vector, or -1
    bool selectIndex(int index);
    bool selectFirst();
    bool selectNext();
    bool selectPrev();

signals:
    void fileActivated(int index);

private:
    QVector<QTreeWidgetItem *> m_leaves; // in display order
};
