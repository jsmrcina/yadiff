#pragma once

#include "DiffPane.h"
#include "FileFilter.h"
#include "GitHubReview.h"
#include "GitRepo.h"
#include "Updater.h"
#include "ReviewStore.h"
#include "Session.h"

#include <QFutureWatcher>
#include <QHash>
#include <QMainWindow>
#include <QtConcurrent/QtConcurrentRun>

#include <atomic>
#include <functional>
#include <memory>

class DiffPane;
class FileList;
class QAction;
class QPushButton;
class QActionGroup;
class QComboBox;
class QDragEnterEvent;
class QDropEvent;
class QLabel;
class QLineEdit;
class QMenu;
class QTimer;
class QToolButton;
class SearchBar;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow(GitRepo repo, Comparison cmp, QWidget *parent = nullptr);

    // Reloads the change list in the background. `afterFile` runs once the reselected
    // file is shown (by default the previous change position is restored).
    void reload(std::function<void()> afterFile = {});
    void setComparison(const Comparison &cmp);

    // True while git work runs in the background (tests wait on this).
    bool isBusy() const { return m_listBusy || m_fileBusy || m_searchBusy || m_jobs > 0; }

    // Developer aid: render every file in both view modes to <prefix>-NN-<mode>.png.
    void grabAll(const QString &prefix);

    // Opens these arguments (as ComparisonArgs::fromArguments() reads them): here when the
    // window is empty, otherwise in a new window. A session is applied once it's shown.
    void openArguments(const QStringList &args, const std::optional<Session> &session = {},
                       const QString &sessionFile = {});
    void applySession(const Session &session);
    // Dropped files: two are compared, one waits for a second (or is a patch or session).
    void openDropped(const QStringList &paths);

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // Runs `job` on a worker thread and `done` back on the UI thread. With
    // YADIFF_SYNCHRONOUS set (tests, screenshots) both run immediately, in order.
    template<class R>
    void runJob(std::function<R()> job, std::function<void(R)> done)
    {
        if (m_sync) {
            done(job());
            return;
        }
        ++m_jobs;
        auto *watcher = new QFutureWatcher<R>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, done] {
            --m_jobs;
            done(watcher->result());
            watcher->deleteLater();
        });
        watcher->setFuture(QtConcurrent::run(std::move(job)));
    }

    void createActions();
    void createToolBar();
    QWidget *createFilePanel();
    QWidget *createCommitBar();
    void updateCommitBar();
    void syncCommitBar();
    void stepCommit(int delta);
    void showCommitStep(int index); // -1: the whole range
    void updateModeUi();
    void showFile(int index);
    void reloadCurrentFile();
    void loadBlame();
    void openBlameCommit(const QString &hash);
    // Updates: `interactive` reports every outcome and asks before installing.
    void checkForUpdates(bool interactive);
    void installUpdate(const Updater::Release &release, bool interactive);
    void restartForUpdate();
    // Shows a different comparison in this window (used by an empty window).
    void setComparison(GitRepo repo, Comparison cmp);
    void nextChange();
    void prevChange();
    void setSideBySide(bool on);
    void setAllChanges(bool on);
    void setDiffOptions(const DiffOptions &o);
    void syncOptionControls();
    void openPreferences();
    void openHistory();
    void registerDifftool();
    void exportReport();
    void onReviewToggled(int index, bool reviewed);
    void updateSummary();
    void updateViewToggle();
    void updateTitle();
    void updateEditActions();
    void updateSyntaxCombo();
    void applyFilter();

    // File list actions (on the selected files) and the File / Marking menus.
    QVector<FileChange> selectedChanges() const;
    void showFileContextMenu(const QPoint &globalPos);
    void stageSelected(bool stage);
    void revertSelected();
    void swapSides();
    void downloadAgain(bool onlySelected);
    void markSelected(bool marked);
    void markByPattern();
    void markIdentical();
    void copyCommitHash();
    void openInGitk();
    void newComparison();
    QStringList comparisonArguments() const;
    void rememberRecent();
    void rebuildRecentMenu();
    void saveSession();
    void openSession();
    void openSessionFile(const QString &path);
    void compareText();
    void compareTexts(const QByteArray &left, const QByteArray &right, const QString &fileName, const QString &title);
    void compareClipboard(bool newSide);
    void openPatch();
    void openPatchFile(const QString &path);
    void applyPatch();
    void saveAsPatch();
    void changeWindowTitle();
    void editIgnorePatterns();
    void editUnimportantText();
    void showUsage();
    void unregisterDifftool();
    // Edit menu.
    void copyFilePath(bool newSide);
    void editExternally(bool newSide);
    void setEditorCommand();
    void goToLineDialog();
    void rebuildLanguageMenu();
    void applyTheme();
    void applyFonts();
    QString scrollKey(const FileChange &fc) const;
    QString describeComparison() const;
    QString reviewRoot() const;

    // Editing and writing files.
    bool maybeKeepEdits(); // false = user cancelled
    bool saveEdits();
    void toggleEditing(bool on);
    void copyChange(int chunk, bool toNewSide);
    void applyLineOp(GitRepo::LineOp op, const QVector<int> &lines);

    // Search.
    void runSearch();
    void searchNext(bool forward);
    void updateSearchResult();

    // Reload when files change.
    void pollForChanges();

    // Pull request review (GitHub or Azure DevOps).
    void loadComments();
    QVector<GitHubReview::Thread> threadsFor(const FileChange &fc) const;
    void addComment(const QString &path, const QString &side, int line, int startLine);
    void replyToThread(qint64 threadId);
    void submitReview(GitHubReview::Verdict verdict);

    GitRepo m_repo;
    Comparison m_cmp;
    QVector<FileChange> m_changes;
    std::unique_ptr<ReviewStore> m_reviews;
    int m_shownIndex = -1;
    bool m_revertingSelection = false;

    // Background work.
    bool m_sync = false;
    int m_jobs = 0;
    quint64 m_listGen = 0, m_fileGen = 0, m_searchGen = 0;
    bool m_listBusy = false, m_fileBusy = false, m_searchBusy = false, m_pollBusy = false;
    std::function<void()> m_afterFileLoad; // runs after the next file finishes loading
    std::shared_ptr<std::atomic_bool> m_searchCancel;
    QSet<int> m_showLarge; // files the user chose to show despite their size

    FileList *m_files = nullptr;
    DiffPane *m_pane = nullptr;
    SearchBar *m_search = nullptr;
    QLabel *m_summary = nullptr;
    QTimer *m_searchTimer = nullptr;
    QTimer *m_pollTimer = nullptr;
    QByteArray m_lastSignature;

    QHash<int, FileDiff> m_diffCache;       // shown and searched diffs
    QHash<int, int> m_matchCounts;          // change index -> matches
    QHash<QString, QString> m_syntaxBySuffix; // user overrides, per file extension

    // File list filter.
    QLineEdit *m_filterEdit = nullptr;
    QLabel *m_filterCount = nullptr;

    // PR review.
    QVector<GitHubReview::Comment> m_comments;
    QMenu *m_reviewMenu = nullptr;
    QAction *m_reviewMenuAction = nullptr;
    QAction *m_reviewToolAction = nullptr;
    QAction *m_expandCommentsAct = nullptr;
    QAction *m_hideMarkedAct = nullptr;
    QHash<QString, QAction *> m_categoryActs; // Options > Show ... files
    QAction *m_useGlobsAct = nullptr;
    QAction *m_rememberScrollAct = nullptr;
    QAction *m_syntaxAct = nullptr;
    QAction *m_lineNumbersAct = nullptr;
    QAction *m_blameAct = nullptr;
    quint64 m_blameGen = 0;
    bool m_updating = false;
    QPushButton *m_restartButton = nullptr;
    QAction *m_trimPrefixAct = nullptr;
    QAction *m_clickWordAct = nullptr;
    QAction *m_stageAct = nullptr;
    QAction *m_unstageAct = nullptr;
    QAction *m_revertAct = nullptr;
    QAction *m_swapAct = nullptr;
    QAction *m_editLeftAct = nullptr;
    QAction *m_editRightAct = nullptr;
    QActionGroup *m_paneGroup = nullptr;
    QMenu *m_languageMenu = nullptr;
    QMenu *m_themeMenu = nullptr;
    QString m_customTitle;
    QHash<QString, DiffPane::ScrollState> m_scrollMemory;
    QAction *m_collapseCommentsAct = nullptr;

    QToolButton *m_viewToggle = nullptr;
    QToolButton *m_whitespaceToggle = nullptr;
    QToolButton *m_editToggle = nullptr;
    QComboBox *m_scopeCombo = nullptr;
    QAction *m_scopeComboAction = nullptr;
    QComboBox *m_algoCombo = nullptr;
    QComboBox *m_syntaxCombo = nullptr;

    QAction *m_historyAct = nullptr;
    QAction *m_toggleViewAct = nullptr;
    QAction *m_inlineAct = nullptr;
    QAction *m_sideAct = nullptr;
    QAction *m_allChangesAct = nullptr;
    QAction *m_vsHeadAct = nullptr;
    QAction *m_collapseAct = nullptr;
    QAction *m_expandAllAct = nullptr;
    QAction *m_showWhitespaceAct = nullptr;
    QAction *m_wordWrapAct = nullptr;
    QAction *m_prevAct = nullptr;
    QAction *m_nextAct = nullptr;
    QAction *m_editAct = nullptr;
    QAction *m_saveAct = nullptr;
    QAction *m_discardAct = nullptr;
    QAction *m_copyRightAct = nullptr;
    QAction *m_copyLeftAct = nullptr;
    // Stepping through commits: the commits of the range (or around one commit), and
    // which one is shown (-1: the whole range, m_rangeCmp).
    QWidget *m_commitBar = nullptr;
    QToolButton *m_commitPrev = nullptr, *m_commitNext = nullptr;
    QComboBox *m_commitCombo = nullptr;
    QLabel *m_commitInfo = nullptr;
    QVector<CommitInfo> m_commitList;
    int m_commitIndex = -1;
    bool m_commitRangeMode = false;
    bool m_steppingCommits = false;
    Comparison m_rangeCmp;
    QAction *m_nextCommitAct = nullptr, *m_prevCommitAct = nullptr;
    QMenu *m_recentMenu = nullptr;
    QAction *m_applyPatchAct = nullptr;
    QString m_selectOnLoad;  // a file to show once the list loads (from a session)
    QString m_droppedLeft;   // a dropped file waiting for the one to compare it with
    QAction *m_stageLinesAct = nullptr;
    QAction *m_unstageLinesAct = nullptr;
    QAction *m_discardLinesAct = nullptr;
    QAction *m_autoReloadAct = nullptr;
    QActionGroup *m_algoGroup = nullptr;
    QAction *m_ignoreSpaceChangeAct = nullptr;
    QAction *m_ignoreAllSpaceAct = nullptr;
    QAction *m_ignoreBlankLinesAct = nullptr;
    QAction *m_ignoreCrAct = nullptr;
    QAction *m_ignoreTrimAct = nullptr;
    QAction *m_ignoreCaseAct = nullptr;
    QAction *m_ignoreCommentsAct = nullptr;
    QAction *m_ignoreUnimportantAct = nullptr;
    // Manual alignment per file (scrollKey): (old line, new line) pairs.
    QHash<QString, QVector<std::pair<int, int>>> m_anchors;
    QAction *m_detectMovesAct = nullptr;
};
