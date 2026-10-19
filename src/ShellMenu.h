#pragma once

#include <QString>

// "Compare with yadiff" in the file manager's right-click menu: a KDE service menu on
// Linux (Dolphin), Explorer verbs under HKEY_CURRENT_USER on Windows. The menu
// compares two selected items directly, or one at a time: "Select as left side", then
// "Compare with left side" on the other.
namespace ShellMenu {

bool install(const QString &executable, QString *summary, QString *error);
bool uninstall(QString *summary, QString *error);
bool isInstalled();

// The left side picked with "Select as left side" (empty when none).
QString selectedLeft();
void setSelectedLeft(const QString &path);

} // namespace ShellMenu
