#include "PrOverviewPanel.h"

#include "Settings.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocumentFragment>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

using GitHubReview::Vote;
using PrOverview::CheckState;

QColor voteColor(Vote vote)
{
    switch (vote) {
    case Vote::Approved:
        return QColor(0x2e, 0xa0, 0x43);
    case Vote::ApprovedWithSuggestions:
        return QColor(0x7c, 0xc0, 0x4f);
    case Vote::WaitingForAuthor:
        return QColor(0xd9, 0x8e, 0x04);
    case Vote::Rejected:
        return QColor(0xd1, 0x3b, 0x3b);
    case Vote::None:
        break;
    }
    return QColor(0x9a, 0x9a, 0x9a);
}

QString voteText(Vote vote, bool github)
{
    switch (vote) {
    case Vote::Approved:
        return QStringLiteral("approved");
    case Vote::ApprovedWithSuggestions:
        return QStringLiteral("approved with suggestions");
    case Vote::WaitingForAuthor:
        return QStringLiteral("waiting for author");
    case Vote::Rejected:
        return github ? QStringLiteral("changes requested") : QStringLiteral("rejected");
    case Vote::None:
        break;
    }
    return QStringLiteral("no vote yet");
}

QColor checkColor(CheckState s)
{
    switch (s) {
    case CheckState::Passed:
        return QColor(0x2e, 0xa0, 0x43);
    case CheckState::Failed:
        return QColor(0xd1, 0x3b, 0x3b);
    case CheckState::Running:
    case CheckState::Queued:
        return QColor(0xd9, 0x8e, 0x04);
    default:
        return QColor(0x9a, 0x9a, 0x9a);
    }
}

QString checkMark(CheckState s)
{
    switch (s) {
    case CheckState::Passed:
        return QStringLiteral("✔");
    case CheckState::Failed:
        return QStringLiteral("✖");
    case CheckState::Running:
        return QStringLiteral("◔");
    case CheckState::Queued:
        return QStringLiteral("○");
    default:
        return QStringLiteral("–");
    }
}

QString esc(const QString &s)
{
    return s.toHtmlEscaped();
}

QString link(const QString &url, const QString &html)
{
    return url.isEmpty() ? html : QStringLiteral("<a href=\"%1\">%2</a>").arg(esc(url), html);
}

// Markdown (and the HTML bots put in it) as rich text, without hidden HTML comments.
void insertMarkdown(QTextCursor &c, QString text, int indent)
{
    static const QRegularExpression htmlComment(QStringLiteral("<!--[\\s\\S]*?-->"));
    text.remove(htmlComment);
    if (text.trimmed().isEmpty())
        return;
    QTextBlockFormat f;
    f.setLeftMargin(indent);
    c.insertBlock(f);
    const int start = c.position();
    c.insertFragment(QTextDocumentFragment::fromMarkdown(text, QTextDocument::MarkdownDialectGitHub));
    for (QTextBlock b = c.document()->findBlock(start); b.isValid() && b.position() <= c.position(); b = b.next()) {
        QTextCursor bc(b);
        QTextBlockFormat bf = b.blockFormat();
        bf.setLeftMargin(bf.leftMargin() + indent);
        bc.setBlockFormat(bf);
    }
}

void heading(QTextCursor &c, const QString &text, const QString &extra = {})
{
    QTextBlockFormat f;
    f.setTopMargin(10);
    f.setBottomMargin(2);
    c.insertBlock(f);
    c.insertHtml(QStringLiteral("<b>%1</b>%2").arg(esc(text), extra));
}

void line(QTextCursor &c, const QString &html, int indent = 8)
{
    QTextBlockFormat f;
    f.setLeftMargin(indent);
    c.insertBlock(f);
    c.insertHtml(html);
}

} // namespace

PrOverviewPanel::PrOverviewPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    auto *header = new QHBoxLayout;
    header->setContentsMargins(2, 0, 2, 0);
    m_fold = new QToolButton;
    m_fold->setAutoRaise(true);
    m_fold->setArrowType(Qt::DownArrow);
    m_fold->setToolTip(tr("Fold the overview away"));
    connect(m_fold, &QToolButton::clicked, this, [this] { setFolded(!m_folded); });
    m_title = new QLabel;
    m_title->setTextFormat(Qt::RichText);
    m_title->setTextInteractionFlags(Qt::NoTextInteraction);
    m_title->setMinimumWidth(10);
    m_reload = new QToolButton;
    m_reload->setAutoRaise(true);
    m_reload->setText(QStringLiteral("↻"));
    m_reload->setToolTip(tr("Read the pull request again"));
    connect(m_reload, &QToolButton::clicked, this, &PrOverviewPanel::reloadRequested);
    header->addWidget(m_fold);
    header->addWidget(m_title, 1);
    header->addWidget(m_reload);
    v->addLayout(header);
    m_browser = new QTextBrowser;
    m_browser->setOpenLinks(false);
    m_browser->setFrameShape(QFrame::NoFrame);
    connect(m_browser, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        const QString s = url.toString();
        if (s.startsWith(QLatin1String("yadiff:reply:")))
            emit replyRequested(s.section(u':', 2).toLongLong());
        else if (s.startsWith(QLatin1String("yadiff:resolve:")))
            emit resolveRequested(s.section(u':', 2).toLongLong(), true);
        else if (s.startsWith(QLatin1String("yadiff:reactivate:")))
            emit resolveRequested(s.section(u':', 2).toLongLong(), false);
        else if (s == QLatin1String("yadiff:comment"))
            emit commentRequested();
        else
            QDesktopServices::openUrl(url);
    });
    v->addWidget(m_browser, 1);
    clear();
}

void PrOverviewPanel::clear()
{
    m_overview.reset();
    m_message.clear();
    render();
}

void PrOverviewPanel::setLoading(const QString &hostName)
{
    m_host = hostName;
    if (!m_overview)
        m_message = tr("Reading the pull request from %1…").arg(hostName);
    render();
}

void PrOverviewPanel::setError(const QString &error)
{
    m_message = tr("Could not read the pull request: %1").arg(error);
    render();
}

void PrOverviewPanel::setOverview(const PrOverview::Overview &o, const QString &hostName, bool canReply)
{
    m_overview = o;
    m_host = hostName;
    m_canReply = canReply;
    m_message.clear();
    render();
}

void PrOverviewPanel::setFolded(bool folded)
{
    if (m_folded == folded)
        return;
    m_folded = folded;
    m_browser->setVisible(!folded);
    m_fold->setArrowType(folded ? Qt::RightArrow : Qt::DownArrow);
    m_fold->setToolTip(folded ? tr("Show the overview") : tr("Fold the overview away"));
    emit foldedChanged(folded);
}

QString PrOverviewPanel::summary() const
{
    return QTextDocumentFragment::fromHtml(m_title->text()).toPlainText();
}

void PrOverviewPanel::render()
{
    m_browser->clear();
    const QString muted = palette().color(QPalette::PlaceholderText).name();
    if (!m_overview) {
        m_title->setText(QStringLiteral("<b>%1</b>").arg(tr("Pull request")));
        m_browser->setHtml(QStringLiteral("<p style=\"color:%1\">%2</p>").arg(muted, esc(m_message)));
        return;
    }
    const PrOverview::Overview &o = *m_overview;
    const bool github = m_host == QLatin1String("GitHub");

    // The header: how it stands at a glance (stays when folded).
    int approved = 0, rejected = 0, failed = 0, running = 0;
    for (const auto &r : o.reviewers) {
        approved += r.vote == Vote::Approved || r.vote == Vote::ApprovedWithSuggestions;
        rejected += r.vote == Vote::Rejected || r.vote == Vote::WaitingForAuthor;
    }
    for (const auto &ch : o.checks) {
        failed += ch.state == CheckState::Failed;
        running += ch.state == CheckState::Running || ch.state == CheckState::Queued;
    }
    QStringList bits;
    bits << tr("%1 approved").arg(approved);
    if (rejected)
        bits << QStringLiteral("<span style=\"color:%1\">%2</span>").arg(voteColor(Vote::WaitingForAuthor).name(), tr("%1 waiting").arg(rejected));
    if (!o.checks.isEmpty()) {
        if (failed)
            bits << QStringLiteral("<span style=\"color:%1\">%2</span>").arg(checkColor(CheckState::Failed).name(), failed == 1 ? tr("1 check failed") : tr("%1 checks failed").arg(failed));
        else if (running)
            bits << QStringLiteral("<span style=\"color:%1\">%2</span>").arg(checkColor(CheckState::Running).name(), running == 1 ? tr("1 check running") : tr("%1 checks running").arg(running));
        else
            bits << QStringLiteral("<span style=\"color:%1\">%2</span>").arg(checkColor(CheckState::Passed).name(), tr("checks passed"));
    }
    if (o.draft)
        bits.prepend(tr("draft"));
    if (!o.status.isEmpty() && o.status != QLatin1String("active") && o.status != QLatin1String("open"))
        bits.prepend(o.status);
    m_title->setText(QStringLiteral("<b>%1</b> <span style=\"color:%2\">· %3</span>").arg(tr("Overview"), muted, bits.join(QStringLiteral(" · "))));

    QTextCursor c(m_browser->document());
    QTextBlockFormat first;
    first.setBottomMargin(2);
    c.setBlockFormat(first);
    c.insertHtml(QStringLiteral("<span style=\"font-size:large\"><b>%1</b></span>").arg(esc(o.title)));
    QStringList meta;
    if (!o.author.isEmpty())
        meta << tr("by %1").arg(esc(o.author));
    if (!o.created.isEmpty())
        meta << esc(GitHubReview::displayTime(o.created));
    if (!o.sourceBranch.isEmpty())
        meta << QStringLiteral("%1 → %2").arg(esc(o.sourceBranch), esc(o.targetBranch));
    line(c, QStringLiteral("<span style=\"color:%1\">%2</span>").arg(muted, meta.join(QStringLiteral(" · "))), 0);
    if (!o.description.trimmed().isEmpty())
        insertMarkdown(c, o.description, 0);
    else
        line(c, QStringLiteral("<i style=\"color:%1\">%2</i>").arg(muted, tr("No description.")), 0);

    if (!o.reviewers.isEmpty()) {
        heading(c, tr("Reviewers"));
        for (const auto &r : o.reviewers) {
            QString extra;
            if (r.required)
                extra += QStringLiteral(" <span style=\"color:%1\">(%2)</span>").arg(muted, tr("required"));
            if (r.group)
                extra += QStringLiteral(" <span style=\"color:%1\">(%2)</span>").arg(muted, tr("group"));
            if (r.declined)
                extra += QStringLiteral(" <span style=\"color:%1\">(%2)</span>").arg(muted, tr("declined"));
            line(c, QStringLiteral("<span style=\"color:%1\">●</span> %2%3 · <span style=\"color:%4\">%5</span>")
                        .arg(voteColor(r.vote).name(), esc(r.name), extra,
                             r.vote == Vote::None ? muted : voteColor(r.vote).name(), esc(voteText(r.vote, github))));
        }
    }

    if (!o.checks.isEmpty()) {
        heading(c, github ? tr("Checks") : tr("Policies and checks"));
        for (const auto &ch : o.checks) {
            QString text = link(ch.url, esc(ch.name));
            if (ch.required)
                text += QStringLiteral(" <span style=\"color:%1\">(%2)</span>").arg(muted, tr("required"));
            QString state = PrOverview::checkStateName(ch.state).toLower();
            if (!ch.detail.isEmpty())
                state += QStringLiteral(" · ") + ch.detail;
            line(c, QStringLiteral("<span style=\"color:%1\">%2</span> %3 <span style=\"color:%4\">· %5</span>")
                        .arg(checkColor(ch.state).name(), checkMark(ch.state), text, muted, esc(state)));
        }
    }

    if (!o.workItems.isEmpty()) {
        heading(c, github ? tr("Closes") : tr("Work items"));
        for (const auto &w : o.workItems) {
            QString text = link(w.url, esc(QStringLiteral("%1 %2").arg(w.type, w.id).trimmed()));
            if (!w.title.isEmpty())
                text += QStringLiteral(": ") + esc(w.title);
            if (!w.state.isEmpty())
                text += QStringLiteral(" <span style=\"color:%1\">· %2</span>").arg(muted, esc(w.state));
            line(c, text);
        }
    }

    heading(c, tr("Comments on the pull request"),
            QStringLiteral(" &nbsp;<a href=\"yadiff:comment\">%1</a>").arg(tr("add a comment…")));
    if (o.discussion.isEmpty())
        line(c, QStringLiteral("<i style=\"color:%1\">%2</i>").arg(muted, tr("None.")));
    int shown = 0;
    for (const GitHubReview::Thread &t : o.discussion) {
        if (++shown > 30) {
            line(c, QStringLiteral("<i style=\"color:%1\">%2</i>").arg(muted, tr("%1 more on %2").arg(o.discussion.size() - 30).arg(m_host)));
            break;
        }
        for (int i = 0; i < t.size(); ++i) {
            const GitHubReview::Comment &cm = t[i];
            QString head = (i == 0 ? QString() : QStringLiteral("↳ ")) + link(cm.url, QStringLiteral("<b>%1</b>").arg(esc(cm.author)));
            if (!cm.createdAt.isEmpty())
                head += QStringLiteral(" <span style=\"color:%1\">· %2</span>").arg(muted, esc(GitHubReview::displayTime(cm.createdAt)));
            if (i == 0 && cm.status != GitHubReview::ThreadStatus::None)
                head += QStringLiteral(" · <b style=\"color:%1\">%2</b>")
                            .arg(GitHubReview::isOpen(cm.status) ? palette().color(QPalette::Link).name() : muted,
                                 esc(GitHubReview::statusName(cm.status)));
            if (i == 0 && m_canReply) {
                head += QStringLiteral(" &nbsp;<a href=\"yadiff:reply:%1\">%2</a>").arg(cm.id).arg(tr("reply"));
                head += GitHubReview::isOpen(cm.status)
                            ? QStringLiteral(" &nbsp;<a href=\"yadiff:resolve:%1\">%2</a>").arg(cm.id).arg(tr("resolve"))
                            : QStringLiteral(" &nbsp;<a href=\"yadiff:reactivate:%1\">%2</a>").arg(cm.id).arg(tr("reactivate"));
            }
            QTextBlockFormat f;
            f.setTopMargin(i == 0 ? 8 : 4);
            f.setLeftMargin(i == 0 ? 8 : 28);
            c.insertBlock(f);
            c.insertHtml(head);
            insertMarkdown(c, cm.body, i == 0 ? 20 : 40);
        }
    }

    if (!o.problems.isEmpty()) {
        heading(c, tr("Not shown"));
        for (const QString &p : o.problems)
            line(c, QStringLiteral("<span style=\"color:%1\">%2</span>").arg(muted, esc(p)));
    }

    // Code in the diff's font, as in review threads.
    const QFont mono = Settings::font();
    for (QTextBlock b = m_browser->document()->begin(); b.isValid(); b = b.next())
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.charFormat().fontFixedPitch() && !b.blockFormat().hasProperty(QTextFormat::BlockCodeFence))
                continue;
            QTextCursor fc(m_browser->document());
            fc.setPosition(frag.position());
            fc.setPosition(frag.position() + frag.length(), QTextCursor::KeepAnchor);
            QTextCharFormat cf;
            cf.setFontFamilies(mono.families());
            fc.mergeCharFormat(cf);
        }
    m_browser->moveCursor(QTextCursor::Start);
}
