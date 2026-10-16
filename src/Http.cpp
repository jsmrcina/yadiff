#include "Http.h"

#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

namespace Http {

Response request(const QByteArray &method, const QUrl &url, const Headers &headers, const QByteArray &body, int timeoutMs)
{
    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QByteArrayLiteral("yadiff"));
    for (const auto &h : headers)
        req.setRawHeader(h.first, h.second);
    if (!body.isEmpty() && !req.hasRawHeader("Content-Type"))
        req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));

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
