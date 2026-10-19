#include "HistoryDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
constexpr int kIndexRole = Qt::UserRole + 1;
constexpr int kPageSize = 300;
} // namespace

HistoryDialog::HistoryDialog(const GitRepo &repo, QWidget *parent) : QDialog(parent), m_repo(repo)
{
    setWindowTitle(tr("Compare commits"));
    resize(1000, 600);

    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(tr("Filter by subject, author, hash or ref"));
    m_filter->setClearButtonEnabled(true);
    m_allRefs = new QCheckBox(tr("All branches"));
    m_source = new QComboBox;
    m_source->addItem(tr("History"));
    m_source->addItem(tr("Branches and tags"));
    m_source->addItem(tr("Stashes"));
    m_source->setToolTip(tr("What to list: the commit history, the tips of branches and tags, or stashes"));

    m_list = new QTreeWidget;
    m_list->setHeaderLabels({tr("Commit"), tr("Subject"), tr("Author"), tr("Date")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setAlternatingRowColors(true);
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_list->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_list->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

    auto *hint = new QLabel(tr("Select one commit to see its changes, or two to compare them (Ctrl+click)."));
    hint->setEnabled(false);

    m_showCommit = new QPushButton(tr("Show commit's changes"));
    m_compareTwo = new QPushButton(tr("Compare the two"));
    m_vsWorking = new QPushButton(tr("Compare with working tree"));
    auto *working = new QPushButton(tr("Working tree changes"));
    m_branchChanges = new QPushButton(tr("Changes since HEAD"));
    m_branchChanges->setToolTip(tr("What the branch has that HEAD doesn't: the branch against its merge base with HEAD"));
    m_branchChanges->setVisible(false);
    m_more = new QPushButton(tr("Load more"));
    m_vsWorking->setVisible(!repo.isBare());
    working->setVisible(!repo.isBare());
    m_showCommit->setDefault(true);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    buttons->addButton(m_branchChanges, QDialogButtonBox::ActionRole);
    buttons->addButton(m_showCommit, QDialogButtonBox::ActionRole);
    buttons->addButton(m_compareTwo, QDialogButtonBox::ActionRole);
    buttons->addButton(m_vsWorking, QDialogButtonBox::ActionRole);
    buttons->addButton(working, QDialogButtonBox::ActionRole);

    auto *top = new QHBoxLayout;
    top->addWidget(m_source);
    top->addWidget(m_filter, 1);
    top->addWidget(m_allRefs);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_more);
    bottom->addWidget(hint, 1);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(m_list, 1);
    layout->addLayout(bottom);
    layout->addWidget(buttons);

    connect(m_filter, &QLineEdit::textChanged, this, &HistoryDialog::applyFilter);
    connect(m_allRefs, &QCheckBox::toggled, this, [this] { load(false); });
    connect(m_source, &QComboBox::currentIndexChanged, this, [this](int source) {
        m_allRefs->setVisible(source == 0);
        m_branchChanges->setVisible(source == 1 && m_repo.hasHead());
        m_showCommit->setText(source == 2 ? tr("Show stash") : tr("Show commit's changes"));
        load(false);
    });
    connect(m_branchChanges, &QPushButton::clicked, this, [this] { finish(Choice::BranchChanges); });
    connect(m_more, &QPushButton::clicked, this, [this] { load(true); });
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, &HistoryDialog::updateButtons);
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this] { finish(Choice::CommitChanges); });
    connect(m_showCommit, &QPushButton::clicked, this, [this] { finish(Choice::CommitChanges); });
    connect(m_compareTwo, &QPushButton::clicked, this, [this] { finish(Choice::CompareTwo); });
    connect(m_vsWorking, &QPushButton::clicked, this, [this] { finish(Choice::CommitVsWorkingTree); });
    connect(working, &QPushButton::clicked, this, [this] {
        m_choice = Choice::WorkingTree;
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    load(false);
}

void HistoryDialog::load(bool more)
{
    QString error;
    const int skip = more ? int(m_commits.size()) : 0;
    const int source = m_source->currentIndex();
    const QVector<CommitInfo> page = source == 1 ? m_repo.refTips(&error)
                                     : source == 2 ? m_repo.stashes(&error)
                                                   : m_repo.log(kPageSize, skip, m_allRefs->isChecked(), &error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, tr("Compare commits"), tr("git log failed:\n%1").arg(error));
        return;
    }
    if (!more) {
        m_commits.clear();
        m_list->clear();
    }
    for (const CommitInfo &c : page) {
        auto *item = new QTreeWidgetItem(m_list);
        item->setText(0, c.shortOid);
        item->setText(1, c.refs.isEmpty() ? c.subject : QStringLiteral("[%1]  %2").arg(c.refs, c.subject));
        item->setText(2, c.author);
        item->setText(3, c.date);
        item->setToolTip(1, c.subject);
        item->setData(0, kIndexRole, int(m_commits.size()));
        QFont mono = item->font(0);
        mono.setFamily(QStringLiteral("monospace"));
        item->setFont(0, mono);
        m_commits.push_back(c);
    }
    m_more->setEnabled(source == 0 && page.size() == kPageSize);
    applyFilter();
    updateButtons();
}

void HistoryDialog::applyFilter()
{
    const QString f = m_filter->text().trimmed();
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_list->topLevelItem(i);
        const CommitInfo &c = m_commits[item->data(0, kIndexRole).toInt()];
        const bool show = f.isEmpty() || c.subject.contains(f, Qt::CaseInsensitive) || c.author.contains(f, Qt::CaseInsensitive)
                       || c.oid.startsWith(f, Qt::CaseInsensitive) || c.refs.contains(f, Qt::CaseInsensitive);
        item->setHidden(!show);
    }
}

QVector<CommitInfo> HistoryDialog::selected() const
{
    QVector<int> idx;
    for (QTreeWidgetItem *item : m_list->selectedItems())
        idx << item->data(0, kIndexRole).toInt();
    std::sort(idx.begin(), idx.end()); // list order: newest first
    QVector<CommitInfo> out;
    for (int i : idx)
        out << m_commits[i];
    return out;
}

void HistoryDialog::updateButtons()
{
    const qsizetype n = m_list->selectedItems().size();
    m_showCommit->setEnabled(n == 1);
    m_vsWorking->setEnabled(n == 1);
    m_compareTwo->setEnabled(n == 2);
    m_branchChanges->setEnabled(n == 1);
}

void HistoryDialog::finish(Choice c)
{
    const QVector<CommitInfo> sel = selected();
    if ((c == Choice::CompareTwo && sel.size() != 2) || (c != Choice::CompareTwo && sel.size() != 1))
        return;
    m_choice = c;
    if (c == Choice::CompareTwo) {
        m_first = sel[1];  // older (further down the list)
        m_second = sel[0]; // newer
    } else {
        m_first = sel[0];
    }
    accept();
}
