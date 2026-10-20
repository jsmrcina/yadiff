#include "PrListDialog.h"

#include "AdoPr.h"
#include "GitHubPr.h"
#include "Log.h"

#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace {

constexpr int kHostRole = Qt::UserRole + 1;
constexpr int kLinkRole = Qt::UserRole + 2;

QString voteText(const PrInbox::Item &it)
{
    using V = GitHubReview::Vote;
    switch (it.myVote) {
    case V::Approved:
        return QStringLiteral("Approved");
    case V::ApprovedWithSuggestions:
        return QStringLiteral("Approved with suggestions");
    case V::WaitingForAuthor:
        return QStringLiteral("Waiting for author");
    case V::Rejected:
        return it.host == QLatin1String("github") ? QStringLiteral("Changes requested") : QStringLiteral("Rejected");
    case V::None:
        break;
    }
    return it.reviewed ? QStringLiteral("Commented") : QStringLiteral("No vote");
}

QColor voteColor(GitHubReview::Vote vote)
{
    using V = GitHubReview::Vote;
    return vote == V::Approved                  ? QColor(0x2e, 0xa0, 0x43)
         : vote == V::ApprovedWithSuggestions   ? QColor(0x7c, 0xc0, 0x4f)
         : vote == V::WaitingForAuthor          ? QColor(0xd9, 0x8e, 0x04)
         : vote == V::Rejected                  ? QColor(0xd1, 0x3b, 0x3b)
                                                : QColor(0x9a, 0x9a, 0x9a);
}

QIcon dot(const QColor &c)
{
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(QRectF(3, 3, 10, 10));
    return QIcon(pm);
}

} // namespace

PrListDialog::PrListDialog(QWidget *parent, bool fetch)
    : QDialog(parent)
{
    setWindowTitle(tr("Open pull request"));
    resize(980, 520);
    auto *v = new QVBoxLayout(this);

    auto *top = new QHBoxLayout;
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(tr("Filter by title, author, repository or number"));
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this, &PrListDialog::applyFilter);
    auto *refresh = new QPushButton(tr("&Refresh"));
    connect(refresh, &QPushButton::clicked, this, &PrListDialog::load);
    auto *add = new QPushButton(tr("Add Azure DevOps &project…"));
    add->setToolTip(tr("Azure DevOps projects are added when you open one of their pull requests; add others here"));
    connect(add, &QPushButton::clicked, this, &PrListDialog::addProject);
    top->addWidget(m_filter, 1);
    top->addWidget(refresh);
    top->addWidget(add);
    v->addLayout(top);

    m_tree = new QTreeWidget;
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setHeaderLabels({tr("Pull request"), tr("Repository"), tr("Author"), tr("Your vote"), tr("Created"), tr("")});
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int c = 1; c < 6; ++c)
        m_tree->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    connect(m_tree, &QTreeWidget::itemActivated, this, [this] { openSelected(); });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this,
            [this] { m_open->setEnabled(!m_tree->selectedItems().isEmpty() || !m_paste->text().trimmed().isEmpty()); });
    v->addWidget(m_tree, 1);
    m_status = new QLabel;
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(m_status);

    auto *bottom = new QHBoxLayout;
    m_paste = new QLineEdit;
    m_paste->setPlaceholderText(tr("…or paste a pull request link (Azure DevOps or GitHub)"));
    connect(m_paste, &QLineEdit::textChanged, this,
            [this](const QString &t) { m_open->setEnabled(!t.trimmed().isEmpty() || !m_tree->selectedItems().isEmpty()); });
    connect(m_paste, &QLineEdit::returnPressed, this, &PrListDialog::openLink);
    bottom->addWidget(m_paste, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_open = buttons->addButton(tr("&Open"), QDialogButtonBox::AcceptRole);
    m_open->setEnabled(false);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_open, &QPushButton::clicked, this, [this] {
        if (!m_paste->text().trimmed().isEmpty() && m_tree->selectedItems().isEmpty())
            openLink();
        else
            openSelected();
    });
    bottom->addWidget(buttons);
    v->addLayout(bottom);
    if (fetch)
        load();
}

PrListDialog::~PrListDialog()
{
    if (m_cancel)
        *m_cancel = true; // stops gh if it's still running
}

void PrListDialog::load()
{
    const QVector<PrInbox::AdoSource> ado = PrInbox::sources();
    m_loading = true;
    m_tree->clear();
    auto *wait = new QTreeWidgetItem(m_tree, {tr("Loading the pull requests waiting for you…")});
    wait->setFlags(Qt::NoItemFlags);
    QStringList where;
    for (const auto &s : ado)
        where << AdoAuth::organizationUrl(s.collection).toString(QUrl::RemoveScheme).mid(2) + u'/' + s.project;
    where << QStringLiteral("GitHub");
    m_status->setText(tr("Looking in %1…").arg(where.join(QStringLiteral(", "))));
    qCInfo(lcApp).noquote() << "listing pull requests to review in" << where.join(QStringLiteral(", "));
    if (m_cancel)
        *m_cancel = true;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    const quint64 gen = ++m_gen;
    auto *watcher = new QFutureWatcher<PrInbox::Listing>(this); // goes with the dialog: no callback after it closes
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, gen] {
        watcher->deleteLater();
        if (gen == m_gen)
            showListing(watcher->result());
    });
    watcher->setFuture(QtConcurrent::run([ado, cancel = m_cancel] {
        const Log::CancelScope scope(cancel);
        return PrInbox::fetchAll(ado, true);
    }));
}

void PrListDialog::showListing(const PrInbox::Listing &listing)
{
    m_loading = false;
    m_tree->clear();
    int fresh = 0, newPush = 0;
    for (const PrInbox::Item &it : listing.items) {
        auto *row = new QTreeWidgetItem(m_tree);
        row->setText(0, QStringLiteral("%1%2 %3").arg(it.host == QLatin1String("ado") ? u'!' : u'#').arg(it.number).arg(it.title));
        row->setToolTip(0, it.link);
        row->setText(1, it.repo);
        row->setText(2, it.author);
        row->setText(3, voteText(it));
        row->setIcon(3, dot(voteColor(it.myVote)));
        row->setText(4, GitHubReview::displayTime(it.created));
        // Since the user last looked: never opened in yadiff, or pushed to since.
        const QString seen = PrInbox::lastSeen(it.link);
        QStringList notes;
        if (it.draft)
            notes << tr("draft");
        if (it.required)
            notes << tr("required");
        if (!it.via.isEmpty())
            notes << tr("via %1").arg(it.via);
        QFont f = row->font(0);
        if (seen.isEmpty()) {
            notes.prepend(tr("new"));
            f.setBold(true);
            ++fresh;
        } else if (!it.headCommit.isEmpty() && seen != it.headCommit) {
            notes.prepend(tr("new push"));
            f.setBold(true);
            ++newPush;
        }
        row->setFont(0, f);
        row->setText(5, notes.join(QStringLiteral(", ")));
        row->setData(0, kHostRole, it.host);
        row->setData(0, kLinkRole, it.link);
    }
    QString text = listing.items.isEmpty() ? tr("No pull requests are waiting for you.")
                                           : tr("%1 pull requests: %2 you haven't opened, %3 with new pushes since you looked.")
                                                 .arg(listing.items.size())
                                                 .arg(fresh)
                                                 .arg(newPush);
    if (PrInbox::sources().isEmpty())
        text += u' ' + tr("No Azure DevOps projects yet: open one of their pull requests, or add a project.");
    for (const QString &p : listing.problems)
        text += QStringLiteral("\n") + tr("Could not read %1").arg(p);
    m_status->setText(text);
    applyFilter();
}

void PrListDialog::applyFilter()
{
    const QStringList terms = m_filter->text().split(u' ', Qt::SkipEmptyParts);
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *row = m_tree->topLevelItem(i);
        bool show = true;
        for (const QString &t : terms) {
            bool any = false;
            for (int c = 0; c < m_tree->columnCount(); ++c)
                any |= row->text(c).contains(t, Qt::CaseInsensitive);
            show = show && any;
        }
        row->setHidden(!show);
    }
}

void PrListDialog::addProject()
{
    bool ok = false;
    const QString url = QInputDialog::getText(this, tr("Add Azure DevOps project"),
                                              tr("A link to the project (or to anything in it):\n"
                                                 "https://dev.azure.com/ORG/PROJECT"),
                                              QLineEdit::Normal, QString(), &ok);
    if (!ok || url.trimmed().isEmpty())
        return;
    const auto s = PrInbox::parseSource(url);
    if (!s) {
        QMessageBox::warning(this, tr("Add Azure DevOps project"), tr("That isn't a link to an Azure DevOps project."));
        return;
    }
    PrInbox::addSource(*s);
    load();
}

void PrListDialog::openSelected()
{
    const QList<QTreeWidgetItem *> sel = m_tree->selectedItems();
    if (sel.isEmpty() || !sel.first()->data(0, kLinkRole).isValid())
        return;
    m_host = sel.first()->data(0, kHostRole).toString();
    m_link = sel.first()->data(0, kLinkRole).toString();
    accept();
}

void PrListDialog::openLink()
{
    const QString text = m_paste->text().trimmed();
    if (AdoPr::parse(text))
        m_host = QStringLiteral("ado");
    else if (GitHubPr::parse(text))
        m_host = QStringLiteral("github");
    else {
        QMessageBox::warning(this, windowTitle(), tr("That isn't a link to an Azure DevOps or GitHub pull request."));
        return;
    }
    m_link = text;
    accept();
}
