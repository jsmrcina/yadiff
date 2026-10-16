#include "ReviewStore.h"

#include <QCryptographicHash>
#include <QStringList>

namespace {
constexpr int kMaxFingerprintsPerFile = 10;

QString hashHex(const QString &s)
{
    return QString::fromLatin1(QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha1).toHex());
}
} // namespace

ReviewStore::ReviewStore(const QString &repoRoot, const QString &iniPath)
    : m_settings(iniPath.isEmpty()
                     ? std::make_unique<QSettings>(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("yadiff"),
                                                   QStringLiteral("reviewed"))
                     : std::make_unique<QSettings>(iniPath, QSettings::IniFormat)),
      m_group(hashHex(repoRoot).left(16))
{
}

QString ReviewStore::pathKey(const QString &path)
{
    // Paths can contain characters QSettings treats specially, so key by hash.
    return hashHex(path);
}

QString ReviewStore::key(const QString &path) const
{
    return m_group + u'/' + pathKey(path);
}

QSet<QString> ReviewStore::keys() const
{
    m_settings->beginGroup(m_group);
    const QStringList k = m_settings->childKeys();
    m_settings->endGroup();
    return QSet<QString>(k.begin(), k.end());
}

bool ReviewStore::hasAny(const QString &path) const
{
    return m_settings->contains(key(path));
}

bool ReviewStore::isReviewed(const QString &path, const QByteArray &fingerprint) const
{
    return m_settings->value(key(path)).toStringList().contains(QString::fromLatin1(fingerprint));
}

void ReviewStore::setReviewed(const QString &path, const QByteArray &fingerprint, bool reviewed)
{
    QStringList list = m_settings->value(key(path)).toStringList();
    const QString fp = QString::fromLatin1(fingerprint);
    list.removeAll(fp);
    if (reviewed)
        list.append(fp);
    while (list.size() > kMaxFingerprintsPerFile)
        list.removeFirst();
    if (list.isEmpty())
        m_settings->remove(key(path));
    else
        m_settings->setValue(key(path), list);
    m_settings->sync();
}
