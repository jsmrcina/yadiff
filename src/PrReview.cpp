#include "PrReview.h"

#include "AdoAuth.h"
#include "AdoPr.h"

#include <QHash>
#include <QMutex>

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

// Looking a credential up can run programs (keyring, az), so do it once per organisation.
std::optional<AdoAuth::Credential> adoCredential(const Comparison &cmp, QString *error)
{
    static QMutex mutex;
    static QHash<QString, AdoAuth::Credential> cache;
    QMutexLocker lock(&mutex);
    if (const auto it = cache.constFind(cmp.prCollectionUrl); it != cache.constEnd())
        return *it;
    auto c = AdoAuth::find(QUrl(cmp.prCollectionUrl), error);
    if (c)
        cache.insert(cmp.prCollectionUrl, *c);
    else if (error)
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

} // namespace PrReview
