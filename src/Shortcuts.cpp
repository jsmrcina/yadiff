#include "Shortcuts.h"

#include <QAction>
#include <QApplication>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>
#include <QSettings>

namespace {

constexpr char kId[] = "yadiffShortcutId";
constexpr char kPath[] = "yadiffShortcutPath";
constexpr char kDefaults[] = "yadiffShortcutDefaults";
constexpr char kToolTip[] = "yadiffShortcutToolTip";

QString clean(QString text)
{
    text.remove(u'&');
    if (text.endsWith(QStringLiteral("…")))
        text.chop(1);
    else if (text.endsWith(QStringLiteral("...")))
        text.chop(3);
    return text.trimmed();
}

void collect(QMenu *menu, const QStringList &where, const QList<QMenu *> &skip, QList<QAction *> &out)
{
    for (QAction *a : menu->actions()) {
        if (a->isSeparator())
            continue;
        if (QMenu *sub = a->menu()) {
            if (!skip.contains(sub))
                collect(sub, where + QStringList{clean(a->text())}, skip, out);
            continue;
        }
        if (a->text().isEmpty() || a->property(kId).isValid())
            continue;
        const QStringList full = where + QStringList{clean(a->text())};
        a->setProperty(kId, full.join(u'/'));
        a->setProperty(kPath, full.join(QStringLiteral(" › ")));
        out << a;
    }
}

struct Registered {
    QPointer<QMainWindow> window;
    QList<QPointer<QAction>> actions;
};

QList<Registered> &windows()
{
    static QList<Registered> list;
    list.removeIf([](const Registered &r) { return r.window.isNull(); });
    return list;
}

void apply(QAction *a, const QHash<QString, QList<QKeySequence>> &o)
{
    const QList<QKeySequence> def = Shortcuts::defaults(a);
    const auto it = o.constFind(Shortcuts::id(a));
    const QList<QKeySequence> keys = it != o.constEnd() ? *it : def;
    if (a->shortcuts() != keys)
        a->setShortcuts(keys);
    // Tooltips that name the default shortcut name the one in use.
    const QString tip = a->property(kToolTip).toString();
    if (!tip.isEmpty() && !def.isEmpty()) {
        const QString was = def.first().toString(QKeySequence::NativeText);
        QString now = tip;
        if (keys.isEmpty())
            now.replace(QStringLiteral(" (%1)").arg(was), QString());
        now.replace(was, keys.isEmpty() ? QString() : keys.first().toString(QKeySequence::NativeText));
        a->setToolTip(now);
    }
}

} // namespace

namespace Shortcuts {

void registerWindow(QMainWindow *window, const QList<QMenu *> &skip)
{
    QList<QAction *> list;
    for (QAction *top : window->menuBar()->actions())
        if (QMenu *m = top->menu())
            collect(m, {clean(top->text())}, skip, list);
    // Window actions outside the menus (Ctrl+P for the file filter).
    for (QAction *a : window->actions()) {
        if (a->property(kId).isValid() || a->shortcuts().isEmpty())
            continue;
        a->setProperty(kId, QStringLiteral("Window/") + clean(a->text()));
        a->setProperty(kPath, clean(a->text()));
        list << a;
    }
    Registered r{window, {}};
    for (QAction *a : std::as_const(list)) {
        QVariantList keys;
        for (const QKeySequence &k : a->shortcuts())
            keys << k;
        a->setProperty(kDefaults, keys);
        if (a->toolTip() != clean(a->text()))
            a->setProperty(kToolTip, a->toolTip());
        r.actions << a;
    }
    windows() << r;
    const auto o = overrides();
    for (QAction *a : std::as_const(list))
        apply(a, o);
}

QList<QAction *> actions(const QMainWindow *window)
{
    QList<QAction *> out;
    for (const Registered &r : windows())
        if (r.window == window)
            for (const QPointer<QAction> &a : r.actions)
                if (a)
                    out << a;
    return out;
}

QString id(const QAction *action)
{
    return action->property(kId).toString();
}

QString path(const QAction *action)
{
    return action->property(kPath).toString();
}

QList<QKeySequence> defaults(const QAction *action)
{
    QList<QKeySequence> out;
    for (const QVariant &v : action->property(kDefaults).toList())
        out << v.value<QKeySequence>();
    return out;
}

QHash<QString, QList<QKeySequence>> overrides()
{
    QHash<QString, QList<QKeySequence>> out;
    const QVariantMap m = QSettings().value(QStringLiteral("keys/shortcuts")).toMap();
    for (auto it = m.constBegin(); it != m.constEnd(); ++it)
        out.insert(it.key(), fromText(it.value().toString()));
    return out;
}

void setOverrides(const QHash<QString, QList<QKeySequence>> &o)
{
    QVariantMap m;
    for (auto it = o.constBegin(); it != o.constEnd(); ++it)
        m.insert(it.key(), toText(it.value()));
    QSettings s;
    if (m.isEmpty())
        s.remove(QStringLiteral("keys/shortcuts"));
    else
        s.setValue(QStringLiteral("keys/shortcuts"), m);
}

void applyAll()
{
    const auto o = overrides();
    for (const Registered &r : windows())
        for (const QPointer<QAction> &a : r.actions)
            if (a)
                apply(a, o);
}

QString toText(const QList<QKeySequence> &keys)
{
    QStringList parts;
    for (const QKeySequence &k : keys)
        if (!k.isEmpty())
            parts << k.toString(QKeySequence::PortableText);
    return parts.join(QStringLiteral("; "));
}

QList<QKeySequence> fromText(const QString &text)
{
    QList<QKeySequence> out;
    for (const QString &part : text.split(u';', Qt::SkipEmptyParts))
        if (const QKeySequence k = QKeySequence::fromString(part.trimmed(), QKeySequence::PortableText); !k.isEmpty())
            out << k;
    return out;
}

} // namespace Shortcuts
