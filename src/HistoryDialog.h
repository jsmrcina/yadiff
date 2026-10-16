#pragma once

#include "GitRepo.h"

#include <QDialog>

class QCheckBox;
class QLineEdit;
class QPushButton;
class QTreeWidget;

// Pick commits from the history to compare.
class HistoryDialog : public QDialog
{
    Q_OBJECT
public:
    enum class Choice { None, CommitChanges, CompareTwo, CommitVsWorkingTree, WorkingTree };

    HistoryDialog(const GitRepo &repo, QWidget *parent = nullptr);

    Choice choice() const { return m_choice; }
    // CommitChanges / CommitVsWorkingTree: first() only. CompareTwo: older first, newer second.
    CommitInfo first() const { return m_first; }
    CommitInfo second() const { return m_second; }

private:
    void load(bool more);
    void applyFilter();
    void updateButtons();
    void finish(Choice c);
    QVector<CommitInfo> selected() const;

    const GitRepo &m_repo;
    QVector<CommitInfo> m_commits;
    QTreeWidget *m_list = nullptr;
    QLineEdit *m_filter = nullptr;
    QCheckBox *m_allRefs = nullptr;
    QPushButton *m_showCommit = nullptr;
    QPushButton *m_compareTwo = nullptr;
    QPushButton *m_vsWorking = nullptr;
    QPushButton *m_more = nullptr;
    Choice m_choice = Choice::None;
    CommitInfo m_first, m_second;
};
