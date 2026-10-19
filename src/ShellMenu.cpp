#include "ShellMenu.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace ShellMenu {

namespace {

#ifdef Q_OS_WIN
const char *const kVerbs[] = {"yadiff.selectleft", "yadiff.compareleft"};
const char *const kRoots[] = {"*", "Directory"};
#else
QString serviceMenuDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/kio/servicemenus");
}
QString pairFile() { return serviceMenuDir() + QStringLiteral("/yadiff-compare.desktop"); }
QString singleFile() { return serviceMenuDir() + QStringLiteral("/yadiff-left.desktop"); }

QString quoted(const QString &exe)
{
    // Desktop entry Exec quoting: double quotes around arguments with spaces.
    if (!exe.contains(u' '))
        return exe;
    QString e = exe;
    e.replace(u'\\', QLatin1String("\\\\")).replace(u'"', QLatin1String("\\\""));
    return u'"' + e + u'"';
}

bool writeExecutable(const QString &path, const QString &text, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(text.toUtf8()) < 0) {
        if (error)
            *error = QStringLiteral("%1: %2").arg(path, f.errorString());
        return false;
    }
    f.close();
    // Plasma 6 only runs service menus in the user's folder when they are executable.
    f.setPermissions(f.permissions() | QFile::ExeOwner | QFile::ExeUser);
    return true;
}
#endif

} // namespace

bool install(const QString &executable, QString *summary, QString *error)
{
#ifdef Q_OS_WIN
    QSettings classes(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"), QSettings::NativeFormat);
    const QString exe = QDir::toNativeSeparators(executable);
    for (const char *root : kRoots) {
        const QString base = QLatin1String(root) + QStringLiteral("/shell/");
        classes.setValue(base + QLatin1String(kVerbs[0]) + QStringLiteral("/MUIVerb"), QStringLiteral("Select as left side for yadiff"));
        classes.setValue(base + QLatin1String(kVerbs[0]) + QStringLiteral("/Icon"), exe);
        classes.setValue(base + QLatin1String(kVerbs[0]) + QStringLiteral("/command/Default"),
                         QStringLiteral("\"%1\" --select-left \"%2\"").arg(exe, QStringLiteral("%1")));
        classes.setValue(base + QLatin1String(kVerbs[1]) + QStringLiteral("/MUIVerb"), QStringLiteral("Compare with left side (yadiff)"));
        classes.setValue(base + QLatin1String(kVerbs[1]) + QStringLiteral("/Icon"), exe);
        classes.setValue(base + QLatin1String(kVerbs[1]) + QStringLiteral("/command/Default"),
                         QStringLiteral("\"%1\" --compare-to-left \"%2\"").arg(exe, QStringLiteral("%1")));
    }
    classes.sync();
    if (classes.status() != QSettings::NoError) {
        if (error)
            *error = QStringLiteral("Could not write to the registry");
        return false;
    }
    if (summary)
        *summary = QStringLiteral("Explorer's right-click menu now has \"Select as left side for yadiff\" and "
                                  "\"Compare with left side (yadiff)\" for files and folders. On Windows 11 they are "
                                  "under \"Show more options\".");
    return true;
#else
    if (!QDir().mkpath(serviceMenuDir())) {
        if (error)
            *error = QStringLiteral("Could not create %1").arg(serviceMenuDir());
        return false;
    }
    const QString exe = quoted(executable);
    const QString pair = QStringLiteral(
                             "[Desktop Entry]\nType=Service\nMimeType=all/all;\nX-KDE-MinNumberOfUrls=2\n"
                             "X-KDE-MaxNumberOfUrls=2\nActions=yadiffCompare;\n\n"
                             "[Desktop Action yadiffCompare]\nName=Compare with yadiff\nIcon=yadiff\nExec=%1 --no-index %F\n")
                             .arg(exe);
    const QString single = QStringLiteral(
                               "[Desktop Entry]\nType=Service\nMimeType=all/all;\nX-KDE-MaxNumberOfUrls=1\n"
                               "Actions=yadiffSelectLeft;yadiffCompareLeft;\n\n"
                               "[Desktop Action yadiffSelectLeft]\nName=Select as left side for yadiff\nIcon=yadiff\n"
                               "Exec=%1 --select-left %f\n\n"
                               "[Desktop Action yadiffCompareLeft]\nName=Compare with left side (yadiff)\nIcon=yadiff\n"
                               "Exec=%1 --compare-to-left %f\n")
                               .arg(exe);
    if (!writeExecutable(pairFile(), pair, error) || !writeExecutable(singleFile(), single, error))
        return false;
    if (summary)
        *summary = QStringLiteral("Dolphin's right-click menu now has \"Compare with yadiff\" for two selected items, and "
                                  "\"Select as left side for yadiff\" / \"Compare with left side (yadiff)\" for one.\n\n"
                                  "Files: %1, %2")
                       .arg(pairFile(), singleFile());
    return true;
#endif
}

bool uninstall(QString *summary, QString *error)
{
#ifdef Q_OS_WIN
    QSettings classes(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"), QSettings::NativeFormat);
    for (const char *root : kRoots)
        for (const char *verb : kVerbs)
            classes.remove(QLatin1String(root) + QStringLiteral("/shell/") + QLatin1String(verb));
    classes.sync();
    Q_UNUSED(error);
#else
    for (const QString &p : {pairFile(), singleFile()})
        if (QFile::exists(p) && !QFile::remove(p)) {
            if (error)
                *error = QStringLiteral("Could not remove %1").arg(p);
            return false;
        }
#endif
    if (summary)
        *summary = QStringLiteral("Removed yadiff from the file manager's right-click menu.");
    return true;
}

bool isInstalled()
{
#ifdef Q_OS_WIN
    QSettings classes(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"), QSettings::NativeFormat);
    return classes.contains(QStringLiteral("*/shell/yadiff.selectleft/command/Default"));
#else
    return QFile::exists(pairFile()) || QFile::exists(singleFile());
#endif
}

QString selectedLeft()
{
    return QSettings().value(QStringLiteral("shell/left")).toString();
}

void setSelectedLeft(const QString &path)
{
    QSettings().setValue(QStringLiteral("shell/left"), path);
}

} // namespace ShellMenu
