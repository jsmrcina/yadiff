#include "GitRepo.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QProcess>
#include <QProcessEnvironment>

namespace {

QProcessEnvironment gitEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0")); // read-only tool: don't take index.lock
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));              // stable, untranslated messages
    env.insert(QStringLiteral("GIT_PAGER"), QStringLiteral("cat"));
    return env;
}

bool runGit(const QString &workDir, const QStringList &args, QByteArray *out, QString *error,
            const QByteArray &input = {})
{
    QProcess p;
    p.setWorkingDirectory(workDir);
    p.setProcessEnvironment(gitEnvironment());
    QStringList full{QStringLiteral("-c"), QStringLiteral("core.quotepath=false"), QStringLiteral("-c"),
                     QStringLiteral("color.ui=false")};
    full += args;
    p.start(QStringLiteral("git"), full);
    if (!p.waitForStarted()) {
        if (error)
            *error = QStringLiteral("Could not start git. Is it installed and on PATH?");
        return false;
    }
    if (!input.isEmpty())
        p.write(input);
    p.closeWriteChannel();
    p.waitForFinished(-1);
    if (out)
        *out = p.readAllStandardOutput();
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        if (error)
            *error = QString::fromUtf8(p.readAllStandardError()).trimmed();
        return false;
    }
    return true;
}

QVector<FileChange> parseNameStatus(const QByteArray &out, ChangeGroup group)
{
    QVector<FileChange> result;
    const QList<QByteArray> parts = out.split('\0');
    qsizetype i = 0;
    while (i < parts.size()) {
        const QByteArray st = parts[i++];
        if (st.isEmpty())
            continue;
        FileChange fc;
        fc.status = QChar(st.at(0));
        fc.group = group;
        if (fc.status == u'R' || fc.status == u'C') {
            if (i + 1 >= parts.size())
                break;
            fc.oldPath = QString::fromUtf8(parts[i++]);
            fc.path = QString::fromUtf8(parts[i++]);
        } else {
            if (i >= parts.size())
                break;
            fc.path = QString::fromUtf8(parts[i++]);
            fc.oldPath = fc.path;
        }
        result.push_back(fc);
    }
    return result;
}

QStringList diffBaseArgs()
{
    return {QStringLiteral("diff"),        QStringLiteral("--no-color"),   QStringLiteral("--no-ext-diff"),
            QStringLiteral("--histogram"), QStringLiteral("-M"),           QStringLiteral("--unified=100000000")};
}

QStringList pathArgs(const FileChange &fc)
{
    QStringList a{QStringLiteral("--")};
    if (fc.oldPath != fc.path)
        a << fc.oldPath;
    a << fc.path;
    return a;
}

} // namespace

QString statusDescription(QChar status)
{
    switch (status.unicode()) {
    case 'M': return QStringLiteral("Modified");
    case 'A': return QStringLiteral("Added");
    case 'D': return QStringLiteral("Deleted");
    case 'R': return QStringLiteral("Renamed");
    case 'C': return QStringLiteral("Copied");
    case 'T': return QStringLiteral("Type changed");
    case 'U': return QStringLiteral("Unmerged");
    case '?': return QStringLiteral("Untracked");
    default: return QStringLiteral("Changed");
    }
}

std::optional<GitRepo> GitRepo::open(const QString &dir, QString *error)
{
    QByteArray out;
    if (!runGit(dir, {QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")}, &out, error))
        return std::nullopt;
    const QString root = QString::fromUtf8(out).trimmed();
    if (root.isEmpty()) {
        if (error)
            *error = QStringLiteral("Not inside a git working tree.");
        return std::nullopt;
    }
    return GitRepo(QDir::cleanPath(root));
}

bool GitRepo::run(const QStringList &args, QByteArray *out, QString *error, const QByteArray &input) const
{
    return runGit(m_root, args, out, error, input);
}

std::optional<QString> GitRepo::resolveCommit(const QString &rev, QString *error) const
{
    if (rev.isEmpty() || rev.startsWith(u'-')) {
        if (error)
            *error = QStringLiteral("Invalid revision: '%1'").arg(rev);
        return std::nullopt;
    }
    QByteArray out;
    if (!run({QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("--quiet"),
              rev + QStringLiteral("^{commit}")},
             &out, nullptr)) {
        if (error)
            *error = QStringLiteral("Unknown revision: '%1'").arg(rev);
        return std::nullopt;
    }
    return QString::fromUtf8(out).trimmed();
}

std::optional<QString> GitRepo::mergeBase(const QString &a, const QString &b, QString *error) const
{
    QByteArray out;
    if (!run({QStringLiteral("merge-base"), a, b}, &out, error)) {
        if (error && error->isEmpty())
            *error = QStringLiteral("No merge base between the given revisions.");
        return std::nullopt;
    }
    return QString::fromUtf8(out).trimmed();
}

bool GitRepo::hasHead() const
{
    return run({QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("--quiet"),
                QStringLiteral("HEAD^{commit}")},
               nullptr, nullptr);
}

QString GitRepo::headOrEmptyTree() const
{
    QByteArray out;
    if (run({QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("--quiet"),
             QStringLiteral("HEAD^{commit}")},
            &out, nullptr))
        return QString::fromUtf8(out).trimmed();
    // No commits yet: compare against the empty tree (works for SHA-1 and SHA-256 repos).
    if (run({QStringLiteral("hash-object"), QStringLiteral("-t"), QStringLiteral("tree"), QStringLiteral("--stdin")},
            &out, nullptr))
        return QString::fromUtf8(out).trimmed();
    return QStringLiteral("4b825dc642cb6eb9a060e54bf8d69288fbee4904");
}

QVector<FileChange> GitRepo::listChanges(const Comparison &cmp, QString *error) const
{
    QByteArray out;
    const QStringList nameStatus{QStringLiteral("diff"), QStringLiteral("--name-status"), QStringLiteral("-z")};

    if (cmp.mode == Comparison::Mode::Revisions) {
        QStringList args = nameStatus;
        args << QStringLiteral("-M") << cmp.rev1;
        if (!cmp.rev2.isEmpty())
            args << cmp.rev2;
        if (!run(args, &out, error))
            return {};
        return parseNameStatus(out, ChangeGroup::Revisions);
    }

    const QString base = headOrEmptyTree();

    if (!run(nameStatus + QStringList{QStringLiteral("--cached"), QStringLiteral("-M"), base}, &out, error))
        return {};
    QVector<FileChange> result = parseNameStatus(out, ChangeGroup::Staged);

    if (!run(nameStatus, &out, error))
        return {};
    QVector<FileChange> unstaged = parseNameStatus(out, ChangeGroup::Unstaged);

    if (cmp.unstagedAgainstHead) {
        // Report status/rename relative to HEAD, since that is what the diff will show.
        if (!run(nameStatus + QStringList{QStringLiteral("-M"), base}, &out, error))
            return {};
        QHash<QString, FileChange> vsHead;
        for (const FileChange &fc : parseNameStatus(out, ChangeGroup::Unstaged))
            vsHead.insert(fc.path, fc);
        for (FileChange &fc : unstaged) {
            const auto it = vsHead.constFind(fc.path);
            if (it != vsHead.constEnd() && fc.status != u'U') {
                fc.status = it->status;
                fc.oldPath = it->oldPath;
            }
        }
    }
    result += unstaged;

    if (!run({QStringLiteral("ls-files"), QStringLiteral("--others"), QStringLiteral("--exclude-standard"),
              QStringLiteral("-z")},
             &out, error))
        return {};
    for (const QByteArray &p : out.split('\0')) {
        if (p.isEmpty())
            continue;
        FileChange fc;
        fc.path = fc.oldPath = QString::fromUtf8(p);
        fc.status = u'?';
        fc.group = ChangeGroup::Unstaged;
        result.push_back(fc);
    }
    return result;
}

FileDiff GitRepo::loadDiff(const FileChange &fc, const Comparison &cmp, QString *error) const
{
    if (fc.status == u'?') {
        QFile f(m_root + u'/' + fc.path);
        if (!f.open(QIODevice::ReadOnly)) {
            if (error)
                *error = f.errorString();
            return {};
        }
        const QByteArray data = f.readAll();
        if (data.left(8000).contains('\0')) {
            FileDiff d;
            d.binary = true;
            d.message = QStringLiteral("Binary file (untracked)");
            return d;
        }
        FileDiff d = DiffModel::allAdded(QString::fromUtf8(data));
        if (d.lines.isEmpty())
            d.message = QStringLiteral("Empty file (untracked)");
        return d;
    }

    QStringList args = diffBaseArgs();
    if (cmp.mode == Comparison::Mode::Revisions) {
        args << cmp.rev1;
        if (!cmp.rev2.isEmpty())
            args << cmp.rev2;
    } else if (fc.status == u'U') {
        args << headOrEmptyTree(); // conflicted: show working tree (with markers) against HEAD
    } else if (fc.group == ChangeGroup::Staged) {
        args << QStringLiteral("--cached") << headOrEmptyTree();
    } else if (cmp.unstagedAgainstHead) {
        args << headOrEmptyTree();
    }
    args += pathArgs(fc);

    QByteArray out;
    if (!run(args, &out, error))
        return {};
    FileDiff d = DiffModel::parseUnified(QString::fromUtf8(out));
    if (d.binary)
        d.message = QStringLiteral("Binary file differs");
    else if (d.lines.isEmpty() && fc.status == u'D')
        d.message = QStringLiteral("Empty file deleted");
    else if (d.lines.isEmpty() && (fc.status == u'A'))
        d.message = QStringLiteral("Empty file added");
    else if (d.lines.isEmpty())
        d.message = QStringLiteral("No content differences (mode or metadata change only)");
    else if (d.chunks.isEmpty())
        d.message.clear();
    return d;
}
