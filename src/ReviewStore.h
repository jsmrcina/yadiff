#pragma once

#include <QByteArray>
#include <QSet>
#include <QSettings>
#include <QString>

#include <memory>

// Remembers which files were marked as reviewed, per repository. A mark is tied to
// a fingerprint of both file versions, so it disappears once either side changes.
class ReviewStore
{
public:
    // Uses ~/.config/yadiff/reviewed.ini (or `iniPath` when given, for tests).
    explicit ReviewStore(const QString &repoRoot, const QString &iniPath = {});

    bool hasAny(const QString &path) const;

    // For background work: hashes of every path with a stored mark, and the hash of one
    // path (thread-safe, no settings access). hasAny(p) == keys().contains(pathKey(p)).
    QSet<QString> keys() const;
    static QString pathKey(const QString &path);
    bool isReviewed(const QString &path, const QByteArray &fingerprint) const;
    void setReviewed(const QString &path, const QByteArray &fingerprint, bool reviewed);

private:
    QString key(const QString &path) const;
    std::unique_ptr<QSettings> m_settings;
    QString m_group;
};
