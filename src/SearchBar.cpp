#include "SearchBar.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QStyle>
#include <QToolButton>

SearchBar::SearchBar(QWidget *parent) : QWidget(parent)
{
    m_edit = new QLineEdit;
    m_edit->setPlaceholderText(tr("Find"));
    m_edit->setClearButtonEnabled(true);
    m_edit->setMinimumWidth(220);

    m_scope = new QComboBox;
    m_scope->addItem(tr("Selection"), int(Scope::Selection));
    m_scope->addItem(tr("This file"), int(Scope::File));
    m_scope->addItem(tr("All changes"), int(Scope::AllChanges));
    m_scope->setCurrentIndex(1);
    m_scope->setToolTip(tr("Where to search"));

    m_mode = new QComboBox;
    m_mode->addItem(tr("Substring"));
    m_mode->addItem(tr("Regex"));
    m_mode->setToolTip(tr("Match the text literally, or as a regular expression"));

    m_case = new QCheckBox(tr("Match case"));

    auto *prev = new QToolButton;
    prev->setIcon(QIcon::fromTheme(QStringLiteral("go-up"), style()->standardIcon(QStyle::SP_ArrowUp)));
    prev->setToolTip(tr("Previous match (Shift+Enter)"));
    prev->setAutoRaise(true);
    auto *nextBtn = new QToolButton;
    nextBtn->setIcon(QIcon::fromTheme(QStringLiteral("go-down"), style()->standardIcon(QStyle::SP_ArrowDown)));
    nextBtn->setToolTip(tr("Next match (Enter)"));
    nextBtn->setAutoRaise(true);

    m_result = new QLabel;
    m_result->setMinimumWidth(140);

    m_stop = new QToolButton;
    m_stop->setText(tr("Stop"));
    m_stop->setToolTip(tr("Stop searching"));
    m_stop->setVisible(false);
    connect(m_stop, &QToolButton::clicked, this, &SearchBar::stopRequested);

    auto *close = new QToolButton;
    close->setIcon(QIcon::fromTheme(QStringLiteral("window-close"), style()->standardIcon(QStyle::SP_TitleBarCloseButton)));
    close->setToolTip(tr("Close (Esc)"));
    close->setAutoRaise(true);

    auto *l = new QHBoxLayout(this);
    l->setContentsMargins(6, 3, 6, 3);
    l->addWidget(m_edit, 1);
    l->addWidget(m_scope);
    l->addWidget(m_mode);
    l->addWidget(m_case);
    l->addWidget(prev);
    l->addWidget(nextBtn);
    l->addWidget(m_result);
    l->addWidget(m_stop);
    l->addWidget(close);

    m_edit->installEventFilter(this);
    connect(m_edit, &QLineEdit::textChanged, this, &SearchBar::queryChanged);
    connect(m_scope, &QComboBox::currentIndexChanged, this, &SearchBar::queryChanged);
    connect(m_mode, &QComboBox::currentIndexChanged, this, &SearchBar::queryChanged);
    connect(m_case, &QCheckBox::toggled, this, &SearchBar::queryChanged);
    connect(prev, &QToolButton::clicked, this, &SearchBar::previous);
    connect(nextBtn, &QToolButton::clicked, this, &SearchBar::next);
    connect(close, &QToolButton::clicked, this, [this] {
        hide();
        emit closed();
    });
}

SearchQuery SearchBar::query() const
{
    return SearchQuery(m_edit->text(), m_mode->currentIndex() == 1, m_case->isChecked());
}

SearchBar::Scope SearchBar::scope() const
{
    return Scope(m_scope->currentData().toInt());
}

void SearchBar::setScope(Scope s)
{
    m_scope->setCurrentIndex(m_scope->findData(int(s)));
}

void SearchBar::activate()
{
    show();
    m_edit->setFocus();
    m_edit->selectAll();
}

void SearchBar::setText(const QString &text)
{
    m_edit->setText(text);
}

void SearchBar::setBusy(bool busy)
{
    m_stop->setVisible(busy);
}

void SearchBar::setResult(const QString &text, bool error)
{
    m_result->setText(text);
    QPalette p = m_result->palette();
    p.setColor(QPalette::WindowText, error ? QColor(0xd1, 0x3b, 0x3b) : palette().color(QPalette::WindowText));
    m_result->setPalette(p);
}

bool SearchBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_edit && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
            if (ke->modifiers() & Qt::ShiftModifier)
                emit previous();
            else
                emit next();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void SearchBar::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        hide();
        emit closed();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && (event->modifiers() & Qt::ShiftModifier)) {
        emit previous();
        return;
    }
    QWidget::keyPressEvent(event);
}
