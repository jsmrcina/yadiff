#include "Settings.h"

#include "Theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPalette>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStyleHints>

#include <algorithm>

namespace {

QColor readColor(QSettings &s, const QString &key, const QColor &fallback)
{
    const QColor c(s.value(key).toString());
    return c.isValid() ? c : fallback;
}

} // namespace

namespace Settings {

bool isDarkTheme()
{
    return Theme::isDark();
}

DiffColors defaultColors()
{
    return {Theme::addedColor(), Theme::removedColor(), Theme::movedColor()};
}

DiffColors colors()
{
    QSettings s;
    const DiffColors d = defaultColors();
    return {readColor(s, QStringLiteral("colors/added"), d.added),
            readColor(s, QStringLiteral("colors/removed"), d.removed),
            readColor(s, QStringLiteral("colors/moved"), d.moved)};
}

void setColors(const DiffColors &c)
{
    QSettings s;
    const DiffColors d = defaultColors();
    // Only persist overrides, so the defaults keep following the light/dark theme.
    auto put = [&](const QString &key, const QColor &v, const QColor &def) {
        if (v == def)
            s.remove(key);
        else
            s.setValue(key, v.name());
    };
    put(QStringLiteral("colors/added"), c.added, d.added);
    put(QStringLiteral("colors/removed"), c.removed, d.removed);
    put(QStringLiteral("colors/moved"), c.moved, d.moved);
}

QColor emphasis(const QColor &base)
{
    QColor c = base.toHsl();
    int h = c.hslHue(), s = c.hslSaturation(), l = c.lightness();
    if (l > 128) {
        l = std::max(0, l - 45);
        s = std::min(255, s + 40);
    } else {
        l = std::min(255, l + 40);
        s = std::min(255, s + 30);
    }
    return QColor::fromHsl(h, s, l);
}

QFont font()
{
    QSettings s;
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const QString stored = s.value(QStringLiteral("view/font")).toString();
    if (!stored.isEmpty())
        f.fromString(stored);
    return f;
}

void setFont(const QFont &f)
{
    QSettings().setValue(QStringLiteral("view/font"), f.toString());
}

int tabWidth()
{
    return std::clamp(QSettings().value(QStringLiteral("view/tabWidth"), 4).toInt(), 1, 16);
}

void setTabWidth(int w)
{
    QSettings().setValue(QStringLiteral("view/tabWidth"), w);
}

bool sideBySide()
{
    return QSettings().value(QStringLiteral("view/sideBySide"), true).toBool();
}

void setSideBySide(bool on)
{
    QSettings().setValue(QStringLiteral("view/sideBySide"), on);
}

bool unstagedAgainstHead()
{
    return QSettings().value(QStringLiteral("compare/unstagedAgainstHead"), true).toBool();
}

void setUnstagedAgainstHead(bool on)
{
    QSettings().setValue(QStringLiteral("compare/unstagedAgainstHead"), on);
}

bool allChanges()
{
    return QSettings().value(QStringLiteral("compare/allChanges"), false).toBool();
}

void setAllChanges(bool on)
{
    QSettings().setValue(QStringLiteral("compare/allChanges"), on);
}

namespace {
bool flag(const char *key, bool def)
{
    return QSettings().value(QLatin1String(key), def).toBool();
}
void setFlag(const char *key, bool on)
{
    QSettings().setValue(QLatin1String(key), on);
}
} // namespace

bool collapseUnchanged() { return flag("view/collapseUnchanged", false); }
void setCollapseUnchanged(bool on) { setFlag("view/collapseUnchanged", on); }
bool showWhitespace() { return flag("view/showWhitespace", false); }
void setShowWhitespace(bool on) { setFlag("view/showWhitespace", on); }
bool wordWrap() { return flag("view/wordWrap", false); }
void setWordWrap(bool on) { setFlag("view/wordWrap", on); }
bool tableView() { return flag("view/tableView", true); }
void setTableView(bool on) { setFlag("view/tableView", on); }
bool hexBinary() { return flag("view/hexBinary", true); }
void setHexBinary(bool on) { setFlag("view/hexBinary", on); }
bool autoReload() { return flag("behaviour/autoReload", false); }
void setAutoReload(bool on) { setFlag("behaviour/autoReload", on); }

QColor markColor()
{
    const QColor c(QSettings().value(QStringLiteral("marking/color")).toString());
    if (c.isValid())
        return c;
    return isDarkTheme() ? QColor(0x8a, 0x6d, 0x0b, 150) : QColor(0xff, 0xd8, 0x4d, 120);
}
void setMarkColor(const QColor &c) { QSettings().setValue(QStringLiteral("marking/color"), c.name(QColor::HexArgb)); }
QFont fileListFont()
{
    QFont f = QApplication::font();
    const QString stored = QSettings().value(QStringLiteral("view/fileListFont")).toString();
    if (!stored.isEmpty())
        f.fromString(stored);
    return f;
}
void setFileListFont(const QFont &f) { QSettings().setValue(QStringLiteral("view/fileListFont"), f.toString()); }
QFont clipboardFont()
{
    QFont f = font();
    const QString stored = QSettings().value(QStringLiteral("view/clipboardFont")).toString();
    if (!stored.isEmpty())
        f.fromString(stored);
    return f;
}
void setClipboardFont(const QFont &f) { QSettings().setValue(QStringLiteral("view/clipboardFont"), f.toString()); }

QStringList ignoreGlobs() { return QSettings().value(QStringLiteral("files/ignoreGlobs")).toStringList(); }
void setIgnoreGlobs(const QStringList &globs) { QSettings().setValue(QStringLiteral("files/ignoreGlobs"), globs); }
bool useIgnoreGlobs() { return flag("files/useIgnoreGlobs", true); }
void setUseIgnoreGlobs(bool on) { setFlag("files/useIgnoreGlobs", on); }
bool showFileCategory(const QString &name)
{
    // Identical files are off by default: in a repository that would list every file.
    return QSettings().value(QStringLiteral("files/show_") + name, name != QLatin1String("identical")).toBool();
}
void setShowFileCategory(const QString &name, bool on) { QSettings().setValue(QStringLiteral("files/show_") + name, on); }
bool trimFolderPrefix() { return flag("files/trimFolderPrefix", false); }
void setTrimFolderPrefix(bool on) { setFlag("files/trimFolderPrefix", on); }
bool rememberScroll() { return flag("view/rememberScroll", false); }
void setRememberScroll(bool on) { setFlag("view/rememberScroll", on); }
int paneVisibility() { return std::clamp(QSettings().value(QStringLiteral("view/panes"), 0).toInt(), 0, 2); }
void setPaneVisibility(int v) { QSettings().setValue(QStringLiteral("view/panes"), v); }
bool autoUpdate() { return flag("update/automatic", false); }
void setAutoUpdate(bool on) { setFlag("update/automatic", on); }
qint64 lastUpdateCheck() { return QSettings().value(QStringLiteral("update/lastCheck"), 0).toLongLong(); }
void setLastUpdateCheck(qint64 secs) { QSettings().setValue(QStringLiteral("update/lastCheck"), secs); }
bool showPrOverview() { return flag("review/overview", true); }
void setShowPrOverview(bool on) { setFlag("review/overview", on); }
bool prOverviewFolded() { return flag("review/overviewFolded", false); }
void setPrOverviewFolded(bool on) { setFlag("review/overviewFolded", on); }
bool openSinceLastReview() { return flag("review/sinceLastReview", true); }
void setOpenSinceLastReview(bool on) { setFlag("review/sinceLastReview", on); }
bool hideResolvedThreads() { return flag("review/hideResolved", false); }
void setHideResolvedThreads(bool on) { setFlag("review/hideResolved", on); }
bool showBlame() { return flag("view/blame", false); }
void setShowBlame(bool on) { setFlag("view/blame", on); }
bool showLineNumbers() { return flag("view/lineNumbers", true); }
void setShowLineNumbers(bool on) { setFlag("view/lineNumbers", on); }
bool syntaxEnabled() { return flag("view/syntax", true); }
void setSyntaxEnabled(bool on) { setFlag("view/syntax", on); }
bool highlightClickedWord() { return flag("view/highlightClickedWord", true); }
void setHighlightClickedWord(bool on) { setFlag("view/highlightClickedWord", on); }
int zoomPercent() { return std::clamp(QSettings().value(QStringLiteral("view/zoom"), 100).toInt(), 30, 400); }
void setZoomPercent(int percent) { QSettings().setValue(QStringLiteral("view/zoom"), std::clamp(percent, 30, 400)); }
QString editorCommand() { return QSettings().value(QStringLiteral("edit/editorCommand")).toString(); }
void setEditorCommand(const QString &cmd) { QSettings().setValue(QStringLiteral("edit/editorCommand"), cmd); }

QVector<RecentComparison> recentComparisons()
{
    QVector<RecentComparison> out;
    const QJsonArray list = QJsonDocument::fromJson(QSettings().value(QStringLiteral("recent/comparisons")).toByteArray()).array();
    for (const QJsonValue &v : list) {
        const QJsonObject o = v.toObject();
        RecentComparison r;
        for (const QJsonValue &a : o.value(QStringLiteral("arguments")).toArray())
            r.arguments << a.toString();
        r.label = o.value(QStringLiteral("label")).toString();
        if (!r.arguments.isEmpty())
            out << r;
    }
    return out;
}

void addRecentComparison(const QStringList &arguments, const QString &label)
{
    QVector<RecentComparison> list = recentComparisons();
    list.removeIf([&](const RecentComparison &r) { return r.arguments == arguments; });
    list.prepend({arguments, label});
    QJsonArray out;
    for (qsizetype i = 0; i < list.size() && i < 20; ++i)
        out.append(QJsonObject{{QStringLiteral("arguments"), QJsonArray::fromStringList(list[i].arguments)},
                               {QStringLiteral("label"), list[i].label}});
    QSettings().setValue(QStringLiteral("recent/comparisons"), QJsonDocument(out).toJson(QJsonDocument::Compact));
}

void clearRecentComparisons()
{
    QSettings().remove(QStringLiteral("recent/comparisons"));
}

QStringList unimportantPatterns()
{
    return QSettings().value(QStringLiteral("diff/unimportantPatterns")).toStringList();
}
void setUnimportantPatterns(const QStringList &patterns)
{
    QSettings().setValue(QStringLiteral("diff/unimportantPatterns"), patterns);
}

DiffOptions diffOptions()
{
    QSettings s;
    DiffOptions o;
    const QString algo = s.value(QStringLiteral("diff/algorithm"), QStringLiteral("patience")).toString();
    for (DiffAlgorithm a : allAlgorithms())
        if (algorithmName(a) == algo)
            o.algorithm = a;
    o.ignoreSpaceChange = s.value(QStringLiteral("diff/ignoreSpaceChange"), false).toBool();
    o.ignoreAllSpace = s.value(QStringLiteral("diff/ignoreAllSpace"), false).toBool();
    o.ignoreBlankLines = s.value(QStringLiteral("diff/ignoreBlankLines"), false).toBool();
    o.ignoreCrAtEol = s.value(QStringLiteral("diff/ignoreCrAtEol"), false).toBool();
    o.ignoreTrim = s.value(QStringLiteral("diff/ignoreTrim"), false).toBool();
    o.ignoreCase = s.value(QStringLiteral("diff/ignoreCase"), false).toBool();
    o.ignoreComments = s.value(QStringLiteral("diff/ignoreComments"), false).toBool();
    if (s.value(QStringLiteral("diff/ignoreUnimportant"), false).toBool())
        o.unimportantPatterns = unimportantPatterns();
    o.detectMoves = s.value(QStringLiteral("diff/detectMoves"), true).toBool();
    return o;
}

void setDiffOptions(const DiffOptions &o)
{
    QSettings s;
    s.setValue(QStringLiteral("diff/algorithm"), algorithmName(o.algorithm));
    s.setValue(QStringLiteral("diff/ignoreSpaceChange"), o.ignoreSpaceChange);
    s.setValue(QStringLiteral("diff/ignoreAllSpace"), o.ignoreAllSpace);
    s.setValue(QStringLiteral("diff/ignoreBlankLines"), o.ignoreBlankLines);
    s.setValue(QStringLiteral("diff/ignoreCrAtEol"), o.ignoreCrAtEol);
    s.setValue(QStringLiteral("diff/ignoreTrim"), o.ignoreTrim);
    s.setValue(QStringLiteral("diff/ignoreCase"), o.ignoreCase);
    s.setValue(QStringLiteral("diff/ignoreComments"), o.ignoreComments);
    s.setValue(QStringLiteral("diff/ignoreUnimportant"), !o.unimportantPatterns.isEmpty());
    s.setValue(QStringLiteral("diff/detectMoves"), o.detectMoves);
}

void resetAppearance()
{
    QSettings s;
    s.remove(QStringLiteral("colors"));
    s.remove(QStringLiteral("view/font"));
    s.remove(QStringLiteral("view/tabWidth"));
}

} // namespace Settings
