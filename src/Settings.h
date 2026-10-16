#pragma once

#include <QColor>
#include <QFont>

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

void resetAppearance();

} // namespace Settings
