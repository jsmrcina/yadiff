#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>
#include <optional>

class QProcess;
class QUrl;

// yadiff's log: one file, shared by every yadiff process (each line names its process),
// so a problem can be traced after the fact. What gets in:
//   info  - startup, every comparison opened, every git/gh/az/secret-tool command
//           (arguments, exit status, time, error output when it failed), every HTTP
//           request (method, URL, status, time), file writes, staging, merges, updates.
//   debug - also the error output of commands that worked, HTTP response headers and
//           error bodies, diff timings and user actions.
// Secrets never get in: process input and output aren't logged, and every line is
// scrubbed of credentials (the ones yadiff used, and anything that looks like a token).
//
// Configured by --log FILE / YADIFF_LOG (a path, "-" for stderr, or "off") and
// --log-level / YADIFF_LOG_LEVEL / Help > Detailed logging (error, warning, info, debug).
Q_DECLARE_LOGGING_CATEGORY(lcApp)    // yadiff.app: startup, windows, user actions
Q_DECLARE_LOGGING_CATEGORY(lcGit)    // yadiff.git: git commands
Q_DECLARE_LOGGING_CATEGORY(lcHttp)   // yadiff.http: HTTP requests
Q_DECLARE_LOGGING_CATEGORY(lcAdo)    // yadiff.ado: Azure DevOps pull requests
Q_DECLARE_LOGGING_CATEGORY(lcGitHub) // yadiff.github: GitHub pull requests (gh)
Q_DECLARE_LOGGING_CATEGORY(lcAuth)   // yadiff.auth: credentials and the keyring (never their values)
Q_DECLARE_LOGGING_CATEGORY(lcDiff)   // yadiff.diff: diff computation
Q_DECLARE_LOGGING_CATEGORY(lcFile)   // yadiff.file: files written, staging, patches, merges
Q_DECLARE_LOGGING_CATEGORY(lcUpdate) // yadiff.update: update checks and installs
Q_DECLARE_LOGGING_CATEGORY(lcProc)   // yadiff.proc: other programs started

namespace Log {

enum class Level { Off, Error, Warning, Info, Debug };

std::optional<Level> parseLevel(const QString &name);
QString levelName(Level level);

// Starts logging. `target` is a file, "-" for stderr, "off", or empty for the default
// file; `level` empty means the saved preference (info unless changed). Until this is
// called (in tests, say) yadiff's categories only print warnings, to stderr.
void init(const QString &target = {}, const QString &level = {});
void shutdown();

Level level();
void setLevel(Level level); // for this process
Level savedLevel();
void saveLevel(Level level); // the preference (Help > Detailed logging)

QString defaultFilePath();
QString filePath(); // the file being written; empty when logging to stderr or off

// Values that must never reach the log (tokens, passwords). Lines are scrubbed of them.
void addSecret(const QString &secret);
// A line with secrets replaced by "***": registered ones, URL passwords, Authorization
// headers, bearer/basic credentials, GitHub and Azure DevOps tokens, JWTs, and
// token-like query parameters.
QString redact(const QString &text);

// A URL for the log: no password, no query values that look secret.
QString url(const QUrl &url);
// A command line for the log, arguments quoted when they need it.
QString commandLine(const QString &program, const QStringList &args);

// Times a child process and logs it when done: its command line, working directory,
// exit status, time and output size, and `errorOutput` (what it wrote to stderr) when it
// failed, or at debug level always. Its input and standard output are never logged.
// `ok` overrides "exited normally with code 0" for commands where other codes are fine.
class ProcessTrace
{
public:
    explicit ProcessTrace(const QLoggingCategory &category);
    // QProcess::waitForFinished that logs "still running after N s" (at 10 s, 30 s, 60 s,
    // then every minute), so a command that hangs shows in the log before it ends.
    bool wait(QProcess &p, int timeoutMs = -1);
    void done(const QProcess &p, qint64 outputBytes = -1, const QByteArray &errorOutput = {},
              std::optional<bool> ok = std::nullopt);

private:
    const QLoggingCategory &m_category;
    QElapsedTimer m_timer;
};

// While one exists, commands this thread runs that succeed are logged at debug level
// only: for routine background work (the auto-reload poll) that would flood the log.
class Quiet
{
public:
    Quiet();
    ~Quiet();
    Quiet(const Quiet &) = delete;
    Quiet &operator=(const Quiet &) = delete;
};

// At exit: every command a ProcessTrace is waiting on is killed (within a quarter of a
// second), and stopping() tells long work to give up, so closing the window never leaves
// yadiff (or a git fetch) running in the background.
void stopChildProcesses();
bool stopping();

// While one exists, the commands this thread waits on are killed once `cancelled` turns
// true: for background work the user can abandon (closing a dialog that is loading).
class CancelScope
{
public:
    explicit CancelScope(std::shared_ptr<std::atomic_bool> cancelled);
    ~CancelScope();
    CancelScope(const CancelScope &) = delete;
    CancelScope &operator=(const CancelScope &) = delete;

private:
    std::shared_ptr<std::atomic_bool> m_previous;
};

// Logs a detached start (a new window, an editor, gitk).
void launched(const QLoggingCategory &category, const QString &program, const QStringList &args, bool ok);

} // namespace Log
