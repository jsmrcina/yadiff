#pragma once

#include <QByteArray>
#include <QString>

// Files yadiff writes so that pasted text and the clipboard can be compared like files:
// each comparison gets its own folder under the cache (~/.cache/yadiff/scratch), which
// later runs clean up after a week.
namespace Scratch {

// A new, empty folder for one comparison (empty on failure).
QString newFolder();
// Writes `data` to `name` in `folder`; returns the file's path (empty on failure).
QString write(const QString &folder, const QString &name, const QByteArray &data);
// Removes scratch folders older than `days`.
void cleanUp(int days = 7);

} // namespace Scratch
