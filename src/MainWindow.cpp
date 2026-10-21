#include "MainWindow.h"

#include "Log.h"

#include "DiffPane.h"
#include "Difftool.h"
#include "FileList.h"
#include "HistoryDialog.h"
#include "PreferencesDialog.h"
#include "SettingsDialog.h"
#include "Shortcuts.h"
#include "SearchBar.h"
#include "Settings.h"
#include "Syntax.h"
#include "AdoAuth.h"
#include "AdoPr.h"
#include "CodeView.h"
#include "GitHubPr.h"
#include "MergeWindow.h"
#include "NewComparisonDialog.h"
#include "PrReview.h"
#include "PrInbox.h"
#include "PrListDialog.h"
#include "PrOverviewPanel.h"
#include "Report.h"
#include "TextFile.h"
#include "Theme.h"
#include "HexDiff.h"
#include "TableDiff.h"
#include "ComparisonArgs.h"
#include "Updater.h"
#include "Usage.h"
#include "Scratch.h"
#include "Session.h"
#include "ShellMenu.h"

#include <QActionGroup>
#include <QAbstractScrollArea>
#include <QDragEnterEvent>
#include <QGridLayout>
#include <QJsonDocument>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QClipboard>
#include <QColorDialog>
#include <QDateTime>
#include <QFontDialog>
#include <QProcess>
#include <QStandardPaths>
#include <QDesktopServices>
#include <QFileDialog>
#include <QImage>
#include <QInputDialog>
#include <QLineEdit>
#include <QPointer>
#include <QProgressBar>
#include <QProgressDialog>
#include <QThread>
#include <QUrl>
#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QHBoxLayout>
#include <QVBoxLayout>

namespace {

// Small glyph for the view toggle: one column (inline) or two (side by side).
QIcon viewIcon(bool sideBySide, const QColor &color)
{
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 2));
    p.setBrush(Qt::NoBrush);
    if (sideBySide) {
        p.drawRoundedRect(QRectF(3, 5, 11, 22), 2, 2);
        p.drawRoundedRect(QRectF(18, 5, 11, 22), 2, 2);
    } else {
        p.drawRoundedRect(QRectF(6, 5, 20, 22), 2, 2);
    }
    for (int y : {11, 16, 21}) {
        if (sideBySide) {
            p.drawLine(6, y, 11, y);
            p.drawLine(21, y, 26, y);
        } else {
            p.drawLine(10, y, 22, y);
        }
    }
    return QIcon(pm);
}

// Up/down chevrons for change navigation, in the theme's text colour (icon themes don't
// follow yadiff's own colour themes).
QIcon arrowIcon(bool up, const QColor &color)
{
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (up)
        p.drawPolyline(QPolygonF{QPointF(8, 20), QPointF(16, 11), QPointF(24, 20)});
    else
        p.drawPolyline(QPolygonF{QPointF(8, 12), QPointF(16, 21), QPointF(24, 12)});
    return QIcon(pm);
}

// Glyph for the whitespace toggle: an open-box "space" symbol.
QIcon whitespaceIcon(const QColor &color)
{
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPolyline(QPolygonF{QPointF(6, 13), QPointF(6, 22), QPointF(26, 22), QPointF(26, 13)});
    return QIcon(pm);
}

QString homeShortened(const QString &path)
{
    const QString home = QDir::homePath();
    return path.startsWith(home + u'/') ? u'~' + path.mid(home.size()) : path;
}

// What the auto-reload watches: changes to it mean the files on disk changed.
QByteArray signatureOf(const GitRepo &repo, const Comparison &cmp)
{
    const Log::Quiet quiet; // runs every two seconds while auto-reload is on
    QCryptographicHash h(QCryptographicHash::Sha1);
    auto addFile = [&h](const QString &path) {
        const QFileInfo fi(path);
        h.addData(path.toUtf8());
        h.addData(QByteArray::number(fi.exists() ? fi.size() : -1));
        h.addData(QByteArray::number(fi.exists() ? fi.lastModified().toMSecsSinceEpoch() : 0));
    };
    if (cmp.mode == Comparison::Mode::Paths) {
        if (cmp.pathsAreFiles) {
            addFile(cmp.leftPath);
            addFile(cmp.rightPath);
        } else {
            for (const QString &root : {cmp.leftPath, cmp.rightPath}) {
                QDirIterator it(root, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
                int n = 0;
                while (it.hasNext() && n++ < 50000)
                    addFile(it.next());
            }
        }
        return h.result();
    }
    if (repo.isBare() || (cmp.mode == Comparison::Mode::Revisions && !cmp.rev2.isEmpty()))
        return {}; // nothing on disk is being compared
    // git status lists what differs; size+mtime of each listed file catches further edits.
    QByteArray out;
    QStringList args{QStringLiteral("status"), QStringLiteral("--porcelain=v1"), QStringLiteral("-z"),
                     QStringLiteral("--untracked-files=all")};
    repo.run(args, &out, nullptr);
    h.addData(out);
    for (const QByteArray &entry : out.split('\0'))
        if (entry.size() > 3)
            addFile(repo.root() + u'/' + QString::fromUtf8(entry.mid(3)));
    QByteArray head;
    repo.run({QStringLiteral("rev-parse"), QStringLiteral("HEAD")}, &head, nullptr);
    h.addData(head);
    QByteArray indexPath;
    repo.run({QStringLiteral("rev-parse"), QStringLiteral("--git-path"), QStringLiteral("index")}, &indexPath, nullptr);
    addFile(QDir(repo.root()).absoluteFilePath(QString::fromUtf8(indexPath).trimmed()));
    return h.result();
}

bool isImagePath(const QString &path)
{
    static const QStringList exts{QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("gif"),
                                  QStringLiteral("bmp"), QStringLiteral("webp"), QStringLiteral("ico"), QStringLiteral("tif"),
                                  QStringLiteral("tiff")};
    return exts.contains(QFileInfo(path).suffix().toLower());
}

constexpr int kLargeDiffLines = 100000;

} // namespace

MainWindow::MainWindow(GitRepo repo, Comparison cmp, QWidget *parent)
    : QMainWindow(parent), m_repo(std::move(repo)), m_cmp(std::move(cmp))
{
    m_sync = qEnvironmentVariableIsSet("YADIFF_SYNCHRONOUS") || qEnvironmentVariableIsSet("YADIFF_GRAB");
    m_cmp.ignoreGlobs = Settings::ignoreGlobs();
    m_cmp.useIgnoreGlobs = Settings::useIgnoreGlobs();
    m_cmp.includeIdentical = Settings::showFileCategory(QStringLiteral("identical"));
    m_reviews = std::make_unique<ReviewStore>(reviewRoot());
    m_files = new FileList;
    m_pane = new DiffPane;
    m_pane->setMode(Settings::sideBySide() ? DiffPane::Mode::SideBySide : DiffPane::Mode::Inline);
    m_pane->setCollapseUnchanged(Settings::collapseUnchanged());
    m_pane->setShowWhitespace(Settings::showWhitespace());
    m_pane->setWordWrap(Settings::wordWrap());
    m_pane->setShowLineNumbers(Settings::showLineNumbers());
    m_pane->setSyntaxEnabled(Settings::syntaxEnabled());
    m_pane->setHighlightClickedWord(Settings::highlightClickedWord());
    m_pane->setPaneVisibility(Settings::paneVisibility());
    m_pane->setTableView(Settings::tableView());
    m_pane->setZoom(Settings::zoomPercent());
    m_files->setMarkColor(Settings::markColor());
    m_files->setFont(Settings::fileListFont());
    DiffFormat::setHtmlFont(Settings::clipboardFont().family(), Settings::clipboardFont().pointSizeF());
    m_search = new SearchBar;
    m_search->hide();

    auto *right = new QWidget;
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    rightLayout->addWidget(createCommitBar());
    rightLayout->addWidget(m_pane, 1);
    rightLayout->addWidget(m_search);

    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(createFilePanel());
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({300, 1000});
    splitter->setChildrenCollapsible(false);
    setCentralWidget(splitter);

    createActions();
    createToolBar();
    m_nextAct->setIcon(arrowIcon(false, palette().color(QPalette::ButtonText)));
    m_prevAct->setIcon(arrowIcon(true, palette().color(QPalette::ButtonText)));
    Shortcuts::registerWindow(this, {m_recentMenu, m_languageMenu});
    syncOptionControls();
    updateViewToggle();

    m_voteButton = new QToolButton;
    m_voteButton->setAutoRaise(true);
    m_voteButton->setPopupMode(QToolButton::InstantPopup);
    m_voteButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_voteMenu = new QMenu(m_voteButton);
    m_voteButton->setMenu(m_voteMenu);
    m_voteButton->hide();
    statusBar()->addPermanentWidget(m_voteButton);
    m_busyBar = new QProgressBar;
    m_busyBar->setMaximumWidth(260);
    m_busyBar->setMaximumHeight(18);
    m_busyBar->setTextVisible(true);
    m_busyBar->hide();
    statusBar()->addPermanentWidget(m_busyBar);
    m_summary = new QLabel;
    statusBar()->addPermanentWidget(m_summary);

    connect(m_files, &FileList::fileActivated, this, &MainWindow::showFile);
    connect(m_files, &FileList::markToggled, this, &MainWindow::onReviewToggled);
    connect(m_files, &FileList::contextMenuRequested, this, &MainWindow::showFileContextMenu);
    connect(m_files, &FileList::viewRequested, this, [this](int) {
        for (CodeView *v : m_pane->findChildren<CodeView *>())
            if (v->isVisible()) {
                v->setFocus();
                break;
            }
    });
    connect(m_pane, &DiffPane::modeChanged, this, [this](DiffPane::Mode) { updateViewToggle(); });
    connect(m_pane, &DiffPane::modifiedChanged, this, [this] {
        updateEditActions();
        updateTitle();
    });
    connect(m_pane, &DiffPane::copyChangeRequested, this, &MainWindow::copyChange);
    connect(m_pane, &DiffPane::blameCommitRequested, this, &MainWindow::openBlameCommit);
    connect(m_pane, &DiffPane::lineOpRequested, this, &MainWindow::applyLineOp);
    connect(m_pane, &DiffPane::hint, this, [this](const QString &text) { statusBar()->showMessage(text, 10000); });
    connect(m_pane, &DiffPane::alignRequested, this, [this](int oldLine, int newLine) {
        if (m_shownIndex < 0 || m_shownIndex >= m_changes.size() || !maybeKeepEdits())
            return;
        auto &anchors = m_anchors[scrollKey(m_changes[m_shownIndex])];
        anchors.removeIf([&](const std::pair<int, int> &a) { return a.first == oldLine || a.second == newLine; });
        anchors.push_back({oldLine, newLine});
        reloadCurrentFile();
        statusBar()->showMessage(tr("Aligned left line %1 with right line %2").arg(oldLine).arg(newLine), 4000);
    });
    connect(m_pane, &DiffPane::clearAlignmentsRequested, this, [this] {
        if (m_shownIndex < 0 || m_shownIndex >= m_changes.size() || !maybeKeepEdits())
            return;
        m_anchors.remove(scrollKey(m_changes[m_shownIndex]));
        reloadCurrentFile();
    });
    connect(m_pane, &DiffPane::showLargeRequested, this, [this] {
        const int index = m_shownIndex;
        m_showLarge.insert(index);
        m_shownIndex = -1;
        showFile(index);
    });
    connect(m_pane, &DiffPane::commentRequested, this, &MainWindow::addComment);
    connect(m_pane, &DiffPane::replyRequested, this, &MainWindow::replyToThread);
    connect(m_pane, &DiffPane::threadStatusRequested, this, &MainWindow::setThreadStatus);
    connect(m_search, &SearchBar::stopRequested, this, [this] {
        if (m_searchCancel)
            *m_searchCancel = true;
    });
    connect(m_pane, &DiffPane::searchResultsChanged, this, &MainWindow::updateSearchResult);

    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(200);
    connect(m_searchTimer, &QTimer::timeout, this, &MainWindow::runSearch);
    connect(m_search, &SearchBar::queryChanged, m_searchTimer, qOverload<>(&QTimer::start));
    connect(m_search, &SearchBar::next, this, [this] { searchNext(true); });
    connect(m_search, &SearchBar::previous, this, [this] { searchNext(false); });
    connect(m_search, &SearchBar::closed, this, [this] {
        m_pane->clearSearch();
        m_matchCounts.clear();
        m_files->setMatchCounts({});
        m_pane->setFocus();
    });

    notePullRequest();

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(2000);
    connect(m_pollTimer, &QTimer::timeout, this, &MainWindow::pollForChanges);
    if (Settings::autoReload())
        m_pollTimer->start();

    updateModeUi();
    updateTitle();

    QSettings s;
    if (!restoreGeometry(s.value(QStringLiteral("window/geometry")).toByteArray()))
        resize(1400, 900);
    splitter->restoreState(s.value(QStringLiteral("window/splitter")).toByteArray());

    // Files and folders dropped anywhere on the window open a comparison.
    setAcceptDrops(true);
    for (QAbstractScrollArea *w : findChildren<QAbstractScrollArea *>())
        w->viewport()->installEventFilter(this);

    rememberRecent();
    reload();

    // Automatic updates: at most one check a day, a little after start-up.
    if (Settings::autoUpdate() && !m_sync && !qEnvironmentVariableIsSet("YADIFF_NO_UPDATE_CHECK")
        && QDateTime::currentSecsSinceEpoch() - Settings::lastUpdateCheck() > 20 * 60 * 60)
        QTimer::singleShot(5000, this, [this] { checkForUpdates(false); });
}

QString MainWindow::reviewRoot() const
{
    if (m_cmp.mode == Comparison::Mode::Paths)
        return m_cmp.leftPath + u'\n' + m_cmp.rightPath;
    return m_repo.root();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!maybeKeepEdits()) {
        event->ignore();
        return;
    }
    qCInfo(lcApp) << "window closed";
    QSettings s;
    s.setValue(QStringLiteral("window/geometry"), saveGeometry());
    if (auto *splitter = qobject_cast<QSplitter *>(centralWidget()))
        s.setValue(QStringLiteral("window/splitter"), splitter->saveState());
    QMainWindow::closeEvent(event);
}

QString MainWindow::describeComparison() const
{
    QString d;
    if (!m_cmp.title.isEmpty())
        d = m_cmp.title;
    else if (m_cmp.mode == Comparison::Mode::Paths)
        d = QStringLiteral("%1 ↔ %2").arg(homeShortened(m_cmp.leftPath), homeShortened(m_cmp.rightPath));
    else if (m_cmp.mode == Comparison::Mode::WorkingTree)
        d = m_cmp.allChanges ? tr("all changes vs HEAD") : tr("staged and unstaged changes");
    else
        d = QStringLiteral("%1 ↔ %2").arg(m_cmp.rev1Name, m_cmp.rev2.isEmpty() ? tr("working tree") : m_cmp.rev2Name);
    if (!m_cmp.paths.isEmpty())
        d += QStringLiteral(" — ") + m_cmp.paths.join(QStringLiteral(", "));
    return d;
}

void MainWindow::updateTitle()
{
    QString name;
    if (m_cmp.empty) {
        setWindowTitle(m_customTitle.isEmpty() ? tr("yadiff") : m_customTitle);
        return;
    }
    if (m_cmp.mode == Comparison::Mode::Paths)
        name = tr("compare");
    else
        name = m_repo.isBare() ? QDir(m_repo.root()).dirName().chopped(4) : QDir(m_repo.root()).dirName();
    const QString dirty = m_pane->isModified() ? QStringLiteral("* ") : QString();
    if (!m_customTitle.isEmpty()) {
        setWindowTitle(dirty + m_customTitle);
        return;
    }
    setWindowTitle(QStringLiteral("%1yadiff — %2 — %3").arg(dirty, name, describeComparison()));
}

void MainWindow::createActions()
{
    // ---- File --------------------------------------------------------------
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    QAction *newAct = fileMenu->addAction(QIcon::fromTheme(QStringLiteral("document-new")), tr("&New comparison…"), this,
                                          &MainWindow::newComparison);
    newAct->setShortcut(QKeySequence::New);
    m_historyAct = fileMenu->addAction(QIcon::fromTheme(QStringLiteral("view-history")), tr("Compare &commits…"),
                                       this, &MainWindow::openHistory);
    m_historyAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
    m_historyAct->setToolTip(tr("Pick commits from the history to compare (Ctrl+L)"));
    QAction *prAct = fileMenu->addAction(tr("Open &pull request\u2026"), this, &MainWindow::openPullRequest);
    prAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    prAct->setStatusTip(tr("The pull requests waiting for your review on Azure DevOps and GitHub, or any PR's link"));
    m_recentMenu = fileMenu->addMenu(tr("&Recent comparisons"));
    connect(m_recentMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildRecentMenu);
    fileMenu->addAction(tr("Compare &text\u2026"), this, &MainWindow::compareText)
        ->setStatusTip(tr("Paste or type two texts and compare them"));
    fileMenu->addAction(tr("Open &patch\u2026"), this, &MainWindow::openPatch)
        ->setStatusTip(tr("Show a .patch or .diff file, on top of the files it applies to when they are there"));
    m_applyPatchAct = fileMenu->addAction(tr("&Apply patch\u2026"), this, &MainWindow::applyPatch);
    m_applyPatchAct->setStatusTip(tr("Apply the patch shown to the files it was made for (git apply)"));
    fileMenu->addAction(tr("Save as patc&h\u2026"), this, &MainWindow::saveAsPatch)
        ->setStatusTip(tr("Write the whole comparison as a patch file that git apply takes"));
    fileMenu->addSeparator();
    fileMenu->addAction(tr("Open sessi&on\u2026"), this, &MainWindow::openSession);
    fileMenu->addAction(tr("Sa&ve session\u2026"), this, &MainWindow::saveSession)
        ->setStatusTip(tr("Save this comparison, the file shown, the view and the diff options to reopen later"));
    fileMenu->addAction(tr("Change window &title…"), this, &MainWindow::changeWindowTitle);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("Edit &globbing ignore patterns…"), this, &MainWindow::editIgnorePatterns)
        ->setProperty("yadiffSetting", true);
    m_useGlobsAct = fileMenu->addAction(tr("&Use globbing ignore patterns"));
    m_useGlobsAct->setCheckable(true);
    m_useGlobsAct->setChecked(Settings::useIgnoreGlobs());
    connect(m_useGlobsAct, &QAction::toggled, this, [this](bool on) {
        Settings::setUseIgnoreGlobs(on);
        m_cmp.useIgnoreGlobs = on;
        m_cmp.ignoreGlobs = Settings::ignoreGlobs();
        if (maybeKeepEdits())
            reload();
    });
    fileMenu->addSeparator();
    m_stageAct = fileMenu->addAction(tr("&Stage files"), this, [this] { stageSelected(true); });
    m_stageAct->setStatusTip(tr("git add the selected files"));
    m_unstageAct = fileMenu->addAction(tr("U&nstage files"), this, [this] { stageSelected(false); });
    m_revertAct = fileMenu->addAction(tr("Re&vert files…"), this, &MainWindow::revertSelected);
    m_resolveAct = fileMenu->addAction(tr("Resolve &conflict\u2026"), this, &MainWindow::resolveConflict);
    m_resolveAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    m_resolveAct->setStatusTip(tr("Open the conflicted file in the three-way merge window"));
    m_revertAct->setStatusTip(tr("Make the selected files' right side match their left side (asks first)"));
    fileMenu->addSeparator();
    QAction *rescan = fileMenu->addAction(tr("Rescan &files"), this, [this] {
        if (!maybeKeepEdits())
            return;
        m_diffCache.clear();
        m_scrollMemory.clear();
        reload();
    });
    rescan->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F5));
    fileMenu->addAction(tr("&Download files again"), this, [this] { downloadAgain(false); })
        ->setStatusTip(tr("Fetch the pull request again (picks up new pushes) and reload"));
    QAction *refresh = fileMenu->addAction(tr("&Refresh list"), this, [this] {
        if (maybeKeepEdits())
            reload();
    });
    refresh->setShortcut(QKeySequence(Qt::Key_F5));
    fileMenu->addSeparator();
    QAction *lightDark = fileMenu->addAction(tr("Switch &light / dark"), this, [this] {
        const QString key = Theme::toggleLightDark();
        applyTheme();
        for (const Theme::Def &d : Theme::all())
            if (d.key == key)
                statusBar()->showMessage(tr("Theme: %1").arg(d.label), 2000);
    });
    lightDark->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D));
    m_swapAct = fileMenu->addAction(tr("S&wap right and left"), this, &MainWindow::swapSides);
    m_swapAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_X));
    QAction *exportAct = fileMenu->addAction(tr("&Export HTML report…"), this, &MainWindow::exportReport);
    exportAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
    exportAct->setStatusTip(tr("Save the whole comparison as one self-contained HTML page"));
    fileMenu->addSeparator();
    QAction *quitAct = fileMenu->addAction(tr("E&xit"), this, &QWidget::close);
    quitAct->setShortcut(QKeySequence::Quit);

    // ---- Edit --------------------------------------------------------------
    QMenu *editMenu = menuBar()->addMenu(tr("&Edit"));
    auto copyAs = [this](DiffPane::CopyKind kind) {
        if (!m_pane->copySelection(kind))
            statusBar()->showMessage(tr("Select some text first"), 2000);
    };
    QAction *copy = editMenu->addAction(tr("&Copy"), this, [copyAs] { copyAs(DiffPane::CopyKind::Rich); });
    copy->setShortcut(QKeySequence::Copy);
    copy->setShortcutContext(Qt::WidgetWithChildrenShortcut); // text fields keep their own Ctrl+C
    copy->setStatusTip(tr("Copy with syntax colours and diff backgrounds"));
    QAction *copyPlain = editMenu->addAction(tr("Copy &without markup"), this, [copyAs] { copyAs(DiffPane::CopyKind::Plain); });
    copyPlain->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    QAction *copyMd = editMenu->addAction(tr("Copy as &Markdown"), this, [copyAs] { copyAs(DiffPane::CopyKind::Markdown); });
    copyMd->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_M));
    QAction *copyReview = editMenu->addAction(tr("Copy for code &review"), this, [copyAs] { copyAs(DiffPane::CopyKind::CodeReview); });
    copyReview->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_R));
    copyReview->setStatusTip(tr("The file and line range, then the selected code"));
    QAction *selectAll = editMenu->addAction(tr("Select &all"), this, [this] { m_pane->selectAll(); });
    selectAll->setShortcut(QKeySequence::SelectAll);
    selectAll->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    editMenu->addAction(tr("Copy &left file path"), this, [this] { copyFilePath(false); });
    editMenu->addAction(tr("Copy ri&ght file path"), this, [this] { copyFilePath(true); });
    editMenu->addAction(tr("Compare clipboard with left file"), this, [this] { compareClipboard(false); });
    editMenu->addAction(tr("Compare clipboard with right file"), this, [this] { compareClipboard(true); });
    editMenu->addSeparator();
    QAction *findAct = editMenu->addAction(tr("&Find…"), this, [this] {
        // This file, unless text is selected: a word (or part of a line) becomes what to
        // find in the file, and lines are searched in.
        if (const QString word = m_pane->selectedWord(); !word.isEmpty()) {
            m_search->setScope(SearchBar::Scope::File);
            m_search->setText(word);
        } else if (m_pane->hasSelection()) {
            m_search->setScope(SearchBar::Scope::Selection);
        } else if (m_search->isHidden()) {
            m_search->setScope(SearchBar::Scope::File);
        }
        m_search->activate();
        runSearch();
    });
    findAct->setShortcuts({QKeySequence(Qt::Key_F3), QKeySequence::Find});
    QAction *findNext = editMenu->addAction(tr("Find &next"), this, [this] { searchNext(true); });
    findNext->setShortcut(QKeySequence(Qt::Key_F8));
    QAction *findPrev = editMenu->addAction(tr("Find pre&vious"), this, [this] { searchNext(false); });
    findPrev->setShortcut(QKeySequence(Qt::Key_F7));
    m_clickWordAct = editMenu->addAction(tr("&Highlight clicked word"));
    m_clickWordAct->setCheckable(true);
    m_clickWordAct->setChecked(Settings::highlightClickedWord());
    connect(m_clickWordAct, &QAction::toggled, this, [this](bool on) {
        Settings::setHighlightClickedWord(on);
        m_pane->setHighlightClickedWord(on);
    });
    QAction *goTo = editMenu->addAction(tr("&Go to line…"), this, &MainWindow::goToLineDialog);
    goTo->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
    editMenu->addSeparator();
    editMenu->addAction(tr("Set &editor…"), this, &MainWindow::setEditorCommand)->setProperty("yadiffSetting", true);
    m_editLeftAct = editMenu->addAction(tr("Edit le&ft file"), this, [this] { editExternally(false); });
    m_editRightAct = editMenu->addAction(tr("Edit righ&t file"), this, [this] { editExternally(true); });
    editMenu->addSeparator();
    m_editAct = editMenu->addAction(QIcon::fromTheme(QStringLiteral("document-edit")), tr("Edit right side &in place"));
    m_editAct->setProperty("yadiffNotASetting", true); // a mode, not a setting
    m_editAct->setCheckable(true);
    m_editAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    m_editAct->setToolTip(tr("Edit the right-hand file directly in yadiff (Ctrl+E). Ctrl+S saves."));
    connect(m_editAct, &QAction::triggered, this, &MainWindow::toggleEditing);
    m_saveAct = editMenu->addAction(QIcon::fromTheme(QStringLiteral("document-save")), tr("&Save"), this, [this] {
        if (saveEdits())
            statusBar()->showMessage(tr("Saved"), 3000);
    });
    m_saveAct->setShortcut(QKeySequence::Save);
    m_discardAct = editMenu->addAction(tr("&Discard edits"), this, [this] {
        m_pane->setEditing(false);
        reloadCurrentFile();
        updateEditActions();
        updateTitle();
    });
    m_copyRightAct = editMenu->addAction(tr("Apply current change's old version to the right"), this, [this] {
        copyChange(m_pane->currentChange(), true);
    });
    m_copyRightAct->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Right));
    m_copyLeftAct = editMenu->addAction(tr("Apply current change's new version to the left"), this, [this] {
        copyChange(m_pane->currentChange(), false);
    });
    m_copyLeftAct->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Left));
    editMenu->addSeparator();
    m_stageLinesAct = editMenu->addAction(tr("Stage current change or selected lines"), this,
                                          [this] { applyLineOp(GitRepo::LineOp::Stage, m_pane->linesForLineOp()); });
    m_stageLinesAct->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_S));
    m_unstageLinesAct = editMenu->addAction(tr("Unstage current change or selected lines"), this,
                                            [this] { applyLineOp(GitRepo::LineOp::Unstage, m_pane->linesForLineOp()); });
    m_unstageLinesAct->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_S));
    m_discardLinesAct = editMenu->addAction(tr("Discard current change or selected lines\u2026"), this,
                                            [this] { applyLineOp(GitRepo::LineOp::Discard, m_pane->linesForLineOp()); });
    editMenu->addSeparator();
    m_nextAct = editMenu->addAction(QIcon::fromTheme(QStringLiteral("go-down"), style()->standardIcon(QStyle::SP_ArrowDown)),
                                    tr("Ne&xt change"), this, &MainWindow::nextChange);
    m_nextAct->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Down));
    m_nextAct->setToolTip(tr("Next change (Alt+Down); after the last one, the next file"));
    m_prevAct = editMenu->addAction(QIcon::fromTheme(QStringLiteral("go-up"), style()->standardIcon(QStyle::SP_ArrowUp)),
                                    tr("Pre&vious change"), this, &MainWindow::prevChange);
    m_prevAct->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
    m_prevAct->setToolTip(tr("Previous change (Alt+Up)"));
    QAction *nextFile = editMenu->addAction(tr("Next file"), this, [this] { m_files->selectNext(); });
    nextFile->setShortcut(QKeySequence(Qt::ALT | Qt::Key_PageDown));
    QAction *prevFile = editMenu->addAction(tr("Previous file"), this, [this] { m_files->selectPrev(); });
    m_nextCommitAct = editMenu->addAction(tr("Next commit"), this, [this] { stepCommit(1); });
    m_nextCommitAct->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_PageDown));
    m_nextCommitAct->setStatusTip(tr("Step to the next (newer) commit of the range or history"));
    m_prevCommitAct = editMenu->addAction(tr("Previous commit"), this, [this] { stepCommit(-1); });
    m_prevCommitAct->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_PageUp));
    m_prevCommitAct->setStatusTip(tr("Step to the previous (older) commit of the range or history"));
    prevFile->setShortcut(QKeySequence(Qt::ALT | Qt::Key_PageUp));

    // ---- View --------------------------------------------------------------
    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    m_toggleViewAct = viewMenu->addAction(tr("&Toggle inline / side by side"), this,
                                          [this] { setSideBySide(m_pane->mode() != DiffPane::Mode::SideBySide); });
    m_toggleViewAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_T));
    auto *modeGroup = new QActionGroup(this);
    m_inlineAct = viewMenu->addAction(tr("&Inline"), this, [this] { setSideBySide(false); });
    m_sideAct = viewMenu->addAction(tr("&Side by side"), this, [this] { setSideBySide(true); });
    for (QAction *a : {m_inlineAct, m_sideAct}) {
        a->setCheckable(true);
        modeGroup->addAction(a);
    }
    m_inlineAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_1));
    m_sideAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_2));
    viewMenu->addSeparator();
    m_paneGroup = new QActionGroup(this);
    const QString paneNames[] = {tr("Show &both"), tr("Show &left only"), tr("Show &right only")};
    QAction *paneActs[3];
    for (int i : {1, 2, 0}) {
        QAction *a = viewMenu->addAction(paneNames[i]);
        a->setCheckable(true);
        a->setData(i);
        a->setChecked(Settings::paneVisibility() == i);
        m_paneGroup->addAction(a);
        paneActs[i] = a;
        connect(a, &QAction::triggered, this, [this, i] {
            Settings::setPaneVisibility(i);
            if (i != 0 && m_pane->mode() != DiffPane::Mode::SideBySide)
                setSideBySide(true);
            m_pane->setPaneVisibility(i);
        });
    }
    paneActs[1]->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_1));
    paneActs[2]->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_2));
    paneActs[0]->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_3));
    viewMenu->addSeparator();
    m_languageMenu = viewMenu->addMenu(tr("&Language"));
    m_languageMenu->setEnabled(Syntax::available());
    m_syntaxAct = viewMenu->addAction(tr("Synta&x highlighting"));
    m_syntaxAct->setCheckable(true);
    m_syntaxAct->setChecked(Settings::syntaxEnabled());
    m_syntaxAct->setEnabled(Syntax::available());
    connect(m_syntaxAct, &QAction::toggled, this, [this](bool on) {
        Settings::setSyntaxEnabled(on);
        m_pane->setSyntaxEnabled(on);
    });
    m_showWhitespaceAct = viewMenu->addAction(tr("Show &whitespace"));
    m_showWhitespaceAct->setCheckable(true);
    m_showWhitespaceAct->setChecked(Settings::showWhitespace());
    m_showWhitespaceAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_8));
    connect(m_showWhitespaceAct, &QAction::toggled, this, [this](bool on) {
        Settings::setShowWhitespace(on);
        m_pane->setShowWhitespace(on);
    });
    m_lineNumbersAct = viewMenu->addAction(tr("Show line &numbers"));
    m_lineNumbersAct->setCheckable(true);
    m_lineNumbersAct->setChecked(Settings::showLineNumbers());
    m_lineNumbersAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L));
    connect(m_lineNumbersAct, &QAction::toggled, this, [this](bool on) {
        Settings::setShowLineNumbers(on);
        m_pane->setShowLineNumbers(on);
    });
    m_blameAct = viewMenu->addAction(tr("Git &blame"));
    m_blameAct->setCheckable(true);
    m_blameAct->setChecked(Settings::showBlame());
    m_blameAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B));
    m_blameAct->setStatusTip(tr("Show who last changed each line; click a commit to open it"));
    connect(m_blameAct, &QAction::toggled, this, [this](bool on) {
        Settings::setShowBlame(on);
        if (on)
            loadBlame();
        else
            m_pane->setBlame({}, {});
    });
    m_wordWrapAct = viewMenu->addAction(tr("Word w&rap"));
    m_wordWrapAct->setCheckable(true);
    m_wordWrapAct->setChecked(Settings::wordWrap());
    m_wordWrapAct->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Z));
    connect(m_wordWrapAct, &QAction::toggled, this, [this](bool on) {
        Settings::setWordWrap(on);
        m_pane->setWordWrap(on);
    });
    QAction *tables = viewMenu->addAction(tr("Show CSV and TSV files as &tables"));
    tables->setCheckable(true);
    tables->setChecked(Settings::tableView());
    tables->setStatusTip(tr("Compare .csv and .tsv files row by row and cell by cell"));
    connect(tables, &QAction::toggled, this, [this](bool on) {
        Settings::setTableView(on);
        m_pane->setTableView(on);
        updateEditActions();
    });
    connect(m_pane, &DiffPane::textViewRequested, tables, [tables] { tables->setChecked(false); });
    QAction *hex = viewMenu->addAction(tr("Show binary files as &hex"));
    hex->setCheckable(true);
    hex->setChecked(Settings::hexBinary());
    hex->setStatusTip(tr("Compare binary files byte by byte, as a hex dump"));
    connect(hex, &QAction::toggled, this, [this](bool on) {
        Settings::setHexBinary(on);
        m_diffCache.clear();
        startPrefetch();
        reloadCurrentFile();
    });
    QMenu *tabMenu = viewMenu->addMenu(tr("Ta&b width"));
    auto *tabGroup = new QActionGroup(this);
    for (int w : {2, 3, 4, 8}) {
        QAction *a = tabMenu->addAction(QString::number(w));
        a->setCheckable(true);
        a->setChecked(Settings::tabWidth() == w);
        tabGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, w] {
            Settings::setTabWidth(w);
            m_pane->applySettings();
        });
    }
    QAction *otherTab = tabMenu->addAction(tr("Other…"), this, [this] {
        bool ok = false;
        const int w = QInputDialog::getInt(this, tr("Tab width"), tr("Spaces per tab:"), Settings::tabWidth(), 1, 16, 1, &ok);
        if (ok) {
            Settings::setTabWidth(w);
            m_pane->applySettings();
        }
    });
    otherTab->setProperty("yadiffSetting", true);
    QMenu *zoomMenu = viewMenu->addMenu(tr("&Zoom"));
    auto zoomTo = [this](int percent) {
        Settings::setZoomPercent(percent);
        m_pane->setZoom(Settings::zoomPercent());
        statusBar()->showMessage(tr("Zoom %1%").arg(Settings::zoomPercent()), 1500);
    };
    QAction *zoomIn = zoomMenu->addAction(tr("Zoom &in"), this, [zoomTo] { zoomTo(Settings::zoomPercent() + 10); });
    zoomIn->setShortcuts({QKeySequence::ZoomIn, QKeySequence(Qt::CTRL | Qt::Key_Equal)});
    QAction *zoomOut = zoomMenu->addAction(tr("Zoom &out"), this, [zoomTo] { zoomTo(Settings::zoomPercent() - 10); });
    zoomOut->setShortcut(QKeySequence::ZoomOut);
    QAction *zoomReset = zoomMenu->addAction(tr("&100%"), this, [zoomTo] { zoomTo(100); });
    zoomReset->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    viewMenu->addSeparator();
    m_collapseAct = viewMenu->addAction(tr("&Collapse unchanged regions"));
    m_collapseAct->setCheckable(true);
    m_collapseAct->setChecked(Settings::collapseUnchanged());
    m_collapseAct->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_U));
    connect(m_collapseAct, &QAction::toggled, this, [this](bool on) {
        Settings::setCollapseUnchanged(on);
        m_pane->setCollapseUnchanged(on);
        m_expandAllAct->setEnabled(on);
    });
    m_expandAllAct = viewMenu->addAction(tr("E&xpand all unchanged regions"), m_pane, &DiffPane::expandAll);
    m_expandAllAct->setEnabled(Settings::collapseUnchanged());
    m_expandCommentsAct = viewMenu->addAction(tr("Expand all review c&omments"), this,
                                              [this] { m_pane->setCommentsExpanded(true); });
    m_expandCommentsAct->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_O));
    m_collapseCommentsAct = viewMenu->addAction(tr("Collapse all review comments"), this,
                                                [this] { m_pane->setCommentsExpanded(false); });
    m_collapseCommentsAct->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_O));
    m_overviewAct = viewMenu->addAction(tr("Pull request o&verview"));
    m_overviewAct->setCheckable(true);
    m_overviewAct->setChecked(Settings::showPrOverview());
    m_overviewAct->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_V));
    m_overviewAct->setStatusTip(tr("Show the pull request's description, reviewers, checks and comments above the files"));
    connect(m_overviewAct, &QAction::toggled, this, [this](bool on) {
        Settings::setShowPrOverview(on);
        updateOverviewVisibility();
        if (on && !m_overview)
            loadOverview();
    });
    m_hideResolvedAct = viewMenu->addAction(tr("&Hide resolved review threads"));
    m_hideResolvedAct->setCheckable(true);
    m_hideResolvedAct->setChecked(Settings::hideResolvedThreads());
    m_hideResolvedAct->setStatusTip(tr("Show only the threads still to deal with: active, pending, or without a status"));
    connect(m_hideResolvedAct, &QAction::toggled, this, [this](bool on) {
        Settings::setHideResolvedThreads(on);
        refreshThreads();
    });
    m_trimPrefixAct = viewMenu->addAction(tr("Trim common &prefix from long folder names"));
    m_trimPrefixAct->setCheckable(true);
    m_trimPrefixAct->setChecked(Settings::trimFolderPrefix());
    connect(m_trimPrefixAct, &QAction::toggled, this, [this](bool on) {
        Settings::setTrimFolderPrefix(on);
        m_files->setFolderDisplay(m_files->property("folderRoot").toString(), on);
    });
    viewMenu->addSeparator();
    m_themeMenu = viewMenu->addMenu(tr("Color t&heme"));
    auto *themeGroup = new QActionGroup(this);
    for (const Theme::Def &d : Theme::all()) {
        QAction *a = m_themeMenu->addAction(d.label);
        a->setCheckable(true);
        a->setData(d.key);
        a->setChecked(d.key == Theme::current());
        themeGroup->addAction(a);
        if (d.key == QLatin1String("system"))
            m_themeMenu->addSeparator();
        connect(a, &QAction::triggered, this, [this, key = d.key] {
            Theme::apply(key);
            applyTheme();
        });
    }
    QMenu *fonts = viewMenu->addMenu(tr("F&onts"));
    auto pickFont = [this](const QString &title, const QFont &current, bool monospaced, auto store) {
        bool ok = false;
        const QFont f = QFontDialog::getFont(&ok, current, this, title,
                                             monospaced ? QFontDialog::MonospacedFonts : QFontDialog::FontDialogOptions());
        if (ok) {
            store(f);
            applyFonts();
        }
    };
    fonts->addAction(tr("&Diff font\u2026"), this, [pickFont] {
        pickFont(tr("Diff font"), Settings::font(), true, [](const QFont &f) { Settings::setFont(f); });
    })->setProperty("yadiffSetting", true);
    fonts->addAction(tr("&File list font\u2026"), this, [pickFont] {
        pickFont(tr("File list font"), Settings::fileListFont(), false, [](const QFont &f) { Settings::setFileListFont(f); });
    })->setProperty("yadiffSetting", true);
    fonts->addAction(tr("&Clipboard font\u2026"), this, [pickFont] {
        pickFont(tr("Font for copied text"), Settings::clipboardFont(), false, [](const QFont &f) { Settings::setClipboardFont(f); });
    });
    fonts->actions().constLast()->setStatusTip(tr("The font used when copying with formatting (Copy, Copy this change as diff)"));
    fonts->actions().constLast()->setProperty("yadiffSetting", true);
    viewMenu->addAction(tr("Diff &colors and tab width\u2026"), this, &MainWindow::openPreferences)
        ->setProperty("yadiffSetting", true);

    // ---- Options -----------------------------------------------------------
    QMenu *optionsMenu = menuBar()->addMenu(tr("&Options"));
    QAction *settingsAct = optionsMenu->addAction(tr("&Settings and keyboard shortcuts\u2026"), this, &MainWindow::openSettings);
    settingsAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    settingsAct->setStatusTip(tr("Every option and keyboard shortcut in one place; change shortcuts there"));
    optionsMenu->addSeparator();
    const struct {
        const char *key;
        QString label;
    } categories[] = {{"leftOnly", tr("Show &left-only files")},   {"rightOnly", tr("Show &right-only files")},
                      {"different", tr("Show &different files")}, {"identical", tr("Show &identical files")},
                      {"blankOnly", tr("Show &blanks-only files")}};
    for (const auto &c : categories) {
        QAction *a = optionsMenu->addAction(c.label);
        a->setCheckable(true);
        a->setChecked(Settings::showFileCategory(QLatin1String(c.key)));
        const QString key = QLatin1String(c.key);
        m_categoryActs.insert(key, a);
        connect(a, &QAction::toggled, this, [this, key](bool on) {
            Settings::setShowFileCategory(key, on);
            if (key == QLatin1String("identical")) {
                // Identical files have to be listed first; the others are a filter.
                m_cmp.includeIdentical = on;
                if (maybeKeepEdits())
                    reload();
            } else {
                applyFilter();
            }
        });
    }
    m_categoryActs.value(QStringLiteral("blankOnly"))->setStatusTip(tr("Files whose only differences are blank lines"));
    optionsMenu->addAction(tr("Show &all files"), this, [this] {
        for (QAction *a : std::as_const(m_categoryActs))
            a->setChecked(true);
    });
    optionsMenu->addAction(tr("Show &no files"), this, [this] {
        for (QAction *a : std::as_const(m_categoryActs))
            a->setChecked(false);
    });
    optionsMenu->addSeparator();
    auto addToggle = [&](QMenu *menu, const QString &text, const QString &tip, bool DiffOptions::*field) {
        QAction *act = menu->addAction(text);
        act->setCheckable(true);
        act->setStatusTip(tip);
        connect(act, &QAction::triggered, this, [this, field](bool on) {
            DiffOptions o = m_cmp.options;
            o.*field = on;
            setDiffOptions(o);
        });
        return act;
    };
    m_ignoreAllSpaceAct = addToggle(optionsMenu, tr("Ignore all &whitespace"),
                                    tr("Ignore whitespace entirely when comparing lines (git diff -w)"), &DiffOptions::ignoreAllSpace);
    m_ignoreAllSpaceAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W));
    m_ignoreSpaceChangeAct = addToggle(optionsMenu, tr("Ignore &extra whitespace"),
                                       tr("Treat runs of whitespace as one space (git diff -b)"), &DiffOptions::ignoreSpaceChange);
    m_ignoreTrimAct = addToggle(optionsMenu, tr("Ignore &leading/trailing whitespace"),
                                tr("Ignore whitespace at the start and end of lines"), &DiffOptions::ignoreTrim);
    m_ignoreBlankLinesAct = addToggle(optionsMenu, tr("Ignore blan&k lines"),
                                      tr("Ignore lines that are added or removed and are blank"), &DiffOptions::ignoreBlankLines);
    m_ignoreCrAct = addToggle(optionsMenu, tr("Ignore C&R at end of line"), tr("Treat CRLF and LF line endings as equal"),
                              &DiffOptions::ignoreCrAtEol);
    m_ignoreCaseAct = addToggle(optionsMenu, tr("Ignore c&ase"), tr("Treat upper and lower case letters as equal"),
                                &DiffOptions::ignoreCase);
    m_ignoreCommentsAct = addToggle(optionsMenu, tr("Ignore c&omments"),
                                    tr("Comments don't count as changes (the comment syntax comes from the file type)"),
                                    &DiffOptions::ignoreComments);
    m_ignoreUnimportantAct = optionsMenu->addAction(tr("Ignore &unimportant text"));
    m_ignoreUnimportantAct->setCheckable(true);
    m_ignoreUnimportantAct->setStatusTip(tr("Text matching your unimportant-text rules doesn't count as a change"));
    connect(m_ignoreUnimportantAct, &QAction::triggered, this, [this](bool on) {
        if (on && Settings::unimportantPatterns().isEmpty()) {
            editUnimportantText(); // nothing to ignore yet: ask for the rules first
            return;
        }
        DiffOptions o = m_cmp.options;
        o.unimportantPatterns = on ? Settings::unimportantPatterns() : QStringList();
        setDiffOptions(o);
    });
    optionsMenu->addAction(tr("Unimportant text &rules\u2026"), this, &MainWindow::editUnimportantText)
        ->setProperty("yadiffSetting", true);
    QMenu *algoMenu = optionsMenu->addMenu(tr("Diff &algorithm"));
    m_algoGroup = new QActionGroup(this);
    for (DiffAlgorithm a : allAlgorithms()) {
        QAction *act = algoMenu->addAction(algorithmLabel(a));
        act->setCheckable(true);
        act->setData(int(a));
        act->setStatusTip(algorithmDescription(a));
        act->setToolTip(algorithmDescription(a));
        m_algoGroup->addAction(act);
        connect(act, &QAction::triggered, this, [this, a] {
            DiffOptions o = m_cmp.options;
            o.algorithm = a;
            setDiffOptions(o);
        });
    }
    algoMenu->setToolTipsVisible(true);
    m_detectMovesAct = addToggle(optionsMenu, tr("Detect &moved lines"), tr("Mark blocks that were moved rather than changed"),
                                 &DiffOptions::detectMoves);
    optionsMenu->addSeparator();
    m_allChangesAct = optionsMenu->addAction(tr("All changes in &one list (working tree vs HEAD)"));
    m_allChangesAct->setCheckable(true);
    m_allChangesAct->setStatusTip(tr("One list of everything that differs from HEAD, instead of staged and unstaged sections"));
    connect(m_allChangesAct, &QAction::triggered, this, &MainWindow::setAllChanges);
    m_vsHeadAct = optionsMenu->addAction(tr("Compare unstaged changes against &HEAD"));
    m_vsHeadAct->setCheckable(true);
    m_vsHeadAct->setChecked(m_cmp.unstagedAgainstHead);
    m_vsHeadAct->setStatusTip(tr("When off, unstaged changes are compared against the index, like git diff"));
    connect(m_vsHeadAct, &QAction::toggled, this, [this](bool on) {
        Settings::setUnstagedAgainstHead(on);
        m_cmp.unstagedAgainstHead = on;
        if (maybeKeepEdits())
            reload();
    });
    m_autoReloadAct = optionsMenu->addAction(tr("Reload automatically when files change on disk"));
    m_autoReloadAct->setCheckable(true);
    m_autoReloadAct->setChecked(Settings::autoReload());
    connect(m_autoReloadAct, &QAction::toggled, this, [this](bool on) {
        Settings::setAutoReload(on);
        m_lastSignature.clear(); // the next check takes a fresh baseline
        if (on)
            m_pollTimer->start();
        else
            m_pollTimer->stop();
    });
    m_rememberScrollAct = optionsMenu->addAction(tr("Remember &scroll positions for files"));
    m_rememberScrollAct->setCheckable(true);
    m_rememberScrollAct->setChecked(Settings::rememberScroll());
    connect(m_rememberScrollAct, &QAction::toggled, this, [](bool on) { Settings::setRememberScroll(on); });

    // ---- Marking -----------------------------------------------------------
    QMenu *markMenu = menuBar()->addMenu(tr("&Marking"));
    QAction *toggleMark = markMenu->addAction(tr("&Mark / unmark selected files"), this, [this] {
        const QVector<int> sel = m_files->selectedIndices();
        bool allMarked = !sel.isEmpty();
        for (int i : sel)
            allMarked = allMarked && m_files->isMarked(i);
        markSelected(!allMarked);
    });
    toggleMark->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));
    QAction *markNext = markMenu->addAction(tr("Mark and go to &next file"), this, [this] {
        const int cur = m_files->currentIndex();
        if (cur >= 0)
            m_files->setMarked(cur, true);
        m_files->selectNext();
    });
    markNext->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
    markMenu->addAction(tr("Mark by &pattern…"), this, &MainWindow::markByPattern);
    markMenu->addAction(tr("Mark &identical files"), this, &MainWindow::markIdentical)
        ->setStatusTip(tr("Mark the files that show no differences with the current options"));
    markMenu->addAction(tr("&Unmark all files"), this, [this] {
        for (int i : m_files->markedIndices())
            m_files->setMarked(i, false);
    });
    markMenu->addSeparator();
    m_hideMarkedAct = markMenu->addAction(tr("&Hide marked files"));
    m_hideMarkedAct->setCheckable(true);
    connect(m_hideMarkedAct, &QAction::toggled, this, &MainWindow::applyFilter);
    QAction *selectMarked = markMenu->addAction(tr("&Select marked files"), this, [this] { m_files->selectIndices(m_files->markedIndices()); });
    selectMarked->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
    markMenu->addAction(tr("Mark highlight &color…"), this, [this] {
        const QColor c = QColorDialog::getColor(Settings::markColor(), this, tr("Mark highlight color"),
                                                QColorDialog::ShowAlphaChannel);
        if (c.isValid()) {
            Settings::setMarkColor(c);
            m_files->setMarkColor(c);
        }
    })->setProperty("yadiffSetting", true);

    // ---- Review (pull requests only) ---------------------------------------
    m_reviewMenu = menuBar()->addMenu(tr("&Review"));
    m_reviewMenuAction = m_reviewMenu->menuAction();
    // The host's votes are inserted here (rebuildVoteActions).
    m_reviewCommentAct = m_reviewMenu->addAction(tr("&Comment on the pull request…"), this,
                                                 [this] { submitReview(GitHubReview::Verdict::Comment); });
    m_reviewMenu->addSeparator();
    m_reviewMenu->addAction(tr("&Reload comments"), this, &MainWindow::loadComments);
    m_reviewMenu->addAction(tr("Compare &pushes…"), this, &MainWindow::choosePrVersions);
    m_sinceReviewAct = m_reviewMenu->addAction(tr("Open at the changes since my last &review"));
    m_sinceReviewAct->setCheckable(true);
    m_sinceReviewAct->setChecked(Settings::openSinceLastReview());
    m_sinceReviewAct->setStatusTip(tr("When a pull request has new pushes since you last voted on it (or reviewed it on GitHub), show only those"));
    connect(m_sinceReviewAct, &QAction::toggled, this, [](bool on) { Settings::setOpenSinceLastReview(on); });
    m_reviewMenu->addAction(tr("&Open pull request in browser"), this,
                            [this] { QDesktopServices::openUrl(QUrl(PrReview::webUrl(m_cmp))); });

    // ---- Help --------------------------------------------------------------
    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&Register as git difftool…"), this, &MainWindow::registerDifftool);
    helpMenu->addAction(tr("U&nregister as git difftool"), this, &MainWindow::unregisterDifftool);
    helpMenu->addAction(tr("Register as git &mergetool\u2026"), this, [this] {
        QString summary, error;
        if (Difftool::registerMergeTool(Difftool::preferredExecutable(), true, &summary, &error))
            QMessageBox::information(this, tr("Registered"),
                                     tr("git mergetool now opens yadiff's merge window:\n\n%1").arg(summary));
        else
            QMessageBox::warning(this, tr("Register"), error);
    });
    helpMenu->addAction(tr("Unregister as git mergetool"), this, [this] {
        QString summary, error;
        if (Difftool::unregisterMergeTool(&summary, &error))
            QMessageBox::information(this, tr("Unregistered"), tr("Done:\n\n%1").arg(summary));
        else
            QMessageBox::warning(this, tr("Unregister"), error);
    });
    helpMenu->addAction(tr("Add to the &file manager's menu"), this, [this] {
        QString summary, error;
        if (ShellMenu::install(Difftool::stableExecutable(), &summary, &error))
            QMessageBox::information(this, tr("File manager menu"), summary);
        else
            QMessageBox::warning(this, tr("File manager menu"), error);
    });
    helpMenu->addAction(tr("Remove from the file manager's menu"), this, [this] {
        QString summary, error;
        if (ShellMenu::uninstall(&summary, &error))
            QMessageBox::information(this, tr("File manager menu"), summary);
        else
            QMessageBox::warning(this, tr("File manager menu"), error);
    });
    helpMenu->addSeparator();
    QAction *autoUpdate = helpMenu->addAction(tr("&Automatically install updates"));
    autoUpdate->setCheckable(true);
    autoUpdate->setChecked(Settings::autoUpdate());
    autoUpdate->setStatusTip(tr("Check GitHub for a new release once a day and install it"));
    connect(autoUpdate, &QAction::toggled, this, [this](bool on) {
        Settings::setAutoUpdate(on);
        if (on)
            checkForUpdates(false);
    });
    helpMenu->addAction(tr("&Check for update now…"), this, [this] { checkForUpdates(true); })
        ->setStatusTip(tr("Look for a new release on GitHub and offer to install it"));
    helpMenu->addSeparator();
    QAction *detailedLog = helpMenu->addAction(tr("&Detailed logging"));
    detailedLog->setCheckable(true);
    detailedLog->setChecked(Log::level() >= Log::Level::Debug);
    detailedLog->setStatusTip(tr("Also log every command's error output, HTTP headers, diff timings and the actions you take"));
    connect(detailedLog, &QAction::toggled, this, [](bool on) {
        const Log::Level level = on ? Log::Level::Debug : Log::Level::Info;
        Log::saveLevel(level);
        Log::setLevel(level);
        qCInfo(lcApp).noquote() << "log level set to" << Log::levelName(level);
    });
    helpMenu->addAction(tr("Open &log file"), this, [this] {
        const QString path = Log::filePath();
        if (path.isEmpty()) {
            QMessageBox::information(this, tr("Log"),
                                     tr("This window isn't logging to a file (--log or YADIFF_LOG chose otherwise)."));
            return;
        }
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    })->setStatusTip(tr("Open yadiff's log, to see what it did or to attach to a bug report"));
    helpMenu->addAction(tr("Show log &folder"), this, [] {
        const QString path = Log::filePath().isEmpty() ? Log::defaultFilePath() : Log::filePath();
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    });
    helpMenu->addSeparator();
    helpMenu->addAction(tr("&Usage"), this, &MainWindow::showUsage);
    helpMenu->addAction(tr("&About yadiff"), this, [this] {
        QMessageBox::about(this, tr("About yadiff"),
                           tr("<b>yadiff</b> %1<br>A graphical diff viewer for Git.<br><br>"
                              "F3 / F8 / F7: find, next, previous &nbsp; Ctrl+G: go to line<br>"
                              "Alt+Down / Alt+Up: next / previous change<br>"
                              "Ctrl+T: toggle inline / side by side &nbsp; Ctrl+M: mark file<br>"
                              "Ctrl+E: edit in place, Ctrl+S: save<br>"
                              "Right-click a change, or a file, for more.<br><br>"
                              "Log: %2")
                               .arg(QApplication::applicationVersion(),
                                    QDir::toNativeSeparators(Log::filePath().isEmpty() ? tr("(none)") : Log::filePath())
                                        .toHtmlEscaped()));
    });
}

void MainWindow::createToolBar()
{
    QToolBar *tb = addToolBar(tr("Toolbar"));
    tb->setObjectName(QStringLiteral("mainToolbar"));
    tb->setMovable(false);
    tb->setFloatable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    // Left: what is compared, and how.
    tb->addAction(m_historyAct);
    auto *reviewButton = new QToolButton;
    reviewButton->setText(tr("Review"));
    reviewButton->setMenu(m_reviewMenu);
    reviewButton->setPopupMode(QToolButton::InstantPopup);
    reviewButton->setToolTip(tr("Vote on the pull request, or comment on it"));
    m_reviewToolAction = tb->addWidget(reviewButton);
    m_versionCombo = new QComboBox;
    // Narrow in the toolbar; the list itself is as wide as its entries.
    m_versionCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_versionCombo->setMinimumContentsLength(26);
    m_versionCombo->setToolTip(tr("Which changes of the pull request to show: all of them, or those since an earlier push"));
    connect(m_versionCombo, &QComboBox::activated, this, [this](int i) {
        const QVariant v = m_versionCombo->itemData(i);
        if (!v.isValid()) {
            choosePrVersions();
            return;
        }
        showPrVersions(v.toPoint().x(), v.toPoint().y());
    });
    m_versionComboAction = tb->addWidget(m_versionCombo);
    m_versionComboAction->setVisible(false);
    m_scopeCombo = new QComboBox;
    m_scopeCombo->addItem(tr("Staged and unstaged"));
    m_scopeCombo->addItem(tr("All changes vs HEAD"));
    m_scopeCombo->setToolTip(tr("Show staged and unstaged changes separately, or everything against HEAD"));
    connect(m_scopeCombo, &QComboBox::activated, this, [this](int i) { setAllChanges(i == 1); });
    m_scopeComboAction = tb->addWidget(m_scopeCombo);
    tb->addSeparator();
    tb->addWidget(new QLabel(tr(" Algorithm: ")));
    m_algoCombo = new QComboBox;
    for (DiffAlgorithm a : allAlgorithms()) {
        m_algoCombo->addItem(algorithmLabel(a), int(a));
        m_algoCombo->setItemData(m_algoCombo->count() - 1, algorithmDescription(a), Qt::ToolTipRole);
    }
    connect(m_algoCombo, &QComboBox::activated, this, [this](int i) {
        DiffOptions o = m_cmp.options;
        o.algorithm = DiffAlgorithm(m_algoCombo->itemData(i).toInt());
        setDiffOptions(o);
    });
    tb->addWidget(m_algoCombo);
    tb->addWidget(new QLabel(tr("  Syntax: ")));
    m_syntaxCombo = new QComboBox;
    m_syntaxCombo->setMaxVisibleItems(25);
    m_syntaxCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_syntaxCombo->setMinimumContentsLength(14);
    m_syntaxCombo->addItem(tr("Auto"), QString());
    m_syntaxCombo->addItem(tr("None"), QStringLiteral("-"));
    for (const QString &lang : Syntax::languages())
        m_syntaxCombo->addItem(lang, lang);
    m_syntaxCombo->setEnabled(Syntax::available());
    m_syntaxCombo->setToolTip(Syntax::available() ? tr("Syntax highlighting: detected from the file name, or pick one")
                                                  : tr("This build has no syntax highlighting (KSyntaxHighlighting)"));
    connect(m_syntaxCombo, &QComboBox::activated, this, [this](int i) {
        const int cur = m_files->currentIndex();
        const QString suffix = cur >= 0 ? QFileInfo(m_changes[cur].path).suffix().toLower() : QString();
        const QString lang = m_syntaxCombo->itemData(i).toString();
        if (lang.isEmpty())
            m_syntaxBySuffix.remove(suffix);
        else
            m_syntaxBySuffix.insert(suffix, lang); // remembered for other files of this type
        m_pane->setLanguageOverride(lang);
        updateSyntaxCombo();
    });
    tb->addWidget(m_syntaxCombo);
    connect(m_languageMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildLanguageMenu);

    auto *spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacer);

    // Right: editing, view toggles, then change navigation.
    m_editToggle = new QToolButton;
    m_editToggle->setDefaultAction(m_editAct);
    m_editToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_editToggle->setAutoRaise(true);
    m_editAct->setIconText(tr("Edit"));
    tb->addWidget(m_editToggle);
    m_saveAct->setIconText(tr("Save"));
    tb->addAction(m_saveAct);
    tb->addSeparator();

    m_viewToggle = new QToolButton;
    m_viewToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_viewToggle->setAutoRaise(true);
    connect(m_viewToggle, &QToolButton::clicked, m_toggleViewAct, &QAction::trigger);
    tb->addWidget(m_viewToggle);

    // Same option as Diff > Ignore all whitespace (git diff -w).
    m_whitespaceToggle = new QToolButton;
    m_whitespaceToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_whitespaceToggle->setAutoRaise(true);
    m_whitespaceToggle->setCheckable(true);
    m_whitespaceToggle->setText(tr("Ignore whitespace"));
    m_whitespaceToggle->setIcon(whitespaceIcon(palette().color(QPalette::ButtonText)));
    m_whitespaceToggle->setToolTip(tr("Hide changes that only add, remove or re-indent whitespace (Ctrl+Shift+W)"));
    connect(m_whitespaceToggle, &QToolButton::clicked, m_ignoreAllSpaceAct, &QAction::trigger);
    tb->addWidget(m_whitespaceToggle);
    tb->addSeparator();
    // Change navigation, in words, with where you are between the buttons.
    m_prevAct->setIconText(tr("Previous"));
    m_nextAct->setIconText(tr("Next"));
    auto *prev = new QToolButton;
    prev->setDefaultAction(m_prevAct);
    auto *next = new QToolButton;
    next->setDefaultAction(m_nextAct);
    for (QToolButton *b : {prev, next}) {
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setAutoRaise(true);
    }
    m_changeLabel = new QLabel;
    m_changeLabel->setAlignment(Qt::AlignCenter);
    m_changeLabel->setToolTip(tr("The change shown, of the changes in this file"));
    tb->addWidget(prev);
    tb->addWidget(m_changeLabel);
    tb->addWidget(next);
    connect(m_pane, &DiffPane::currentChangeChanged, this, [this](int index, int count) {
        m_changeLabel->setText(count == 0 ? tr(" No changes ")
                                          : index < 0 ? tr(" %n changes ", nullptr, count)
                                                      : tr(" Change %1 of %2 ").arg(index + 1).arg(count));
    });
}

void MainWindow::updateModeUi()
{
    const bool working = m_cmp.mode == Comparison::Mode::WorkingTree;
    m_scopeComboAction->setVisible(working);
    m_allChangesAct->setEnabled(working);
    m_vsHeadAct->setEnabled(working);
    m_historyAct->setEnabled(m_repo.isRepository());
    m_reviewMenuAction->setVisible(m_cmp.prNumber > 0);
    m_expandCommentsAct->setVisible(m_cmp.prNumber > 0);
    m_collapseCommentsAct->setVisible(m_cmp.prNumber > 0);
    m_hideResolvedAct->setVisible(m_cmp.prNumber > 0);
    m_overviewAct->setVisible(m_cmp.prNumber > 0);
    updateOverviewVisibility();
    m_reviewToolAction->setVisible(m_cmp.prNumber > 0);
    rebuildVoteActions();
    const bool workingTree = m_repo.isRepository() && !m_repo.isBare()
                          && (working || (m_cmp.mode == Comparison::Mode::Revisions && m_cmp.rev2.isEmpty()));
    m_stageAct->setEnabled(workingTree);
    m_unstageAct->setEnabled(workingTree);
    // Revert writes the right side: the working tree (unless swapped) or a compared folder.
    m_revertAct->setEnabled((workingTree && !m_cmp.reversed) || (m_cmp.mode == Comparison::Mode::Paths && !m_cmp.readOnly));
    m_applyPatchAct->setEnabled(m_cmp.mode == Comparison::Mode::Patch);
    syncOptionControls();
    updateCommitBar();
}

// ---- Stepping through commits -------------------------------------------------

QWidget *MainWindow::createCommitBar()
{
    m_commitBar = new QWidget;
    auto *layout = new QHBoxLayout(m_commitBar);
    layout->setContentsMargins(6, 3, 6 , 3);
    m_commitPrev = new QToolButton;
    m_commitPrev->setArrowType(Qt::LeftArrow);
    m_commitPrev->setAutoRaise(true);
    m_commitNext = new QToolButton;
    m_commitNext->setArrowType(Qt::RightArrow);
    m_commitNext->setAutoRaise(true);
    m_commitCombo = new QComboBox;
    m_commitCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_commitCombo->setMinimumContentsLength(40);
    m_commitInfo = new QLabel;
    m_commitInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_commitInfo->setMinimumWidth(50);
    layout->addWidget(new QLabel(tr("Commit:")));
    layout->addWidget(m_commitPrev);
    layout->addWidget(m_commitCombo);
    layout->addWidget(m_commitNext);
    layout->addWidget(m_commitInfo, 1);
    m_commitBar->hide();
    connect(m_commitPrev, &QToolButton::clicked, this, [this] { stepCommit(-1); });
    connect(m_commitNext, &QToolButton::clicked, this, [this] { stepCommit(1); });
    connect(m_commitCombo, &QComboBox::activated, this, [this](int item) {
        showCommitStep(m_commitCombo->itemData(item).toInt());
    });
    return m_commitBar;
}

void MainWindow::updateCommitBar()
{
    if (m_steppingCommits)
        return; // the list stays while stepping through it
    m_commitList.clear();
    m_commitIndex = -1;
    m_commitRangeMode = false;
    m_rangeCmp = m_cmp;
    m_commitCombo->clear();
    m_commitBar->hide();
    m_prevCommitAct->setEnabled(false);
    m_nextCommitAct->setEnabled(false);
    const quint64 gen = ++m_commitGen;
    const bool revisions = m_repo.isRepository() && m_cmp.mode == Comparison::Mode::Revisions && !m_cmp.rev2.isEmpty();
    if (!revisions)
        return;
    struct CommitSteps {
        QVector<CommitInfo> list;
        int index = -1;
        bool range = false;
    };
    // In the background: in a large repository (or a virtualized one, where git may
    // download commits) walking history can take minutes, and the window must not wait.
    runJob<CommitSteps>(
        [repo = m_repo, cmp = m_cmp] {
            CommitSteps r;
            // A range of several commits steps through them; a single commit through its
            // neighbours. A pull request's neighbours are the target branch's history, which
            // isn't what the review is about, so a one-commit PR gets no bar.
            QByteArray parent;
            const bool hasParent = repo.run({QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("--quiet"),
                                             cmp.rev2 + QStringLiteral("^1")},
                                            &parent, nullptr);
            const bool single = hasParent ? QString::fromUtf8(parent.trimmed()) == cmp.rev1 : cmp.rev1 == repo.emptyTree();
            if (single) {
                if (cmp.prNumber == 0)
                    r.list = repo.commitNeighbours(cmp.rev2, 300, &r.index);
            } else if (repo.run({QStringLiteral("merge-base"), QStringLiteral("--is-ancestor"), cmp.rev1, cmp.rev2}, nullptr,
                                nullptr)) {
                r.list = repo.commitsInRange(cmp.rev1, cmp.rev2, 2000, cmp.paths);
                r.range = true;
                if (r.list.size() < 2)
                    r.list.clear();
            }
            return r;
        },
        [this, gen](CommitSteps r) {
            if (gen != m_commitGen)
                return; // the comparison changed meanwhile
            m_commitList = r.list;
            m_commitIndex = r.index;
            m_commitRangeMode = r.range;
            fillCommitBar();
        });
}

void MainWindow::fillCommitBar()
{
    m_commitCombo->clear();
    if (m_commitRangeMode && !m_commitList.isEmpty())
        m_commitCombo->addItem(tr("All %n commits", nullptr, int(m_commitList.size())), -1);
    for (int i = 0; i < m_commitList.size(); ++i)
        m_commitCombo->addItem(QStringLiteral("%1  %2").arg(m_commitList[i].shortOid, m_commitList[i].subject), i);
    m_commitBar->setVisible(!m_commitList.isEmpty());
    syncCommitBar();
}

void MainWindow::syncCommitBar()
{
    if (m_commitList.isEmpty())
        return;
    m_commitCombo->setCurrentIndex(m_commitCombo->findData(m_commitIndex));
    const int first = m_commitRangeMode ? -1 : 0;
    m_commitPrev->setEnabled(m_commitIndex > first);
    m_commitNext->setEnabled(m_commitIndex < m_commitList.size() - 1);
    m_commitPrev->setToolTip(tr("Previous (older) commit (Ctrl+Alt+PgUp)"));
    m_commitNext->setToolTip(tr("Next (newer) commit (Ctrl+Alt+PgDown)"));
    m_prevCommitAct->setEnabled(m_commitPrev->isEnabled());
    m_nextCommitAct->setEnabled(m_commitNext->isEnabled());
    if (m_commitIndex < 0) {
        m_commitInfo->setText(tr("%1 to %2").arg(m_rangeCmp.rev1Name, m_rangeCmp.rev2Name));
        m_commitInfo->setToolTip(QString());
        return;
    }
    const CommitInfo &c = m_commitList[m_commitIndex];
    m_commitInfo->setText(QStringLiteral("%1 · %2 · %3").arg(c.author, c.date, QString::number(m_commitIndex + 1) + u'/'
                                                                                + QString::number(m_commitList.size())));
    m_commitInfo->setToolTip(c.subject.toHtmlEscaped());
    // The full message for the tooltip, without holding up the window.
    const quint64 gen = m_commitGen;
    const int index = m_commitIndex;
    runJob<QString>([repo = m_repo, oid = c.oid] { return repo.commitMessage(oid); },
                    [this, gen, index](QString message) {
                        if (gen == m_commitGen && index == m_commitIndex && !message.isEmpty())
                            m_commitInfo->setToolTip(QStringLiteral("<pre>%1</pre>").arg(message.toHtmlEscaped()));
                    });
}

void MainWindow::stepCommit(int delta)
{
    if (m_commitList.isEmpty())
        return;
    const int first = m_commitRangeMode ? -1 : 0;
    const int target = std::clamp(m_commitIndex + delta, first, int(m_commitList.size()) - 1);
    if (target != m_commitIndex)
        showCommitStep(target);
}

void MainWindow::showCommitStep(int index)
{
    if (index == m_commitIndex || !maybeKeepEdits()) {
        syncCommitBar();
        return;
    }
    Comparison c = m_rangeCmp;
    if (index >= 0) {
        const CommitInfo &commit = m_commitList[index];
        c.rev1 = commit.parents.isEmpty() ? m_repo.emptyTree() : commit.parents.first();
        c.rev1Name = commit.parents.isEmpty() ? tr("(no parent)") : commit.shortOid + u'^';
        c.rev2 = commit.oid;
        c.rev2Name = commit.shortOid;
        c.title = QStringLiteral("%1 %2").arg(commit.shortOid, commit.subject);
        // Review comments belong to the whole pull request, not to one of its commits.
        c.prNumber = 0;
        c.prHost.clear();
    }
    m_commitIndex = index;
    m_steppingCommits = true;
    m_diffCache.clear();
    setComparison(c);
    m_steppingCommits = false;
    syncCommitBar();
}

void MainWindow::updateViewToggle()
{
    const bool side = m_pane->mode() == DiffPane::Mode::SideBySide;
    (side ? m_sideAct : m_inlineAct)->setChecked(true);
    m_viewToggle->setText(side ? tr("Side by side") : tr("Inline"));
    m_viewToggle->setIcon(viewIcon(side, palette().color(QPalette::ButtonText)));
    m_viewToggle->setToolTip(side ? tr("Showing side by side. Click to switch to inline (Ctrl+T)")
                                  : tr("Showing inline. Click to switch to side by side (Ctrl+T)"));
}

void MainWindow::updateEditActions()
{
    const bool editing = m_pane->isEditing();
    m_resolveAct->setEnabled(m_shownIndex >= 0 && m_shownIndex < m_changes.size() && m_changes[m_shownIndex].status == u'U'
                             && m_repo.isRepository() && !m_repo.isBare());
    m_editAct->setEnabled(m_pane->canEdit());
    m_editAct->setChecked(editing);
    m_saveAct->setEnabled(editing && m_pane->isModified());
    m_editLeftAct->setEnabled(m_pane->hasDiff());
    m_editRightAct->setEnabled(m_pane->hasDiff());
    m_discardAct->setEnabled(editing);
    m_copyRightAct->setEnabled(m_pane->canCopyTo(true) && m_pane->currentChange() >= 0);
    m_copyLeftAct->setEnabled(m_pane->canCopyTo(false) && m_pane->currentChange() >= 0);
    m_stageLinesAct->setEnabled(m_pane->canLineOp(GitRepo::LineOp::Stage));
    m_unstageLinesAct->setEnabled(m_pane->canLineOp(GitRepo::LineOp::Unstage));
    m_discardLinesAct->setEnabled(m_pane->canLineOp(GitRepo::LineOp::Discard));
}

void MainWindow::resolveConflict()
{
    if (m_shownIndex < 0 || m_shownIndex >= m_changes.size() || m_changes[m_shownIndex].status != u'U') {
        statusBar()->showMessage(tr("Pick a conflicted file (U) first"), 3000);
        return;
    }
    const FileChange fc = m_changes[m_shownIndex];
    // The index holds the three versions of a conflicted file: 1 base, 2 ours, 3 theirs.
    auto stage = [&](int n) {
        QByteArray out;
        m_repo.run({QStringLiteral("show"), QStringLiteral(":%1:%2").arg(n).arg(fc.path)}, &out, nullptr);
        return out;
    };
    MergeWindow::Input in;
    in.base = stage(1);
    in.ours = stage(2);
    in.theirs = stage(3);
    if (in.ours.left(8000).contains('\0') || in.theirs.left(8000).contains('\0')) {
        QMessageBox::information(this, tr("Resolve conflict"), tr("%1 is a binary file; yadiff merges text files only.").arg(fc.path));
        return;
    }
    in.outputPath = QDir(m_repo.root()).filePath(fc.path);
    in.fileName = fc.path;
    in.oursLabel = tr("Ours (HEAD)");
    in.theirsLabel = tr("Theirs");
    auto *w = new MergeWindow(in);
    w->setAttribute(Qt::WA_DeleteOnClose);
    connect(w, &MergeWindow::saved, this, [this, fc](bool resolved) {
        if (!resolved)
            return;
        // Resolved and saved: mark it so, as git add would after a merge tool.
        QString error;
        if (!m_repo.stage({fc}, &error))
            QMessageBox::warning(this, tr("Resolve conflict"), error);
        m_diffCache.clear();
        reload();
        statusBar()->showMessage(tr("Resolved %1 (staged)").arg(fc.path), 5000);
    });
    w->show();
}

void MainWindow::applyLineOp(GitRepo::LineOp op, const QVector<int> &lines)
{
    if (m_shownIndex < 0 || m_shownIndex >= m_changes.size() || !m_pane->canLineOp(op))
        return;
    if (lines.isEmpty()) {
        statusBar()->showMessage(tr("Select changed lines, or move to a change"), 3000);
        return;
    }
    const FileChange fc = m_changes[m_shownIndex];
    QSet<int> oldLines, newLines;
    for (int i : lines) {
        const DiffLine &l = m_pane->diff().lines.value(i);
        if (l.kind == LineKind::Removed)
            oldLines.insert(l.oldNo);
        else if (l.kind == LineKind::Added)
            newLines.insert(l.newNo);
    }
    const int count = int(oldLines.size() + newLines.size());
    if (op == GitRepo::LineOp::Discard
        && QMessageBox::question(this, tr("Discard"),
                                 tr("Discard %n changed line(s) in %1? The working-tree file is rewritten; this "
                                    "can't be undone.",
                                    nullptr, count)
                                     .arg(fc.path))
               != QMessageBox::Yes)
        return;
    QString error;
    if (!m_repo.applyLines(op, fc, m_cmp, oldLines, newLines, &error)) {
        QMessageBox::warning(this, tr("Line staging"), error);
        return;
    }
    const QString what = op == GitRepo::LineOp::Stage     ? tr("Staged %n line(s)", nullptr, count)
                         : op == GitRepo::LineOp::Unstage ? tr("Unstaged %n line(s)", nullptr, count)
                                                          : tr("Discarded %n line(s)", nullptr, count);
    m_diffCache.clear();
    reload();
    statusBar()->showMessage(tr("%1 of %2").arg(what, fc.path), 4000);
}

void MainWindow::updateSyntaxCombo()
{
    const QString detected = m_pane->detectedLanguage();
    m_syntaxCombo->setItemText(0, detected.isEmpty() ? tr("Auto") : tr("Auto (%1)").arg(detected));
    const int cur = m_files->currentIndex();
    const QString suffix = cur >= 0 && cur < m_changes.size() ? QFileInfo(m_changes[cur].path).suffix().toLower() : QString();
    const QString override = m_syntaxBySuffix.value(suffix);
    const int idx = m_syntaxCombo->findData(override);
    m_syntaxCombo->setCurrentIndex(idx < 0 ? 0 : idx);
}

void MainWindow::syncOptionControls()
{
    const DiffOptions &o = m_cmp.options;
    for (QAction *a : m_algoGroup->actions())
        a->setChecked(a->data().toInt() == int(o.algorithm));
    if (m_algoCombo)
        m_algoCombo->setCurrentIndex(m_algoCombo->findData(int(o.algorithm)));
    m_ignoreSpaceChangeAct->setChecked(o.ignoreSpaceChange);
    m_ignoreAllSpaceAct->setChecked(o.ignoreAllSpace);
    if (m_whitespaceToggle)
        m_whitespaceToggle->setChecked(o.ignoreAllSpace);
    m_ignoreBlankLinesAct->setChecked(o.ignoreBlankLines);
    m_ignoreCrAct->setChecked(o.ignoreCrAtEol);
    m_ignoreTrimAct->setChecked(o.ignoreTrim);
    m_ignoreCaseAct->setChecked(o.ignoreCase);
    m_ignoreCommentsAct->setChecked(o.ignoreComments);
    m_ignoreUnimportantAct->setChecked(!o.unimportantPatterns.isEmpty());
    m_detectMovesAct->setChecked(o.detectMoves);
    m_allChangesAct->setChecked(m_cmp.allChanges);
    if (m_scopeCombo)
        m_scopeCombo->setCurrentIndex(m_cmp.allChanges ? 1 : 0);
}

void MainWindow::setDiffOptions(const DiffOptions &o)
{
    if (!maybeKeepEdits()) {
        syncOptionControls();
        return;
    }
    const bool listChanges = o.whitespaceArgs() != m_cmp.options.whitespaceArgs();
    m_cmp.options = o;
    Settings::setDiffOptions(o);
    syncOptionControls();
    m_diffCache.clear();
    if (listChanges) {
        // Whitespace options decide which files appear at all, so rebuild the list.
        reload();
        statusBar()->showMessage(o.ignoresWhitespace() ? tr("Ignoring whitespace") : tr("Showing whitespace changes"), 3000);
    } else {
        startPrefetch();
        reloadCurrentFile();
        statusBar()->showMessage(tr("Diff algorithm: %1").arg(algorithmLabel(o.algorithm)), 3000);
    }
}

void MainWindow::setAllChanges(bool on)
{
    if (m_cmp.mode != Comparison::Mode::WorkingTree || m_cmp.allChanges == on)
        return;
    if (!maybeKeepEdits()) {
        syncOptionControls();
        return;
    }
    m_cmp.allChanges = on;
    Settings::setAllChanges(on);
    syncOptionControls();
    updateTitle();
    reload();
}

void MainWindow::setComparison(const Comparison &cmp)
{
    const DiffOptions options = m_cmp.options;
    m_cmp = cmp;
    m_cmp.options = options;
    m_changes.clear();
    m_shownIndex = -1;
    qCInfo(lcApp).noquote() << "switched to" << m_cmp.describe();
    updateModeUi();
    updateTitle();
    reload();
}

void MainWindow::reload(std::function<void()> afterFile)
{
    // Remember the current file and change so a reload keeps the user's place.
    QString keepPath;
    ChangeGroup keepGroup = ChangeGroup::Revisions;
    int keepChange = -1;
    if (const int cur = m_files->currentIndex(); cur >= 0 && cur < m_changes.size()) {
        keepPath = m_changes[cur].path;
        keepGroup = m_changes[cur].group;
        keepChange = m_pane->currentChange();
    }
    if (!m_selectOnLoad.isEmpty()) { // a session's file
        keepPath = std::exchange(m_selectOnLoad, QString());
        keepChange = -1;
    }

    struct ListResult {
        QVector<FileChange> changes;
        QString error;
        QHash<int, QByteArray> fingerprints; // only for files that have review marks
        QByteArray signature;
    };
    const quint64 gen = ++m_listGen;
    m_listBusy = true;
    statusBar()->showMessage(tr("Loading changes…"));
    // The old list's background loading stops; the new list starts its own.
    ++m_prefetchGen;
    m_prefetchQueue.clear();
    m_prefetchInFlight.clear();
    m_prefetchClaimed.clear();
    m_prefetchDone = m_prefetchTotal = 0;
    m_waitingFor = -1;
    if (m_changes.isEmpty() && !m_cmp.empty) {
        m_files->setPlaceholder(tr("Listing changed files…"));
        m_pane->showMessage(QString(), tr("Listing changed files…"));
    }
    updateBusy();
    const QSet<QString> reviewKeys = m_reviews->keys();
    // With background loading the line counts come from the diffs: counting them with git
    // first reads every changed file, which in a large repository holds up the list.
    const bool lineCounts = !prefetchable();
    runJob<ListResult>(
        [repo = m_repo, cmp = m_cmp, reviewKeys, lineCounts] {
            ListResult r;
            r.changes = repo.listChanges(cmp, &r.error, lineCounts);
            for (int i = 0; i < r.changes.size(); ++i)
                if (reviewKeys.contains(ReviewStore::pathKey(r.changes[i].path)))
                    r.fingerprints.insert(i, repo.fingerprint(r.changes[i], cmp));
            r.signature = signatureOf(repo, cmp);
            return r;
        },
        [this, gen, keepPath, keepGroup, keepChange, afterFile](ListResult r) {
            if (gen != m_listGen)
                return; // a newer reload superseded this one
            m_listBusy = false;
            m_files->setPlaceholder({});
            updateBusy();
            statusBar()->clearMessage();
            m_diffCache.clear();
            m_changes = r.changes;
            m_lastSignature = r.signature;
            m_shownIndex = -1;
            if (!r.error.isEmpty()) {
                m_changes.clear();
                m_files->setChanges(m_changes, false);
                m_pane->showMessage(QString(), tr("git failed:\n%1").arg(r.error));
                updateSummary();
                return;
            }

            QVector<bool> reviewed(m_changes.size(), false);
            for (auto it = r.fingerprints.constBegin(); it != r.fingerprints.constEnd(); ++it)
                reviewed[it.key()] = m_reviews->isReviewed(m_changes[it.key()].path, it.value());

            const bool groups = m_cmp.mode == Comparison::Mode::WorkingTree && !m_cmp.allChanges;
            // Fully qualified folder names: the working tree or compared folder's path.
            QString folderRoot;
            if (m_cmp.mode == Comparison::Mode::Paths)
                // A downloaded PR's folders are in yadiff's cache: show them repository-relative.
                folderRoot = m_cmp.pathsAreFiles || m_cmp.readOnly ? QString() : m_cmp.rightPath;
            else if (!m_repo.isBare())
                folderRoot = m_repo.root();
            m_files->setProperty("folderRoot", folderRoot);
            m_files->setFolderDisplay(folderRoot, Settings::trimFolderPrefix());
            m_files->setChanges(m_changes, groups, reviewed);
            if (!m_comments.isEmpty())
                refreshThreads();
            applyFilter();
            updateSummary();
            if (m_cmp.prNumber > 0) {
                loadComments();
                if (!m_prHistory && !m_historyBusy)
                    loadPrHistory();
                if (!m_overview)
                    loadOverview();
                if (!m_myVote && !m_voteBusy)
                    loadMyVote();
            }

            if (m_changes.isEmpty() || m_files->visibleCount() == 0) {
                m_pane->showMessage(QString(), m_cmp.empty ? tr("Nothing to compare yet.\n\nUse File → New comparison (Ctrl+N) to "
                                                                "pick a repository, two revisions, two folders, two files or a PR link.")
                                               : m_changes.isEmpty() ? (m_cmp.paths.isEmpty() ? tr("No changes")
                                                                                            : tr("No changes in the selected paths"))
                                                                   : tr("No files match the filter"));
                updateEditActions();
                return;
            }
            startPrefetch();
            // Prefer the same file and section; fall back to the same file in another section.
            int target = -1, fallback = -1;
            for (int i = 0; i < m_changes.size(); ++i) {
                if (m_changes[i].path != keepPath)
                    continue;
                if (m_changes[i].group == keepGroup) {
                    target = i;
                    break;
                }
                if (fallback < 0)
                    fallback = i;
            }
            if (target < 0)
                target = fallback;
            if (afterFile)
                m_afterFileLoad = afterFile;
            else if (target >= 0 && keepChange > 0)
                m_afterFileLoad = [this, keepChange] { m_pane->goToChange(keepChange); };
            if (target < 0 || !m_files->selectIndex(target))
                m_files->selectFirst();
            if (!m_search->isHidden() && m_search->scope() == SearchBar::Scope::AllChanges)
                runSearch();
        });
}

void MainWindow::updateBusy()
{
    if (m_resolving || m_listBusy) {
        // Indeterminate: a moving bar, with what is happening beside it in the pane.
        m_busyBar->setRange(0, 0);
        m_busyBar->setFormat(m_resolving ? m_resolveStep : tr("Listing changed files…"));
        m_busyBar->setToolTip(m_busyBar->format());
    } else if (m_prefetchTotal > 0 && m_prefetchDone < m_prefetchTotal) {
        m_busyBar->setRange(0, m_prefetchTotal);
        m_busyBar->setValue(m_prefetchDone);
        m_busyBar->setFormat(tr("Loading diffs %v of %m"));
        m_busyBar->setToolTip(tr("Loading each file's diff in the background"));
    } else {
        m_busyBar->hide();
        return;
    }
    m_busyBar->show();
}

bool MainWindow::prefetchable() const
{
    // Only comparisons of commits: their files can't change while they are shown.
    return !m_cmp.empty && m_repo.isRepository() && m_cmp.mode == Comparison::Mode::Revisions && !m_cmp.rev2.isEmpty();
}

void MainWindow::startPrefetch()
{
    ++m_prefetchGen;
    m_prefetchQueue.clear();
    m_prefetchInFlight.clear();
    m_prefetchClaimed.clear();
    m_prefetchDone = m_prefetchTotal = 0;
    m_waitingFor = -1;
    m_files->clearLoadStates();
    if (!prefetchable()) {
        updateBusy();
        return;
    }
    // In list order, so the files the user is likely to look at first come first.
    constexpr int kMaxPrefetch = 300;
    QVector<int> counted;
    for (int idx : m_files->displayOrder(true)) {
        const FileChange &fc = m_changes[idx];
        if (m_diffCache.contains(idx) || isImagePath(fc.path) || isImagePath(fc.oldPath))
            continue;
        if (m_prefetchQueue.size() + m_prefetchClaimed.size() >= kMaxPrefetch)
            break;
        counted << idx;
        if (m_fileBusy && idx == m_shownIndex) {
            m_prefetchClaimed.insert(idx); // showFile is loading it already
            m_files->setLoadState(idx, FileList::LoadState::Loading);
            continue;
        }
        m_prefetchQueue << idx;
        m_files->setLoadState(idx, FileList::LoadState::Queued);
    }
    m_prefetchTotal = int(m_prefetchQueue.size() + m_prefetchClaimed.size());

    // Files past the limit (or images) still get line counts, from git.
    bool uncounted = false;
    for (int i = 0; i < m_changes.size(); ++i)
        uncounted = uncounted || (m_changes[i].added < 0 && !counted.contains(i));
    if (uncounted) {
        const quint64 gen = m_prefetchGen;
        runJob<QVector<FileChange>>([repo = m_repo, cmp = m_cmp] { return repo.listChanges(cmp, nullptr, true); },
                                    [this, gen](QVector<FileChange> counts) {
                                        if (gen != m_prefetchGen)
                                            return;
                                        for (const FileChange &c : counts)
                                            for (int i = 0; i < m_changes.size(); ++i)
                                                if (m_changes[i].added < 0 && m_changes[i].path == c.path && c.added >= 0) {
                                                    m_changes[i].added = c.added;
                                                    m_changes[i].removed = c.removed;
                                                    m_changes[i].blankOnly = c.blankOnly;
                                                    m_files->setStats(i, c.added, c.removed, c.blankOnly);
                                                }
                                        updateSummary();
                                    });
    }
    pumpPrefetch();
    updateBusy();
    updateSummary();
}

void MainWindow::pumpPrefetch()
{
    // A few at a time: each is a git process, and in a virtualized repository each
    // downloads its files, so a handful in parallel hides most of the waiting.
    constexpr int kParallel = 4;
    while (m_prefetchInFlight.size() < kParallel && !m_prefetchQueue.isEmpty()) {
        const int idx = m_prefetchQueue.takeFirst();
        if (m_diffCache.contains(idx)) { // showFile got there first
            fileLoaded(idx, m_diffCache.value(idx), QString());
            continue;
        }
        m_prefetchInFlight.insert(idx);
        m_files->setLoadState(idx, FileList::LoadState::Loading);
        FileChange fc = m_changes[idx];
        fc.anchors = m_anchors.value(scrollKey(fc));
        const quint64 gen = m_prefetchGen;
        runJob<std::pair<FileDiff, QString>>(
            [repo = m_repo, cmp = m_cmp, fc] {
                QString error;
                FileDiff d = repo.loadDiff(fc, cmp, &error);
                return std::pair<FileDiff, QString>(std::move(d), error);
            },
            [this, gen, idx](std::pair<FileDiff, QString> r) {
                if (gen != m_prefetchGen)
                    return; // the comparison or its options changed meanwhile
                m_prefetchInFlight.remove(idx);
                fileLoaded(idx, r.first, r.second);
                pumpPrefetch();
            });
    }
    updateBusy();
}

void MainWindow::fileLoaded(int index, const FileDiff &diff, const QString &error)
{
    if (index < 0 || index >= m_changes.size())
        return;
    const bool ok = error.isEmpty();
    if (ok) {
        m_diffCache.insert(index, diff);
        FileChange &fc = m_changes[index];
        if (fc.added < 0 && !diff.binary && !diff.hexDump) {
            // The line counts git would give, from the diff itself.
            int added = 0, removed = 0;
            bool blank = true;
            for (const DiffLine &l : diff.lines) {
                if (l.kind != LineKind::Added && l.kind != LineKind::Removed)
                    continue;
                (l.kind == LineKind::Added ? added : removed)++;
                blank = blank && l.text.trimmed().isEmpty();
            }
            fc.added = added;
            fc.removed = removed;
            fc.blankOnly = fc.status == u'M' && blank && added + removed > 0;
            m_files->setStats(index, added, removed, fc.blankOnly);
        }
    }
    m_files->setLoadState(index, ok ? FileList::LoadState::Loaded : FileList::LoadState::Failed);
    ++m_prefetchDone;
    if (m_prefetchDone >= m_prefetchTotal)
        updateSummary();
    if (m_waitingFor == index) {
        // The user picked this file while it was loading: show it now.
        m_waitingFor = -1;
        m_afterFileLoad = std::exchange(m_waitingThen, nullptr);
        m_shownIndex = -1;
        showFile(index);
    }
    updateBusy();
}

void MainWindow::openInBackground(ComparisonArgs::Request req, const std::optional<Session> &session)
{
    const QString link = req.ado.isEmpty() ? req.ghpr : req.ado;
    m_resolving = true;
    m_resolveStep = tr("Opening the pull request…");
    // Nothing from the placeholder comparison may draw over the progress.
    ++m_listGen;
    ++m_fileGen;
    m_cmp.title = link;
    updateTitle();
    m_files->setChanges({}, false);
    m_files->setPlaceholder(tr("Opening the pull request…"));
    m_pane->showMessage(link, m_resolveStep);
    updateBusy();
    qCInfo(lcApp).noquote() << "opening" << link << "in the background";

    QPointer<MainWindow> self(this);
    req.progress = [self, link](const QString &message) {
        QMetaObject::invokeMethod(
            qApp,
            [self, link, message] {
                if (!self || !self->m_resolving)
                    return;
                self->m_resolveStep = message;
                self->m_pane->showMessage(link, message);
                self->updateBusy();
            },
            Qt::QueuedConnection);
    };
    // Sign-in is a dialog, so it happens on the UI thread while the worker waits.
    req.signIn = [](const QUrl &collection, const QString &reason) -> std::optional<AdoAuth::Credential> {
        if (QThread::currentThread() == qApp->thread())
            return ComparisonArgs::signInWithDialog(collection, reason);
        std::optional<AdoAuth::Credential> out;
        QMetaObject::invokeMethod(
            qApp, [&] { out = ComparisonArgs::signInWithDialog(collection, reason); }, Qt::BlockingQueuedConnection);
        return out;
    };
    struct Opened {
        std::optional<ComparisonArgs::Result> result;
        QString error;
    };
    runJob<std::shared_ptr<Opened>>(
        [req] {
            auto o = std::make_shared<Opened>();
            o->result = ComparisonArgs::resolve(req, &o->error);
            return o;
        },
        [this, session, link](std::shared_ptr<Opened> o) {
            m_resolving = false;
            updateBusy();
            if (!o->result) {
                m_files->setPlaceholder({});
                m_pane->showMessage(link, tr("Could not open the pull request:\n\n%1").arg(o->error));
                QMessageBox::warning(this, tr("Open pull request"), o->error);
                return;
            }
            setComparison(std::move(o->result->repo), o->result->cmp);
            if (session)
                applySession(*session);
        });
}

void MainWindow::updateSummary()
{
    QString text;
    if (m_cmp.mode == Comparison::Mode::WorkingTree && !m_cmp.allChanges) {
        int staged = 0;
        for (const FileChange &fc : std::as_const(m_changes))
            staged += fc.group == ChangeGroup::Staged;
        text = tr("%1 staged, %2 unstaged").arg(staged).arg(m_changes.size() - staged);
    } else {
        text = m_changes.size() == 1 ? tr("1 file changed") : tr("%1 files changed").arg(m_changes.size());
    }
    int added = 0, removed = 0;
    for (const FileChange &fc : std::as_const(m_changes)) {
        added += std::max(0, fc.added);
        removed += std::max(0, fc.removed);
    }
    // While diffs are still loading in the background their counts aren't known yet.
    const bool counting = m_prefetchTotal > 0 && m_prefetchDone < m_prefetchTotal;
    if (!m_changes.isEmpty()) {
        if (!counting)
            text += QStringLiteral("  ·  +%1 −%2").arg(added).arg(removed);
        text += tr("  \u00b7  %1 of %2 marked").arg(m_files->markedCount()).arg(m_changes.size());
    }
    m_summary->setText(text);
}

void MainWindow::onReviewToggled(int index, bool reviewed)
{
    if (index < 0 || index >= m_changes.size())
        return;
    const FileChange fc = m_changes[index];
    // The fingerprint asks git for both versions' ids: in the background, latest toggle wins.
    const quint64 gen = ++m_markGen[fc.path];
    runJob<QByteArray>([repo = m_repo, cmp = m_cmp, fc] { return repo.fingerprint(fc, cmp); },
                       [this, fc, reviewed, gen, store = m_reviews.get()](QByteArray fingerprint) {
                           if (m_markGen.value(fc.path) != gen || store != m_reviews.get())
                               return; // toggled again since, or another comparison
                           m_markGen.remove(fc.path);
                           m_reviews->setReviewed(fc.path, fingerprint, reviewed);
                           updateSummary();
                       });
}

void MainWindow::reloadCurrentFile()
{
    const int cur = m_files->currentIndex();
    if (cur < 0)
        return;
    const int keep = m_pane->currentChange();
    m_diffCache.remove(cur);
    if (keep > 0)
        m_afterFileLoad = [this, keep] { m_pane->goToChange(keep); };
    m_shownIndex = -1;
    showFile(cur);
}

void MainWindow::showFile(int index)
{
    if (m_revertingSelection)
        return;
    if (index != m_shownIndex && m_pane->isModified() && !maybeKeepEdits()) {
        // The user cancelled: put the selection back on the file being edited.
        m_afterFileLoad = nullptr;
        m_revertingSelection = true;
        m_files->selectIndex(m_shownIndex);
        m_revertingSelection = false;
        return;
    }
    if (index < 0 || index >= m_changes.size())
        return;
    std::function<void()> then = std::move(m_afterFileLoad);
    m_afterFileLoad = nullptr;
    if (Settings::rememberScroll() && m_shownIndex >= 0 && m_shownIndex < m_changes.size() && m_shownIndex != index)
        m_scrollMemory.insert(scrollKey(m_changes[m_shownIndex]), m_pane->scrollState());
    if (!then && Settings::rememberScroll()) {
        if (const auto it = m_scrollMemory.constFind(scrollKey(m_changes[index])); it != m_scrollMemory.constEnd()) {
            const DiffPane::ScrollState state = *it;
            then = [this, state] { m_pane->restoreScroll(state); };
        }
    }
    FileChange fc = m_changes[index];
    fc.anchors = m_anchors.value(scrollKey(fc));
    const bool wasEditing = m_pane->isEditing() && index == m_shownIndex;

    QString left, right;
    if (m_cmp.mode == Comparison::Mode::Patch) {
        left = tr("Before the patch");
        right = tr("After the patch");
    } else if (m_cmp.mode == Comparison::Mode::Paths) {
        left = homeShortened(m_cmp.pathsAreFiles ? QFileInfo(m_cmp.leftPath).absolutePath() : m_cmp.leftPath);
        if (!m_cmp.rev1Name.isEmpty()) // named sides (a downloaded pull request), not cache folders
            left = m_cmp.rev1Name;
        right = homeShortened(m_cmp.pathsAreFiles ? QFileInfo(m_cmp.rightPath).absolutePath() : m_cmp.rightPath);
        if (!m_cmp.rev2Name.isEmpty())
            right = m_cmp.rev2Name;
    } else if (m_cmp.mode == Comparison::Mode::Revisions) {
        left = m_cmp.rev1Name;
        right = m_cmp.rev2.isEmpty() ? tr("Working tree") : m_cmp.rev2Name;
    } else if (m_cmp.allChanges) {
        left = QStringLiteral("HEAD");
        right = tr("Working tree");
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
    if (m_cmp.reversed)
        std::swap(left, right); // swapped sides: the working tree is on the left
    // An untracked file exists only in the working tree, on whichever side that is.
    const bool isNew = fc.status == u'A' || (fc.status == u'?' && !m_cmp.reversed);
    const bool isGone = fc.status == u'D' || (fc.status == u'?' && m_cmp.reversed);
    left = isNew ? tr("%1: (file does not exist)").arg(left) : left + QStringLiteral(": ") + fc.oldPath;
    right = isGone ? tr("%1: (file deleted)").arg(right) : right + QStringLiteral(": ") + fc.path;

    QString title = fc.path;
    if (fc.oldPath != fc.path)
        title = tr("%1  (%2 from %3)").arg(fc.path, statusDescription(fc.status).toLower(), fc.oldPath);
    else
        title += QStringLiteral("  (%1)").arg(statusDescription(fc.status).toLower());
    // yadiff's own comparisons can differ from git's counts: those come from the diff.
    const bool countsFromDiff = (m_cmp.options.inProcess() || !fc.anchors.isEmpty()) && fc.status != u'?';
    if (fc.added >= 0 && !countsFromDiff)
        title += QStringLiteral("  +%1 −%2").arg(fc.added).arg(fc.removed);

    const bool imageFile = isImagePath(fc.path) || isImagePath(fc.oldPath);
    const bool cachedNow = m_diffCache.contains(index) && !imageFile;
    if (!cachedNow && m_prefetchInFlight.contains(index)) {
        // Loading in the background already: switch to it now, show it when it arrives.
        m_shownIndex = index;
        ++m_fileGen;
        m_fileBusy = false;
        m_waitingFor = index;
        m_waitingThen = then;
        m_pane->showMessage(title, tr("Loading…"));
        updateEditActions();
        updateTitle();
        return;
    }
    m_waitingFor = -1;
    if (!cachedNow && m_prefetchQueue.removeOne(index)) {
        // Still waiting its turn: load it now, here, and count it as loaded.
        m_prefetchClaimed.insert(index);
        m_files->setLoadState(index, FileList::LoadState::Loading);
    }
    const int previous = m_shownIndex;
    m_shownIndex = index;
    const quint64 gen = ++m_fileGen;
    m_fileBusy = true;
    statusBar()->showMessage(tr("Loading %1…").arg(fc.path));
    if (!cachedNow && index != previous)
        // A slow load switches to the file at once, rather than leaving the last one up.
        QTimer::singleShot(150, this, [this, gen, title] {
            if (gen == m_fileGen && m_fileBusy)
                m_pane->showMessage(title, tr("Loading…"));
        });

    struct FileResult {
        FileDiff diff;
        QString error;
        QImage oldImage, newImage;
        qint64 oldBytes = 0, newBytes = 0;
        QVector<BlameLine> oldBlame, newBlame;
        QString oldTable, newTable;
    };
    const bool imagePath = isImagePath(fc.path) || isImagePath(fc.oldPath);
    const bool tablePath = TableDiff::isTableFile(fc.path);
    const bool hexBinary = Settings::hexBinary();
    const auto cached = m_diffCache.constFind(index);
    const bool haveCached = cached != m_diffCache.constEnd() && !imagePath;
    const FileDiff cachedDiff = haveCached ? *cached : FileDiff();
    const bool large = m_showLarge.contains(index);

    runJob<FileResult>(
        [repo = m_repo, cmp = m_cmp, fc, imagePath, tablePath, hexBinary, haveCached, cachedDiff,
         blame = Settings::showBlame()] {
            FileResult r;
            r.diff = haveCached ? cachedDiff : repo.loadDiff(fc, cmp, &r.error);
            if (r.diff.binary && !imagePath && hexBinary && r.error.isEmpty()) {
                // Binary: a hex dump of both sides (when they aren't huge).
                constexpr qint64 kMaxHex = 8 * 1024 * 1024;
                const auto oldBytes = repo.fileContent(fc, cmp, false, kMaxHex);
                const auto newBytes = repo.fileContent(fc, cmp, true, kMaxHex);
                if (oldBytes || newBytes) {
                    r.diff = HexDiff::build(oldBytes.value_or(QByteArray()), newBytes.value_or(QByteArray()));
                    if (r.diff.lines.isEmpty())
                        r.diff.message = QStringLiteral("Both sides are empty");
                }
            }
            if (tablePath && !r.diff.binary && r.error.isEmpty()) {
                constexpr qint64 kMaxTable = 32 * 1024 * 1024;
                r.oldTable = QString::fromUtf8(repo.fileContent(fc, cmp, false, kMaxTable).value_or(QByteArray()));
                r.newTable = QString::fromUtf8(repo.fileContent(fc, cmp, true, kMaxTable).value_or(QByteArray()));
            }
            if (blame && !r.diff.binary && !r.diff.hexDump && r.error.isEmpty()) {
                r.oldBlame = repo.blame(fc, cmp, false);
                r.newBlame = repo.blame(fc, cmp, true);
            }
            if (imagePath && r.diff.binary) {
                // Images are decoded here too, off the UI thread.
                if (const auto bytes = repo.fileContent(fc, cmp, false)) {
                    r.oldImage = QImage::fromData(*bytes);
                    r.oldBytes = bytes->size();
                }
                if (const auto bytes = repo.fileContent(fc, cmp, true)) {
                    r.newImage = QImage::fromData(*bytes);
                    r.newBytes = bytes->size();
                }
            }
            return r;
        },
        [this, gen, index, fc, left, right, title, isNew, isGone, wasEditing, then, large, countsFromDiff, tablePath,
         prefetchGen = m_prefetchGen](FileResult r) {
            if (prefetchGen == m_prefetchGen && m_prefetchClaimed.remove(index))
                fileLoaded(index, r.diff, r.error); // counted in the background loading's progress
            if (gen != m_fileGen)
                return; // the user moved on to another file
            m_fileBusy = false;
            statusBar()->clearMessage();
            if (!r.error.isEmpty()) {
                m_pane->showMessage(title, tr("git failed:\n%1").arg(r.error));
                updateEditActions();
                return;
            }
            m_diffCache.insert(index, r.diff);
            if (r.diff.lines.size() > kLargeDiffLines && !large) {
                m_pane->showLargeNotice(title, int(r.diff.lines.size()));
                updateEditActions();
                return;
            }

            FileView view;
            view.diff = r.diff;
            view.title = title;
            if (countsFromDiff && !r.diff.binary) {
                int added = 0, removed = 0;
                for (const DiffLine &l : r.diff.lines) {
                    added += l.kind == LineKind::Added && !l.unimportant;
                    removed += l.kind == LineKind::Removed && !l.unimportant;
                }
                view.title += QStringLiteral("  +%1 −%2").arg(added).arg(removed);
            }
            view.leftLabel = left;
            view.rightLabel = right;
            view.paths = {isNew ? QString() : fc.oldPath, isGone ? QString() : fc.path};
            view.fileName = fc.path;
            view.oldOnDisk = !m_repo.diskPath(fc, m_cmp, false).isEmpty();
            view.newOnDisk = !m_repo.diskPath(fc, m_cmp, true).isEmpty();
            view.isImage = !r.oldImage.isNull() || !r.newImage.isNull();
            view.oldImage = r.oldImage;
            view.newImage = r.newImage;
            view.oldBytes = r.oldBytes;
            view.newBytes = r.newBytes;
            view.reviewEnabled = m_cmp.prNumber > 0;
            view.threads = threadsFor(fc);
            for (const GitHubReview::ThreadStatus s : PrReview::threadStatuses(m_cmp))
                view.threadStatuses.push_back({s, PrReview::threadStatusName(m_cmp, s)});
            view.oldBlame = r.oldBlame;
            view.newBlame = r.newBlame;
            view.lineOps = r.diff.hexDump ? QVector<GitRepo::LineOp>() : m_repo.lineOps(fc, m_cmp);
            view.isHex = r.diff.hexDump;
            if (r.diff.hexDump)
                view.fileName.clear(); // no syntax highlighting for a dump
            view.isTable = tablePath && !r.diff.binary && (!r.oldTable.isEmpty() || !r.newTable.isEmpty());
            view.oldTable = r.oldTable;
            view.newTable = r.newTable;
            m_pane->setLanguageOverride(m_syntaxBySuffix.value(QFileInfo(fc.path).suffix().toLower()));
            m_pane->setFile(view);
            if (wasEditing)
                m_pane->setEditing(true);
            updateSyntaxCombo();
            updateEditActions();
            updateTitle();
            if (then)
                then();
        });
}

void MainWindow::nextChange()
{
    if (m_pane->nextChange()) {
        updateEditActions();
        return;
    }
    // No more changes in this file: move on to the next file (which opens at its first change).
    if (!m_files->selectNext())
        statusBar()->showMessage(tr("No more changes"), 3000);
}

void MainWindow::prevChange()
{
    if (m_pane->prevChange()) {
        updateEditActions();
        return;
    }
    // Open the previous file at its last change (once it has loaded).
    m_afterFileLoad = [this] { m_pane->goToLastChange(); };
    if (!m_files->selectPrev()) {
        m_afterFileLoad = nullptr;
        statusBar()->showMessage(tr("No earlier changes"), 3000);
    }
}

void MainWindow::setSideBySide(bool on)
{
    if (!on && m_pane->isEditing()) {
        statusBar()->showMessage(tr("Editing works in the side-by-side view"), 3000);
        updateViewToggle();
        return;
    }
    Settings::setSideBySide(on);
    m_pane->setMode(on ? DiffPane::Mode::SideBySide : DiffPane::Mode::Inline);
    updateViewToggle();
}

void MainWindow::openSettings()
{
    SettingsDialog dlg(this, {m_recentMenu, m_languageMenu}, this);
    dlg.exec();
}

void MainWindow::openPreferences()
{
    PreferencesDialog dlg(this);
    connect(&dlg, &PreferencesDialog::applied, m_pane, &DiffPane::applySettings);
    dlg.exec();
}

// ---- Editing ----------------------------------------------------------------

bool MainWindow::maybeKeepEdits()
{
    if (!m_pane->isModified())
        return true;
    const auto answer = QMessageBox::question(this, tr("Unsaved changes"),
                                              tr("You edited %1. Save your changes?")
                                                  .arg(m_shownIndex >= 0 ? m_changes.value(m_shownIndex).path : QString()),
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                              QMessageBox::Save);
    if (answer == QMessageBox::Cancel)
        return false;
    if (answer == QMessageBox::Save)
        return saveEdits();
    m_pane->setEditing(false);
    updateEditActions();
    updateTitle();
    return true;
}

bool MainWindow::saveEdits()
{
    if (!m_pane->isEditing() || m_shownIndex < 0 || m_shownIndex >= m_changes.size())
        return true;
    const FileChange fc = m_changes[m_shownIndex];
    const QString path = m_repo.diskPath(fc, m_cmp, true);
    if (path.isEmpty()) {
        QMessageBox::warning(this, tr("Save"), tr("%1 is not a file on disk.").arg(fc.path));
        return false;
    }
    QString error;
    TextFile::Content content = TextFile::read(path, &error).value_or(TextFile::Content());
    content.lines = m_pane->editedLines();
    if (!TextFile::write(path, content, &error)) {
        QMessageBox::warning(this, tr("Save"), tr("Could not save %1:\n%2").arg(path, error));
        return false;
    }
    // Re-diff, staying in edit mode at the same place.
    const int keep = m_pane->currentChange();
    m_pane->setEditing(false);
    const int index = m_shownIndex;
    m_diffCache.remove(index);
    m_shownIndex = -1;
    m_afterFileLoad = [this, keep] {
        if (keep > 0)
            m_pane->goToChange(keep);
        m_pane->setEditing(true);
        updateEditActions();
        updateTitle();
    };
    showFile(index);
    m_lastSignature.clear(); // the next poll re-reads the state instead of reloading again
    updateEditActions();
    updateTitle();
    return true;
}

void MainWindow::toggleEditing(bool on)
{
    if (on) {
        if (!m_pane->canEdit()) {
            statusBar()->showMessage(tr("Only working-tree files can be edited"), 3000);
            updateEditActions();
            return;
        }
        m_pane->setEditing(true);
        statusBar()->showMessage(tr("Editing %1. Ctrl+S saves.").arg(m_changes.value(m_shownIndex).path), 4000);
    } else if (maybeKeepEdits()) {
        m_pane->setEditing(false);
    }
    updateViewToggle();
    updateEditActions();
    updateTitle();
}

void MainWindow::copyChange(int chunk, bool toNewSide)
{
    if (chunk < 0 || m_shownIndex < 0 || m_shownIndex >= m_changes.size())
        return;
    if (m_pane->isModified()) {
        QMessageBox::information(this, tr("Apply change"), tr("Save or discard your edits first."));
        return;
    }
    const FileChange fc = m_changes[m_shownIndex];
    const QString path = m_repo.diskPath(fc, m_cmp, toNewSide);
    if (path.isEmpty()) {
        statusBar()->showMessage(tr("That side is not a file on disk"), 3000);
        return;
    }
    QString error;
    auto content = TextFile::read(path, &error);
    std::optional<QStringList> lines;
    if (content)
        lines = TextFile::applyChunk(content->lines, m_pane->diff(), chunk, toNewSide, &error);
    if (!content || !lines || !TextFile::write(path, TextFile::Content{*lines, content->eol, content->trailingNewline}, &error)) {
        QMessageBox::warning(this, tr("Apply change"), tr("Could not update %1:\n%2").arg(path, error));
        return;
    }
    statusBar()->showMessage(tr("Updated %1").arg(homeShortened(path)), 3000);
    const bool editing = m_pane->isEditing();
    reload([this, chunk, editing] {
        m_pane->goToChange(chunk);
        if (editing)
            m_pane->setEditing(true);
        updateEditActions();
    });
}

// ---- History and difftool ----------------------------------------------------

void MainWindow::openHistory()
{
    if (!m_repo.isRepository() || !maybeKeepEdits())
        return;
    HistoryDialog dlg(m_repo, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    Comparison c;
    c.unstagedAgainstHead = m_cmp.unstagedAgainstHead;
    c.allChanges = Settings::allChanges();
    switch (dlg.choice()) {
    case HistoryDialog::Choice::None:
        return;
    case HistoryDialog::Choice::WorkingTree:
        c.mode = Comparison::Mode::WorkingTree;
        break;
    case HistoryDialog::Choice::CommitChanges: {
        const CommitInfo commit = dlg.first();
        c.mode = Comparison::Mode::Revisions;
        if (commit.parents.isEmpty()) {
            c.rev1 = m_repo.emptyTree();
            c.rev1Name = tr("(no parent)");
        } else {
            c.rev1 = commit.parents.first();
            c.rev1Name = commit.shortOid + u'^';
        }
        c.rev2 = commit.oid;
        c.rev2Name = commit.shortOid;
        c.title = QStringLiteral("%1 %2").arg(commit.shortOid, commit.subject);
        break;
    }
    case HistoryDialog::Choice::CompareTwo:
        c.mode = Comparison::Mode::Revisions;
        c.rev1 = dlg.first().oid;
        c.rev1Name = dlg.first().shortOid;
        c.rev2 = dlg.second().oid;
        c.rev2Name = dlg.second().shortOid;
        break;
    case HistoryDialog::Choice::CommitVsWorkingTree:
        c.mode = Comparison::Mode::Revisions;
        c.rev1 = dlg.first().oid;
        c.rev1Name = dlg.first().shortOid;
        break;
    case HistoryDialog::Choice::BranchChanges: {
        // The merge base can take a long walk through history: in the background.
        const CommitInfo tip = dlg.first();
        c.paths = m_cmp.paths;
        statusBar()->showMessage(tr("Finding the merge base with HEAD…"));
        using Result = std::pair<std::optional<QString>, QString>;
        runJob<Result>(
            [repo = m_repo, oid = tip.oid] {
                QString error;
                auto base = repo.mergeBase(QStringLiteral("HEAD"), oid, &error);
                return Result{base, error};
            },
            [this, c, tip](Result r) mutable {
                statusBar()->clearMessage();
                if (!r.first) {
                    QMessageBox::warning(this, tr("Compare commits"), tr("No merge base with HEAD:\n%1").arg(r.second));
                    return;
                }
                const QString base = *r.first;
                const QString name = tip.refs.split(QStringLiteral(", ")).value(0).remove(QStringLiteral("tag: "));
                c.mode = Comparison::Mode::Revisions;
                c.rev1 = base;
                c.rev1Name = tr("merge base %1").arg(base.left(7));
                c.rev2 = tip.oid;
                c.rev2Name = name.isEmpty() ? tip.shortOid : name;
                c.title = tr("Changes on %1 since HEAD").arg(c.rev2Name);
                setComparison(c);
            });
        return;
    }
    }
    c.paths = m_cmp.paths;
    setComparison(c);
}

void MainWindow::registerDifftool()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Register as git difftool"));
    auto *layout = new QVBoxLayout(&dlg);
    const QString exe = Difftool::preferredExecutable();
    auto *text = new QLabel(tr("This adds yadiff to your global git configuration (~/.gitconfig):"
                               "<pre>difftool.yadiff.cmd = %1</pre>"
                               "Then <code>git difftool -t yadiff</code> opens one file at a time, and "
                               "<code>git difftool -d -t yadiff</code> compares whole directories.")
                                .arg(Difftool::command(exe).toHtmlEscaped()));
    text->setTextFormat(Qt::RichText);
    text->setWordWrap(true);
    auto *makeDefault = new QCheckBox(tr("Make yadiff the default difftool (diff.tool = yadiff, no prompt)"));
    makeDefault->setChecked(true);
    layout->addWidget(text);
    layout->addWidget(makeDefault);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    QPushButton *reg = buttons->addButton(tr("Register"), QDialogButtonBox::AcceptRole);
    QPushButton *unreg = buttons->addButton(tr("Unregister"), QDialogButtonBox::DestructiveRole);
    unreg->setEnabled(Difftool::isRegistered());
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(reg, &QPushButton::clicked, &dlg, [&] {
        QString summary, error;
        if (Difftool::registerTool(exe, makeDefault->isChecked(), &summary, &error))
            QMessageBox::information(&dlg, tr("Registered"), tr("Done:\n\n%1").arg(summary));
        else
            QMessageBox::warning(&dlg, tr("Register"), error);
        dlg.accept();
    });
    connect(unreg, &QPushButton::clicked, &dlg, [&] {
        QString summary, error;
        if (Difftool::unregisterTool(&summary, &error))
            QMessageBox::information(&dlg, tr("Unregistered"), tr("Done:\n\n%1").arg(summary));
        else
            QMessageBox::warning(&dlg, tr("Unregister"), error);
        dlg.accept();
    });
    dlg.exec();
}

// ---- Search -----------------------------------------------------------------


void MainWindow::runSearch()
{
    if (m_search->isHidden())
        return;
    if (m_searchCancel)
        *m_searchCancel = true; // stop any search still running
    const SearchQuery q = m_search->query();
    m_matchCounts.clear();
    if (q.isEmpty() || !q.isValid()) {
        m_pane->clearSearch();
        m_files->setMatchCounts({});
        if (!q.isEmpty())
            m_search->setResult(tr("Invalid regex: %1").arg(q.errorString()), true);
        else
            m_search->setResult(QString());
        return;
    }
    const auto scope = m_search->scope();
    if (scope == SearchBar::Scope::Selection && !m_pane->hasSearchSelection()) {
        m_pane->clearSearch();
        m_files->setMatchCounts({});
        m_search->setResult(tr("Select some text first"), true);
        return;
    }
    if (scope != SearchBar::Scope::AllChanges) {
        m_files->setMatchCounts({});
        m_pane->setSearch(q, scope == SearchBar::Scope::Selection ? DiffPane::SearchScope::Selection
                                                                  : DiffPane::SearchScope::File);
        updateSearchResult();
        return;
    }

    // All changes: load and search every file in the background, with progress.
    struct SearchResult {
        QHash<int, int> counts;
        QHash<int, FileDiff> loaded;
        bool cancelled = false;
    };
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_searchCancel = cancel;
    const quint64 gen = ++m_searchGen;
    m_searchBusy = true;
    m_search->setBusy(true);
    m_search->setResult(tr("Searching…"));
    QPointer<MainWindow> self(this);
    runJob<SearchResult>(
        [repo = m_repo, cmp = m_cmp, changes = m_changes, cache = m_diffCache, q, cancel, self, gen] {
            SearchResult r;
            for (int i = 0; i < changes.size(); ++i) {
                if (*cancel) {
                    r.cancelled = true;
                    break;
                }
                FileDiff d;
                if (const auto it = cache.constFind(i); it != cache.constEnd()) {
                    d = *it;
                } else {
                    QString error;
                    d = repo.loadDiff(changes[i], cmp, &error);
                    r.loaded.insert(i, d);
                }
                if (const int n = int(findInDiff(d, q).size()))
                    r.counts.insert(i, n);
                if (i % 10 == 9) {
                    const int done = i + 1, total = int(changes.size());
                    QMetaObject::invokeMethod(
                        self.data(),
                        [self, gen, done, total] {
                            if (self && self->m_searchGen == gen && self->m_searchBusy)
                                self->m_search->setResult(tr("Searching… %1 of %2 files").arg(done).arg(total));
                        },
                        Qt::QueuedConnection);
                }
            }
            return r;
        },
        [this, gen, q](SearchResult r) {
            if (gen != m_searchGen)
                return;
            m_searchBusy = false;
            m_search->setBusy(false);
            for (auto it = r.loaded.constBegin(); it != r.loaded.constEnd(); ++it)
                m_diffCache.insert(it.key(), it.value());
            if (r.cancelled) {
                m_search->setResult(tr("Stopped"));
                return;
            }
            m_matchCounts = r.counts;
            m_files->setMatchCounts(m_matchCounts);
            m_pane->setSearch(q, DiffPane::SearchScope::File);
            if (m_pane->matchCount() == 0 && !m_matchCounts.isEmpty())
                searchNext(true); // jump to the first file that has matches
            updateSearchResult();
        });
}

void MainWindow::searchNext(bool forward)
{
    if (m_search->isHidden()) {
        m_search->activate();
        runSearch();
        return;
    }
    if (forward ? m_pane->nextMatch() : m_pane->prevMatch())
        return;
    if (m_search->scope() != SearchBar::Scope::AllChanges || m_matchCounts.isEmpty()) {
        // Within the file (or selection): round to the other end.
        if (forward ? m_pane->nextMatch(true) : m_pane->prevMatch(true))
            statusBar()->showMessage(forward ? tr("Passed the end; back at the first match")
                                             : tr("Passed the start; back at the last match"),
                                     2000);
        return;
    }
    // Move to the next (or previous) file with matches, wrapping around.
    const QVector<int> order = m_files->displayOrder();
    const qsizetype pos = order.indexOf(m_files->currentIndex());
    for (qsizetype step = 1; step <= order.size(); ++step) {
        const qsizetype k = ((pos + (forward ? step : -step)) % order.size() + order.size()) % order.size();
        if (m_matchCounts.value(order[k], 0) > 0) {
            m_afterFileLoad = [this, forward] {
                m_pane->goToMatch(forward ? 0 : -1);
                updateSearchResult();
            };
            m_files->selectIndex(order[k]);
            return;
        }
    }
}

void MainWindow::updateSearchResult()
{
    if (m_search->isHidden())
        return;
    const int n = m_pane->matchCount();
    const int cur = m_pane->currentMatch();
    QString text;
    if (m_search->scope() == SearchBar::Scope::AllChanges) {
        int total = 0;
        for (int c : std::as_const(m_matchCounts))
            total += c;
        text = total == 0 ? tr("No matches")
                          : tr("%1 of %2 here, %3 in %4 files").arg(cur + 1).arg(n).arg(total).arg(m_matchCounts.size());
        if (n == 0 && total > 0)
            text = tr("%1 matches in %2 files").arg(total).arg(m_matchCounts.size());
    } else {
        text = n == 0 ? tr("No matches") : tr("%1 of %2").arg(cur + 1).arg(n);
    }
    m_search->setResult(text, n == 0 && m_matchCounts.isEmpty());
}

// ---- Reload on change -------------------------------------------------------


void MainWindow::pollForChanges()
{
    if (!isVisible() || QApplication::activeModalWidget() || m_pollBusy || m_listBusy)
        return;
    m_pollBusy = true;
    runJob<QByteArray>([repo = m_repo, cmp = m_cmp] { return signatureOf(repo, cmp); },
                       [this](QByteArray sig) {
                           m_pollBusy = false;
                           if (sig.isEmpty() || sig == m_lastSignature)
                               return;
                           if (m_lastSignature.isEmpty()) { // first look after our own save
                               m_lastSignature = sig;
                               return;
                           }
                           m_lastSignature = sig;
                           if (m_pane->isModified()) {
                               statusBar()->showMessage(tr("Files changed on disk. Save or discard your edits to refresh."), 5000);
                               return;
                           }
                           const bool editing = m_pane->isEditing();
                           const int keep = m_pane->currentChange();
                           reload([this, editing, keep] {
                               if (keep > 0)
                                   m_pane->goToChange(keep);
                               if (editing)
                                   m_pane->setEditing(true);
                               updateEditActions();
                               statusBar()->showMessage(tr("Reloaded: files changed on disk"), 3000);
                           });
                       });
}

// ---- File filter ------------------------------------------------------------

QWidget *MainWindow::createFilePanel()
{
    // Pull requests: their overview above the files, in a splitter.
    auto *split = new QSplitter(Qt::Vertical);
    split->setChildrenCollapsible(false);
    m_overviewPanel = new PrOverviewPanel;
    m_overviewPanel->setVisible(false);
    m_overviewPanel->setFolded(Settings::prOverviewFolded());
    connect(m_overviewPanel, &PrOverviewPanel::foldedChanged, this, [this, split](bool folded) {
        Settings::setPrOverviewFolded(folded);
        const int total = split->height();
        const int top = folded ? m_overviewPanel->minimumSizeHint().height() : total / 3;
        split->setSizes({top, total - top});
    });
    connect(m_overviewPanel, &PrOverviewPanel::reloadRequested, this, &MainWindow::loadOverview);
    connect(m_overviewPanel, &PrOverviewPanel::replyRequested, this, &MainWindow::replyToThread);
    connect(m_overviewPanel, &PrOverviewPanel::resolveRequested, this, [this](qint64 id, bool resolve) {
        setThreadStatus(id, resolve ? GitHubReview::ThreadStatus::Resolved : GitHubReview::ThreadStatus::Active);
    });
    connect(m_overviewPanel, &PrOverviewPanel::commentRequested, this, [this] { submitReview(GitHubReview::Verdict::Comment); });

    auto *panel = new QWidget;
    auto *v = new QVBoxLayout(panel);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    m_filterEdit = new QLineEdit;
    m_filterEdit->setPlaceholderText(tr("Filter files (Ctrl+P)"));
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setToolTip(tr("Space-separated terms. Text matches anywhere in the path, *.ext and other globs match "
                                "names, and a leading ! excludes (for example: src !*.lock)."));
    connect(m_filterEdit, &QLineEdit::textChanged, this, &MainWindow::applyFilter);
    m_filterCount = new QLabel;
    m_filterCount->setContentsMargins(4, 0, 4, 2);
    m_filterCount->setVisible(false);
    v->addWidget(m_filterEdit);
    v->addWidget(m_files, 1);
    v->addWidget(m_filterCount);
    split->addWidget(m_overviewPanel);
    split->addWidget(panel);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 2);

    auto *focus = new QAction(tr("Filter files"), this);
    focus->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_P));
    connect(focus, &QAction::triggered, this, [this] {
        m_filterEdit->setFocus();
        m_filterEdit->selectAll();
    });
    addAction(focus);
    return split;
}

void MainWindow::applyFilter()
{
    FileFilter f(m_filterEdit->text());
    f.hideReviewed = m_hideMarkedAct && m_hideMarkedAct->isChecked();
    f.reversed = m_cmp.reversed;
    const struct {
        const char *key;
        FileCategory category;
    } map[] = {{"leftOnly", FileCategory::LeftOnly},
               {"rightOnly", FileCategory::RightOnly},
               {"different", FileCategory::Different},
               {"identical", FileCategory::Identical}};
    for (const auto &m : map)
        if (QAction *a = m_categoryActs.value(QLatin1String(m.key)); a && !a->isChecked())
            f.hiddenCategories.insert(m.category);
    if (QAction *a = m_categoryActs.value(QStringLiteral("blankOnly")); a && !a->isChecked())
        f.hideBlankOnly = true;
    m_files->setFilter(f);
    const int shown = m_files->visibleCount();
    m_filterCount->setVisible(f.isActive() && shown != m_changes.size());
    m_filterCount->setText(tr("Showing %1 of %2 files").arg(shown).arg(m_changes.size()));
}

// ---- Report -----------------------------------------------------------------

void MainWindow::exportReport()
{
    if (m_changes.isEmpty()) {
        statusBar()->showMessage(tr("Nothing to export"), 3000);
        return;
    }
    const QString suggested = QDir::home().filePath(QStringLiteral("yadiff-report.html"));
    const QString path = QFileDialog::getSaveFileName(this, tr("Export HTML report"), suggested, tr("HTML files (*.html)"));
    if (path.isEmpty())
        return;

    QVector<FileChange> ordered;
    QSet<int> reviewed;
    for (int idx : m_files->displayOrder(true)) {
        ordered << m_changes[idx];
        if (m_files->isMarked(idx))
            reviewed.insert(int(ordered.size()) - 1);
    }
    auto cancel = std::make_shared<std::atomic_bool>(false);
    auto *progress = new QProgressDialog(tr("Collecting changes…"), tr("Cancel"), 0, int(ordered.size()), this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(300);
    connect(progress, &QProgressDialog::canceled, this, [cancel] { *cancel = true; });
    QPointer<QProgressDialog> guard(progress);

    // Reviewed flags travel by position, since FileChange has no identity of its own.
    auto positions = std::make_shared<QHash<QString, int>>();
    for (int i = 0; i < ordered.size(); ++i)
        positions->insert(QString::number(int(ordered[i].group)) + u'\n' + ordered[i].path, i);

    runJob<std::optional<Report::Input>>(
        [repo = m_repo, cmp = m_cmp, ordered, reviewed, positions, cancel, guard] {
            return Report::gather(
                repo, cmp, ordered,
                [&](const FileChange &fc) {
                    return reviewed.contains(positions->value(QString::number(int(fc.group)) + u'\n' + fc.path, -1));
                },
                [&](int done, int) {
                    QMetaObject::invokeMethod(guard.data(), [guard, done] {
                        if (guard)
                            guard->setValue(done);
                    }, Qt::QueuedConnection);
                    return !*cancel;
                });
        },
        [this, path, guard](std::optional<Report::Input> in) {
            if (guard)
                guard->deleteLater();
            if (!in) {
                statusBar()->showMessage(tr("Export cancelled"), 3000);
                return;
            }
            in->title = QStringLiteral("yadiff — %1").arg(m_cmp.mode == Comparison::Mode::Paths ? tr("compare")
                                                                                                  : QDir(m_repo.root()).dirName());
            in->description = describeComparison();
            in->leftName = m_cmp.mode == Comparison::Mode::Revisions ? m_cmp.rev1Name : tr("old");
            in->rightName = m_cmp.mode == Comparison::Mode::Revisions && !m_cmp.rev2.isEmpty() ? m_cmp.rev2Name : tr("new");
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(Report::html(*in).toUtf8()) < 0) {
                QMessageBox::warning(this, tr("Export"), tr("Could not write %1:\n%2").arg(path, f.errorString()));
                return;
            }
            f.close();
            qCInfo(lcFile).noquote() << "exported the report" << path << "(" << in->files.size() << "files)";
            if (m_sync) {
                statusBar()->showMessage(tr("Saved %1").arg(path), 3000);
                return;
            }
            const auto answer = QMessageBox::question(this, tr("Report saved"),
                                                      tr("Saved %1.\nOpen it in your browser?").arg(homeShortened(path)));
            if (answer == QMessageBox::Yes)
                QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        });
}

// ---- Pull request review ----------------------------------------------------

QVector<GitHubReview::Thread> MainWindow::threadsFor(const FileChange &fc) const
{
    QVector<GitHubReview::Comment> mine;
    for (const GitHubReview::Comment &c : m_comments)
        if (c.path == fc.path || c.path == fc.oldPath)
            mine << c;
    QVector<GitHubReview::Thread> out = GitHubReview::threads(mine);
    // Comments are on the latest version (new side) and the merge base (old side): an
    // earlier version has neither, and a later old side lacks the old-side ones.
    if (m_cmp.prToVersion != 0)
        return {};
    if (m_cmp.prFromVersion != 0)
        out.removeIf([](const GitHubReview::Thread &t) { return t.first().side == QLatin1String("LEFT"); });
    if (m_hideResolvedAct && m_hideResolvedAct->isChecked())
        out.removeIf([](const GitHubReview::Thread &t) { return !GitHubReview::isOpen(t.first().status); });
    return out;
}

void MainWindow::refreshThreads()
{
    // Per file: open threads and all of them (outdated ones aren't shown, so not counted).
    QHash<QString, std::pair<int, int>> byPath;
    for (const GitHubReview::Thread &t : GitHubReview::threads(m_comments)) {
        // Only the threads shown: none in an earlier version, no old-side ones after a later old side.
        if (t.first().outdated() || m_cmp.prToVersion != 0 || (m_cmp.prFromVersion != 0 && t.first().side == QLatin1String("LEFT")))
            continue;
        auto &n = byPath[t.first().path];
        n.first += GitHubReview::isOpen(t.first().status);
        ++n.second;
    }
    QHash<int, std::pair<int, int>> counts;
    for (int i = 0; i < m_changes.size(); ++i) {
        auto n = byPath.value(m_changes[i].path);
        if (m_changes[i].oldPath != m_changes[i].path) {
            const auto o = byPath.value(m_changes[i].oldPath);
            n = {n.first + o.first, n.second + o.second};
        }
        if (n.second > 0)
            counts.insert(i, n);
    }
    m_files->setThreadCounts(counts);
    if (m_shownIndex >= 0 && m_shownIndex < m_changes.size())
        m_pane->setThreads(threadsFor(m_changes[m_shownIndex]));
}

void MainWindow::setThreadStatus(qint64 threadId, GitHubReview::ThreadStatus status)
{
    std::optional<GitHubReview::Comment> root;
    for (const GitHubReview::Comment &c : std::as_const(m_comments))
        if (c.id == threadId && c.inReplyTo == 0)
            root = c;
    if (!root && m_overview) // a comment on the PR itself
        for (const GitHubReview::Thread &t : std::as_const(m_overview->discussion))
            if (t.first().id == threadId)
                root = t.first();
    if (!root)
        return;
    const QString name = PrReview::threadStatusName(m_cmp, status);
    statusBar()->showMessage(tr("Setting the thread to %1…").arg(name));
    runJob<QString>(
        [cmp = m_cmp, root = *root, status] {
            QString error;
            return PrReview::setThreadStatus(cmp, root, status, &error) ? QString() : (error.isEmpty() ? QStringLiteral("?") : error);
        },
        [this, threadId, status, name](QString error) {
            if (!error.isEmpty()) {
                statusBar()->clearMessage();
                QMessageBox::warning(this, tr("Thread status"), tr("%1 refused the change:\n%2").arg(PrReview::hostName(m_cmp), error));
                return;
            }
            // Every comment of the thread carries it (Azure DevOps); GitHub's on the first.
            const qint64 adoThread = m_cmp.prHost == QLatin1String("ado") ? threadId : -1;
            for (GitHubReview::Comment &c : m_comments)
                if (c.id == threadId || c.inReplyTo == threadId
                    || (adoThread > 0 && c.id >= AdoPr::kReplyBase && c.id / AdoPr::kReplyBase == adoThread))
                    c.status = status;
            if (m_overview) {
                for (GitHubReview::Thread &t : m_overview->discussion)
                    if (t.first().id == threadId)
                        for (GitHubReview::Comment &c : t)
                            c.status = status;
                m_overviewPanel->setOverview(*m_overview, PrReview::hostName(m_cmp), m_cmp.prHost == QLatin1String("ado"));
            }
            statusBar()->showMessage(tr("Thread set to %1").arg(name), 3000);
            refreshThreads();
        });
}

void MainWindow::loadComments()
{
    if (m_cmp.prNumber <= 0)
        return;
    runJob<std::pair<QVector<GitHubReview::Comment>, QString>>(
        [cmp = m_cmp] {
            QString error;
            auto c = PrReview::fetch(cmp, &error);
            return std::make_pair(c.value_or(QVector<GitHubReview::Comment>()), c ? QString() : error);
        },
        [this](std::pair<QVector<GitHubReview::Comment>, QString> r) {
            if (!r.second.isEmpty()) {
                statusBar()->showMessage(tr("Could not load review comments: %1").arg(r.second.section(u'\n', 0, 0)), 6000);
                return;
            }
            m_comments = r.first;
            int outdated = 0, open = 0;
            const auto all = GitHubReview::threads(m_comments);
            for (const auto &t : all) {
                outdated += t.first().outdated();
                open += !t.first().outdated() && GitHubReview::isOpen(t.first().status);
            }
            statusBar()->showMessage(tr("%1 review threads: %2 open, %3 resolved (%4 outdated, not shown)")
                                         .arg(all.size())
                                         .arg(open)
                                         .arg(all.size() - outdated - open)
                                         .arg(outdated),
                                     5000);
            refreshThreads();
        });
}

void MainWindow::addComment(const QString &path, const QString &side, int line, int startLine)
{
    bool ok = false;
    const QString where = startLine ? tr("lines %1–%2").arg(startLine).arg(line) : tr("line %1").arg(line);
    const QString body = QInputDialog::getMultiLineText(
        this, tr("Add review comment"),
        tr("Comment on %1, %2 (%3 side).\nIt is posted to %4 straight away.")
            .arg(path, where, side == QLatin1String("LEFT") ? tr("old") : tr("new"), PrReview::hostName(m_cmp)),
        QString(), &ok);
    if (!ok || body.trimmed().isEmpty())
        return;
    statusBar()->showMessage(tr("Posting comment…"));
    runJob<std::pair<std::optional<GitHubReview::Comment>, QString>>(
        [cmp = m_cmp, path, side, line, startLine, body] {
            QString error;
            auto c = PrReview::post(cmp, path, side, line, startLine, body, &error);
            return std::make_pair(c, error);
        },
        [this](std::pair<std::optional<GitHubReview::Comment>, QString> r) {
            if (!r.first) {
                QMessageBox::warning(this, tr("Add review comment"), tr("%1 refused the comment:\n%2").arg(PrReview::hostName(m_cmp), r.second));
                return;
            }
            m_comments << *r.first;
            statusBar()->showMessage(tr("Comment posted"), 3000);
            refreshThreads();
        });
}

void MainWindow::replyToThread(qint64 threadId)
{
    bool ok = false;
    const QString body = QInputDialog::getMultiLineText(this, tr("Reply"), tr("Reply (posted to %1 straight away):").arg(PrReview::hostName(m_cmp)),
                                                        QString(), &ok);
    if (!ok || body.trimmed().isEmpty())
        return;
    runJob<std::pair<std::optional<GitHubReview::Comment>, QString>>(
        [cmp = m_cmp, threadId, body] {
            QString error;
            auto c = PrReview::reply(cmp, threadId, body, &error);
            return std::make_pair(c, error);
        },
        [this, threadId](std::pair<std::optional<GitHubReview::Comment>, QString> r) {
            if (!r.first) {
                QMessageBox::warning(this, tr("Reply"), tr("%1 refused the reply:\n%2").arg(PrReview::hostName(m_cmp), r.second));
                return;
            }
            GitHubReview::Comment c = *r.first;
            for (const GitHubReview::Comment &root : std::as_const(m_comments)) {
                if (root.id == threadId || root.id == c.inReplyTo) {
                    c.path = root.path;
                    c.side = root.side;
                    c.line = root.line;
                    c.startLine = root.startLine;
                    if (c.inReplyTo == 0)
                        c.inReplyTo = root.id;
                    break;
                }
            }
            if (c.path.isEmpty()) { // a comment on the PR itself: in the overview
                statusBar()->showMessage(tr("Reply posted"), 3000);
                loadOverview();
                return;
            }
            m_comments << c;
            statusBar()->showMessage(tr("Reply posted"), 3000);
            refreshThreads();
        });
}

namespace {

QIcon voteIcon(std::optional<GitHubReview::Vote> vote)
{
    using V = GitHubReview::Vote;
    QColor c(0x9a, 0x9a, 0x9a); // unknown or no vote
    if (vote == V::Approved)
        c = QColor(0x2e, 0xa0, 0x43);
    else if (vote == V::ApprovedWithSuggestions)
        c = QColor(0x7c, 0xc0, 0x4f);
    else if (vote == V::WaitingForAuthor)
        c = QColor(0xd9, 0x8e, 0x04);
    else if (vote == V::Rejected)
        c = QColor(0xd1, 0x3b, 0x3b);
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(c);
    p.setPen(vote ? Qt::NoPen : QPen(c, 3));
    if (!vote)
        p.setBrush(Qt::NoBrush); // a hollow dot until the vote is known
    p.drawEllipse(QRectF(6, 6, 20, 20));
    p.end();
    return QIcon(pm);
}

} // namespace

void MainWindow::rebuildVoteActions()
{
    qDeleteAll(m_voteActions);
    m_voteActions.clear();
    m_voteMenu->clear();
    const bool pr = m_cmp.prNumber > 0;
    m_voteButton->setVisible(pr);
    if (!pr)
        return;
    auto *group = new QActionGroup(this);
    for (const GitHubReview::Vote vote : PrReview::votes(m_cmp)) {
        auto *a = new QAction(voteIcon(vote), PrReview::voteAction(m_cmp, vote) + QStringLiteral("…"), this);
        a->setCheckable(vote != GitHubReview::Vote::None);
        a->setProperty("yadiffNotASetting", true);
        if (a->isCheckable())
            group->addAction(a);
        connect(a, &QAction::triggered, this, [this, vote] { castVote(vote); });
        m_voteActions << a;
    }
    // Reset reads as an action, not a state: after a separator.
    for (QAction *a : std::as_const(m_voteActions)) {
        if (!a->isCheckable()) {
            m_reviewMenu->insertSeparator(m_reviewCommentAct);
            m_voteMenu->addSeparator();
        }
        m_reviewMenu->insertAction(m_reviewCommentAct, a);
        m_voteMenu->addAction(a);
    }
    m_reviewMenu->insertSeparator(m_reviewCommentAct);
    m_voteMenu->addSeparator();
    m_voteMenu->addAction(tr("Check my vote again"), this, &MainWindow::loadMyVote);
    m_voteMenu->addAction(m_reviewCommentAct);
    // Separators left from an earlier comparison's votes: no two in a row, none first.
    QAction *previous = nullptr;
    for (QAction *a : m_reviewMenu->actions()) {
        if (a->isSeparator() && (!previous || previous->isSeparator()))
            delete a;
        else
            previous = a;
    }
    updateVoteButton();
}

void MainWindow::updateVoteButton()
{
    if (m_cmp.prNumber <= 0)
        return;
    QString state;
    if (m_voteBusy)
        state = m_myVote ? PrReview::voteState(m_cmp, *m_myVote) + QStringLiteral("…") : tr("checking…");
    else if (m_myVote)
        state = PrReview::voteState(m_cmp, *m_myVote);
    else if (!m_voteError.isEmpty())
        state = tr("unknown");
    else
        state = tr("checking…");
    m_voteButton->setText(tr("Your vote: %1").arg(state));
    m_voteButton->setIcon(voteIcon(m_voteBusy ? std::nullopt : m_myVote));
    m_voteButton->setToolTip(m_voteError.isEmpty()
                                 ? tr("Your vote on this pull request on %1. Click to change it.").arg(PrReview::hostName(m_cmp))
                                 : tr("Could not read your vote: %1").arg(m_voteError));
    const QList<GitHubReview::Vote> all = PrReview::votes(m_cmp);
    for (int i = 0; i < m_voteActions.size() && i < all.size(); ++i)
        if (m_voteActions[i]->isCheckable())
            m_voteActions[i]->setChecked(m_myVote == all[i]);
}

void MainWindow::loadMyVote()
{
    if (m_cmp.prNumber <= 0)
        return;
    m_voteBusy = true;
    updateVoteButton();
    const quint64 gen = ++m_voteGen;
    runJob<std::pair<std::optional<GitHubReview::Vote>, QString>>(
        [cmp = m_cmp] {
            QString error;
            const auto vote = PrReview::myVote(cmp, &error);
            return std::make_pair(vote, vote ? QString() : (error.isEmpty() ? QStringLiteral("?") : error));
        },
        [this, gen](std::pair<std::optional<GitHubReview::Vote>, QString> r) {
            if (gen != m_voteGen)
                return;
            m_voteBusy = false;
            if (r.first) {
                m_myVote = r.first;
                m_voteError.clear();
            } else {
                m_voteError = r.second.section(u'\n', 0, 0);
            }
            updateVoteButton();
        });
}

void MainWindow::castVote(GitHubReview::Vote vote)
{
    if (m_cmp.prNumber <= 0)
        return;
    const QString action = PrReview::voteAction(m_cmp, vote);
    const bool needsMessage = PrReview::voteNeedsMessage(m_cmp, vote);
    const QString about = needsMessage ? tr("A message is required.")
                        : m_cmp.prHost == QLatin1String("ado")
                            ? tr("A message is optional; it is posted as a comment on the pull request.")
                            : tr("A message is optional.");
    bool ok = false;
    const QString message = QInputDialog::getMultiLineText(
        this, action, tr("%1: %2\n\n%3").arg(action, m_cmp.title.isEmpty() ? PrReview::webUrl(m_cmp) : m_cmp.title, about),
        QString(), &ok);
    if (!ok || (needsMessage && message.trimmed().isEmpty())) {
        updateVoteButton(); // puts the check mark back
        return;
    }
    m_voteBusy = true;
    updateVoteButton();
    const quint64 gen = ++m_voteGen;
    runJob<QString>(
        [cmp = m_cmp, vote, message] {
            QString error;
            return PrReview::setVote(cmp, vote, message, &error) ? QString() : (error.isEmpty() ? QStringLiteral("?") : error);
        },
        [this, gen, vote, action](QString error) {
            if (gen != m_voteGen)
                return;
            m_voteBusy = false;
            if (!error.isEmpty()) {
                updateVoteButton();
                QMessageBox::warning(this, action, tr("%1 refused the vote:\n%2").arg(PrReview::hostName(m_cmp), error));
                return;
            }
            m_myVote = vote;
            m_voteError.clear();
            updateVoteButton();
            statusBar()->showMessage(tr("%1: done").arg(action), 4000);
            loadOverview(); // the reviewers' votes, and any message
        });
}

void MainWindow::submitReview(GitHubReview::Verdict verdict)
{
    if (m_cmp.prNumber <= 0)
        return;
    const QString what = verdict == GitHubReview::Verdict::Approve          ? tr("Approve")
                         : verdict == GitHubReview::Verdict::RequestChanges ? tr("Request changes")
                                                                            : tr("Comment");
    bool ok = false;
    const QString body = QInputDialog::getMultiLineText(
        this, what,
        tr("%1 on %2.%3")
            .arg(what, m_cmp.title.isEmpty() ? PrReview::webUrl(m_cmp) : m_cmp.title)
            .arg(verdict == GitHubReview::Verdict::Approve ? tr(" A message is optional.") : tr(" A message is required.")),
        QString(), &ok);
    if (!ok || (verdict != GitHubReview::Verdict::Approve && body.trimmed().isEmpty()))
        return;
    runJob<QString>(
        [cmp = m_cmp, verdict, body] {
            QString error;
            return PrReview::submit(cmp, verdict, body, &error) ? QString() : (error.isEmpty() ? QStringLiteral("?") : error);
        },
        [this, what](QString error) {
            if (!error.isEmpty())
                QMessageBox::warning(this, what, tr("%1 refused the review:\n%2").arg(PrReview::hostName(m_cmp), error));
            else {
                statusBar()->showMessage(tr("%1: submitted").arg(what), 4000);
                loadOverview();
            }
        });
}

void MainWindow::notePullRequest()
{
    // A pull request's versions stay while it's shown; the whole PR is what to come back to.
    const QString key = m_cmp.prNumber > 0 ? PrReview::webUrl(m_cmp) : QString();
    if (key != m_prKey) {
        m_prKey = key;
        m_overview.reset();
        ++m_overviewGen;
        m_overviewPanel->clear();
        m_prHistory.reset();
        m_prAutoShown = false;
        m_historyBusy = false;
        ++m_historyGen;
    }
    if (!key.isEmpty() && m_cmp.prFromVersion == 0 && m_cmp.prToVersion == 0) {
        m_prWhole = m_cmp;
        m_prWholeRoot = m_repo.root();
    }
    // For File > Open pull request: where to look next time, and what was seen.
    if (!key.isEmpty() && m_cmp.prToVersion == 0)
        PrInbox::markSeen(key, m_cmp.prHead);
    if (m_cmp.prHost == QLatin1String("ado") && m_cmp.prNumber > 0 && !m_cmp.prCollectionUrl.isEmpty() && !m_cmp.prProject.isEmpty())
        PrInbox::addSource({QUrl(m_cmp.prCollectionUrl), m_cmp.prProject});
}

void MainWindow::showPrHistory(const PrReview::History &history)
{
    ++m_historyGen;
    m_historyBusy = false;
    m_prHistory = history;
    fillVersionCombo();
    maybeShowSinceLastReview();
}

void MainWindow::maybeShowSinceLastReview()
{
    // New pushes since the user's last review: show just those (once per PR).
    if (!m_prHistory || m_prHistory->versions.isEmpty())
        return;
    const PrReview::History &h = *m_prHistory;
    const int latest = h.versions.last().number;
    const bool unknownCommit = h.lastReviewed == 0 && !h.lastReviewedCommit.isEmpty();
    if (!m_prAutoShown && m_sinceReviewAct->isChecked() && m_cmp.prFromVersion == 0 && m_cmp.prToVersion == 0
        && ((h.lastReviewed > 0 && h.lastReviewed < latest) || unknownCommit)) {
        m_prAutoShown = true;
        showPrVersions(unknownCommit ? -1 : h.lastReviewed, latest);
    }
}

void MainWindow::openPullRequest()
{
    PrListDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted || dlg.link().isEmpty())
        return;
    // From the repository shown (the user's clone, where a PR is fetched into), else home.
    QString dir = QDir::homePath();
    if (!m_cmp.empty && m_repo.isRepository() && !m_repo.isBare() && !m_cmp.readOnly)
        dir = m_repo.root();
    else if (m_cmp.empty && m_repo.isRepository())
        dir = m_repo.root();
    qCInfo(lcApp).noquote() << "opening" << dlg.link() << "from the pull request list";
    openArguments({QStringLiteral("-C"), dir, dlg.host() == QLatin1String("ado") ? QStringLiteral("-ado") : QStringLiteral("-ghpr"),
                   dlg.link()});
}

void MainWindow::loadOverview()
{
    if (m_cmp.prNumber <= 0 || !m_overviewAct->isChecked())
        return;
    m_overviewPanel->setLoading(PrReview::hostName(m_cmp));
    const quint64 gen = ++m_overviewGen;
    using Result = std::pair<std::optional<PrOverview::Overview>, QString>;
    runJob<Result>(
        [cmp = m_cmp] {
            QString error;
            auto o = PrReview::overview(cmp, &error);
            return Result{o, error};
        },
        [this, gen](Result r) {
            if (gen != m_overviewGen)
                return;
            if (!r.first) {
                if (!m_overview)
                    m_overviewPanel->setError(r.second.section(u'\n', 0, 0));
                return;
            }
            m_overview = r.first;
            m_overviewPanel->setOverview(*m_overview, PrReview::hostName(m_cmp), m_cmp.prHost == QLatin1String("ado"));
        });
}

void MainWindow::updateOverviewVisibility()
{
    m_overviewPanel->setVisible(m_cmp.prNumber > 0 && m_overviewAct->isChecked());
}

void MainWindow::loadPrHistory()
{
    if (m_cmp.prNumber <= 0)
        return;
    m_historyBusy = true;
    const quint64 gen = ++m_historyGen;
    using Result = std::pair<std::optional<PrReview::History>, QString>;
    runJob<Result>(
        [cmp = m_cmp] {
            QString error;
            auto h = PrReview::history(cmp, &error);
            return Result{h, error};
        },
        [this, gen](Result r) {
            if (gen != m_historyGen)
                return;
            m_historyBusy = false;
            if (!r.first) {
                qCInfo(lcApp).noquote() << "could not read the pull request's pushes:" << r.second;
                return;
            }
            m_prHistory = r.first;
            fillVersionCombo();
            maybeShowSinceLastReview();
        });
}

void MainWindow::fillVersionCombo()
{
    m_versionCombo->clear();
    const bool show = m_cmp.prNumber > 0 && m_prHistory && m_prHistory->versions.size() >= 2;
    m_versionComboAction->setVisible(show || (m_cmp.prNumber > 0 && m_prHistory && !m_prHistory->lastReviewedCommit.isEmpty()
                                              && m_prHistory->lastReviewed == 0));
    if (!m_prHistory || m_prHistory->versions.isEmpty())
        return;
    const PrReview::History &h = *m_prHistory;
    const int latest = h.versions.last().number;
    const bool ado = m_cmp.prHost == QLatin1String("ado");
    auto add = [&](const QString &text, int from, int to, const QString &tip = {}) {
        m_versionCombo->addItem(text, QPoint(from, to));
        if (!tip.isEmpty())
            m_versionCombo->setItemData(m_versionCombo->count() - 1, tip, Qt::ToolTipRole);
    };
    add(ado ? tr("All changes (%1 pushes)").arg(h.versions.size()) : tr("All changes (%1 commits)").arg(h.versions.size()), 0, latest);
    if (h.lastReviewed > 0 && h.lastReviewed < latest) {
        const int i = h.indexOf(h.lastReviewed);
        add(tr("Since your last review (%1)").arg(PrReview::versionName(m_cmp, h.versions[i])), h.lastReviewed, latest,
            tr("The changes pushed since the version you last voted on or reviewed"));
    } else if (h.lastReviewed == 0 && !h.lastReviewedCommit.isEmpty()) {
        add(tr("Since your last review (%1, pushed over since)").arg(h.lastReviewedCommit.left(7)), -1, latest);
    }
    // Since each earlier version, newest first (at most 30).
    int listed = 0;
    for (int i = int(h.versions.size()) - 2; i >= 0 && listed < 30; --i, ++listed) {
        const PrReview::Version &v = h.versions[i];
        QString text = tr("Since %1").arg(PrReview::versionName(m_cmp, v));
        const QString when = GitHubReview::displayTime(v.date);
        if (!v.subject.isEmpty())
            text += QStringLiteral(": ") + (v.subject.size() > 50 ? v.subject.left(48) + QStringLiteral("…") : v.subject);
        else if (!when.isEmpty())
            text += QStringLiteral(" (%1)").arg(when);
        add(text, v.number, latest, when);
    }
    m_versionCombo->addItem(ado ? tr("Compare two pushes…") : tr("Compare two commits…"));
    m_versionCombo->view()->setMinimumWidth(m_versionCombo->view()->sizeHintForColumn(0) + 32);
    // Select what's shown; a pair chosen in the dialog gets its own entry.
    const QPoint shown(m_cmp.prFromVersion, m_cmp.prToVersion == 0 ? latest : m_cmp.prToVersion);
    int found = -1;
    for (int i = 0; i < m_versionCombo->count(); ++i)
        if (found < 0 && m_versionCombo->itemData(i).isValid() && m_versionCombo->itemData(i).toPoint() == shown)
            found = i; // the first: "since your last review" rather than the same push further down
    if (found < 0) {
        auto name = [&](int n) {
            const int i = h.indexOf(n);
            return n == 0 ? tr("merge base") : n == -1 ? tr("your last review") : i >= 0 ? PrReview::versionName(m_cmp, h.versions[i]) : QString::number(n);
        };
        m_versionCombo->insertItem(1, tr("%1 → %2").arg(name(shown.x()), name(shown.y())), shown);
        found = 1;
    }
    m_versionCombo->setCurrentIndex(found);
}

void MainWindow::showPrVersions(int from, int to)
{
    if (!m_prHistory || m_prKey.isEmpty() || !maybeKeepEdits())
        return;
    const int latest = m_prHistory->versions.last().number;
    if (from == m_cmp.prFromVersion && (to == latest ? 0 : to) == m_cmp.prToVersion) {
        fillVersionCombo();
        return;
    }
    m_resolving = true;
    m_resolveStep = tr("Loading the pull request's pushes…");
    updateBusy();
    QPointer<MainWindow> self(this);
    auto progress = [self](const QString &message) {
        QMetaObject::invokeMethod(
            qApp,
            [self, message] {
                if (!self || !self->m_resolving)
                    return;
                self->m_resolveStep = message;
                self->updateBusy();
            },
            Qt::QueuedConnection);
    };
    struct Result {
        std::optional<Comparison> cmp;
        QString root, error;
    };
    const quint64 gen = ++m_historyGen;
    runJob<std::shared_ptr<Result>>(
        [whole = m_prWhole, root = m_prWholeRoot, h = *m_prHistory, from, to, progress] {
            auto r = std::make_shared<Result>();
            r->cmp = PrReview::versionComparison(whole, root, h, from, to, &r->root, &r->error, progress);
            return r;
        },
        [this, gen](std::shared_ptr<Result> r) {
            m_resolving = false;
            updateBusy();
            if (gen != m_historyGen)
                return;
            if (!r->cmp) {
                fillVersionCombo();
                QMessageBox::warning(this, tr("Compare pushes"), r->error);
                return;
            }
            QString error;
            auto repo = r->cmp->mode == Comparison::Mode::Paths ? std::optional<GitRepo>(GitRepo::forPaths(r->root))
                                                                 : GitRepo::open(r->root, &error);
            if (!repo) {
                fillVersionCombo();
                QMessageBox::warning(this, tr("Compare pushes"), error);
                return;
            }
            Comparison c = *r->cmp;
            c.options = m_cmp.options;
            const auto history = m_prHistory;
            const auto myVote = m_myVote;
            setComparison(std::move(*repo), c);
            m_prHistory = history; // the same PR: keep what's known about it
            m_myVote = myVote;
            updateVoteButton();
            fillVersionCombo();
            if (c.prFromVersion != 0 || c.prToVersion != 0)
                statusBar()->showMessage(tr("Showing %1 → %2. Choose \"All changes\" in the toolbar for the whole pull request.")
                                             .arg(c.rev1Name, c.rev2Name),
                                         8000);
        });
}

void MainWindow::choosePrVersions()
{
    if (!m_prHistory || m_prHistory->versions.isEmpty()) {
        fillVersionCombo();
        statusBar()->showMessage(m_historyBusy ? tr("Still reading the pull request's pushes…")
                                               : tr("This pull request's pushes aren't known"),
                                 4000);
        return;
    }
    const PrReview::History &h = *m_prHistory;
    QDialog dlg(this);
    const bool ado = m_cmp.prHost == QLatin1String("ado");
    dlg.setWindowTitle(ado ? tr("Compare pushes") : tr("Compare commits of the pull request"));
    auto *form = new QFormLayout(&dlg);
    auto *fromBox = new QComboBox, *toBox = new QComboBox;
    fromBox->addItem(tr("Merge base (the target branch)"), 0);
    if (h.lastReviewed == 0 && !h.lastReviewedCommit.isEmpty())
        fromBox->addItem(tr("Your last review (%1)").arg(h.lastReviewedCommit.left(7)), -1);
    for (const PrReview::Version &v : h.versions) {
        QString text = PrReview::versionName(m_cmp, v);
        if (!v.subject.isEmpty())
            text += QStringLiteral(": ") + v.subject.left(60);
        else if (!v.date.isEmpty())
            text += QStringLiteral(" (%1)").arg(GitHubReview::displayTime(v.date));
        if (v.number == h.lastReviewed)
            text += tr("  ← your last review");
        fromBox->addItem(text, v.number);
        toBox->addItem(text, v.number);
    }
    fromBox->setCurrentIndex(std::max(0, fromBox->findData(m_cmp.prFromVersion)));
    toBox->setCurrentIndex(m_cmp.prToVersion == 0 ? toBox->count() - 1 : std::max(0, toBox->findData(m_cmp.prToVersion)));
    form->addRow(tr("From (old side):"), fromBox);
    form->addRow(tr("To (new side):"), toBox);
    auto *note = new QLabel(tr("Review comments show only when the new side is the latest version."));
    note->setWordWrap(true);
    form->addRow(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted) {
        fillVersionCombo();
        return;
    }
    const int from = fromBox->currentData().toInt(), to = toBox->currentData().toInt();
    if (from > 0 && from >= to) {
        fillVersionCombo();
        QMessageBox::information(this, dlg.windowTitle(), tr("Choose an older version on the left than on the right."));
        return;
    }
    showPrVersions(from, to);
}

void MainWindow::grabAll(const QString &prefix)
{
    const bool wasSide = m_pane->mode() == DiffPane::Mode::SideBySide;
    m_files->selectFirst();
    int n = 0;
    do {
        for (const bool side : {true, false}) {
            m_pane->setMode(side ? DiffPane::Mode::SideBySide : DiffPane::Mode::Inline);
            updateViewToggle();
            QApplication::processEvents();
            grab().save(QStringLiteral("%1-%2-%3.png")
                            .arg(prefix)
                            .arg(n, 2, 10, QLatin1Char('0'))
                            .arg(side ? QStringLiteral("side") : QStringLiteral("inline")));
        }
        ++n;
    } while (m_files->selectNext());
    m_pane->setMode(wasSide ? DiffPane::Mode::SideBySide : DiffPane::Mode::Inline);
    updateViewToggle();
}

// ---- File list context menu and file operations -----------------------------

QVector<FileChange> MainWindow::selectedChanges() const
{
    QVector<FileChange> out;
    for (int i : m_files->selectedIndices())
        if (i >= 0 && i < m_changes.size())
            out << m_changes[i];
    return out;
}

QString MainWindow::scrollKey(const FileChange &fc) const
{
    return QString::number(int(fc.group)) + u'\n' + fc.path;
}

void MainWindow::showFileContextMenu(const QPoint &globalPos)
{
    const QVector<int> sel = m_files->selectedIndices();
    if (sel.isEmpty())
        return;
    const bool working = m_repo.isRepository() && !m_repo.isBare();
    const bool pr = m_cmp.prNumber > 0;
    QMenu menu(this);
    menu.addAction(tr("&Copy"), this, [this] {
        QStringList paths;
        for (const FileChange &fc : selectedChanges())
            paths << fc.path;
        QApplication::clipboard()->setText(paths.join(u'\n'));
    });
    menu.addAction(tr("Select &all"), m_files, &QTreeWidget::selectAll);
    QAction *hash = menu.addAction(tr("Copy commit &hash"), this, &MainWindow::copyCommitHash);
    hash->setEnabled(m_repo.isRepository() && sel.size() == 1);
    menu.addSeparator();
    QAction *view = menu.addAction(tr("&View differences"), this, [this, sel] { emit m_files->viewRequested(sel.first()); });
    view->setShortcut(QKeySequence(Qt::Key_Return));
    QAction *gitk = menu.addAction(tr("View filelog in &gitk"), this, &MainWindow::openInGitk);
    gitk->setEnabled(m_repo.isRepository() && sel.size() == 1 && !QStandardPaths::findExecutable(QStringLiteral("gitk")).isEmpty());
    menu.addSeparator();
    bool allMarked = true;
    for (int i : sel)
        allMarked = allMarked && m_files->isMarked(i);
    menu.addAction(allMarked ? tr("Un&mark file") : tr("&Mark file"), this, [this, allMarked] { markSelected(!allMarked); });
    bool anyStaged = false, anyUnstaged = false;
    for (const FileChange &fc : selectedChanges()) {
        anyStaged = anyStaged || fc.group == ChangeGroup::Staged;
        anyUnstaged = anyUnstaged || fc.group != ChangeGroup::Staged;
    }
    QAction *stage = menu.addAction(tr("&Stage file"), this, [this] { stageSelected(true); });
    stage->setEnabled(working && anyUnstaged && m_cmp.mode != Comparison::Mode::Paths);
    if (anyStaged)
        menu.addAction(tr("&Unstage file"), this, [this] { stageSelected(false); })->setEnabled(working);
    if (sel.size() == 1 && m_changes.value(sel.first()).status == u'U')
        menu.addAction(tr("Resolve &conflict\u2026"), this, [this, index = sel.first()] {
            if (m_shownIndex != index)
                m_files->selectIndex(index);
            resolveConflict();
        });
    QAction *revert = menu.addAction(tr("&Revert file…"), this, &MainWindow::revertSelected);
    revert->setEnabled(m_revertAct->isEnabled());
    QAction *download = menu.addAction(tr("&Download file again"), this, [this] { downloadAgain(true); });
    download->setEnabled(pr || m_cmp.mode != Comparison::Mode::WorkingTree);
    menu.addSeparator();
    QAction *refresh = menu.addAction(tr("Re&fresh list"), this, [this] {
        if (maybeKeepEdits())
            reload();
    });
    refresh->setShortcut(QKeySequence(Qt::Key_F5));
    menu.exec(globalPos);
}

void MainWindow::stageSelected(bool stage)
{
    QVector<FileChange> picked;
    for (const FileChange &fc : selectedChanges())
        if (fc.status != u'=' && (stage ? fc.group != ChangeGroup::Staged : fc.group == ChangeGroup::Staged || m_cmp.allChanges))
            picked << fc;
    if (picked.isEmpty()) {
        statusBar()->showMessage(stage ? tr("Nothing to stage in the selection") : tr("Nothing to unstage in the selection"), 3000);
        return;
    }
    if (!maybeKeepEdits())
        return;
    // git add in a large repository can take a while: in the background.
    statusBar()->showMessage(stage ? tr("Staging…") : tr("Unstaging…"));
    runJob<QString>(
        [repo = m_repo, picked, stage] {
            QString error;
            const bool ok = stage ? repo.stage(picked, &error) : repo.unstage(picked, &error);
            return ok ? QString() : (error.isEmpty() ? QStringLiteral("?") : error);
        },
        [this, picked, stage](QString error) {
            if (!error.isEmpty()) {
                statusBar()->clearMessage();
                QMessageBox::warning(this, stage ? tr("Stage") : tr("Unstage"), error);
                return;
            }
            const QString files = picked.size() == 1 ? picked.first().path : tr("%1 files").arg(picked.size());
            statusBar()->showMessage(stage ? tr("Staged %1").arg(files) : tr("Unstaged %1").arg(files), 3000);
            reload();
        });
}

void MainWindow::revertSelected()
{
    QVector<FileChange> picked;
    for (const FileChange &fc : selectedChanges())
        if (fc.status != u'=')
            picked << fc;
    if (picked.isEmpty())
        return;
    if (!maybeKeepEdits())
        return;
    QStringList names;
    for (const FileChange &fc : picked)
        names << fc.path;
    const QString what = m_cmp.mode == Comparison::Mode::Paths
                             ? tr("The right-hand files are replaced by the left-hand ones (or removed when the left has none).")
                             : tr("Their changes are discarded. Files the left side doesn't have are moved to the trash.");
    QMessageBox box(QMessageBox::Warning, tr("Revert files"),
                    picked.size() == 1 ? tr("Revert %1?").arg(names.first()) : tr("Revert %1 files?").arg(picked.size()),
                    QMessageBox::Cancel, this);
    box.setInformativeText(what + tr("\nThis cannot be undone from yadiff."));
    if (picked.size() > 1)
        box.setDetailedText(names.join(u'\n'));
    QPushButton *revert = box.addButton(tr("Revert"), QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != revert)
        return;
    statusBar()->showMessage(tr("Reverting…"));
    runJob<QString>(
        [repo = m_repo, picked, cmp = m_cmp] {
            QString error;
            return repo.revert(picked, cmp, &error) ? QString() : (error.isEmpty() ? QStringLiteral("?") : error);
        },
        [this, picked, names](QString error) {
            if (!error.isEmpty()) {
                statusBar()->clearMessage();
                QMessageBox::warning(this, tr("Revert files"), error);
            } else {
                statusBar()->showMessage(tr("Reverted %1").arg(picked.size() == 1 ? names.first() : tr("%1 files").arg(picked.size())), 4000);
            }
            reload();
        });
}

void MainWindow::swapSides()
{
    if (!maybeKeepEdits())
        return;
    Comparison c = m_cmp;
    if (c.mode == Comparison::Mode::Paths) {
        std::swap(c.leftPath, c.rightPath);
    } else if (c.mode == Comparison::Mode::Revisions && !c.rev2.isEmpty()) {
        std::swap(c.rev1, c.rev2);
        std::swap(c.rev1Name, c.rev2Name);
    } else {
        c.reversed = !c.reversed; // the working tree changes sides (git diff -R)
    }
    // Keep everything else the comparison carries.
    const DiffOptions options = m_cmp.options;
    m_cmp = c;
    m_cmp.options = options;
    m_changes.clear();
    m_shownIndex = -1;
    qCInfo(lcApp).noquote() << "swapped sides:" << m_cmp.describe();
    updateModeUi();
    updateTitle();
    reload();
    statusBar()->showMessage(tr("Swapped left and right"), 2000);
}

void MainWindow::downloadAgain(bool onlySelected)
{
    if (!maybeKeepEdits())
        return;
    if (onlySelected) {
        for (int i : m_files->selectedIndices())
            m_diffCache.remove(i);
        reloadCurrentFile();
        return;
    }
    if (m_cmp.prNumber <= 0) {
        m_diffCache.clear();
        reload();
        return;
    }
    if (m_cmp.readOnly) {
        // Downloaded through the API: open it again, which downloads a newer iteration.
        QString error;
        if (auto req = ComparisonArgs::fromArguments(comparisonArguments(), QDir::homePath(), &error))
            openInBackground(*req);
        else
            QMessageBox::warning(this, tr("Download again"), error);
        return;
    }
    // Pull requests: fetch again, which also picks up commits pushed since.
    statusBar()->showMessage(tr("Fetching the pull request again…"));
    struct Result {
        QString base, head, baseName, headName, title, error;
    };
    runJob<Result>(
        [cmp = m_cmp, cwd = m_repo.root()] {
            Result r;
            if (cmp.prHost == QLatin1String("ado")) {
                AdoPr::Ref ref;
                ref.collectionUrl = QUrl(cmp.prCollectionUrl);
                ref.project = cmp.prProject;
                ref.repo = cmp.prRepo;
                ref.id = cmp.prNumber;
                ref.webUrl = QUrl(cmp.prWebUrl);
                const auto cred = AdoAuth::find(ref.collectionUrl, &r.error);
                if (!cred)
                    return r;
                r.error.clear();
                if (const auto pr = AdoPr::resolve(ref, *cred, cwd, &r.error))
                    r = {pr->baseOid, pr->headOid, pr->baseName, pr->headName, pr->title, {}};
            } else {
                if (const auto pr = GitHubPr::resolve({cmp.prOwner, cmp.prRepo, cmp.prNumber}, cwd, &r.error))
                    r = {pr->baseOid, pr->headOid, pr->baseName, pr->headName, pr->title, {}};
            }
            return r;
        },
        [this](Result r) {
            if (!r.error.isEmpty() || r.head.isEmpty()) {
                QMessageBox::warning(this, tr("Download again"), r.error.isEmpty() ? tr("The pull request could not be fetched.") : r.error);
                return;
            }
            const bool moved = r.head != m_cmp.rev2;
            m_cmp.rev1 = r.base;
            m_cmp.rev2 = r.head;
            m_cmp.rev1Name = r.baseName;
            m_cmp.rev2Name = r.headName;
            m_cmp.title = r.title;
            m_diffCache.clear();
            updateTitle();
            reload();
            statusBar()->showMessage(moved ? tr("The pull request has new commits; showing the latest") : tr("Up to date"), 4000);
        });
}

void MainWindow::markSelected(bool marked)
{
    for (int i : m_files->selectedIndices())
        m_files->setMarked(i, marked);
}

void MainWindow::markByPattern()
{
    bool ok = false;
    const QString pattern = QInputDialog::getText(this, tr("Mark by pattern"),
                                                  tr("Mark the files matching (same syntax as the filter box, e.g. *.h src !test):"),
                                                  QLineEdit::Normal, m_filterEdit->text(), &ok);
    if (!ok || pattern.trimmed().isEmpty())
        return;
    const FileFilter f(pattern);
    int n = 0;
    for (int i = 0; i < m_changes.size(); ++i)
        if (f.matches(m_changes[i].path, m_changes[i].status, false)) {
            m_files->setMarked(i, true);
            ++n;
        }
    statusBar()->showMessage(n == 1 ? tr("Marked 1 file") : tr("Marked %1 files").arg(n), 3000);
}

void MainWindow::markIdentical()
{
    // A file is identical when it shows no changes with the current options (it may
    // still be listed: e.g. only whitespace differs while whitespace is ignored).
    statusBar()->showMessage(tr("Looking for identical files…"));
    runJob<QVector<int>>(
        [repo = m_repo, cmp = m_cmp, changes = m_changes, cache = m_diffCache] {
            QVector<int> out;
            for (int i = 0; i < changes.size(); ++i) {
                if (changes[i].status == u'=') {
                    out << i;
                    continue;
                }
                QString error;
                const FileDiff d = cache.contains(i) ? cache.value(i) : repo.loadDiff(changes[i], cmp, &error);
                if (error.isEmpty() && !d.binary && d.chunks.isEmpty())
                    out << i;
            }
            return out;
        },
        [this](QVector<int> identical) {
            for (int i : identical)
                m_files->setMarked(i, true);
            statusBar()->showMessage(identical.size() == 1 ? tr("Marked 1 identical file") : tr("Marked %1 identical files").arg(identical.size()), 3000);
        });
}

void MainWindow::copyCommitHash()
{
    const QVector<FileChange> sel = selectedChanges();
    if (sel.size() != 1 || !m_repo.isRepository())
        return;
    // The commit the right side comes from; for the working tree, the file's last commit.
    auto copy = [this, path = sel.first().path](const QString &hash) {
        if (hash.isEmpty()) {
            statusBar()->showMessage(tr("%1 has no commits yet").arg(path), 3000);
            return;
        }
        QApplication::clipboard()->setText(hash);
        statusBar()->showMessage(tr("Copied %1").arg(hash.left(12)), 3000);
    };
    if (m_cmp.mode == Comparison::Mode::Revisions && !m_cmp.rev2.isEmpty() && !m_cmp.reversed) {
        copy(m_cmp.rev2);
        return;
    }
    // A file's last commit walks history: slow in a large repository.
    statusBar()->showMessage(tr("Finding the last commit of %1…").arg(sel.first().path));
    runJob<QString>([repo = m_repo, path = sel.first().oldPath] { return repo.lastCommit(path); }, copy);
}

void MainWindow::openInGitk()
{
    const QVector<FileChange> sel = selectedChanges();
    if (sel.size() != 1)
        return;
    QProcess p;
    p.setProgram(QStringLiteral("gitk"));
    QStringList args;
    if (m_cmp.mode == Comparison::Mode::Revisions && !m_cmp.rev2.isEmpty())
        args << m_cmp.rev2; // a PR's history isn't on any local branch
    args << QStringLiteral("--") << sel.first().path;
    p.setArguments(args);
    p.setWorkingDirectory(m_repo.root());
    if (m_repo.isBare()) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("GIT_DIR"), m_repo.root());
        p.setProcessEnvironment(env);
    }
    const bool started = p.startDetached();
    Log::launched(lcProc(), p.program(), p.arguments(), started);
    if (!started)
        QMessageBox::warning(this, tr("gitk"), tr("Could not start gitk."));
}

void MainWindow::newComparison()
{
    NewComparisonDialog dlg(m_repo.isRepository() && !m_repo.isBare() ? m_repo.root() : QString(), this);
    if (dlg.exec() == QDialog::Accepted)
        openArguments(dlg.arguments());
}

void MainWindow::openArguments(const QStringList &args, const std::optional<Session> &session, const QString &sessionFile)
{
    if (!m_cmp.empty) {
        // A new window (its own process), so this comparison stays as it is.
        const QStringList childArgs = sessionFile.isEmpty() ? args : QStringList{QStringLiteral("--session"), sessionFile};
        const bool started = QProcess::startDetached(QCoreApplication::applicationFilePath(), childArgs);
        Log::launched(lcApp(), QCoreApplication::applicationFilePath(), childArgs, started);
        if (!started)
            QMessageBox::warning(this, tr("New comparison"), tr("Could not start yadiff."));
        return;
    }
    // An empty window has nothing to keep: show the comparison right here.
    QString error;
    auto req = ComparisonArgs::fromArguments(args, m_repo.root(), &error);
    if (req && (!req->ghpr.isEmpty() || !req->ado.isEmpty())) {
        openInBackground(*req, session);
        return;
    }
    std::optional<ComparisonArgs::Result> resolved;
    if (req) {
        req->progress = [this](const QString &msg) {
            statusBar()->showMessage(msg);
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        };
        QApplication::setOverrideCursor(Qt::WaitCursor);
        resolved = ComparisonArgs::resolve(*req, &error);
        QApplication::restoreOverrideCursor();
        statusBar()->clearMessage();
    }
    if (!resolved) {
        QMessageBox::warning(this, tr("New comparison"), error);
        return;
    }
    setComparison(std::move(resolved->repo), resolved->cmp);
    if (session)
        applySession(*session);
}

// ---- Recent comparisons and sessions -------------------------------------------

QStringList MainWindow::comparisonArguments() const
{
    // Arguments that open this comparison again (from any directory).
    const Comparison &c = m_cmp;
    // A downloaded PR's folder is in yadiff's cache and goes after a week: not a place to start from.
    QStringList a{QStringLiteral("-C"), c.mode == Comparison::Mode::Patch ? c.patchBase
                                        : c.readOnly                      ? QDir::homePath()
                                                                          : m_repo.root()};
    if (c.prNumber > 0 && c.prHost == QLatin1String("github"))
        return a << QStringLiteral("-ghpr")
                 << QStringLiteral("https://github.com/%1/%2/pull/%3").arg(c.prOwner, c.prRepo).arg(c.prNumber);
    if (c.prNumber > 0 && c.prHost == QLatin1String("ado") && !c.prWebUrl.isEmpty())
        return a << QStringLiteral("-ado") << c.prWebUrl;
    switch (c.mode) {
    case Comparison::Mode::Patch:
        a << QStringLiteral("--patch") << c.patchPath;
        break;
    case Comparison::Mode::Paths:
        a << QStringLiteral("--no-index") << c.leftPath << c.rightPath;
        break;
    case Comparison::Mode::Revisions:
        a << c.rev1;
        if (!c.rev2.isEmpty())
            a << c.rev2;
        break;
    case Comparison::Mode::WorkingTree:
        if (c.allChanges)
            a << QStringLiteral("-a");
        break;
    }
    if (!c.title.isEmpty() && c.mode != Comparison::Mode::Patch)
        a = a.mid(0, 2) + QStringList{QStringLiteral("--title"), c.title} + a.mid(2); // options before the paths
    if (!c.paths.isEmpty() && c.mode != Comparison::Mode::Paths) {
        a << QStringLiteral("--");
        for (const QString &p : c.paths)
            a << QDir(m_repo.root()).filePath(p);
    }
    return a;
}

void MainWindow::rememberRecent()
{
    if (m_cmp.empty || m_steppingCommits)
        return;
    // git difftool's temporary copies (straight in the temp folder, or a git-difftool.*
    // folder there) are gone once it finishes.
    auto difftoolTemp = [](const QString &p) {
        return QFileInfo(p).absolutePath() == QDir::tempPath() || p.contains(QLatin1String("/git-difftool."));
    };
    if (m_cmp.mode == Comparison::Mode::Paths && (difftoolTemp(m_cmp.leftPath) || difftoolTemp(m_cmp.rightPath)))
        return;
    QString where = m_cmp.mode == Comparison::Mode::Paths   ? QString()
                    : m_cmp.mode == Comparison::Mode::Patch ? homeShortened(m_cmp.patchBase)
                                                            : QDir(m_repo.root()).dirName();
    const QString label = where.isEmpty() ? describeComparison() : QStringLiteral("%1 \u2014 %2").arg(where, describeComparison());
    Settings::addRecentComparison(comparisonArguments(), label);
}

void MainWindow::rebuildRecentMenu()
{
    m_recentMenu->clear();
    const QVector<Settings::RecentComparison> recent = Settings::recentComparisons();
    for (const Settings::RecentComparison &r : recent) {
        QAction *a = m_recentMenu->addAction(r.label, this, [this, args = r.arguments] { openArguments(args); });
        a->setToolTip(QStringLiteral("yadiff ") + r.arguments.join(u' '));
        a->setStatusTip(a->toolTip());
    }
    m_recentMenu->setToolTipsVisible(true);
    if (recent.isEmpty())
        m_recentMenu->addAction(tr("(none yet)"))->setEnabled(false);
    m_recentMenu->addSeparator();
    m_recentMenu->addAction(tr("Clear the list"), this, [] { Settings::clearRecentComparisons(); })->setEnabled(!recent.isEmpty());
}

void MainWindow::saveSession()
{
    if (m_cmp.empty)
        return;
    const QString path = QFileDialog::getSaveFileName(this, tr("Save session"), QDir::homePath() + QStringLiteral("/comparison.yadiff"),
                                                      tr("yadiff sessions (*.yadiff)"));
    if (path.isEmpty())
        return;
    Session session;
    session.arguments = comparisonArguments();
    session.title = m_customTitle;
    if (m_shownIndex >= 0 && m_shownIndex < m_changes.size())
        session.currentFile = m_changes[m_shownIndex].path;
    session.sideBySide = m_pane->mode() == DiffPane::Mode::SideBySide ? 1 : 0;
    session.hasOptions = true;
    session.options = m_cmp.options;
    QString error;
    if (!session.write(path, &error))
        QMessageBox::warning(this, tr("Save session"), error);
    else
        statusBar()->showMessage(tr("Saved %1").arg(path), 4000);
}

void MainWindow::openSession()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Open session"), QDir::homePath(), tr("yadiff sessions (*.yadiff)"));
    if (!path.isEmpty())
        openSessionFile(path);
}

void MainWindow::openSessionFile(const QString &path)
{
    QString error;
    const std::optional<Session> session = Session::read(path, &error);
    if (!session) {
        QMessageBox::warning(this, tr("Open session"), error);
        return;
    }
    openArguments(session->arguments, session, QFileInfo(path).absoluteFilePath());
}

void MainWindow::applySession(const Session &session)
{
    if (!session.title.isEmpty()) {
        m_customTitle = session.title;
        updateTitle();
    }
    if (session.sideBySide >= 0)
        setSideBySide(session.sideBySide == 1);
    m_selectOnLoad = session.currentFile;
    if (session.hasOptions) {
        // The session's options for this window only; the saved defaults stay.
        m_cmp.options = session.options;
        syncOptionControls();
        m_diffCache.clear();
    }
    reload();
}

// ---- Text, clipboard and patches ------------------------------------------------

void MainWindow::compareText()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Compare text"));
    dlg.resize(1000, 600);
    auto *left = new QPlainTextEdit;
    auto *right = new QPlainTextEdit;
    for (QPlainTextEdit *e : {left, right}) {
        e->setFont(Settings::font());
        e->setLineWrapMode(QPlainTextEdit::NoWrap);
    }
    auto *name = new QLineEdit(QStringLiteral("text.txt"));
    name->setToolTip(tr("The name's extension sets the syntax highlighting and comment syntax"));
    auto paste = [](QPlainTextEdit *e) { e->setPlainText(QApplication::clipboard()->text()); };
    auto *grid = new QGridLayout;
    auto *pasteLeft = new QPushButton(tr("Paste"));
    auto *pasteRight = new QPushButton(tr("Paste"));
    connect(pasteLeft, &QPushButton::clicked, &dlg, [=] { paste(left); });
    connect(pasteRight, &QPushButton::clicked, &dlg, [=] { paste(right); });
    grid->addWidget(new QLabel(tr("Left")), 0, 0);
    grid->addWidget(pasteLeft, 0, 1, Qt::AlignRight);
    grid->addWidget(new QLabel(tr("Right")), 0, 2);
    grid->addWidget(pasteRight, 0, 3, Qt::AlignRight);
    grid->addWidget(left, 1, 0, 1, 2);
    grid->addWidget(right, 1, 2, 1, 2);
    auto *nameRow = new QHBoxLayout;
    nameRow->addWidget(new QLabel(tr("File name:")));
    nameRow->addWidget(name, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Compare"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dlg);
    layout->addLayout(grid, 1);
    layout->addLayout(nameRow);
    layout->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString fileName = QFileInfo(name->text().trimmed()).fileName().isEmpty() ? QStringLiteral("text.txt")
                                                                                    : QFileInfo(name->text().trimmed()).fileName();
    compareTexts(left->toPlainText().toUtf8(), right->toPlainText().toUtf8(), fileName, tr("Text comparison"));
}

void MainWindow::compareTexts(const QByteArray &left, const QByteArray &right, const QString &fileName, const QString &title)
{
    // Each side is its own scratch folder so both files can keep the same name.
    const QString l = Scratch::write(Scratch::newFolder(), fileName, left);
    const QString r = Scratch::write(Scratch::newFolder(), fileName, right);
    if (l.isEmpty() || r.isEmpty()) {
        QMessageBox::warning(this, title, tr("Could not write the texts to yadiff's cache folder."));
        return;
    }
    openArguments({QStringLiteral("-C"), QFileInfo(l).absolutePath(), QStringLiteral("--title"), title,
                   QStringLiteral("--no-index"), l, r});
}

void MainWindow::compareClipboard(bool newSide)
{
    if (m_shownIndex < 0 || m_shownIndex >= m_changes.size()) {
        statusBar()->showMessage(tr("Open a file first"), 3000);
        return;
    }
    const FileChange &fc = m_changes[m_shownIndex];
    const QString clip = QApplication::clipboard()->text();
    if (clip.isEmpty()) {
        statusBar()->showMessage(tr("The clipboard has no text"), 3000);
        return;
    }
    const QString name = QFileInfo(newSide ? fc.path : fc.oldPath).fileName();
    QString file = m_repo.diskPath(fc, m_cmp, newSide);
    if (file.isEmpty()) {
        // A side from a commit: compare against a copy of it.
        const auto content = m_repo.fileContent(fc, m_cmp, newSide);
        if (!content) {
            statusBar()->showMessage(tr("That side of the file doesn't exist"), 3000);
            return;
        }
        file = Scratch::write(Scratch::newFolder(), name, *content);
    }
    const QString clipFile = Scratch::write(Scratch::newFolder(), name, clip.toUtf8());
    if (file.isEmpty() || clipFile.isEmpty())
        return;
    openArguments({QStringLiteral("-C"), QFileInfo(clipFile).absolutePath(), QStringLiteral("--title"),
                   tr("%1 \u2194 clipboard").arg(name), QStringLiteral("--no-index"), file, clipFile});
}

void MainWindow::openPatch()
{
    const QString start = m_repo.isRepository() ? m_repo.root() : QDir::homePath();
    const QString path = QFileDialog::getOpenFileName(this, tr("Open patch"), start,
                                                      tr("Patches (*.patch *.diff);;All files (*)"));
    if (!path.isEmpty())
        openPatchFile(path);
}

void MainWindow::openPatchFile(const QString &path)
{
    // The patch applies to the repository it's in, else to this window's repository.
    QString base = QFileInfo(path).absolutePath();
    if (const auto here = GitRepo::open(base, nullptr))
        base = here->root();
    else if (m_repo.isRepository() && !m_repo.isBare())
        base = m_repo.root();
    openArguments({QStringLiteral("-C"), base, QStringLiteral("--patch"), QFileInfo(path).absoluteFilePath()});
}

void MainWindow::applyPatch()
{
    if (m_cmp.mode != Comparison::Mode::Patch)
        return;
    QMessageBox box(QMessageBox::Question, tr("Apply patch"),
                    tr("Apply %1 to the files in %2?").arg(QFileInfo(m_cmp.patchPath).fileName(), homeShortened(m_cmp.patchBase)),
                    QMessageBox::Cancel, this);
    QPushButton *work = box.addButton(tr("Apply"), QMessageBox::AcceptRole);
    QPushButton *index = m_repo.isRepository() ? box.addButton(tr("Apply and stage"), QMessageBox::AcceptRole) : nullptr;
    QPushButton *reverse = box.addButton(tr("Reverse (undo it)"), QMessageBox::AcceptRole);
    box.setDefaultButton(work);
    box.exec();
    QStringList extra;
    if (box.clickedButton() == index)
        extra << QStringLiteral("--index");
    else if (box.clickedButton() == reverse)
        extra << QStringLiteral("-R");
    else if (box.clickedButton() != work)
        return;
    // git apply works on plain folders too; --check first so nothing is half applied.
    QProcess p;
    p.setWorkingDirectory(m_cmp.patchBase);
    const QStringList args = QStringList{QStringLiteral("apply")} + extra;
    QByteArray stdErr;
    auto gitApply = [&](const QStringList &more) {
        Log::ProcessTrace trace(lcFile());
        p.start(QStringLiteral("git"), args + more);
        p.waitForFinished(60000);
        stdErr = p.readAllStandardError();
        trace.done(p, -1, stdErr);
    };
    gitApply({QStringLiteral("--check"), m_cmp.patchPath});
    if (p.exitCode() == 0)
        gitApply({m_cmp.patchPath});
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        QMessageBox::warning(this, tr("Apply patch"),
                             tr("git apply failed; nothing was changed:\n\n%1").arg(QString::fromUtf8(stdErr)));
        return;
    }
    m_diffCache.clear();
    reload();
    statusBar()->showMessage(tr("Applied %1").arg(QFileInfo(m_cmp.patchPath).fileName()), 5000);
}

void MainWindow::saveAsPatch()
{
    if (m_cmp.empty)
        return;
    const QString path = QFileDialog::getSaveFileName(this, tr("Save as patch"), QDir::homePath() + QStringLiteral("/changes.patch"),
                                                      tr("Patches (*.patch *.diff)"));
    if (path.isEmpty())
        return;
    statusBar()->showMessage(tr("Writing the patch…"));
    using Result = std::pair<std::optional<QByteArray>, QString>;
    runJob<Result>(
        [repo = m_repo, cmp = m_cmp] {
            QString error;
            auto patch = repo.patch(cmp, &error);
            return Result{patch, error};
        },
        [this, path](Result r) {
            const auto &patch = r.first;
            QFile f(path);
            if (!patch || !f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(*patch) != patch->size()) {
                qCWarning(lcFile).noquote() << "could not save the patch" << path << ":" << (patch ? f.errorString() : r.second);
                statusBar()->clearMessage();
                QMessageBox::warning(this, tr("Save as patch"), patch ? f.errorString() : r.second);
                return;
            }
            qCInfo(lcFile).noquote() << "saved the patch" << path << "(" << patch->size() << "bytes)";
            statusBar()->showMessage(tr("Wrote %1").arg(path), 4000);
        });
}

// ---- Drag and drop -----------------------------------------------------------------

namespace {
QStringList localPaths(const QMimeData *mime)
{
    QStringList out;
    if (mime && mime->hasUrls())
        for (const QUrl &u : mime->urls())
            if (u.isLocalFile())
                out << QDir::cleanPath(u.toLocalFile());
    return out;
}
} // namespace

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (!localPaths(event->mimeData()).isEmpty())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const QStringList paths = localPaths(event->mimeData());
    if (paths.isEmpty())
        return;
    event->acceptProposedAction();
    openDropped(paths);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // The text views would take a drop of files as text; the window handles it instead.
    if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
        auto *e = static_cast<QDragMoveEvent *>(event);
        if (!localPaths(e->mimeData()).isEmpty()) {
            e->acceptProposedAction();
            return true;
        }
    } else if (event->type() == QEvent::Drop) {
        auto *e = static_cast<QDropEvent *>(event);
        const QStringList paths = localPaths(e->mimeData());
        if (!paths.isEmpty()) {
            e->acceptProposedAction();
            openDropped(paths);
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::openDropped(const QStringList &paths)
{
    auto open2 = [this](const QString &a, const QString &b) {
        if (QFileInfo(a).isDir() != QFileInfo(b).isDir()) {
            QMessageBox::warning(this, tr("Compare"), tr("Drop two files or two folders (not one of each)."));
            return;
        }
        openArguments({QStringLiteral("-C"), QFileInfo(a).absolutePath(), QStringLiteral("--no-index"), a, b});
    };
    if (paths.size() >= 2) {
        m_droppedLeft.clear();
        open2(paths[0], paths[1]);
        return;
    }
    const QString p = paths.first();
    const QString suffix = QFileInfo(p).suffix().toLower();
    if (suffix == QLatin1String("yadiff")) {
        openSessionFile(p);
    } else if (suffix == QLatin1String("patch") || suffix == QLatin1String("diff")) {
        openPatchFile(p);
    } else if (m_droppedLeft.isEmpty() || m_droppedLeft == p) {
        m_droppedLeft = p;
        statusBar()->showMessage(tr("Left side: %1 \u2014 drop another file or folder to compare it with").arg(homeShortened(p)));
    } else {
        const QString left = std::exchange(m_droppedLeft, QString());
        statusBar()->clearMessage();
        open2(left, p);
    }
}

void MainWindow::changeWindowTitle()
{
    bool ok = false;
    const QString title = QInputDialog::getText(this, tr("Change window title"),
                                                tr("Window title (empty: the default):"), QLineEdit::Normal,
                                                m_customTitle.isEmpty() ? windowTitle() : m_customTitle, &ok);
    if (!ok)
        return;
    m_customTitle = title.trimmed();
    updateTitle();
}

void MainWindow::editIgnorePatterns()
{
    bool ok = false;
    const QString text = QInputDialog::getMultiLineText(
        this, tr("Globbing ignore patterns"),
        tr("Files matching any of these are left out of the list, one pattern per line.\n"
           "Like .gitignore: *.lock, build/, src/**/gen/*, # comments."),
        Settings::ignoreGlobs().join(u'\n'), &ok);
    if (!ok)
        return;
    QStringList globs;
    for (const QString &line : text.split(u'\n'))
        if (!line.trimmed().isEmpty())
            globs << line.trimmed();
    Settings::setIgnoreGlobs(globs);
    m_cmp.ignoreGlobs = globs;
    if (!globs.isEmpty() && !m_useGlobsAct->isChecked())
        m_useGlobsAct->setChecked(true); // reloads
    else if (maybeKeepEdits())
        reload();
}

void MainWindow::editUnimportantText()
{
    QString text = Settings::unimportantPatterns().join(u'\n');
    for (;;) {
        bool ok = false;
        text = QInputDialog::getMultiLineText(
            this, tr("Unimportant text"),
            tr("Text matching any of these regular expressions doesn't count as a change, one per line.\n"
               "Lines that differ only in such text show as unchanged (greyed). For example:\n"
               "  \\d{4}-\\d{2}-\\d{2}[ T]\\d{2}:\\d{2}(:\\d{2})?    dates and times\n"
               "  \\$(Id|Revision|Date):[^$]*\\$    version-control keywords\n"
               "  [0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}    GUIDs"),
            text, &ok);
        if (!ok)
            return;
        QStringList patterns, bad;
        for (const QString &line : text.split(u'\n')) {
            if (line.trimmed().isEmpty())
                continue;
            if (const QRegularExpression re(line); !re.isValid())
                bad << tr("%1: %2").arg(line, re.errorString());
            patterns << line;
        }
        if (!bad.isEmpty()) {
            QMessageBox::warning(this, tr("Unimportant text"), tr("Not a valid regular expression:\n\n%1").arg(bad.join(u'\n')));
            continue;
        }
        Settings::setUnimportantPatterns(patterns);
        DiffOptions o = m_cmp.options;
        o.unimportantPatterns = patterns; // editing the rules turns them on (or off, when there are none)
        setDiffOptions(o);
        return;
    }
}

void MainWindow::showUsage()
{
    QMessageBox box(QMessageBox::Information, tr("yadiff usage"), tr("Command line"), QMessageBox::Close, this);
    box.setInformativeText(QStringLiteral("<pre style=\"font-family:monospace\">%1</pre>").arg(yadiffUsageText().toHtmlEscaped()));
    box.setTextFormat(Qt::RichText);
    box.exec();
}

void MainWindow::unregisterDifftool()
{
    if (!Difftool::isRegistered()) {
        QMessageBox::information(this, tr("Unregister"), tr("yadiff is not registered as a git difftool."));
        return;
    }
    QString summary, error;
    if (Difftool::unregisterTool(&summary, &error))
        QMessageBox::information(this, tr("Unregistered"), tr("Done:\n\n%1").arg(summary));
    else
        QMessageBox::warning(this, tr("Unregister"), error);
}

// ---- Edit menu --------------------------------------------------------------

void MainWindow::copyFilePath(bool newSide)
{
    if (m_shownIndex < 0 || m_shownIndex >= m_changes.size())
        return;
    const FileChange &fc = m_changes[m_shownIndex];
    QString path = m_repo.diskPath(fc, m_cmp, newSide);
    if (path.isEmpty()) // not a file on disk (a commit or the index): its path in the repository
        path = newSide ? fc.path : fc.oldPath;
    QApplication::clipboard()->setText(path);
    statusBar()->showMessage(tr("Copied %1").arg(path), 3000);
}

void MainWindow::setEditorCommand()
{
    bool ok = false;
    const QString cmd = QInputDialog::getText(
        this, tr("Set editor"),
        tr("Command to open a file. {file} and {line} are replaced; leave empty for the system default.\n"
           "Examples:  code --goto {file}:{line}   kate --line {line} {file}   notepad++ -n{line} {file}"),
        QLineEdit::Normal, Settings::editorCommand(), &ok);
    if (ok)
        Settings::setEditorCommand(cmd.trimmed());
}

void MainWindow::editExternally(bool newSide)
{
    if (m_shownIndex < 0 || m_shownIndex >= m_changes.size())
        return;
    const FileChange fc = m_changes[m_shownIndex];
    QString path = m_repo.diskPath(fc, m_cmp, newSide);
    if (path.isEmpty()) {
        // A commit or the index: open a read-only copy.
        const auto content = m_repo.fileContent(fc, m_cmp, newSide);
        if (!content) {
            statusBar()->showMessage(tr("That side has no file"), 3000);
            return;
        }
        const QString dir = QDir::temp().filePath(QStringLiteral("yadiff-%1/%2").arg(QCoreApplication::applicationPid()).arg(newSide ? QStringLiteral("right") : QStringLiteral("left")));
        path = QDir(dir).filePath(newSide ? fc.path : fc.oldPath);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        f.setPermissions(QFile::WriteOwner | QFile::ReadOwner);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QMessageBox::warning(this, tr("Edit file"), f.errorString());
            return;
        }
        f.write(*content);
        f.close();
        f.setPermissions(QFile::ReadOwner | QFile::ReadGroup | QFile::ReadOther);
        statusBar()->showMessage(tr("Opened a read-only copy: that side is not a file on disk"), 4000);
    }
    const int line = std::max(1, m_pane->currentLine(newSide));
    const QString cmd = Settings::editorCommand();
    if (cmd.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        return;
    }
    QStringList parts = QProcess::splitCommand(cmd);
    if (parts.isEmpty())
        return;
    bool hadFile = false;
    for (QString &p : parts) {
        hadFile = hadFile || p.contains(QLatin1String("{file}"));
        p.replace(QLatin1String("{file}"), path).replace(QLatin1String("{line}"), QString::number(line));
    }
    if (!hadFile)
        parts << path;
    const QString program = parts.takeFirst();
    const bool started = QProcess::startDetached(program, parts);
    Log::launched(lcProc(), program, parts, started);
    if (!started)
        QMessageBox::warning(this, tr("Edit file"), tr("Could not start %1. Check Edit > Set editor.").arg(program));
}

void MainWindow::goToLineDialog()
{
    if (!m_pane->hasDiff())
        return;
    bool ok = false;
    const QString text = QInputDialog::getText(this, tr("Go to line"),
                                               tr("Line number on the right (prefix with L for the left side, e.g. L42):"),
                                               QLineEdit::Normal, QString(), &ok);
    if (!ok || text.trimmed().isEmpty())
        return;
    QString t = text.trimmed();
    bool newSide = true;
    if (t.startsWith(u'L', Qt::CaseInsensitive)) {
        newSide = false;
        t = t.mid(1);
    } else if (t.startsWith(u'R', Qt::CaseInsensitive)) {
        t = t.mid(1);
    }
    const int line = t.toInt(&ok);
    if (!ok || !m_pane->goToLine(line, newSide))
        statusBar()->showMessage(tr("No line %1 on the %2 side").arg(t, newSide ? tr("right") : tr("left")), 3000);
}

void MainWindow::rebuildLanguageMenu()
{
    // View > Language: the same choice as the toolbar's Syntax box.
    m_languageMenu->clear();
    auto *group = new QActionGroup(m_languageMenu);
    const QString current = m_syntaxCombo->currentData().toString();
    auto add = [&](QMenu *menu, const QString &label, const QString &value) {
        QAction *a = menu->addAction(label);
        a->setCheckable(true);
        a->setChecked(value == current);
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, value] {
            const int idx = m_syntaxCombo->findData(value);
            if (idx >= 0) {
                m_syntaxCombo->setCurrentIndex(idx);
                emit m_syntaxCombo->activated(idx);
            }
        });
    };
    add(m_languageMenu, m_syntaxCombo->itemText(0), QString());
    add(m_languageMenu, tr("None"), QStringLiteral("-"));
    m_languageMenu->addSeparator();
    for (const auto &section : Syntax::languagesBySection()) {
        QMenu *sub = m_languageMenu->addMenu(section.first);
        for (const QString &lang : section.second)
            add(sub, lang, lang);
    }
}

// ---- Themes and fonts -------------------------------------------------------

void MainWindow::applyTheme()
{
    // Everything that caches colours picks up the new palette.
    for (QAction *a : m_themeMenu->actions())
        a->setChecked(a->data().toString() == Theme::current());
    m_files->setMarkColor(Settings::markColor());
    m_pane->applySettings();
    updateViewToggle();
    m_whitespaceToggle->setIcon(whitespaceIcon(palette().color(QPalette::ButtonText)));
    m_nextAct->setIcon(arrowIcon(false, palette().color(QPalette::ButtonText)));
    m_prevAct->setIcon(arrowIcon(true, palette().color(QPalette::ButtonText)));
}

void MainWindow::applyFonts()
{
    m_files->setFont(Settings::fileListFont());
    DiffFormat::setHtmlFont(Settings::clipboardFont().family(), Settings::clipboardFont().pointSizeF());
    m_pane->applySettings();
}

void MainWindow::loadBlame()
{
    const int index = m_shownIndex;
    if (index < 0 || index >= m_changes.size() || !m_pane->hasDiff())
        return;
    const FileChange fc = m_changes[index];
    const quint64 gen = ++m_blameGen;
    const quint64 fileGen = m_fileGen;
    statusBar()->showMessage(tr("Running git blame…"));
    using Pair = std::pair<QVector<BlameLine>, QVector<BlameLine>>;
    runJob<Pair>([repo = m_repo, cmp = m_cmp, fc] { return Pair{repo.blame(fc, cmp, false), repo.blame(fc, cmp, true)}; },
                 [this, gen, fileGen](Pair r) {
                     if (gen != m_blameGen || fileGen != m_fileGen || !Settings::showBlame())
                         return;
                     statusBar()->clearMessage();
                     if (r.first.isEmpty() && r.second.isEmpty())
                         statusBar()->showMessage(tr("No blame for this file"), 3000);
                     m_pane->setBlame(r.first, r.second);
                 });
}

void MainWindow::openBlameCommit(const QString &hash)
{
    // The commit against its first parent (the empty tree for a root commit), in a new window.
    QString parent = hash + QStringLiteral("^");
    if (!m_repo.resolveCommit(parent, nullptr))
        parent = m_repo.emptyTree();
    const QStringList args{QStringLiteral("-C"), m_repo.root(), parent, hash};
    const bool started = QProcess::startDetached(QCoreApplication::applicationFilePath(), args);
    Log::launched(lcApp(), QCoreApplication::applicationFilePath(), args, started);
    if (!started)
        QMessageBox::warning(this, tr("Git blame"), tr("Could not start yadiff."));
}

// ---- Updates ---------------------------------------------------------------

void MainWindow::checkForUpdates(bool interactive)
{
    if (m_updating) {
        if (interactive)
            statusBar()->showMessage(tr("An update is already being installed"), 4000);
        return;
    }
    if (interactive)
        statusBar()->showMessage(tr("Checking for updates…"));
    struct Result {
        std::optional<Updater::Release> release;
        QString error;
    };
    runJob<Result>(
        [] {
            Result r;
            r.release = Updater::latest(&r.error);
            return r;
        },
        [this, interactive](Result r) {
            statusBar()->clearMessage();
            if (!r.release) {
                if (interactive)
                    QMessageBox::warning(this, tr("Check for update"), tr("Could not check for updates.\n\n%1").arg(r.error));
                return;
            }
            Settings::setLastUpdateCheck(QDateTime::currentSecsSinceEpoch());
            const Updater::Release rel = *r.release;
            const QString current = QApplication::applicationVersion();
            if (!Updater::isNewer(rel.tag, current)) {
                if (interactive)
                    QMessageBox::information(this, tr("Check for update"),
                                             tr("yadiff %1 is the latest version.").arg(current));
                return;
            }
            const bool canInstall = Updater::currentInstallKind() != Updater::InstallKind::Unsupported;
            if (!interactive) {
                if (canInstall)
                    installUpdate(rel, false);
                else
                    statusBar()->showMessage(tr("yadiff %1 is available").arg(rel.tag), 10000);
                return;
            }
            QMessageBox box(this);
            box.setWindowTitle(tr("Update available"));
            box.setIcon(QMessageBox::Information);
            box.setText(tr("yadiff %1 is available (you have %2).").arg(rel.tag, current));
            box.setInformativeText(canInstall ? tr("Install it now? yadiff keeps working while it installs.")
                                              : Updater::unsupportedReason());
            if (!rel.notes.isEmpty())
                box.setDetailedText(rel.notes);
            QPushButton *install = canInstall ? box.addButton(tr("Install"), QMessageBox::AcceptRole) : nullptr;
            QPushButton *page = box.addButton(tr("Open release page"), QMessageBox::ActionRole);
            box.addButton(canInstall ? tr("Later") : tr("Close"), QMessageBox::RejectRole);
            box.setDefaultButton(install ? install : page);
            box.exec();
            if (install && box.clickedButton() == install)
                installUpdate(rel, true);
            else if (box.clickedButton() == page && !rel.url.isEmpty())
                QDesktopServices::openUrl(QUrl(rel.url));
        });
}

void MainWindow::installUpdate(const Updater::Release &release, bool interactive)
{
    m_updating = true;
    QPointer<MainWindow> self(this);
    auto progress = [self](const QString &message) {
        QMetaObject::invokeMethod(
            qApp,
            [self, message] {
                if (self)
                    self->statusBar()->showMessage(message);
            },
            Qt::QueuedConnection);
    };
    using Result = std::pair<bool, QString>;
    runJob<Result>(
        [release, progress] {
            QString error;
            const bool ok = Updater::install(release, progress, &error);
            return Result{ok, error};
        },
        [this, release, interactive](Result r) {
            m_updating = false;
            statusBar()->clearMessage();
            if (!r.first) {
                if (interactive)
                    QMessageBox::warning(this, tr("Update"), tr("Installing yadiff %1 failed.\n\n%2").arg(release.tag, r.second));
                else
                    statusBar()->showMessage(tr("Installing yadiff %1 failed: %2").arg(release.tag, r.second.section(u'\n', 0, 0)),
                                             15000);
                return;
            }
            // The new version starts next time; offer to restart now.
            if (!m_restartButton) {
                m_restartButton = new QPushButton(this);
                m_restartButton->setFlat(true);
                connect(m_restartButton, &QPushButton::clicked, this, &MainWindow::restartForUpdate);
                statusBar()->addPermanentWidget(m_restartButton);
            }
            m_restartButton->setText(tr("yadiff %1 installed. Restart now").arg(release.tag));
            m_restartButton->show();
            if (interactive
                && QMessageBox::question(this, tr("Update"),
                                         tr("yadiff %1 is installed. Restart now to use it?").arg(release.tag))
                       == QMessageBox::Yes)
                restartForUpdate();
        });
}

void MainWindow::restartForUpdate()
{
    if (!maybeKeepEdits())
        return;
    // Same arguments, in a new process of the updated install.
    const bool started = QProcess::startDetached(Updater::restartProgram(), QApplication::arguments().mid(1));
    Log::launched(lcUpdate(), Updater::restartProgram(), QApplication::arguments().mid(1), started);
    if (!started) {
        QMessageBox::warning(this, tr("Update"), tr("Could not start the updated yadiff. Start it yourself."));
        return;
    }
    close();
}

void MainWindow::setComparison(GitRepo repo, Comparison cmp)
{
    m_repo = std::move(repo);
    m_cmp = std::move(cmp);
    m_cmp.ignoreGlobs = Settings::ignoreGlobs();
    m_cmp.useIgnoreGlobs = Settings::useIgnoreGlobs();
    m_cmp.includeIdentical = Settings::showFileCategory(QStringLiteral("identical"));
    // Everything that belonged to the old comparison goes.
    ++m_listGen;
    ++m_fileGen;
    m_reviews = std::make_unique<ReviewStore>(reviewRoot());
    m_changes.clear();
    m_diffCache.clear();
    m_matchCounts.clear();
    m_showLarge.clear();
    m_comments.clear();
    m_scrollMemory.clear();
    m_anchors.clear();
    m_lastSignature.clear();
    m_shownIndex = -1;
    m_customTitle.clear();
    m_myVote.reset();
    m_voteError.clear();
    m_voteBusy = false;
    ++m_voteGen;
    notePullRequest();
    fillVersionCombo();
    qCInfo(lcApp).noquote() << "showing" << m_cmp.describe() << "in" << QDir::toNativeSeparators(m_repo.root());
    updateModeUi();
    updateTitle();
    rememberRecent();
    reload();
}
