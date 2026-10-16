#pragma once

#include "GitRepo.h"

#include <QMainWindow>

class DiffPane;
class FileList;
class QAction;
class QLabel;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow(GitRepo repo, Comparison cmp, QWidget *parent = nullptr);

    void reload();

    // Developer aid: render every file in both view modes to <prefix>-NN-<mode>.png.
    void grabAll(const QString &prefix);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void createActions();
    void showFile(int index);
    void nextChange();
    void prevChange();
    void setSideBySide(bool on);
    void openPreferences();
    void updateChangeLabel(int index, int count);
    QString describeComparison() const;

    GitRepo m_repo;
    Comparison m_cmp;
    QVector<FileChange> m_changes;

    FileList *m_files = nullptr;
    DiffPane *m_pane = nullptr;
    QLabel *m_changeLabel = nullptr;
    QLabel *m_summary = nullptr;

    QAction *m_inlineAct = nullptr;
    QAction *m_sideAct = nullptr;
    QAction *m_prevAct = nullptr;
    QAction *m_nextAct = nullptr;
};
