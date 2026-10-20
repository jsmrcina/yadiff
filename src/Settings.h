#pragma once

#include <QColor>
#include <QFont>

#include "GitRepo.h"

struct DiffColors {
    QColor added;
    QColor removed;
    QColor moved;
};

namespace Settings {

bool isDarkTheme();
DiffColors defaultColors();
DiffColors colors();
void setColors(const DiffColors &c);

// Stronger variant of a line color, used for intra-line highlights and borders.
QColor emphasis(const QColor &base);

QFont font();
void setFont(const QFont &f);

int tabWidth();
void setTabWidth(int w);

bool sideBySide();
void setSideBySide(bool on);

bool unstagedAgainstHead();
void setUnstagedAgainstHead(bool on);

bool allChanges();
void setAllChanges(bool on);

bool collapseUnchanged();
void setCollapseUnchanged(bool on);
bool showWhitespace();
void setShowWhitespace(bool on);
bool wordWrap();
void setWordWrap(bool on);
bool tableView(); // CSV and TSV files as tables
void setTableView(bool on);
bool hexBinary(); // binary files as a hex dump
void setHexBinary(bool on);
bool autoReload();
void setAutoReload(bool on);

// Fonts besides the diff font (font()): the file list, and text copied as rich text.
QFont fileListFont();
void setFileListFont(const QFont &f);
QFont clipboardFont();
void setClipboardFont(const QFont &f);

// File list and marking.
QColor markColor();
void setMarkColor(const QColor &c);
QStringList ignoreGlobs();
void setIgnoreGlobs(const QStringList &globs);
bool useIgnoreGlobs();
void setUseIgnoreGlobs(bool on);
bool showFileCategory(const QString &name); // "leftOnly", "rightOnly", "different", "identical", "blankOnly"
void setShowFileCategory(const QString &name, bool on);
bool trimFolderPrefix();
void setTrimFolderPrefix(bool on);
bool rememberScroll();
void setRememberScroll(bool on);

// Viewer.
int paneVisibility(); // 0 both, 1 left only, 2 right only
void setPaneVisibility(int v);
bool autoUpdate();
void setAutoUpdate(bool on);
qint64 lastUpdateCheck(); // seconds since the epoch, 0 never
void setLastUpdateCheck(qint64 secs);
bool hideResolvedThreads();
void setHideResolvedThreads(bool on);
bool showBlame();
void setShowBlame(bool on);
bool showLineNumbers();
void setShowLineNumbers(bool on);
bool syntaxEnabled();
void setSyntaxEnabled(bool on);
bool highlightClickedWord();
void setHighlightClickedWord(bool on);
int zoomPercent();
void setZoomPercent(int percent);
// External editor command; {file} and {line} are substituted. Empty: the system default.
QString editorCommand();
void setEditorCommand(const QString &cmd);

// Comparisons opened recently (newest first), for File > Recent comparisons.
struct RecentComparison {
    QStringList arguments; // as ComparisonArgs::fromArguments() reads them
    QString label;
};
QVector<RecentComparison> recentComparisons();
void addRecentComparison(const QStringList &arguments, const QString &label);
void clearRecentComparisons();

// Regular expressions for "unimportant text" (used while diff/ignoreUnimportant is on).
QStringList unimportantPatterns();
void setUnimportantPatterns(const QStringList &patterns);
DiffOptions diffOptions();
void setDiffOptions(const DiffOptions &o);

void resetAppearance();

} // namespace Settings
