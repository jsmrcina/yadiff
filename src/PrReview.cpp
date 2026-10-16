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

bool submit(const Comparison &cmp, GitHubReview::Verdict verdict, const QString &body, QString *error)
{
    if (!isAdo(cmp))
        return GitHubReview::submitReview(gh(cmp), verdict, body, error);
    const auto cred = adoCredential(cmp, error);
    return cred && AdoPr::submit(ado(cmp), *cred, verdict, body, error);
}

} // namespace PrReview
