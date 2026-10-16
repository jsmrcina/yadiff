#include "Settings.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPalette>
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
    if (QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark)
        return true;
    return QApplication::palette().color(QPalette::Base).lightness() < 128;
}

DiffColors defaultColors()
{
    if (isDarkTheme())
        return {QColor(0x21, 0x3d, 0x2a), QColor(0x4b, 0x23, 0x27), QColor(0x2c, 0x2f, 0x5c)};
    return {QColor(0xd7, 0xf5, 0xdd), QColor(0xfb, 0xdc, 0xdc), QColor(0xdd, 0xe2, 0xff)};
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

void resetAppearance()
{
    QSettings s;
    s.remove(QStringLiteral("colors"));
    s.remove(QStringLiteral("view/font"));
    s.remove(QStringLiteral("view/tabWidth"));
}

} // namespace Settings
