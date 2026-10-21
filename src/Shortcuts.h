#pragma once

#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QString>

class QAction;
class QMainWindow;
class QMenu;

// Keyboard shortcuts the user has changed (Options > Settings > Keyboard shortcuts).
// Each menu action gets a stable id from its menu path ("View/Show line numbers"), keeps
// the shortcuts it was built with as its defaults, and takes the user's instead where
// they set one.
namespace Shortcuts {

// Gives the window's actions (its menus', and any with a shortcut added to it) their ids
// and defaults, then applies the saved shortcuts. `skip`: menus whose entries change as
// you use them (recent comparisons, languages), left out.
void registerWindow(QMainWindow *window, const QList<QMenu *> &skip = {});

// The actions registered on a window, in menu order.
QList<QAction *> actions(const QMainWindow *window);
QString id(const QAction *action);
QString path(const QAction *action); // "View › Show line numbers", for showing
QList<QKeySequence> defaults(const QAction *action);

// What the user set, by id. A present id with an empty list: no shortcut at all.
QHash<QString, QList<QKeySequence>> overrides();
void setOverrides(const QHash<QString, QList<QKeySequence>> &o);

// Puts the saved shortcuts (or the defaults) on every registered window's actions.
void applyAll();

// "Ctrl+F, F3"; and back.
QString toText(const QList<QKeySequence> &keys);
QList<QKeySequence> fromText(const QString &text);

} // namespace Shortcuts
