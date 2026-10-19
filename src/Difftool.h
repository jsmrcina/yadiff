#pragma once

#include <QString>

// Registering yadiff as a `git difftool` in the user's global git config.
namespace Difftool {

// The difftool command line git will run (it substitutes $LOCAL and $REMOTE).
QString command(const QString &executable);

// Sets difftool.yadiff.cmd (and, with makeDefault, diff.tool=yadiff and
// difftool.prompt=false). Returns a human-readable summary of what changed.
bool registerTool(const QString &executable, bool makeDefault, QString *summary, QString *error);
bool unregisterTool(QString *summary, QString *error);
bool isRegistered();

// The same for `git mergetool`: mergetool.yadiff.cmd and trustExitCode (and, with
// makeDefault, merge.tool=yadiff and mergetool.prompt=false).
QString mergeCommand(const QString &executable);
bool registerMergeTool(const QString &executable, bool makeDefault, QString *summary, QString *error);
bool unregisterMergeTool(QString *summary, QString *error);
bool isMergeToolRegistered();

// The executable to put in the command: `yadiff` on PATH when it is this program, else our own path.
QString preferredExecutable();
// An absolute path for places that don't search PATH as a shell does, such as the
// desktop's file manager (whose PATH may not have ~/.local/bin).
QString stableExecutable();

} // namespace Difftool
