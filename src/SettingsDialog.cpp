#include "SettingsDialog.h"

#include "Shortcuts.h"

#include <QAction>
#include <QActionGroup>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

QString clean(QString text)
{
    text.remove(u'&');
    return text.trimmed();
}

// Keeps a check box or radio button in step with its action, both ways.
void bind(QAbstractButton *b, QAction *a)
{
    auto sync = [b, a] {
        const QSignalBlocker block(b);
        b->setChecked(a->isChecked());
        b->setEnabled(a->isEnabled());
    };
    sync();
    QObject::connect(a, &QAction::changed, b, sync);
    QObject::connect(a, &QAction::toggled, b, sync);
    QObject::connect(b, &QAbstractButton::clicked, a, [a, b, sync](bool on) {
        // Radio buttons only ever turn on; a check box turns its action on or off.
        if (a->isChecked() != on || (qobject_cast<QRadioButton *>(b) && !a->isChecked()))
            a->trigger();
        sync(); // the action may have said no (it asked for something first)
    });
}

} // namespace

SettingsDialog::SettingsDialog(QMainWindow *window, const QList<QMenu *> &skip, QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Settings"));
    resize(900, 680);
    auto *v = new QVBoxLayout(this);
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(tr("Find a setting or shortcut"));
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this, &SettingsDialog::applyFilter);
    v->addWidget(m_filter);
    m_tabs = new QTabWidget;
    m_tabs->addTab(buildOptions(window, skip), tr("&Options"));
    m_tabs->addTab(buildShortcuts(window), tr("&Keyboard shortcuts"));
    v->addWidget(m_tabs, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        applyShortcuts();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &SettingsDialog::applyShortcuts);
    v->addWidget(buttons);
    m_filter->setFocus();
}

// ---- Options ----------------------------------------------------------------

QWidget *SettingsDialog::buildOptions(QMainWindow *window, const QList<QMenu *> &skip)
{
    auto *page = new QWidget;
    auto *outer = new QVBoxLayout(page);
    auto *note = new QLabel(tr("These are the options from the menus; a change takes effect at once."));
    note->setWordWrap(true);
    outer->addWidget(note);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget;
    auto *grid = new QGridLayout(content);
    grid->setAlignment(Qt::AlignTop);
    QList<QGroupBox *> boxes;
    for (QAction *top : window->menuBar()->actions()) {
        QMenu *m = top->menu();
        if (!m || skip.contains(m))
            continue;
        auto *box = new QGroupBox(clean(top->text()));
        auto *lay = new QVBoxLayout(box);
        addOptions(m, QString(), skip, lay);
        if (lay->count() == 0) {
            delete box;
            continue;
        }
        lay->addStretch();
        boxes << box;
        m_optionGroups << box;
    }
    // Two columns, the longer groups spread between them.
    int heights[2] = {0, 0}, rows[2] = {0, 0};
    for (QGroupBox *box : std::as_const(boxes)) {
        const int col = heights[0] <= heights[1] ? 0 : 1;
        grid->addWidget(box, rows[col]++, col);
        heights[col] += box->sizeHint().height();
    }
    grid->setRowStretch(std::max(rows[0], rows[1]), 1);
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);
    return page;
}

void SettingsDialog::addOptions(QMenu *menu, const QString &title, const QList<QMenu *> &skip, QVBoxLayout *into)
{
    QLabel *heading = nullptr;
    if (!title.isEmpty()) {
        heading = new QLabel(QStringLiteral("<b>%1</b>").arg(title.toHtmlEscaped()));
        into->addWidget(heading);
    }
    const int before = into->count();
    QHash<QActionGroup *, QButtonGroup *> groups;
    for (QAction *a : menu->actions()) {
        if (a->isSeparator() || a->property("yadiffNotASetting").toBool())
            continue;
        if (QMenu *sub = a->menu()) {
            if (!skip.contains(sub))
                addOptions(sub, clean(a->text()), skip, into);
            continue;
        }
        QWidget *w = nullptr;
        const QString tip = a->statusTip().isEmpty() ? a->toolTip() : a->statusTip();
        if (a->isCheckable()) {
            QAbstractButton *b = nullptr;
            if (QActionGroup *g = a->actionGroup(); g && g->isExclusive()) {
                auto *radio = new QRadioButton(clean(a->text()));
                QButtonGroup *&bg = groups[g];
                if (!bg)
                    bg = new QButtonGroup(radio);
                bg->addButton(radio);
                b = radio;
            } else {
                b = new QCheckBox(clean(a->text()));
            }
            bind(b, a);
            w = b;
        } else if (a->property("yadiffSetting").toBool()) {
            // Settings with their own dialog (fonts, colours, patterns).
            auto *button = new QPushButton(clean(a->text()));
            button->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
            connect(button, &QPushButton::clicked, a, &QAction::trigger);
            w = button;
        } else {
            continue;
        }
        if (clean(a->text()) != tip && tip != a->text())
            w->setToolTip(tip);
        if (!title.isEmpty())
            w->setContentsMargins(16, 0, 0, 0);
        into->addWidget(w);
        m_optionWidgets << std::pair{w, clean(a->text()) + u' ' + title + u' ' + tip};
    }
    if (heading) {
        if (into->count() == before + 0) {
            into->removeWidget(heading);
            delete heading;
        } else {
            m_optionWidgets << std::pair{static_cast<QWidget *>(heading), title};
        }
    }
}

// ---- Keyboard shortcuts -----------------------------------------------------

QWidget *SettingsDialog::buildShortcuts(QMainWindow *window)
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    m_tree = new QTreeWidget;
    m_tree->setHeaderLabels({tr("Command"), tr("Shortcut"), tr("Default")});
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    const auto saved = Shortcuts::overrides();
    QHash<QString, QTreeWidgetItem *> groups;
    for (QAction *a : Shortcuts::actions(window)) {
        const QStringList parts = Shortcuts::path(a).split(QStringLiteral(" › "));
        const QString group = parts.size() > 1 ? parts.first() : tr("Window");
        QTreeWidgetItem *&g = groups[group];
        if (!g) {
            g = new QTreeWidgetItem(m_tree, {group});
            g->setFlags(Qt::ItemIsEnabled);
            g->setFirstColumnSpanned(true);
            QFont f = g->font(0);
            f.setBold(true);
            g->setFont(0, f);
        }
        auto *row = new QTreeWidgetItem(g, {parts.size() > 1 ? parts.mid(1).join(QStringLiteral(" › ")) : parts.first()});
        row->setData(0, Qt::UserRole, QVariant::fromValue(static_cast<QObject *>(a)));
        row->setToolTip(0, a->statusTip().isEmpty() ? a->toolTip() : a->statusTip());
        m_rows.insert(a, row);
        m_keys.insert(a, a->shortcuts());
        refreshRow(a);
    }
    m_tree->expandAll();
    connect(m_tree, &QTreeWidget::currentItemChanged, this, &SettingsDialog::showSelected);
    v->addWidget(m_tree, 1);

    auto *edit = new QGridLayout;
    m_primary = new QKeySequenceEdit;
    m_alternate = new QKeySequenceEdit;
    for (QKeySequenceEdit *e : {m_primary, m_alternate}) {
        e->setMaximumSequenceLength(1);
        e->setClearButtonEnabled(true);
        connect(e, &QKeySequenceEdit::editingFinished, this, &SettingsDialog::editSelected);
        connect(e, &QKeySequenceEdit::keySequenceChanged, this, [this, e](const QKeySequence &k) {
            if (k.isEmpty() && !m_loadingEditors && !e->hasFocus())
                editSelected(); // the clear button
        });
    }
    m_clear = new QPushButton(tr("&None"));
    m_clear->setToolTip(tr("No shortcut for this command"));
    connect(m_clear, &QPushButton::clicked, this, [this] {
        if (QAction *a = selectedAction())
            assign(a, {});
    });
    m_default = new QPushButton(tr("&Default"));
    m_default->setToolTip(tr("Back to the shortcut yadiff comes with"));
    connect(m_default, &QPushButton::clicked, this, [this] {
        if (QAction *a = selectedAction())
            assign(a, Shortcuts::defaults(a));
    });
    auto *resetAll = new QPushButton(tr("Reset &all to defaults"));
    connect(resetAll, &QPushButton::clicked, this, [this] {
        for (auto it = m_keys.begin(); it != m_keys.end(); ++it) {
            it.value() = Shortcuts::defaults(it.key());
            refreshRow(it.key());
        }
        showSelected();
    });
    edit->addWidget(new QLabel(tr("Shortcut:")), 0, 0);
    edit->addWidget(m_primary, 0, 1);
    edit->addWidget(new QLabel(tr("Alternative:")), 0, 2);
    edit->addWidget(m_alternate, 0, 3);
    edit->addWidget(m_clear, 0, 4);
    edit->addWidget(m_default, 0, 5);
    edit->addWidget(resetAll, 0, 6);
    edit->setColumnStretch(1, 1);
    edit->setColumnStretch(3, 1);
    v->addLayout(edit);
    m_note = new QLabel(tr("Pick a command, click in a box and press the keys."));
    m_note->setWordWrap(true);
    v->addWidget(m_note);
    showSelected();
    return page;
}

QAction *SettingsDialog::selectedAction() const
{
    const QTreeWidgetItem *it = m_tree->currentItem();
    return it ? qobject_cast<QAction *>(it->data(0, Qt::UserRole).value<QObject *>()) : nullptr;
}

void SettingsDialog::refreshRow(QAction *a)
{
    QTreeWidgetItem *row = m_rows.value(a);
    if (!row)
        return;
    const QList<QKeySequence> keys = m_keys.value(a), def = Shortcuts::defaults(a);
    auto text = [](const QList<QKeySequence> &k) {
        QStringList parts;
        for (const QKeySequence &s : k)
            parts << s.toString(QKeySequence::NativeText);
        return parts.join(QStringLiteral(", "));
    };
    row->setText(1, text(keys));
    row->setText(2, text(def));
    QFont f = row->font(1);
    f.setBold(keys != def); // changed from the default
    row->setFont(1, f);
}

void SettingsDialog::showSelected()
{
    QAction *a = selectedAction();
    m_loadingEditors = true;
    const QList<QKeySequence> keys = a ? m_keys.value(a) : QList<QKeySequence>();
    m_primary->setKeySequence(keys.value(0));
    m_alternate->setKeySequence(keys.value(1));
    m_loadingEditors = false;
    for (QWidget *w : std::initializer_list<QWidget *>{m_primary, m_alternate, m_clear, m_default})
        w->setEnabled(a);
    if (a)
        m_note->setText(tr("%1: click in a box and press the keys.").arg(Shortcuts::path(a)));
}

void SettingsDialog::editSelected()
{
    if (m_loadingEditors)
        return;
    QAction *a = selectedAction();
    if (!a)
        return;
    QList<QKeySequence> keys;
    for (QKeySequenceEdit *e : {m_primary, m_alternate})
        if (!e->keySequence().isEmpty() && !keys.contains(e->keySequence()))
            keys << e->keySequence();
    if (keys != m_keys.value(a) && !assign(a, keys))
        showSelected(); // kept with the other command: back to what it was
}

bool SettingsDialog::assign(QAction *action, const QList<QKeySequence> &keys, bool confirm)
{
    // A key can do one thing: offer to take it from the command that has it.
    for (const QKeySequence &k : keys) {
        for (auto it = m_keys.begin(); it != m_keys.end(); ++it) {
            if (it.key() == action || !it.value().contains(k))
                continue;
            if (confirm
                && QMessageBox::question(this, tr("Shortcut in use"),
                                         tr("%1 is the shortcut for %2.\n\nUse it for %3 instead?")
                                             .arg(k.toString(QKeySequence::NativeText), Shortcuts::path(it.key()),
                                                  Shortcuts::path(action)))
                       != QMessageBox::Yes)
                return false;
            it.value().removeAll(k);
            refreshRow(it.key());
        }
    }
    m_keys[action] = keys;
    refreshRow(action);
    if (action == selectedAction())
        showSelected();
    return true;
}

void SettingsDialog::applyShortcuts()
{
    auto o = Shortcuts::overrides();
    for (auto it = m_keys.constBegin(); it != m_keys.constEnd(); ++it) {
        const QString id = Shortcuts::id(it.key());
        if (it.value() == Shortcuts::defaults(it.key()))
            o.remove(id);
        else
            o.insert(id, it.value());
    }
    Shortcuts::setOverrides(o);
    Shortcuts::applyAll();
}

// ---- Filter -----------------------------------------------------------------

void SettingsDialog::applyFilter()
{
    const QStringList terms = m_filter->text().split(u' ', Qt::SkipEmptyParts);
    auto matches = [&terms](const QString &text) {
        for (const QString &t : terms)
            if (!text.contains(t, Qt::CaseInsensitive))
                return false;
        return true;
    };
    for (const auto &[w, words] : std::as_const(m_optionWidgets))
        w->setVisible(matches(words + u' ' + static_cast<QGroupBox *>(w->parentWidget())->title()));
    for (QWidget *g : std::as_const(m_optionGroups)) {
        bool any = false;
        for (const auto &[w, words] : std::as_const(m_optionWidgets))
            any |= w->parentWidget() == g && !w->isHidden();
        g->setVisible(any);
    }
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *g = m_tree->topLevelItem(i);
        bool any = false;
        for (int c = 0; c < g->childCount(); ++c) {
            QTreeWidgetItem *row = g->child(c);
            const bool show = matches(g->text(0) + u' ' + row->text(0) + u' ' + row->text(1) + u' ' + row->toolTip(0));
            row->setHidden(!show);
            any |= show;
        }
        g->setHidden(!any);
    }
}
