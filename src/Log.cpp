#include "Log.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <exception>

#ifdef Q_OS_WIN
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif
#if defined(__GLIBC__)
#include <execinfo.h>
#endif

// Quiet (warnings only) until Log::init raises them, so tests don't print every command.
Q_LOGGING_CATEGORY(lcApp, "yadiff.app", QtWarningMsg)
Q_LOGGING_CATEGORY(lcGit, "yadiff.git", QtWarningMsg)
Q_LOGGING_CATEGORY(lcHttp, "yadiff.http", QtWarningMsg)
Q_LOGGING_CATEGORY(lcAdo, "yadiff.ado", QtWarningMsg)
Q_LOGGING_CATEGORY(lcGitHub, "yadiff.github", QtWarningMsg)
Q_LOGGING_CATEGORY(lcAuth, "yadiff.auth", QtWarningMsg)
Q_LOGGING_CATEGORY(lcDiff, "yadiff.diff", QtWarningMsg)
Q_LOGGING_CATEGORY(lcFile, "yadiff.file", QtWarningMsg)
Q_LOGGING_CATEGORY(lcUpdate, "yadiff.update", QtWarningMsg)
Q_LOGGING_CATEGORY(lcProc, "yadiff.proc", QtWarningMsg)

namespace Log {

namespace {

constexpr qint64 kRotateBytes = 5 * 1024 * 1024;
constexpr int kMaxErrorOutput = 4000;
constexpr int kMaxCommandLine = 2000;

struct State {
    QMutex mutex; // the file
    FILE *file = nullptr;
    bool toStderr = false;
    QString path;
    Level level = Level::Warning;
    QtMessageHandler previous = nullptr;

    QMutex secretsMutex;
    QStringList secrets; // longest first
};

// Never destroyed: Qt may still log while static objects are torn down.
State &state()
{
    static State *s = new State;
    return *s;
}

std::atomic<int> g_crashFd{-1};
thread_local int t_quiet = 0;

const char *typeName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return "DEBUG";
    case QtInfoMsg:
        return "INFO ";
    case QtWarningMsg:
        return "WARN ";
    case QtCriticalMsg:
        return "ERROR";
    case QtFatalMsg:
        return "FATAL";
    }
    return "?    ";
}

QString threadName()
{
    if (QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread())
        return QStringLiteral("main");
    static std::atomic<int> next{1};
    thread_local const int id = next++;
    return QStringLiteral("w%1").arg(id);
}

QByteArray format(QtMsgType type, const char *category, const QString &text)
{
    QString head = QStringLiteral("%1 %2 %3 %4 %5 ")
                       .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")))
                       .arg(QCoreApplication::applicationPid(), 6)
                       .arg(threadName(), -4)
                       .arg(QLatin1String(typeName(type)))
                       .arg(QLatin1String(category), -12);
    // Continuation lines (stderr of a command, say) are indented under the first.
    QString body = text;
    body.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    body.replace(u'\n', QStringLiteral("\n    | "));
    return (head + body + u'\n').toUtf8();
}

void handler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    State &s = state();
    const QString text = redact(message);
    const QByteArray line = format(type, context.category ? context.category : "default", text);
    {
        QMutexLocker lock(&s.mutex);
        if (s.file) {
            std::fwrite(line.constData(), 1, size_t(line.size()), s.file);
            std::fflush(s.file);
        }
    }
    // Warnings and worse still reach the terminal, as they would without a log.
    const bool serious = type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg;
    if (s.toStderr) {
        if (!s.file) {
            std::fwrite(line.constData(), 1, size_t(line.size()), stderr);
            std::fflush(stderr);
        }
    } else if (serious && s.previous) {
        s.previous(type, context, text);
    }
}

void applyRules(Level level)
{
    QString rules;
    auto set = [&](const char *kind, bool on) {
        rules += QStringLiteral("yadiff.*.%1=%2\n").arg(QLatin1String(kind), on ? QStringLiteral("true") : QStringLiteral("false"));
    };
    set("debug", level >= Level::Debug);
    set("info", level >= Level::Info);
    set("warning", level >= Level::Warning || level == Level::Off);
    set("critical", true);
    QLoggingCategory::setFilterRules(rules);
}

void writeRaw(int fd, const char *text)
{
    if (fd < 0)
        return;
    size_t n = 0;
    while (text[n])
        ++n;
#ifdef Q_OS_WIN
    _write(fd, text, unsigned(n));
#else
    [[maybe_unused]] const ssize_t written = ::write(fd, text, n);
#endif
}

#ifndef Q_OS_WIN
void crashed(int sig)
{
    // Only async-signal-safe calls here.
    const char *what = sig == SIGSEGV   ? "*** yadiff crashed: SIGSEGV (invalid memory access)\n"
                       : sig == SIGBUS  ? "*** yadiff crashed: SIGBUS\n"
                       : sig == SIGILL  ? "*** yadiff crashed: SIGILL\n"
                       : sig == SIGFPE  ? "*** yadiff crashed: SIGFPE\n"
                       : sig == SIGABRT ? "*** yadiff aborted: SIGABRT\n"
                                        : "*** yadiff crashed\n";
    const int fd = g_crashFd.load();
    writeRaw(fd, what);
    writeRaw(2, what);
#if defined(__GLIBC__)
    void *frames[64];
    const int n = backtrace(frames, 64);
    if (fd >= 0)
        backtrace_symbols_fd(frames, n, fd);
#endif
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
#else
LONG WINAPI crashed(EXCEPTION_POINTERS *info)
{
    char text[160];
    std::snprintf(text, sizeof text, "*** yadiff crashed: exception 0x%08lx at %p\n",
                  info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0ul,
                  info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr);
    writeRaw(g_crashFd.load(), text);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

void installCrashHandlers()
{
#ifndef Q_OS_WIN
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT})
        std::signal(sig, crashed);
#else
    SetUnhandledExceptionFilter(crashed);
#endif
    static std::terminate_handler previous = std::set_terminate([] {
        QString what = QStringLiteral("unknown exception");
        if (const std::exception_ptr e = std::current_exception()) {
            try {
                std::rethrow_exception(e);
            } catch (const std::exception &ex) {
                what = QString::fromLocal8Bit(ex.what());
            } catch (...) {
            }
        } else {
            what = QStringLiteral("no active exception");
        }
        qCCritical(lcApp).noquote() << "std::terminate:" << what;
        if (previous)
            previous();
        std::abort();
    });
}

const QList<QPair<QRegularExpression, QString>> &patterns()
{
    using RE = QRegularExpression;
    static const QList<QPair<RE, QString>> list{
        // https://user:password@host
        {RE(QStringLiteral(R"((\b[a-zA-Z][a-zA-Z0-9+.-]*://[^/\s:@]*):[^/\s@]+@)")), QStringLiteral("\\1:***@")},
        {RE(QStringLiteral(R"(\b(Bearer|Basic)\s+[A-Za-z0-9_.~+/=-]{8,})")), QStringLiteral("\\1 ***")},
        {RE(QStringLiteral(R"((Authorization\s*[:=]\s*token\s+)\S+)"), RE::CaseInsensitiveOption), QStringLiteral("\\1***")},
        // GitHub tokens
        {RE(QStringLiteral(R"(\b(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}))")), QStringLiteral("***")},
        // JWTs (Microsoft Entra access tokens)
        {RE(QStringLiteral(R"(\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]*)")), QStringLiteral("***")},
        // Azure DevOps personal access tokens, old (52 base32) and new (84, "AZDO" signature)
        {RE(QStringLiteral(R"(\b[a-z2-7]{52}\b)")), QStringLiteral("***")},
        {RE(QStringLiteral(R"(\b[A-Za-z0-9]{76}AZDO[A-Za-z0-9]{4}\b)")), QStringLiteral("***")},
        {RE(QStringLiteral(R"(([?&](?:access_token|token|code|sig|signature|password|pat|client_secret|key)=)[^&\s#]+)"),
            RE::CaseInsensitiveOption),
         QStringLiteral("\\1***")},
        {RE(QStringLiteral(R"((\bpassword=)\S+)"), RE::CaseInsensitiveOption), QStringLiteral("\\1***")},
        {RE(QStringLiteral(R"(\b((?:YADIFF_ADO_TOKEN|YADIFF_ADO_PAT|AZURE_DEVOPS_EXT_PAT|SYSTEM_ACCESSTOKEN|GH_TOKEN|GITHUB_TOKEN)=)\S+)")),
         QStringLiteral("\\1***")},
    };
    return list;
}

bool needsQuotes(const QString &arg)
{
    if (arg.isEmpty())
        return true;
    for (QChar c : arg)
        if (c.isSpace() || c == u'\'' || c == u'"' || c == u'\\' || c == u'$' || c == u'*' || c == u'?' || c == u';'
            || c == u'&' || c == u'|' || c == u'<' || c == u'>' || c == u'(' || c == u')')
            return true;
    return false;
}

} // namespace

std::optional<Level> parseLevel(const QString &name)
{
    const QString n = name.trimmed().toLower();
    if (n == QLatin1String("off") || n == QLatin1String("none"))
        return Level::Off;
    if (n == QLatin1String("error"))
        return Level::Error;
    if (n == QLatin1String("warning") || n == QLatin1String("warn"))
        return Level::Warning;
    if (n == QLatin1String("info") || n == QLatin1String("normal"))
        return Level::Info;
    if (n == QLatin1String("debug") || n == QLatin1String("detailed") || n == QLatin1String("verbose"))
        return Level::Debug;
    return std::nullopt;
}

QString levelName(Level level)
{
    switch (level) {
    case Level::Off:
        return QStringLiteral("off");
    case Level::Error:
        return QStringLiteral("error");
    case Level::Warning:
        return QStringLiteral("warning");
    case Level::Info:
        return QStringLiteral("info");
    case Level::Debug:
        return QStringLiteral("debug");
    }
    return {};
}

QString defaultFilePath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation))
        .filePath(QStringLiteral("yadiff/yadiff.log"));
}

void init(const QString &target, const QString &levelArg)
{
    State &s = state();
    QString where = target.isEmpty() ? qEnvironmentVariable("YADIFF_LOG") : target;
    QString levelText = levelArg.isEmpty() ? qEnvironmentVariable("YADIFF_LOG_LEVEL") : levelArg;
    Level lvl = parseLevel(levelText).value_or(savedLevel());
    if (where.compare(QLatin1String("off"), Qt::CaseInsensitive) == 0)
        lvl = Level::Off;

    {
        QMutexLocker lock(&s.mutex);
        if (s.file) {
            std::fclose(s.file);
            s.file = nullptr;
        }
        s.toStderr = false;
        s.path.clear();
        if (lvl != Level::Off) {
            if (where == QLatin1String("-")) {
                s.toStderr = true;
            } else {
                const QString path = QFileInfo(where.isEmpty() ? defaultFilePath() : where).absoluteFilePath();
                QDir().mkpath(QFileInfo(path).absolutePath());
                // One old file is kept; the check is only at startup, so a long session can run over.
                if (QFileInfo(path).size() > kRotateBytes) {
                    QFile::remove(path + QStringLiteral(".1"));
                    QFile::rename(path, path + QStringLiteral(".1"));
                }
#ifdef Q_OS_WIN
                s.file = _wfopen(reinterpret_cast<const wchar_t *>(path.utf16()), L"ab");
#else
                s.file = std::fopen(QFile::encodeName(path).constData(), "ab");
#endif
                if (s.file)
                    s.path = path;
                else
                    std::fprintf(stderr, "yadiff: cannot write the log file %s\n", qPrintable(path));
            }
        }
        s.level = lvl;
#ifdef Q_OS_WIN
        g_crashFd = s.file ? _fileno(s.file) : -1;
#else
        g_crashFd = s.file ? fileno(s.file) : -1;
#endif
    }
    applyRules(lvl);
    if (!s.previous) {
        s.previous = qInstallMessageHandler(handler);
        installCrashHandlers();
    }
}

void shutdown()
{
    State &s = state();
    QMutexLocker lock(&s.mutex);
    g_crashFd = -1;
    if (s.file) {
        std::fclose(s.file);
        s.file = nullptr;
    }
}

Level level()
{
    return state().level;
}

void setLevel(Level lvl)
{
    State &s = state();
    if (lvl == Level::Off || s.level == Level::Off) {
        // Opening or closing the file: start over with the default target.
        init({}, levelName(lvl));
        return;
    }
    s.level = lvl;
    applyRules(lvl);
}

Level savedLevel()
{
    return parseLevel(QSettings().value(QStringLiteral("log/level")).toString()).value_or(Level::Info);
}

void saveLevel(Level lvl)
{
    QSettings().setValue(QStringLiteral("log/level"), levelName(lvl));
}

QString filePath()
{
    State &s = state();
    QMutexLocker lock(&s.mutex);
    return s.path;
}

void addSecret(const QString &secret)
{
    const QString v = secret.trimmed();
    if (v.size() < 6)
        return; // too short to replace without mangling ordinary text
    State &s = state();
    QMutexLocker lock(&s.secretsMutex);
    if (s.secrets.contains(v))
        return;
    s.secrets << v;
    std::sort(s.secrets.begin(), s.secrets.end(), [](const QString &a, const QString &b) { return a.size() > b.size(); });
}

QString redact(const QString &text)
{
    QString out = text;
    {
        State &s = state();
        QMutexLocker lock(&s.secretsMutex);
        for (const QString &secret : std::as_const(s.secrets))
            out.replace(secret, QStringLiteral("***"));
    }
    for (const auto &[re, replacement] : patterns())
        out.replace(re, replacement);
    return out;
}

QString url(const QUrl &u)
{
    return redact(u.toString(QUrl::RemovePassword));
}

QString commandLine(const QString &program, const QStringList &args)
{
    QStringList parts{needsQuotes(program) ? u'\'' + program + u'\'' : program};
    for (const QString &a : args) {
        if (needsQuotes(a)) {
            QString q = a;
            q.replace(u'\'', QStringLiteral("'\\''"));
            parts << u'\'' + q + u'\'';
        } else {
            parts << a;
        }
    }
    QString line = parts.join(u' ');
    if (line.size() > kMaxCommandLine)
        line = line.left(kMaxCommandLine) + QStringLiteral(" ... (%1 arguments)").arg(args.size());
    return line;
}

Quiet::Quiet()
{
    ++t_quiet;
}

Quiet::~Quiet()
{
    --t_quiet;
}

ProcessTrace::ProcessTrace(const QLoggingCategory &category)
    : m_category(category)
{
    m_timer.start();
}

void ProcessTrace::done(const QProcess &p, qint64 outputBytes, const QByteArray &errorOutput, std::optional<bool> ok)
{
    if (!m_category.isInfoEnabled())
        return;
    const qint64 ms = m_timer.elapsed();
    const bool exited = p.error() != QProcess::FailedToStart && p.state() == QProcess::NotRunning
                     && p.exitStatus() == QProcess::NormalExit;
    if (t_quiet > 0 && exited && ok.value_or(p.exitCode() == 0) && !m_category.isDebugEnabled())
        return;
    QString outcome;
    bool good;
    if (p.error() == QProcess::FailedToStart) {
        outcome = QStringLiteral("could not start (%1)").arg(p.errorString());
        good = false;
    } else if (p.state() != QProcess::NotRunning) {
        outcome = QStringLiteral("still running (timed out)");
        good = false;
    } else if (p.exitStatus() == QProcess::CrashExit) {
        outcome = QStringLiteral("crashed or was killed (timeout?)");
        good = false;
    } else {
        outcome = QStringLiteral("exit %1").arg(p.exitCode());
        good = ok.value_or(p.exitCode() == 0);
    }
    if (ok.has_value() && !*ok)
        good = false;
    QString line = commandLine(p.program(), p.arguments());
    if (!p.workingDirectory().isEmpty())
        line += QStringLiteral("  [in %1]").arg(QDir::toNativeSeparators(p.workingDirectory()));
    line += QStringLiteral(" -> %1%2, %3 ms").arg(good ? QString() : QStringLiteral("FAILED: "), outcome).arg(ms);
    if (outputBytes >= 0)
        line += QStringLiteral(", %1 bytes out").arg(outputBytes);
    const QString err = QString::fromUtf8(errorOutput).trimmed();
    if (!err.isEmpty() && (!good || m_category.isDebugEnabled()))
        line += QStringLiteral("\nstderr: ") + (err.size() > kMaxErrorOutput ? err.right(kMaxErrorOutput) : err);
    QMessageLogger logger(nullptr, 0, nullptr, m_category.categoryName());
    if (t_quiet > 0 && good)
        logger.debug().noquote() << line;
    else
        logger.info().noquote() << line;
}

void launched(const QLoggingCategory &category, const QString &program, const QStringList &args, bool ok)
{
    if (!category.isInfoEnabled())
        return;
    QMessageLogger(nullptr, 0, nullptr, category.categoryName()).info().noquote()
        << (ok ? QStringLiteral("started ") : QStringLiteral("FAILED to start ")) + commandLine(program, args);
}

} // namespace Log
