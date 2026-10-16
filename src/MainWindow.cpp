#include "MainWindow.h"

#include "DiffPane.h"
#include "FileList.h"
#include "PreferencesDialog.h"
#include "Settings.h"

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>

MainWindow::MainWindow(GitRepo repo, Comparison cmp, QWidget *parent)
    : QMainWindow(parent), m_repo(std::move(repo)), m_cmp(std::move(cmp))
{
    m_files = new FileList;
    m_pane = new DiffPane;
    m_pane->setMode(Settings::sideBySide() ? DiffPane::Mode::SideBySide : DiffPane::Mode::Inline);

    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(m_files);
    splitter->addWidget(m_pane);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({280, 1000});
    splitter->setChildrenCollapsible(false);
    setCentralWidget(splitter);

    createActions();

    m_summary = new QLabel;
    statusBar()->addPermanentWidget(m_summary);

    connect(m_files, &FileList::fileActivated, this, &MainWindow::showFile);
    connect(m_pane, &DiffPane::currentChangeChanged, this, &MainWindow::updateChangeLabel);

    setWindowTitle(QStringLiteral("yadiff — %1 — %2").arg(QDir(m_repo.root()).dirName(), describeComparison()));

    QSettings s;
    if (!restoreGeometry(s.value(QStringLiteral("window/geometry")).toByteArray()))
        resize(1400, 900);
    splitter->restoreState(s.value(QStringLiteral("window/splitter")).toByteArray());

    reload();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    QSettings s;
    s.setValue(QStringLiteral("window/geometry"), saveGeometry());
    if (auto *splitter = qobject_cast<QSplitter *>(centralWidget()))
        s.setValue(QStringLiteral("window/splitter"), splitter->saveState());
    QMainWindow::closeEvent(event);
}

QString MainWindow::describeComparison() const
{
    if (m_cmp.mode == Comparison::Mode::WorkingTree)
        return tr("staged and unstaged changes");
    return QStringLiteral("%1 ↔ %2").arg(m_cmp.rev1Name, m_cmp.rev2.isEmpty() ? tr("working tree") : m_cmp.rev2Name);
}

void MainWindow::createActions()
{
    // File
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    QAction *reloadAct = fileMenu->addAction(tr("&Reload"), this, &MainWindow::reload);
    reloadAct->setShortcut(QKeySequence::Refresh);
    fileMenu->addSeparator();
    QAction *quitAct = fileMenu->addAction(tr("&Quit"), this, &QWidget::close);
    quitAct->setShortcut(QKeySequence::Quit);

    // View
    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    auto *modeGroup = new QActionGroup(this);
    m_inlineAct = viewMenu->addAction(tr("&Inline"));
    m_sideAct = viewMenu->addAction(tr("&Side by side"));
    for (QAction *a : {m_inlineAct, m_sideAct}) {
        a->setCheckable(true);
        modeGroup->addAction(a);
    }
    m_inlineAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_1));
    m_sideAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_2));
    m_inlineAct->setToolTip(tr("Inline view (Ctrl+1)"));
    m_sideAct->setToolTip(tr("Side-by-side view (Ctrl+2)"));
    (Settings::sideBySide() ? m_sideAct : m_inlineAct)->setChecked(true);
    connect(m_inlineAct, &QAction::triggered, this, [this] { setSideBySide(false); });
    connect(m_sideAct, &QAction::triggered, this, [this] { setSideBySide(true); });

    // Navigate
    QMenu *navMenu = menuBar()->addMenu(tr("&Navigate"));
    m_nextAct = navMenu->addAction(QIcon::fromTheme(QStringLiteral("go-down"), style()->standardIcon(QStyle::SP_ArrowDown)),
                                   tr("&Next change"), this, &MainWindow::nextChange);
    m_nextAct->setShortcut(QKeySequence(Qt::Key_F3));
    m_nextAct->setToolTip(tr("Next change (F3)"));
    m_prevAct = navMenu->addAction(QIcon::fromTheme(QStringLiteral("go-up"), style()->standardIcon(QStyle::SP_ArrowUp)),
                                   tr("&Previous change"), this, &MainWindow::prevChange);
    m_prevAct->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3));
    m_prevAct->setToolTip(tr("Previous change (Shift+F3)"));
    navMenu->addSeparator();
    QAction *nextFile = navMenu->addAction(tr("Next &file"), this, [this] { m_files->selectNext(); });
    nextFile->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F3));
    QAction *prevFile = navMenu->addAction(tr("Previous f&ile"), this, [this] { m_files->selectPrev(); });
    prevFile->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F3));

    // Preferences
    QMenu *prefMenu = menuBar()->addMenu(tr("&Preferences"));
    prefMenu->addAction(tr("&Colors and Font…"), this, &MainWindow::openPreferences);
    if (m_cmp.mode == Comparison::Mode::WorkingTree) {
        QAction *vsHead = prefMenu->addAction(tr("Compare unstaged changes against &HEAD"));
        vsHead->setCheckable(true);
        vsHead->setChecked(m_cmp.unstagedAgainstHead);
        vsHead->setToolTip(tr("When off, unstaged changes are compared against the index"));
        connect(vsHead, &QAction::toggled, this, [this](bool on) {
            Settings::setUnstagedAgainstHead(on);
            m_cmp.unstagedAgainstHead = on;
            reload();
        });
    }

    // Help
    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&About yadiff"), this, [this] {
        QMessageBox::about(this, tr("About yadiff"),
                           tr("<b>yadiff</b> %1<br>A graphical diff viewer for Git.<br><br>"
                              "F3 / Shift+F3: next / previous change<br>"
                              "Ctrl+1 / Ctrl+2: inline / side by side<br>"
                              "Double-click a moved block to jump to its other location.")
                               .arg(QApplication::applicationVersion()));
    });

    // Toolbar: view mode on the left, change navigation on the far right.
    QToolBar *tb = addToolBar(tr("Toolbar"));
    tb->setObjectName(QStringLiteral("mainToolbar"));
    tb->setMovable(false);
    tb->setFloatable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextOnly);
    tb->addAction(m_inlineAct);
    tb->addAction(m_sideAct);
    auto *spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacer);
    m_changeLabel = new QLabel;
    m_changeLabel->setContentsMargins(0, 0, 8, 0);
    tb->addWidget(m_changeLabel);
    auto *navBar = new QToolBar;
    navBar->setIconSize(QSize(20, 20));
    navBar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    navBar->addAction(m_prevAct);
    navBar->addAction(m_nextAct);
    tb->addWidget(navBar);
}

void MainWindow::reload()
{
    // Remember the current file so a reload keeps the user's place.
    QString keepPath;
    ChangeGroup keepGroup = ChangeGroup::Revisions;
    if (const int cur = m_files->currentIndex(); cur >= 0 && cur < m_changes.size()) {
        keepPath = m_changes[cur].path;
        keepGroup = m_changes[cur].group;
    }

    QString error;
    m_changes = m_repo.listChanges(m_cmp, &error);
    if (!error.isEmpty()) {
        m_changes.clear();
        m_files->setChanges(m_changes, false);
        m_pane->showMessage(QString(), tr("git failed:\n%1").arg(error));
        return;
    }
    const bool working = m_cmp.mode == Comparison::Mode::WorkingTree;
    m_files->setChanges(m_changes, working);

    if (working) {
        int staged = 0;
        for (const FileChange &fc : std::as_const(m_changes))
            staged += fc.group == ChangeGroup::Staged;
        m_summary->setText(tr("%1 staged, %2 unstaged").arg(staged).arg(m_changes.size() - staged));
    } else {
        m_summary->setText(m_changes.size() == 1 ? tr("1 file changed") : tr("%1 files changed").arg(m_changes.size()));
    }
    statusBar()->showMessage(m_repo.root(), 4000);

    if (m_changes.isEmpty()) {
        m_pane->showMessage(QString(), tr("No changes"));
        return;
    }
    for (int i = 0; i < m_changes.size(); ++i) {
        if (m_changes[i].path == keepPath && m_changes[i].group == keepGroup) {
            m_files->selectIndex(i);
            return;
        }
    }
    m_files->selectFirst();
}

void MainWindow::showFile(int index)
{
    if (index < 0 || index >= m_changes.size())
        return;
    const FileChange &fc = m_changes[index];

    QString left, right;
    if (m_cmp.mode == Comparison::Mode::Revisions) {
        left = m_cmp.rev1Name;
        right = m_cmp.rev2.isEmpty() ? tr("Working tree") : m_cmp.rev2Name;
    } else if (fc.status == u'U') {
        left = QStringLiteral("HEAD");
        right = tr("Working tree (unmerged)");
    } else if (fc.group == ChangeGroup::Staged) {
        left = QStringLiteral("HEAD");
        right = tr("Index (staged)");
    } else {
        left = m_cmp.unstagedAgainstHead ? QStringLiteral("HEAD") : tr("Index");
        right = tr("Working tree");
    }
    const bool isNew = fc.status == u'A' || fc.status == u'?';
    const bool isGone = fc.status == u'D';
    left = isNew ? tr("%1: (file does not exist)").arg(left) : left + QStringLiteral(": ") + fc.oldPath;
    right = isGone ? tr("%1: (file deleted)").arg(right) : right + QStringLiteral(": ") + fc.path;

    QString title = fc.path;
    if (fc.oldPath != fc.path)
        title = tr("%1  (%2 from %3)").arg(fc.path, statusDescription(fc.status).toLower(), fc.oldPath);
    else
        title += QStringLiteral("  (%1)").arg(statusDescription(fc.status).toLower());

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const FileDiff diff = m_repo.loadDiff(fc, m_cmp, &error);
    QApplication::restoreOverrideCursor();
    if (!error.isEmpty()) {
        m_pane->showMessage(title, tr("git failed:\n%1").arg(error));
        return;
    }
    m_pane->setDiff(diff, title, left, right);
}

void MainWindow::nextChange()
{
    if (m_pane->nextChange())
        return;
    // No more changes in this file: move on to the next file (which opens at its first change).
    if (!m_files->selectNext())
        statusBar()->showMessage(tr("No more changes"), 3000);
}

void MainWindow::prevChange()
{
    if (m_pane->prevChange())
        return;
    if (m_files->selectPrev())
        m_pane->goToLastChange();
    else
        statusBar()->showMessage(tr("No earlier changes"), 3000);
}

void MainWindow::setSideBySide(bool on)
{
    Settings::setSideBySide(on);
    m_pane->setMode(on ? DiffPane::Mode::SideBySide : DiffPane::Mode::Inline);
}

void MainWindow::openPreferences()
{
    PreferencesDialog dlg(this);
    connect(&dlg, &PreferencesDialog::applied, m_pane, &DiffPane::applySettings);
    dlg.exec();
}

void MainWindow::updateChangeLabel(int index, int count)
{
    if (count == 0)
        m_changeLabel->setText(tr("No changes in file"));
    else
        m_changeLabel->setText(tr("Change %1 of %2").arg(index + 1).arg(count));
}

void MainWindow::grabAll(const QString &prefix)
{
    const bool wasSide = Settings::sideBySide();
    m_files->selectFirst();
    int n = 0;
    do {
        for (const bool side : {true, false}) {
            m_pane->setMode(side ? DiffPane::Mode::SideBySide : DiffPane::Mode::Inline);
            QApplication::processEvents();
            grab().save(QStringLiteral("%1-%2-%3.png").arg(prefix).arg(n, 2, 10, QLatin1Char('0')).arg(side ? QStringLiteral("side") : QStringLiteral("inline")));
        }
        ++n;
    } while (m_files->selectNext());
    m_pane->setMode(wasSide ? DiffPane::Mode::SideBySide : DiffPane::Mode::Inline);
}
