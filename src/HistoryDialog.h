#pragma once

#include "GitRepo.h"

#include <QDialog>

#include <atomic>
#include <memory>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QTreeWidget;

// Pick commits from the history to compare.
class HistoryDialog : public QDialog
{
    Q_OBJECT
public:
    // BranchChanges: what first() (a branch or tag) has that HEAD doesn't, from their merge base.
    enum class Choice { None, CommitChanges, CompareTwo, CommitVsWorkingTree, WorkingTree, BranchChanges };

    HistoryDialog(const GitRepo &repo, QWidget *parent = nullptr);
    ~HistoryDialog() override;
    bool isLoading() const { return m_loading; }

    Choice choice() const { return m_choice; }
    // CommitChanges / CommitVsWorkingTree: first() only. CompareTwo: older first, newer second.
    CommitInfo first() const { return m_first; }
    CommitInfo second() const { return m_second; }

private:
    // Loads in the background (git log can take minutes in a large repository); a newer
    // load, or closing the dialog, stops the one running.
    void load(bool more);
    void showPage(const QVector<CommitInfo> &page, const QString &error, int source);
    void applyFilter();
    void updateButtons();
    void finish(Choice c);
    QVector<CommitInfo> selected() const;

    const GitRepo &m_repo;
    QVector<CommitInfo> m_commits;
    QTreeWidget *m_list = nullptr;
    QLineEdit *m_filter = nullptr;
    QCheckBox *m_allRefs = nullptr;
    QComboBox *m_source = nullptr; // history, branches and tags, or stashes
    QPushButton *m_branchChanges = nullptr;
    QPushButton *m_showCommit = nullptr;
    QPushButton *m_compareTwo = nullptr;
    QPushButton *m_vsWorking = nullptr;
    QPushButton *m_more = nullptr;
    Choice m_choice = Choice::None;
    bool m_loading = false;
    quint64 m_loadGen = 0;
    std::shared_ptr<std::atomic_bool> m_cancel; // the running load's
    CommitInfo m_first, m_second;
};
