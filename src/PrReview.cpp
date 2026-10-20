#include "PrReview.h"

#include "AdoAuth.h"
#include "AdoPr.h"
#include "GitHubPr.h"
#include "Log.h"

#include <QDir>


namespace {

bool isAdo(const Comparison &cmp)
{
    return cmp.prHost == QLatin1String("ado");
}

GitHubReview::PrId gh(const Comparison &cmp)
{
    return {cmp.prOwner, cmp.prRepo, cmp.prNumber};
}

AdoPr::Ref ado(const Comparison &cmp)
{
    AdoPr::Ref r;
    r.collectionUrl = QUrl(cmp.prCollectionUrl);
    r.project = cmp.prProject;
    r.repo = cmp.prRepo;
    r.id = cmp.prNumber;
    r.webUrl = QUrl(cmp.prWebUrl);
    return r;
}

// AdoAuth remembers what it found, so this is cheap after the first time.
std::optional<AdoAuth::Credential> adoCredential(const Comparison &cmp, QString *error)
{
    auto c = AdoAuth::find(QUrl(cmp.prCollectionUrl), error);
    if (!c && error)
        *error += QStringLiteral(" Run `yadiff --ado-login` to sign in.");
    return c;
}

} // namespace

namespace PrReview {

bool enabled(const Comparison &cmp)
{
    return cmp.prNumber > 0;
}

QString hostName(const Comparison &cmp)
{
    return isAdo(cmp) ? QStringLiteral("Azure DevOps") : QStringLiteral("GitHub");
}

QString webUrl(const Comparison &cmp)
{
    if (isAdo(cmp))
        return cmp.prWebUrl;
    return QStringLiteral("https://github.com/%1/%2/pull/%3").arg(cmp.prOwner, cmp.prRepo).arg(cmp.prNumber);
}

std::optional<QVector<GitHubReview::Comment>> fetch(const Comparison &cmp, QString *error)
{
    if (!isAdo(cmp))
        return GitHubReview::fetch(gh(cmp), error);
    const auto cred = adoCredential(cmp, error);
    return cred ? AdoPr::fetchComments(ado(cmp), *cred, error) : std::nullopt;
}

std::optional<GitHubReview::Comment> post(const Comparison &cmp, const QString &path, const QString &side, int line,
                                          int startLine, const QString &body, QString *error)
{
    if (!isAdo(cmp))
        return GitHubReview::post(gh(cmp), cmp.rev2, path, side, line, startLine, body, error);
    const auto cred = adoCredential(cmp, error);
    return cred ? AdoPr::post(ado(cmp), *cred, path, side, line, startLine, body, error) : std::nullopt;
}

std::optional<GitHubReview::Comment> reply(const Comparison &cmp, qint64 threadId, const QString &body, QString *error)
{
    if (!isAdo(cmp))
        return GitHubReview::reply(gh(cmp), threadId, body, error);
    const auto cred = adoCredential(cmp, error);
    // Reply ids encode their thread (see AdoPr::kReplyBase); roots are the thread id.
    const qint64 thread = threadId >= AdoPr::kReplyBase ? threadId / AdoPr::kReplyBase : threadId;
    return cred ? AdoPr::reply(ado(cmp), *cred, thread, body, error) : std::nullopt;
}

QVector<ThreadStatus> threadStatuses(const Comparison &cmp)
{
    if (!isAdo(cmp))
        return {ThreadStatus::Active, ThreadStatus::Resolved};
    return {ThreadStatus::Active,  ThreadStatus::Pending,  ThreadStatus::Resolved,
            ThreadStatus::WontFix, ThreadStatus::ByDesign, ThreadStatus::Closed};
}

QString threadStatusName(const Comparison &cmp, ThreadStatus status)
{
    if (!isAdo(cmp) && status == ThreadStatus::Active)
        return QStringLiteral("Unresolved");
    return GitHubReview::statusName(status);
}

bool setThreadStatus(const Comparison &cmp, const GitHubReview::Comment &root, ThreadStatus status, QString *error)
{
    if (!isAdo(cmp)) {
        if (status != ThreadStatus::Active && status != ThreadStatus::Resolved) {
            if (error)
                *error = QStringLiteral("GitHub threads are only resolved or unresolved.");
            return false;
        }
        return GitHubReview::setResolved(root.threadNode, status == ThreadStatus::Resolved, error);
    }
    const auto cred = adoCredential(cmp, error);
    const qint64 thread = root.id >= AdoPr::kReplyBase ? root.id / AdoPr::kReplyBase : root.id;
    return cred && AdoPr::setThreadStatus(ado(cmp), *cred, thread, status, error);
}

QVector<Vote> votes(const Comparison &cmp)
{
    if (!isAdo(cmp))
        return {Vote::Approved, Vote::Rejected};
    return {Vote::Approved, Vote::ApprovedWithSuggestions, Vote::WaitingForAuthor, Vote::Rejected, Vote::None};
}

QString voteState(const Comparison &cmp, Vote vote)
{
    switch (vote) {
    case Vote::Approved:
        return QStringLiteral("Approved");
    case Vote::ApprovedWithSuggestions:
        return QStringLiteral("Approved with suggestions");
    case Vote::WaitingForAuthor:
        return QStringLiteral("Waiting for author");
    case Vote::Rejected:
        return isAdo(cmp) ? QStringLiteral("Rejected") : QStringLiteral("Changes requested");
    case Vote::None:
        break;
    }
    return QStringLiteral("No vote");
}

QString voteAction(const Comparison &cmp, Vote vote)
{
    switch (vote) {
    case Vote::Approved:
        return QStringLiteral("Approve");
    case Vote::ApprovedWithSuggestions:
        return QStringLiteral("Approve with suggestions");
    case Vote::WaitingForAuthor:
        return QStringLiteral("Wait for author");
    case Vote::Rejected:
        return isAdo(cmp) ? QStringLiteral("Reject") : QStringLiteral("Request changes");
    case Vote::None:
        break;
    }
    return QStringLiteral("Reset vote");
}

bool voteNeedsMessage(const Comparison &cmp, Vote vote)
{
    return !isAdo(cmp) && vote == Vote::Rejected;
}

int adoVoteValue(Vote vote)
{
    switch (vote) {
    case Vote::Approved:
        return 10;
    case Vote::ApprovedWithSuggestions:
        return 5;
    case Vote::WaitingForAuthor:
        return -5;
    case Vote::Rejected:
        return -10;
    case Vote::None:
        break;
    }
    return 0;
}

Vote adoVote(int value)
{
    return value >= 10 ? Vote::Approved
         : value > 0   ? Vote::ApprovedWithSuggestions
         : value <= -10 ? Vote::Rejected
         : value < 0    ? Vote::WaitingForAuthor
                        : Vote::None;
}

std::optional<Vote> myVote(const Comparison &cmp, QString *error)
{
    if (!isAdo(cmp))
        return GitHubReview::myVote(gh(cmp), error);
    const auto cred = adoCredential(cmp, error);
    if (!cred)
        return std::nullopt;
    const auto value = AdoPr::myVote(ado(cmp), *cred, error);
    return value ? std::optional<Vote>(adoVote(*value)) : std::nullopt;
}

bool setVote(const Comparison &cmp, Vote vote, const QString &message, QString *error)
{
    if (!isAdo(cmp)) {
        if (vote != Vote::Approved && vote != Vote::Rejected) {
            if (error)
                *error = QStringLiteral("GitHub can only approve or request changes.");
            return false;
        }
        return GitHubReview::submitReview(gh(cmp),
                                          vote == Vote::Approved ? GitHubReview::Verdict::Approve
                                                                 : GitHubReview::Verdict::RequestChanges,
                                          message, error);
    }
    const auto cred = adoCredential(cmp, error);
    if (!cred || !AdoPr::setVote(ado(cmp), *cred, adoVoteValue(vote), error))
        return false;
    return message.trimmed().isEmpty() || AdoPr::postMessage(ado(cmp), *cred, message, error);
}

bool submit(const Comparison &cmp, GitHubReview::Verdict verdict, const QString &body, QString *error)
{
    if (!isAdo(cmp))
        return GitHubReview::submitReview(gh(cmp), verdict, body, error);
    const auto cred = adoCredential(cmp, error);
    return cred && AdoPr::submit(ado(cmp), *cred, verdict, body, error);
}

int History::indexOf(int number) const
{
    for (int i = 0; i < versions.size(); ++i)
        if (versions[i].number == number)
            return i;
    return -1;
}

std::optional<History> history(const Comparison &cmp, QString *error)
{
    History h;
    if (!isAdo(cmp)) {
        const auto commits = GitHubReview::commits(gh(cmp), error);
        if (!commits)
            return std::nullopt;
        for (const GitHubReview::PrCommit &c : *commits)
            h.versions.push_back({int(h.versions.size()) + 1, c.sha, {}, c.date, c.subject});
        // Not knowing what was reviewed isn't fatal: there's just no "since your review".
        QString reviewError;
        h.lastReviewedCommit = GitHubReview::myLastReviewedCommit(gh(cmp), &reviewError).value_or(QString());
        for (const Version &v : std::as_const(h.versions))
            if (v.headCommit == h.lastReviewedCommit)
                h.lastReviewed = v.number;
        return h;
    }
    const auto cred = adoCredential(cmp, error);
    if (!cred)
        return std::nullopt;
    const auto its = AdoPr::iterations(ado(cmp), *cred, error);
    if (!its)
        return std::nullopt;
    for (const AdoPr::Iteration &it : *its)
        h.versions.push_back({it.id, it.headCommit, it.baseCommit, it.created, it.description});
    QString voteError;
    h.lastReviewed = AdoPr::lastVotedIteration(ado(cmp), *cred, *its, &voteError).value_or(0);
    return h;
}

QString versionName(const Comparison &cmp, const Version &v)
{
    if (isAdo(cmp))
        return QStringLiteral("Push %1").arg(v.number);
    return QStringLiteral("commit %1").arg(v.headCommit.left(7));
}

std::optional<Comparison> versionComparison(const Comparison &whole, const QString &wholeRoot, const History &h, int from,
                                            int to, QString *repoRoot, QString *error,
                                            const std::function<void(const QString &)> &progress)
{
    const int toIndex = h.indexOf(to);
    const int fromIndex = from > 0 ? h.indexOf(from) : -1;
    if (toIndex < 0 || (from > 0 && fromIndex < 0) || (from == -1 && h.lastReviewedCommit.isEmpty())) {
        if (error)
            *error = QStringLiteral("No such version of the pull request.");
        return std::nullopt;
    }
    const Version &toV = h.versions[toIndex];
    const bool latest = toIndex == h.versions.size() - 1;
    Comparison c = whole;
    c.prFromVersion = from;
    c.prToVersion = latest ? 0 : to;
    *repoRoot = wholeRoot;
    if (from == 0 && latest)
        return c; // the whole PR

    const QString fromCommit = from == -1 ? h.lastReviewedCommit : from > 0 ? h.versions[fromIndex].headCommit : QString();
    const QString fromName = from == -1 ? QStringLiteral("your last review (%1)").arg(fromCommit.left(7))
                           : from > 0  ? versionName(whole, h.versions[fromIndex]) + QStringLiteral(" (%1)").arg(fromCommit.left(10))
                                       : whole.rev1Name;
    const QString toName = latest ? whole.rev2Name : versionName(whole, toV) + QStringLiteral(" (%1)").arg(toV.headCommit.left(10));
    qCInfo(lcApp).noquote() << "PR" << whole.prNumber << ": comparing" << fromName << "with" << toName;

    if (whole.mode == Comparison::Mode::Paths) {
        // Downloaded through the API (Azure DevOps without a clone): download this pair.
        const auto cred = adoCredential(whole, error);
        if (!cred)
            return std::nullopt;
        AdoPr::Iteration a, b;
        if (from > 0)
            a = {h.versions[fromIndex].number, h.versions[fromIndex].headCommit, h.versions[fromIndex].baseCommit, {}, {}};
        b = {toV.number, toV.headCommit, toV.baseCommit, {}, {}};
        const auto r = AdoPr::downloadVersions(ado(whole), *cred, a, b, error, progress);
        if (!r)
            return std::nullopt;
        *repoRoot = r->repoPath;
        c.leftPath = r->baseDir;
        c.rightPath = r->headDir;
        c.rev1Name = from > 0 ? fromName : QStringLiteral("merge base (%1)").arg(b.baseCommit.left(10));
        c.rev2Name = toName;
        return c;
    }

    // Commits in a repository: fetch the ones it doesn't have (pushed over since, say).
    const QString toCommit = latest ? whole.rev2 : toV.headCommit;
    if (progress)
        progress(QStringLiteral("Fetching the pull request's earlier commits…"));
    bool fetched = false;
    if (isAdo(whole)) {
        const auto cred = adoCredential(whole, error);
        fetched = cred && AdoPr::fetchCommits(ado(whole), *cred, wholeRoot, {fromCommit, toCommit}, error);
    } else {
        fetched = GitHubPr::fetchCommits({whole.prOwner, whole.prRepo, whole.prNumber}, wholeRoot, {fromCommit, toCommit}, error);
    }
    if (!fetched)
        return std::nullopt;
    if (from != 0) {
        c.rev1 = fromCommit;
        c.rev1Name = fromName;
    }
    c.rev2 = toCommit;
    c.rev2Name = toName;
    return c;
}

std::optional<PrOverview::Overview> overview(const Comparison &cmp, QString *error)
{
    if (!isAdo(cmp)) {
        const auto json = GitHubReview::overviewJson(gh(cmp), error);
        return json ? std::optional<PrOverview::Overview>(PrOverview::parseGitHubPullRequest(*json)) : std::nullopt;
    }
    const auto cred = adoCredential(cmp, error);
    return cred ? AdoPr::overview(ado(cmp), *cred, error) : std::nullopt;
}

} // namespace PrReview
