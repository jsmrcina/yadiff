#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QUrl>

// Small synchronous HTTP client (runs its own event loop, so it works on worker threads).
namespace Http {

using Headers = QList<QPair<QByteArray, QByteArray>>;

struct Response {
    int status = 0;
    QByteArray body;
    QString error; // transport error (DNS, TLS, timeout); empty when a response arrived
    bool ok() const { return error.isEmpty() && status >= 200 && status < 300; }
};

Response request(const QByteArray &method, const QUrl &url, const Headers &headers, const QByteArray &body = {},
                 int timeoutMs = 30000);

// Credentials may only travel over HTTPS, or plain HTTP to this machine (tests).
bool safeForCredentials(const QUrl &url);

} // namespace Http
