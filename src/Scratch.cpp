#include "Scratch.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QStandardPaths>

namespace Scratch {

namespace {
QString root()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/yadiff/scratch");
}
} // namespace

QString newFolder()
{
    const QString name = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-"))
                       + QString::number(QRandomGenerator::global()->bounded(1u << 30), 36);
    const QString path = root() + u'/' + name;
    return QDir().mkpath(path) ? path : QString();
}

QString write(const QString &folder, const QString &name, const QByteArray &data)
{
    const QString path = folder + u'/' + name;
    QFile f(path);
    if (folder.isEmpty() || !f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(data) != data.size())
        return {};
    return path;
}

void cleanUp(int days)
{
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-days);
    for (const QFileInfo &fi : QDir(root()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (fi.lastModified() < cutoff)
            QDir(fi.absoluteFilePath()).removeRecursively();
}

} // namespace Scratch
