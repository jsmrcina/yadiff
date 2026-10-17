#include "NewComparisonDialog.h"

#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

// A line edit with a "…" button that browses for a folder or file.
QWidget *browseRow(QLineEdit *edit, bool folder, QWidget *parent)
{
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    auto *b = new QToolButton;
    b->setText(QStringLiteral("…"));
    b->setToolTip(folder ? QObject::tr("Choose a folder") : QObject::tr("Choose a file"));
    QObject::connect(b, &QToolButton::clicked, parent, [edit, folder, parent] {
        const QString start = edit->text();
        const QString picked = folder ? QFileDialog::getExistingDirectory(parent, QString(), start)
                                      : QFileDialog::getOpenFileName(parent, QString(), start);
        if (!picked.isEmpty())
            edit->setText(picked);
    });
    h->addWidget(edit, 1);
    h->addWidget(b);
    return w;
}

} // namespace

NewComparisonDialog::NewComparisonDialog(const QString &currentRepo, QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("New comparison"));
    setMinimumWidth(620);

    const QStringList kinds{tr("Working tree changes of a repository"), tr("Two revisions"), tr("Two folders"),
                            tr("Two files"), tr("A pull request (GitHub or Azure DevOps)")};
    m_kind = new QButtonGroup(this);
    auto *choices = new QVBoxLayout;
    for (int i = 0; i < kinds.size(); ++i) {
        auto *r = new QRadioButton(kinds[i]);
        m_kind->addButton(r, i);
        choices->addWidget(r);
    }

    m_pages = new QStackedWidget;
    auto page = [&](const QList<QPair<QString, QWidget *>> &rows) {
        auto *w = new QWidget;
        auto *f = new QFormLayout(w);
        for (const auto &row : rows)
            f->addRow(row.first, row.second);
        m_pages->addWidget(w);
    };
    m_repo = new QLineEdit(currentRepo);
    page({{tr("Repository:"), browseRow(m_repo, true, this)}});
    m_revRepo = new QLineEdit(currentRepo);
    m_rev1 = new QLineEdit;
    m_rev1->setPlaceholderText(tr("e.g. main, v1.2, HEAD~3, a commit id"));
    m_rev2 = new QLineEdit;
    m_rev2->setPlaceholderText(tr("empty: the working tree"));
    page({{tr("Repository:"), browseRow(m_revRepo, true, this)}, {tr("Old revision:"), m_rev1}, {tr("New revision:"), m_rev2}});
    m_leftDir = new QLineEdit;
    m_leftDir->setObjectName(QStringLiteral("leftDir"));
    m_rightDir = new QLineEdit;
    m_rightDir->setObjectName(QStringLiteral("rightDir"));
    page({{tr("Left folder:"), browseRow(m_leftDir, true, this)}, {tr("Right folder:"), browseRow(m_rightDir, true, this)}});
    m_leftFile = new QLineEdit;
    m_rightFile = new QLineEdit;
    page({{tr("Left file:"), browseRow(m_leftFile, false, this)}, {tr("Right file:"), browseRow(m_rightFile, false, this)}});
    m_prUrl = new QLineEdit;
    m_prUrl->setObjectName(QStringLiteral("prUrl"));
    m_prUrl->setPlaceholderText(QStringLiteral("https://github.com/OWNER/REPO/pull/N  or  https://dev.azure.com/ORG/PROJECT/_git/REPO/pullrequest/N"));
    page({{tr("Link:"), m_prUrl}});
    connect(m_kind, &QButtonGroup::idClicked, m_pages, &QStackedWidget::setCurrentIndex);
    m_kind->button(currentRepo.isEmpty() ? 2 : 0)->setChecked(true);
    m_pages->setCurrentIndex(currentRepo.isEmpty() ? 2 : 0);

    auto *hint = new QLabel(tr("The comparison opens in a new window."));
    hint->setEnabled(false);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    buttons->addButton(tr("Compare"), QDialogButtonBox::AcceptRole)->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &NewComparisonDialog::validateAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(choices);
    layout->addWidget(m_pages);
    layout->addWidget(hint);
    layout->addWidget(buttons);
}

QStringList NewComparisonDialog::arguments() const
{
    return m_args;
}

void NewComparisonDialog::validateAndAccept()
{
    auto problem = [this](const QString &msg) { QMessageBox::warning(this, windowTitle(), msg); };
    m_args.clear();
    switch (m_kind->checkedId()) {
    case 0:
        if (!QFileInfo(m_repo->text()).isDir())
            return problem(tr("Choose the repository's folder."));
        m_args << QStringLiteral("-C") << m_repo->text();
        break;
    case 1:
        if (!QFileInfo(m_revRepo->text()).isDir() || m_rev1->text().trimmed().isEmpty())
            return problem(tr("Choose the repository and at least the old revision."));
        m_args << QStringLiteral("-C") << m_revRepo->text() << m_rev1->text().trimmed();
        if (!m_rev2->text().trimmed().isEmpty())
            m_args << m_rev2->text().trimmed();
        break;
    case 2:
        if (!QFileInfo(m_leftDir->text()).isDir() || !QFileInfo(m_rightDir->text()).isDir())
            return problem(tr("Choose two existing folders."));
        m_args << QStringLiteral("--no-index") << m_leftDir->text() << m_rightDir->text();
        break;
    case 3:
        if (!QFileInfo(m_leftFile->text()).isFile() || !QFileInfo(m_rightFile->text()).isFile())
            return problem(tr("Choose two existing files."));
        m_args << QStringLiteral("--no-index") << m_leftFile->text() << m_rightFile->text();
        break;
    case 4: {
        const QString url = m_prUrl->text().trimmed();
        if (url.contains(QLatin1String("github.com")))
            m_args << QStringLiteral("-ghpr") << url;
        else if (url.contains(QLatin1String("/_git/")))
            m_args << QStringLiteral("-ado") << url;
        else
            return problem(tr("Paste a GitHub or Azure DevOps pull request link."));
        break;
    }
    default:
        return;
    }
    accept();
}
