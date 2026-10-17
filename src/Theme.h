#pragma once

#include <QColor>
#include <QString>
#include <QVector>

// Colour themes: the window's colours, the syntax colours and the diff colours together.
// "system" follows the desktop; the others use Qt's Fusion style with their own palette.
namespace Theme {

struct Def {
    QString key, label;
    bool dark = false;
    QString syntaxTheme;  // KSyntaxHighlighting theme name
    QColor window, base, text, accent, selection;
    QString counterpart;  // the matching light/dark theme
};

QVector<Def> all();
QString current();
Def currentDef();
bool isDark();

// Call once after QApplication exists: remembers the desktop style and applies the saved theme.
void initialize();
void apply(const QString &key);
// Switches between the current theme and its light/dark counterpart.
QString toggleLightDark();

// Line colours for this theme (used unless the user picked their own).
QColor addedColor();
QColor removedColor();
QColor movedColor();

} // namespace Theme
