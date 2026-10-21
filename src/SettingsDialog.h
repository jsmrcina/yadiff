#pragma once

#include <QDialog>
#include <QHash>
#include <QKeySequence>
#include <QList>

class QAction;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QMainWindow;
class QMenu;
class QPushButton;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

// Options > Settings: every option in the window's menus in one place, and every
// keyboard shortcut, which can be changed. Options apply at once (they are the menus'
// own actions); shortcuts apply with OK or Apply, to every window.
class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    // `skip`: menus whose entries aren't settings (recent comparisons, languages).
    SettingsDialog(QMainWindow *window, const QList<QMenu *> &skip, QWidget *parent = nullptr);

    // Tests.
    QTabWidget *tabs() const { return m_tabs; }
    QTreeWidget *shortcutTree() const { return m_tree; }
    QLineEdit *filter() const { return m_filter; }
    // Gives `action` these shortcuts, as if typed in; false when the user kept them
    // with another action (`confirm` answers the question about moving them).
    bool assign(QAction *action, const QList<QKeySequence> &keys, bool confirm = true);
    QList<QKeySequence> pending(QAction *action) const { return m_keys.value(action); }
    void applyShortcuts();

private:
    QWidget *buildOptions(QMainWindow *window, const QList<QMenu *> &skip);
    QWidget *buildShortcuts(QMainWindow *window);
    void addOptions(QMenu *menu, const QString &title, const QList<QMenu *> &skip, QVBoxLayout *into);
    void applyFilter();
    void showSelected();
    void editSelected();
    void refreshRow(QAction *action);
    QAction *selectedAction() const;

    QLineEdit *m_filter = nullptr;
    QTabWidget *m_tabs = nullptr;
    // Options: each widget with the words it is found by.
    QList<std::pair<QWidget *, QString>> m_optionWidgets;
    QList<QWidget *> m_optionGroups;
    // Shortcuts.
    QTreeWidget *m_tree = nullptr;
    QKeySequenceEdit *m_primary = nullptr;
    QKeySequenceEdit *m_alternate = nullptr;
    QPushButton *m_clear = nullptr;
    QPushButton *m_default = nullptr;
    QLabel *m_note = nullptr;
    QHash<QAction *, QList<QKeySequence>> m_keys;
    QHash<QAction *, QTreeWidgetItem *> m_rows;
    bool m_loadingEditors = false;
};
