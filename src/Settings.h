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
bool autoReload();
void setAutoReload(bool on);

DiffOptions diffOptions();
void setDiffOptions(const DiffOptions &o);

void resetAppearance();

} // namespace Settings
