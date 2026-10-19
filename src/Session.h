#pragma once

#include "GitRepo.h"

#include <QString>
#include <QStringList>

#include <optional>

// A saved comparison (File > Save session): the arguments that open it again, and the
// view to restore. Stored as JSON in a .yadiff file.
struct Session {
    QStringList arguments;  // as ComparisonArgs::fromArguments() reads them
    QString title;          // the window title, when the user changed it
    QString currentFile;    // repository-relative path of the file shown
    int sideBySide = -1;    // 1 side by side, 0 inline, -1 as it is
    bool hasOptions = false;
    DiffOptions options;

    static std::optional<Session> read(const QString &path, QString *error);
    bool write(const QString &path, QString *error) const;
};
