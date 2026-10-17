#include "Theme.h"

#include "Syntax.h"

#include <QApplication>
#include <QPalette>
#include <QPointer>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

namespace {

QString g_systemStyle;
QPalette g_systemPalette;
bool g_initialized = false;

Theme::Def def(const char *key, const char *label, bool dark, const char *syntax, const char *window, const char *base,
               const char *text, const char *accent, const char *selection, const char *counterpart)
{
    return {QString::fromLatin1(key), QString::fromUtf8(label), dark,     QString::fromUtf8(syntax), QColor(window),
            QColor(base),             QColor(text),              QColor(accent), QColor(selection), QString::fromLatin1(counterpart)};
}

QColor mix(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(float(a.redF() * (1 - t) + b.redF() * t), float(a.greenF() * (1 - t) + b.greenF() * t),
                            float(a.blueF() * (1 - t) + b.blueF() * t));
}

bool systemIsDark()
{
    if (QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark)
        return true;
    return g_systemPalette.color(QPalette::Base).lightness() < 128;
}

QPalette paletteFor(const Theme::Def &t)
{
    QPalette p;
    const QColor dim = mix(t.text, t.base, 0.45);
    const QColor button = t.dark ? t.window.lighter(125) : t.window.darker(104);
    for (QPalette::ColorGroup g : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        const bool disabled = g == QPalette::Disabled;
        p.setColor(g, QPalette::Window, t.window);
        p.setColor(g, QPalette::WindowText, disabled ? dim : t.text);
        p.setColor(g, QPalette::Base, t.base);
        p.setColor(g, QPalette::AlternateBase, mix(t.base, t.window, 0.5));
        p.setColor(g, QPalette::Text, disabled ? dim : t.text);
        p.setColor(g, QPalette::Button, button);
        p.setColor(g, QPalette::ButtonText, disabled ? dim : t.text);
        p.setColor(g, QPalette::BrightText, Qt::white);
        p.setColor(g, QPalette::Highlight, disabled ? mix(t.accent, t.base, 0.5) : t.accent);
        p.setColor(g, QPalette::HighlightedText, t.dark || t.accent.lightness() < 150 ? QColor(Qt::white) : QColor(Qt::black));
        p.setColor(g, QPalette::ToolTipBase, t.window);
        p.setColor(g, QPalette::ToolTipText, t.text);
        p.setColor(g, QPalette::PlaceholderText, dim);
        p.setColor(g, QPalette::Link, t.dark ? t.accent.lighter(130) : t.accent);
        p.setColor(g, QPalette::LinkVisited, t.dark ? t.accent.lighter(110) : t.accent.darker(120));
        p.setColor(g, QPalette::Light, button.lighter(130));
        p.setColor(g, QPalette::Midlight, button.lighter(115));
        p.setColor(g, QPalette::Mid, button.darker(130));
        p.setColor(g, QPalette::Dark, button.darker(160));
        p.setColor(g, QPalette::Shadow, Qt::black);
    }
    return p;
}

} // namespace

namespace Theme {

QVector<Def> all()
{
    static const QVector<Def> themes{
        {QStringLiteral("system"), QStringLiteral("System"), false, QString(), {}, {}, {}, {}, {}, QString()},
        def("dark-plus", "Dark+ (VS Code)", true, "VSCodium Dark", "#252526", "#1e1e1e", "#d4d4d4", "#0e639c", "#264f78", "light-plus"),
        def("light-plus", "Light+ (VS Code)", false, "VS Code Light+", "#f3f3f3", "#ffffff", "#1f1f1f", "#005fb8", "#add6ff", "dark-plus"),
        def("monokai", "Monokai", true, "Monokai", "#1e1f1c", "#272822", "#f8f8f2", "#75715e", "#49483e", "light-plus"),
        def("solarized-dark", "Solarized Dark", true, "Solarized Dark", "#073642", "#002b36", "#93a1a1", "#268bd2", "#274642", "solarized-light"),
        def("solarized-light", "Solarized Light", false, "Solarized Light", "#eee8d5", "#fdf6e3", "#586e75", "#268bd2", "#e6dfc6", "solarized-dark"),
        def("one-dark", "One Dark", true, "Atom One Dark", "#21252b", "#282c34", "#abb2bf", "#4d78cc", "#3e4451", "one-light"),
        def("one-light", "One Light", false, "Atom One Light", "#eaeaeb", "#fafafa", "#383a42", "#4078f2", "#e5e5e6", "one-dark"),
        def("github-light", "GitHub Light", false, "GitHub Light", "#f6f8fa", "#ffffff", "#1f2328", "#0969da", "#b6e3ff", "github-dark"),
        def("github-dark", "GitHub Dark", true, "GitHub Dark", "#161b22", "#0d1117", "#e6edf3", "#1f6feb", "#264f78", "github-light"),
        def("dracula", "Dracula", true, "Dracula", "#21222c", "#282a36", "#f8f8f2", "#bd93f9", "#44475a", "light-plus"),
        def("vibrant-ink", "Vibrant Ink", true, "Vibrant Ink", "#121212", "#000000", "#ffffff", "#ff6600", "#35493c", "light-plus"),
    };
    return themes;
}

QString current()
{
    const QString key = QSettings().value(QStringLiteral("view/theme"), QStringLiteral("system")).toString();
    for (const Def &d : all())
        if (d.key == key)
            return key;
    return QStringLiteral("system");
}

Def currentDef()
{
    const QString key = current();
    for (const Def &d : all())
        if (d.key == key)
            return d;
    return all().first();
}

bool isDark()
{
    const Def d = currentDef();
    return d.key == QLatin1String("system") ? systemIsDark() : d.dark;
}

void initialize()
{
    if (!g_initialized) {
        g_systemStyle = QApplication::style()->name();
        g_systemPalette = QApplication::palette();
        g_initialized = true;
    }
    apply(current());
}

void apply(const QString &key)
{
    if (!g_initialized) {
        g_systemStyle = QApplication::style()->name();
        g_systemPalette = QApplication::palette();
        g_initialized = true;
    }
    QSettings().setValue(QStringLiteral("view/theme"), key);
    const Def d = currentDef();
    if (d.key == QLatin1String("system")) {
        if (QApplication::style()->name().compare(g_systemStyle, Qt::CaseInsensitive) != 0)
            QApplication::setStyle(QStyleFactory::create(g_systemStyle));
        QApplication::setPalette(g_systemPalette);
        Syntax::setThemeName(QString()); // Breeze light/dark, by the palette
    } else {
        if (QApplication::style()->name().compare(QLatin1String("fusion"), Qt::CaseInsensitive) != 0)
            QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        QApplication::setPalette(paletteFor(d));
        Syntax::setThemeName(d.syntaxTheme);
    }
}

QString toggleLightDark()
{
    const Def d = currentDef();
    QString target = d.counterpart;
    if (d.key == QLatin1String("system"))
        target = systemIsDark() ? QStringLiteral("light-plus") : QStringLiteral("dark-plus");
    apply(target);
    return target;
}

QColor addedColor()
{
    const Def d = currentDef();
    if (d.key == QLatin1String("system"))
        return isDark() ? QColor(0x21, 0x3d, 0x2a) : QColor(0xd7, 0xf5, 0xdd);
    return mix(d.base, QColor(0x2e, 0xa0, 0x43), d.dark ? 0.26 : 0.18);
}

QColor removedColor()
{
    const Def d = currentDef();
    if (d.key == QLatin1String("system"))
        return isDark() ? QColor(0x4b, 0x23, 0x27) : QColor(0xfb, 0xdc, 0xdc);
    return mix(d.base, QColor(0xf8, 0x51, 0x49), d.dark ? 0.26 : 0.16);
}

QColor movedColor()
{
    const Def d = currentDef();
    if (d.key == QLatin1String("system"))
        return isDark() ? QColor(0x2c, 0x2f, 0x5c) : QColor(0xdd, 0xe2, 0xff);
    return mix(d.base, QColor(0x6e, 0x7b, 0xff), d.dark ? 0.30 : 0.18);
}

} // namespace Theme
