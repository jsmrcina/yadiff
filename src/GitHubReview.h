#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <optional>

// Pull request review comments through the `gh` CLI (which handles authentication).
namespace GitHubReview {

struct PrId {
    QString owner, repo;
    int number = 0;
    bool isValid() const { return number > 0 && !owner.isEmpty() && !repo.isEmpty(); }
};

struct Comment {
    qint64 id = 0;
    qint64 inReplyTo = 0;  // 0 for the first comment of a thread
    QString path;
    QString side;          // "RIGHT" (new side) or "LEFT" (old side)
    int line = 0;          // last line the comment covers; 0 when outdated
    int startLine = 0;     // first line of a multi-line comment, 0 when single-line
    QString author, body, createdAt, url;
    bool outdated() const { return line <= 0; }
};

using Thread = QVector<Comment>; // first comment, then replies in order

// "2026-10-12T14:03:21Z" -> "2026-10-12 16:03" in local time (falls back to the input).
QString displayTime(const QString &iso);

enum class Verdict { Comment, Approve, RequestChanges };

// Parses GitHub's JSON (a list, or gh --slurp's list of pages).
QVector<Comment> parseComments(const QByteArray &json);
// Groups replies under their first comment, threads in creation order.
QVector<Thread> threads(const QVector<Comment> &comments);

// Request bodies (exposed for tests).
QByteArray commentPayload(const QString &commitId, const QString &path, const QString &side, int line, int startLine,
                          const QString &body);
QByteArray reviewPayload(Verdict verdict, const QString &body);

std::optional<QVector<Comment>> fetch(const PrId &pr, QString *error);
std::optional<Comment> post(const PrId &pr, const QString &commitId, const QString &path, const QString &side, int line,
                            int startLine, const QString &body, QString *error);
std::optional<Comment> reply(const PrId &pr, qint64 threadId, const QString &body, QString *error);
bool submitReview(const PrId &pr, Verdict verdict, const QString &body, QString *error);

} // namespace GitHubReview
