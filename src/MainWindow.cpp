#include "MainWindow.h"

#include "DiffPane.h"
#include "Difftool.h"
#include "FileList.h"
#include "HistoryDialog.h"
#include "PreferencesDialog.h"
#include "SearchBar.h"
#include "Settings.h"
#include "Syntax.h"
#include "AdoAuth.h"
#include "AdoPr.h"
#include "CodeView.h"
#include "GitHubPr.h"
#include "NewComparisonDialog.h"
#include "PrReview.h"
#include "Report.h"
#include "TextFile.h"
#include "Theme.h"
#include "Updater.h"
#include "Usage.h"

#include <QActionGroup>
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
#include <QProgressDialog>
#include <QUrl>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
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
    syncOptionControls();
    updateViewToggle();

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
    connect(m_pane, &DiffPane::showLargeRequested, this, [this] {
        const int index = m_shownIndex;
        m_showLarge.insert(index);
        m_shownIndex = -1;
        showFile(index);
    });
    connect(m_pane, &DiffPane::commentRequested, this, &MainWindow::addComment);
    connect(m_pane, &DiffPane::replyRequested, this, &MainWindow::replyToThread);
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
    fileMenu->addAction(tr("Change window &title…"), this, &MainWindow::changeWindowTitle);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("Edit &globbing ignore patterns…"), this, &MainWindow::editIgnorePatterns);
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
    editMenu->addSeparator();
    QAction *findAct = editMenu->addAction(tr("&Find…"), this, [this] {
        if (m_pane->hasSelection() && m_search->isHidden())
            m_search->setScope(SearchBar::Scope::Selection);
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
    editMenu->addAction(tr("Set &editor…"), this, &MainWindow::setEditorCommand);
    m_editLeftAct = editMenu->addAction(tr("Edit le&ft file"), this, [this] { editExternally(false); });
    m_editRightAct = editMenu->addAction(tr("Edit righ&t file"), this, [this] { editExternally(true); });
    editMenu->addSeparator();
    m_editAct = editMenu->addAction(QIcon::fromTheme(QStringLiteral("document-edit")), tr("Edit right side &in place"));
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
    tabMenu->addAction(tr("Other…"), this, [this] {
        bool ok = false;
        const int w = QInputDialog::getInt(this, tr("Tab width"), tr("Spaces per tab:"), Settings::tabWidth(), 1, 16, 1, &ok);
        if (ok) {
            Settings::setTabWidth(w);
            m_pane->applySettings();
        }
    });
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
    });
    fonts->addAction(tr("&File list font\u2026"), this, [pickFont] {
        pickFont(tr("File list font"), Settings::fileListFont(), false, [](const QFont &f) { Settings::setFileListFont(f); });
    });
    fonts->addAction(tr("&Clipboard font\u2026"), this, [pickFont] {
        pickFont(tr("Font for copied text"), Settings::clipboardFont(), false, [](const QFont &f) { Settings::setClipboardFont(f); });
    })->setStatusTip(tr("The font used when copying with formatting (Copy, Copy this change as diff)"));
    viewMenu->addAction(tr("Diff &colors and tab width\u2026"), this, &MainWindow::openPreferences);

    // ---- Options -----------------------------------------------------------
    QMenu *optionsMenu = menuBar()->addMenu(tr("&Options"));
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
    });

    // ---- Review (pull requests only) ---------------------------------------
    m_reviewMenu = menuBar()->addMenu(tr("&Review"));
    m_reviewMenuAction = m_reviewMenu->menuAction();
    m_reviewMenu->addAction(tr("&Approve…"), this, [this] { submitReview(GitHubReview::Verdict::Approve); });
    m_reviewMenu->addAction(tr("Request &changes…"), this, [this] { submitReview(GitHubReview::Verdict::RequestChanges); });
    m_reviewMenu->addAction(tr("&Comment on the pull request…"), this,
                            [this] { submitReview(GitHubReview::Verdict::Comment); });
    m_reviewMenu->addSeparator();
    m_reviewMenu->addAction(tr("&Reload comments"), this, &MainWindow::loadComments);
    m_reviewMenu->addAction(tr("&Open pull request in browser"), this,
                            [this] { QDesktopServices::openUrl(QUrl(PrReview::webUrl(m_cmp))); });

    // ---- Help --------------------------------------------------------------
    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&Register as git difftool…"), this, &MainWindow::registerDifftool);
    helpMenu->addAction(tr("U&nregister as git difftool"), this, &MainWindow::unregisterDifftool);
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
    helpMenu->addAction(tr("&Usage"), this, &MainWindow::showUsage);
    helpMenu->addAction(tr("&About yadiff"), this, [this] {
        QMessageBox::about(this, tr("About yadiff"),
                           tr("<b>yadiff</b> %1<br>A graphical diff viewer for Git.<br><br>"
                              "F3 / F8 / F7: find, next, previous &nbsp; Ctrl+G: go to line<br>"
                              "Alt+Down / Alt+Up: next / previous change<br>"
                              "Ctrl+T: toggle inline / side by side &nbsp; Ctrl+M: mark file<br>"
                              "Ctrl+E: edit in place, Ctrl+S: save<br>"
                              "Right-click a change, or a file, for more.")
                               .arg(QApplication::applicationVersion()));
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
    reviewButton->setToolTip(tr("Approve, request changes, or comment on the pull request"));
    m_reviewToolAction = tb->addWidget(reviewButton);
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
    auto *prev = new QToolButton;
    prev->setDefaultAction(m_prevAct);
    prev->setAutoRaise(true);
    tb->addWidget(prev);
    auto *next = new QToolButton;
    next->setDefaultAction(m_nextAct);
    next->setAutoRaise(true);
    tb->addWidget(next);
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
    m_reviewToolAction->setVisible(m_cmp.prNumber > 0);
    const bool workingTree = m_repo.isRepository() && !m_repo.isBare()
                          && (working || (m_cmp.mode == Comparison::Mode::Revisions && m_cmp.rev2.isEmpty()));
    m_stageAct->setEnabled(workingTree);
    m_unstageAct->setEnabled(workingTree);
    // Revert writes the right side: the working tree (unless swapped) or a compared folder.
    m_revertAct->setEnabled((workingTree && !m_cmp.reversed) || m_cmp.mode == Comparison::Mode::Paths);
    syncOptionControls();
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
    m_editAct->setEnabled(m_pane->canEdit());
    m_editAct->setChecked(editing);
    m_saveAct->setEnabled(editing && m_pane->isModified());
    m_editLeftAct->setEnabled(m_pane->hasDiff());
    m_editRightAct->setEnabled(m_pane->hasDiff());
    m_discardAct->setEnabled(editing);
    m_copyRightAct->setEnabled(m_pane->canCopyTo(true) && m_pane->currentChange() >= 0);
    m_copyLeftAct->setEnabled(m_pane->canCopyTo(false) && m_pane->currentChange() >= 0);
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

    struct ListResult {
        QVector<FileChange> changes;
        QString error;
        QHash<int, QByteArray> fingerprints; // only for files that have review marks
        QByteArray signature;
    };
    const quint64 gen = ++m_listGen;
    m_listBusy = true;
    statusBar()->showMessage(tr("Loading changes…"));
    const QSet<QString> reviewKeys = m_reviews->keys();
    runJob<ListResult>(
        [repo = m_repo, cmp = m_cmp, reviewKeys] {
            ListResult r;
            r.changes = repo.listChanges(cmp, &r.error);
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
                folderRoot = m_cmp.pathsAreFiles ? QString() : m_cmp.rightPath;
            else if (!m_repo.isBare())
                folderRoot = m_repo.root();
            m_files->setProperty("folderRoot", folderRoot);
            m_files->setFolderDisplay(folderRoot, Settings::trimFolderPrefix());
            m_files->setChanges(m_changes, groups, reviewed);
            applyFilter();
            updateSummary();
            if (m_cmp.prNumber > 0)
                loadComments();

            if (m_changes.isEmpty() || m_files->visibleCount() == 0) {
                m_pane->showMessage(QString(), m_changes.isEmpty() ? (m_cmp.paths.isEmpty() ? tr("No changes")
                                                                                            : tr("No changes in the selected paths"))
                                                                   : tr("No files match the filter"));
                updateEditActions();
                return;
            }
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
    if (!m_changes.isEmpty()) {
        text += QStringLiteral("  ·  +%1 −%2").arg(added).arg(removed);
        text += tr("  \u00b7  %1 of %2 marked").arg(m_files->markedCount()).arg(m_changes.size());
    }
    m_summary->setText(text);
}

void MainWindow::onReviewToggled(int index, bool reviewed)
{
    if (index < 0 || index >= m_changes.size())
        return;
    const FileChange &fc = m_changes[index];
    m_reviews->setReviewed(fc.path, m_repo.fingerprint(fc, m_cmp), reviewed);
    updateSummary();
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
    const FileChange fc = m_changes[index];
    const bool wasEditing = m_pane->isEditing() && index == m_shownIndex;

    QString left, right;
    if (m_cmp.mode == Comparison::Mode::Paths) {
        left = homeShortened(m_cmp.pathsAreFiles ? QFileInfo(m_cmp.leftPath).absolutePath() : m_cmp.leftPath);
        right = homeShortened(m_cmp.pathsAreFiles ? QFileInfo(m_cmp.rightPath).absolutePath() : m_cmp.rightPath);
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
    const bool countsFromDiff = m_cmp.options.inProcess() && fc.status != u'?';
    if (fc.added >= 0 && !countsFromDiff)
        title += QStringLiteral("  +%1 −%2").arg(fc.added).arg(fc.removed);

    m_shownIndex = index;
    const quint64 gen = ++m_fileGen;
    m_fileBusy = true;
    statusBar()->showMessage(tr("Loading %1…").arg(fc.path));

    struct FileResult {
        FileDiff diff;
        QString error;
        QImage oldImage, newImage;
        qint64 oldBytes = 0, newBytes = 0;
        QVector<BlameLine> oldBlame, newBlame;
    };
    const bool imagePath = isImagePath(fc.path) || isImagePath(fc.oldPath);
    const auto cached = m_diffCache.constFind(index);
    const bool haveCached = cached != m_diffCache.constEnd() && !imagePath;
    const FileDiff cachedDiff = haveCached ? *cached : FileDiff();
    const bool large = m_showLarge.contains(index);

    runJob<FileResult>(
        [repo = m_repo, cmp = m_cmp, fc, imagePath, haveCached, cachedDiff, blame = Settings::showBlame()] {
            FileResult r;
            r.diff = haveCached ? cachedDiff : repo.loadDiff(fc, cmp, &r.error);
            if (blame && !r.diff.binary && r.error.isEmpty()) {
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
        [this, gen, index, fc, left, right, title, isNew, isGone, wasEditing, then, large, countsFromDiff](FileResult r) {
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
                    added += l.kind == LineKind::Added;
                    removed += l.kind == LineKind::Removed;
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
            view.oldBlame = r.oldBlame;
            view.newBlame = r.newBlame;
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
            QByteArray out;
            m_repo.run({QStringLiteral("hash-object"), QStringLiteral("-t"), QStringLiteral("tree"), QStringLiteral("--stdin")},
                       &out, nullptr);
            c.rev1 = QString::fromUtf8(out).trimmed();
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
    if (scope == SearchBar::Scope::Selection && !m_pane->hasSelection()) {
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
        statusBar()->showMessage(forward ? tr("No more matches") : tr("No earlier matches"), 2000);
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

    auto *focus = new QAction(tr("Filter files"), this);
    focus->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_P));
    connect(focus, &QAction::triggered, this, [this] {
        m_filterEdit->setFocus();
        m_filterEdit->selectAll();
    });
    addAction(focus);
    return panel;
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
    return GitHubReview::threads(mine);
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
            int outdated = 0;
            const auto all = GitHubReview::threads(m_comments);
            for (const auto &t : all)
                outdated += t.first().outdated();
            statusBar()->showMessage(tr("%1 review threads (%2 outdated, not shown)").arg(all.size()).arg(outdated), 5000);
            if (m_shownIndex >= 0 && m_shownIndex < m_changes.size())
                m_pane->setThreads(threadsFor(m_changes[m_shownIndex]));
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
            if (m_shownIndex >= 0)
                m_pane->setThreads(threadsFor(m_changes[m_shownIndex]));
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
            m_comments << c;
            statusBar()->showMessage(tr("Reply posted"), 3000);
            if (m_shownIndex >= 0)
                m_pane->setThreads(threadsFor(m_changes[m_shownIndex]));
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
            else
                statusBar()->showMessage(tr("%1: submitted").arg(what), 4000);
        });
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
    QString error;
    if (!(stage ? m_repo.stage(picked, &error) : m_repo.unstage(picked, &error))) {
        QMessageBox::warning(this, stage ? tr("Stage") : tr("Unstage"), error);
        return;
    }
    const QString files = picked.size() == 1 ? picked.first().path : tr("%1 files").arg(picked.size());
    statusBar()->showMessage(stage ? tr("Staged %1").arg(files) : tr("Unstaged %1").arg(files), 3000);
    reload();
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
    QString error;
    if (!m_repo.revert(picked, m_cmp, &error))
        QMessageBox::warning(this, tr("Revert files"), error);
    else
        statusBar()->showMessage(tr("Reverted %1").arg(picked.size() == 1 ? names.first() : tr("%1 files").arg(picked.size())), 4000);
    reload();
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
    QString hash;
    if (m_cmp.mode == Comparison::Mode::Revisions && !m_cmp.rev2.isEmpty() && !m_cmp.reversed)
        hash = m_cmp.rev2;
    else
        hash = m_repo.lastCommit(sel.first().oldPath);
    if (hash.isEmpty()) {
        statusBar()->showMessage(tr("%1 has no commits yet").arg(sel.first().path), 3000);
        return;
    }
    QApplication::clipboard()->setText(hash);
    statusBar()->showMessage(tr("Copied %1").arg(hash.left(12)), 3000);
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
    if (!p.startDetached())
        QMessageBox::warning(this, tr("gitk"), tr("Could not start gitk."));
}

void MainWindow::newComparison()
{
    NewComparisonDialog dlg(m_repo.isRepository() && !m_repo.isBare() ? m_repo.root() : QString(), this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    // A new window (its own process), so this comparison stays as it is.
    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), dlg.arguments()))
        QMessageBox::warning(this, tr("New comparison"), tr("Could not start yadiff."));
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
    if (!QProcess::startDetached(program, parts))
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
    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), args))
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
    if (!QProcess::startDetached(Updater::restartProgram(), QApplication::arguments().mid(1))) {
        QMessageBox::warning(this, tr("Update"), tr("Could not start the updated yadiff. Start it yourself."));
        return;
    }
    close();
}
