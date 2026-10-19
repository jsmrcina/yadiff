#include "Http.h"

#include "Log.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

namespace Http {

namespace {

// Response headers that help trace a failed request (Azure DevOps and GitHub request ids,
// why authentication failed, where a redirect went).
const QList<QByteArray> kTraceHeaders{"x-vss-e2eid", "activityid", "x-tfs-processid", "x-tfs-serviceerror",
                                      "x-vss-userid", "x-github-request-id", "www-authenticate", "location",
                                      "x-ratelimit-remaining", "content-type"};

// The credential in an Authorization header, so the log never shows it.
void registerSecret(const QByteArray &value)
{
    Log::addSecret(QString::fromUtf8(value));
    const qsizetype space = value.indexOf(' ');
    if (space < 0)
        return;
    const QByteArray credential = value.mid(space + 1).trimmed();
    Log::addSecret(QString::fromUtf8(credential));
    if (value.left(space).compare("Basic", Qt::CaseInsensitive) == 0) {
        const QByteArray decoded = QByteArray::fromBase64(credential);
        Log::addSecret(QString::fromUtf8(decoded));
        Log::addSecret(QString::fromUtf8(decoded.mid(decoded.indexOf(':') + 1)));
    }
}

} // namespace

Response request(const QByteArray &method, const QUrl &url, const Headers &headers, const QByteArray &body, int timeoutMs)
{
    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QByteArrayLiteral("yadiff"));
    for (const auto &h : headers) {
        if (h.first.compare("Authorization", Qt::CaseInsensitive) == 0)
            registerSecret(h.second);
        req.setRawHeader(h.first, h.second);
    }
    if (!body.isEmpty() && !req.hasRawHeader("Content-Type"))
        req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));

    QElapsedTimer elapsed;
    elapsed.start();
    QNetworkReply *reply = nam.sendCustomRequest(req, method, body);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();

    Response r;
    if (!reply->isFinished()) {
        reply->abort();
        r.error = QStringLiteral("Timed out after %1 s talking to %2").arg(timeoutMs / 1000).arg(url.host());
    } else {
        r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        r.body = reply->readAll();
        // HTTP error statuses still carry a response; only report transport failures here.
        if (reply->error() != QNetworkReply::NoError && r.status == 0)
            r.error = reply->errorString();
    }
    if (lcHttp().isInfoEnabled()) {
        QStringList sent;
        for (const auto &h : headers)
            sent << QString::fromLatin1(h.first);
        QString line = QStringLiteral("%1 %2 -> ").arg(QString::fromLatin1(method), Log::url(url));
        line += r.error.isEmpty() ? QStringLiteral("HTTP %1").arg(r.status) : QStringLiteral("FAILED: %1").arg(r.error);
        line += QStringLiteral(", %1 ms, %2 bytes").arg(elapsed.elapsed()).arg(r.body.size());
        if (!body.isEmpty())
            line += QStringLiteral(" (sent %1 bytes)").arg(body.size());
        if (reply->url() != url)
            line += QStringLiteral("\nredirected to %1").arg(Log::url(reply->url()));
        const bool failed = !r.ok() || r.status == 203; // Azure DevOps: 203 is its sign-in page
        if (failed || lcHttp().isDebugEnabled()) {
            line += QStringLiteral("\nrequest headers: %1").arg(sent.join(QStringLiteral(", ")));
            for (const auto &[name, value] : reply->rawHeaderPairs())
                if (lcHttp().isDebugEnabled() ? name.compare("set-cookie", Qt::CaseInsensitive) != 0
                                              : kTraceHeaders.contains(name.toLower()))
                    line += QStringLiteral("\n%1: %2").arg(QString::fromLatin1(name), QString::fromUtf8(value));
        }
        if (failed && !r.body.isEmpty())
            line += QStringLiteral("\nbody: %1").arg(QString::fromUtf8(r.body.left(4000)).trimmed());
        qCInfo(lcHttp).noquote() << line;
    }
    reply->deleteLater();
    return r;
}

bool safeForCredentials(const QUrl &url)
{
    if (url.scheme() == QLatin1String("https"))
        return true;
    if (url.scheme() != QLatin1String("http"))
        return false;
    const QString host = url.host();
    return host == QLatin1String("localhost") || QHostAddress(host).isLoopback();
}

} // namespace Http
