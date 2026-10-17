#pragma once

#include <QString>

#include <functional>
#include <optional>

// Updates from GitHub releases (Help > Check for update now / Automatically install
// updates). Releases are read with the gh CLI, which handles authentication for the
// private repository; without gh, GitHub's REST API is used (with GH_TOKEN or
// GITHUB_TOKEN when set).
//
// Installing depends on how yadiff was installed:
//   - Linux, an install.sh snapshot (~/.local/opt/yadiff-<version>, linked from
//     ~/.local/bin/yadiff): the release's source is built and installed as a new
//     snapshot, then the link is moved to it. Releases have no Linux binaries, and a
//     build against this machine's own Qt keeps working across Qt updates.
//   - Windows, an unzipped folder: the release zip replaces the files in place. Files
//     in use are renamed to *.old first and removed on the next start.
// Anything else (a package manager, a build tree) is left alone.
namespace Updater {

struct Release {
    QString tag;   // "v0.7.0"
    QString name;
    QString notes; // Markdown
    QString url;   // release page
};

// The repository releases come from ("owner/name"); YADIFF_UPDATE_REPO overrides it.
QString repository();

// The latest release. Long-running: call it off the UI thread.
std::optional<Release> latest(QString *error);

// Whether release `tag` ("v1.2.3") is newer than `current` ("1.2.2").
bool isNewer(const QString &tag, const QString &current);

enum class InstallKind { LinuxSnapshot, WindowsFolder, Unsupported };
// How the program at `exePath` was installed (`home` is the user's home directory).
InstallKind installKind(const QString &exePath, const QString &home);
InstallKind currentInstallKind();
// Why an Unsupported install can't update itself, for the user.
QString unsupportedReason();

// Downloads and installs `release`; `progress` gets short status lines. Long-running:
// call it off the UI thread. Only one install runs at a time across processes.
bool install(const Release &release, const std::function<void(const QString &)> &progress, QString *error);

// The program to start after an update (the updated install).
QString restartProgram();

// Windows: removes files left as *.old by an update of the folder `dir`.
void cleanUpOldFiles(const QString &dir);

// Exposed for tests: copies every file under `from` into `to`, renaming each file it
// replaces to <name>.old first (so files in use can be replaced on Windows).
bool replaceFiles(const QString &from, const QString &to, QString *error);

} // namespace Updater
