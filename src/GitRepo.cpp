#include "GitRepo.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QProcessEnvironment>

#include <functional>

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
            const QByteArray &input = {}, bool allowExitOne = false)
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
    const bool ok = p.exitCode() == 0 || (allowExitOne && p.exitCode() == 1);
    if (p.exitStatus() != QProcess::NormalExit || !ok) {
        if (error)
            *error = QString::fromUtf8(p.readAllStandardError()).trimmed();
        return false;
    }
    return true;
}

struct LineStat {
    int added = -1;
    int removed = -1;
};

// git diff --numstat -z: "A<TAB>D<TAB>path\0", or "A<TAB>D<TAB>\0old\0new\0" for renames
// (and for every entry with --no-index). Binary files report "-". Keyed by the new path,
// or the old one for deletions, after `strip` removes a leading prefix.
QHash<QString, LineStat> parseNumstat(const QByteArray &out, const std::function<QString(const QString &)> &strip = {})
{
    QHash<QString, LineStat> result;
    const QList<QByteArray> parts = out.split('\0');
    auto clean = [&](const QByteArray &p) {
        const QString s = QString::fromUtf8(p);
        return strip ? strip(s) : s;
    };
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const QList<QByteArray> fields = parts[i].split('\t');
        if (fields.size() < 3)
            continue;
        bool okA = false, okD = false;
        LineStat st{fields[0].toInt(&okA), fields[1].toInt(&okD)};
        if (!okA || !okD)
            st = {};
        QString key;
        if (fields[2].isEmpty()) {
            if (i + 2 >= parts.size())
                break;
            const QString oldPath = clean(parts[i + 1]);
            const QString newPath = clean(parts[i + 2]);
            key = newPath.isEmpty() || parts[i + 2] == "/dev/null" ? oldPath : newPath;
            i += 2;
        } else {
            key = clean(fields[2]);
        }
        result.insert(key, st);
    }
    return result;
}

void applyStats(QVector<FileChange> &changes, const QHash<QString, LineStat> &stats, ChangeGroup onlyGroup,
                bool anyGroup = false)
{
    for (FileChange &fc : changes) {
        if (!anyGroup && fc.group != onlyGroup)
            continue;
        const auto it = stats.constFind(fc.path);
        const auto it2 = it == stats.constEnd() ? stats.constFind(fc.oldPath) : it;
        if (it2 != stats.constEnd()) {
            fc.added = it2->added;
            fc.removed = it2->removed;
        }
    }
}

// Line count of an untracked text file, -1 for binary or unreadable files.
int countLines(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return -1;
    const QByteArray data = f.readAll();
    if (data.left(8000).contains('\0'))
        return -1;
    if (data.isEmpty())
        return 0;
    return int(data.count('\n')) + (data.endsWith('\n') ? 0 : 1);
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

QStringList diffBaseArgs(const DiffOptions &o)
{
    QStringList a{QStringLiteral("diff"), QStringLiteral("--no-color"), QStringLiteral("--no-ext-diff"),
                  QStringLiteral("-M"), QStringLiteral("--unified=100000000")};
    return a + o.gitArgs();
}

// Working-tree comparison against a commit, as one list (used by "all changes" and `yadiff REV`).
bool isAgainstWorkingTree(const Comparison &cmp)
{
    return (cmp.mode == Comparison::Mode::Revisions && cmp.rev2.isEmpty())
        || (cmp.mode == Comparison::Mode::WorkingTree && cmp.allChanges);
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
    case '=': return QStringLiteral("Identical");
    default: return QStringLiteral("Changed");
    }
}

QString algorithmName(DiffAlgorithm a)
{
    switch (a) {
    case DiffAlgorithm::Histogram: return QStringLiteral("histogram");
    case DiffAlgorithm::Patience: return QStringLiteral("patience");
    case DiffAlgorithm::Myers: return QStringLiteral("myers");
    case DiffAlgorithm::Minimal: return QStringLiteral("minimal");
    }
    return QStringLiteral("histogram");
}

QString algorithmLabel(DiffAlgorithm a)
{
    switch (a) {
    case DiffAlgorithm::Histogram: return QStringLiteral("Histogram");
    case DiffAlgorithm::Patience: return QStringLiteral("Patience");
    case DiffAlgorithm::Myers: return QStringLiteral("Myers");
    case DiffAlgorithm::Minimal: return QStringLiteral("Minimal");
    }
    return {};
}

QString algorithmDescription(DiffAlgorithm a)
{
    switch (a) {
    case DiffAlgorithm::Histogram:
        return QStringLiteral("Patience extended to low-occurrence lines. Fast, and usually close to Patience.");
    case DiffAlgorithm::Patience:
        return QStringLiteral("Anchors on unique lines; good at keeping functions and blocks intact (default).");
    case DiffAlgorithm::Myers:
        return QStringLiteral("Git's classic greedy algorithm.");
    case DiffAlgorithm::Minimal:
        return QStringLiteral("Myers, spending extra time to find the smallest possible diff.");
    }
    return {};
}

QStringList DiffOptions::gitArgs() const
{
    return QStringList{QStringLiteral("--diff-algorithm=") + algorithmName(algorithm)} + whitespaceArgs();
}

QStringList DiffOptions::whitespaceArgs() const
{
    QStringList a;
    if (ignoreAllSpace)
        a << QStringLiteral("--ignore-all-space");
    else if (ignoreSpaceChange)
        a << QStringLiteral("--ignore-space-change");
    if (ignoreBlankLines)
        a << QStringLiteral("--ignore-blank-lines");
    if (ignoreCrAtEol)
        a << QStringLiteral("--ignore-cr-at-eol");
    return a;
}

std::optional<GitRepo> GitRepo::open(const QString &dir, QString *error)
{
    QByteArray out;
    if (runGit(dir, {QStringLiteral("rev-parse"), QStringLiteral("--is-bare-repository")}, &out, nullptr)
        && out.trimmed() == "true") {
        if (!runGit(dir, {QStringLiteral("rev-parse"), QStringLiteral("--absolute-git-dir")}, &out, error))
            return std::nullopt;
        return GitRepo(QDir::cleanPath(QString::fromUtf8(out).trimmed()), true);
    }
    if (!runGit(dir, {QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")}, &out, error))
        return std::nullopt;
    const QString root = QString::fromUtf8(out).trimmed();
    if (root.isEmpty()) {
        if (error)
            *error = QStringLiteral("Not inside a git working tree.");
        return std::nullopt;
    }
    return GitRepo(QDir::cleanPath(root), false);
}

QStringList GitRepo::pathspecs(const Comparison &cmp) const
{
    if (cmp.paths.isEmpty())
        return {};
    QStringList a{QStringLiteral("--")};
    for (const QString &p : cmp.paths)
        a << QStringLiteral(":(top,literal)") + p;
    return a;
}

bool GitRepo::hasCommit(const QString &oid) const
{
    return run({QStringLiteral("cat-file"), QStringLiteral("-e"), oid + QStringLiteral("^{commit}")}, nullptr, nullptr);
}

bool GitRepo::knowsPath(const QString &path) const
{
    if (run({QStringLiteral("ls-files"), QStringLiteral("--error-unmatch"), QStringLiteral("--"),
             QStringLiteral(":(top,literal)") + path},
            nullptr, nullptr))
        return true;
    QByteArray out;
    return run({QStringLiteral("ls-tree"), QStringLiteral("--name-only"), QStringLiteral("HEAD"), QStringLiteral("--"),
                QStringLiteral(":(top,literal)") + path},
               &out, nullptr)
        && !out.trimmed().isEmpty();
}

bool GitRepo::run(const QStringList &args, QByteArray *out, QString *error, const QByteArray &input) const
{
    return runGit(m_root, args, out, error, input);
}

bool GitRepo::runDiff(const QStringList &args, QByteArray *out, QString *error) const
{
    return runGit(m_root, args, out, error, {}, true);
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
    if (cmp.mode == Comparison::Mode::Paths)
        return listPathChanges(cmp, error);

    QByteArray out;
    // With whitespace ignored, git leaves whitespace-only files out of --name-status.
    const QStringList ws = cmp.options.whitespaceArgs();
    const QStringList nameStatus = QStringList{QStringLiteral("diff"), QStringLiteral("--name-status"), QStringLiteral("-z")} + ws;
    const QStringList numstat = QStringList{QStringLiteral("diff"), QStringLiteral("--numstat"), QStringLiteral("-z")} + ws;
    const QStringList specs = pathspecs(cmp);

    // Runs `numstat + extra` and copies the counts onto the matching changes.
    auto stats = [&](QVector<FileChange> &list, const QStringList &extra, ChangeGroup group, bool anyGroup) {
        QByteArray statOut;
        if (run(numstat + extra + specs, &statOut, nullptr))
            applyStats(list, parseNumstat(statOut), group, anyGroup);
    };

    auto appendUntracked = [&](QVector<FileChange> &result, ChangeGroup group) {
        if (m_bare)
            return true;
        if (!run(QStringList{QStringLiteral("ls-files"), QStringLiteral("--others"), QStringLiteral("--exclude-standard"),
                             QStringLiteral("-z")}
                     + specs,
                 &out, error))
            return false;
        for (const QByteArray &p : out.split('\0')) {
            if (p.isEmpty())
                continue;
            FileChange fc;
            fc.path = fc.oldPath = QString::fromUtf8(p);
            fc.status = u'?';
            fc.group = group;
            const int n = countLines(m_root + u'/' + fc.path);
            if (n >= 0) {
                fc.added = n;
                fc.removed = 0;
            }
            result.push_back(fc);
        }
        return true;
    };

    if (cmp.mode == Comparison::Mode::Revisions || cmp.allChanges) {
        QStringList revs{QStringLiteral("-M")};
        if (cmp.mode == Comparison::Mode::Revisions) {
            revs << cmp.rev1;
            if (!cmp.rev2.isEmpty())
                revs << cmp.rev2;
        } else {
            revs << headOrEmptyTree();
        }
        if (!run(nameStatus + revs + specs, &out, error))
            return {};
        QVector<FileChange> result = parseNameStatus(out, ChangeGroup::Revisions);
        stats(result, revs, ChangeGroup::Revisions, true);
        if (isAgainstWorkingTree(cmp) && !appendUntracked(result, ChangeGroup::Revisions))
            return {};
        return result;
    }

    const QString base = headOrEmptyTree();
    const QStringList stagedArgs{QStringLiteral("--cached"), QStringLiteral("-M"), base};

    if (!run(nameStatus + stagedArgs + specs, &out, error))
        return {};
    QVector<FileChange> result = parseNameStatus(out, ChangeGroup::Staged);
    stats(result, stagedArgs, ChangeGroup::Staged, false);

    if (!run(nameStatus + specs, &out, error))
        return {};
    QVector<FileChange> unstaged = parseNameStatus(out, ChangeGroup::Unstaged);

    if (cmp.unstagedAgainstHead) {
        // Report status/rename relative to HEAD, since that is what the diff will show.
        if (!run(nameStatus + QStringList{QStringLiteral("-M"), base} + specs, &out, error))
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
        stats(unstaged, {QStringLiteral("-M"), base}, ChangeGroup::Unstaged, false);
    } else {
        stats(unstaged, {}, ChangeGroup::Unstaged, false);
    }
    result += unstaged;
    if (!appendUntracked(result, ChangeGroup::Unstaged))
        return {};
    return result;
}

QVector<FileChange> GitRepo::listPathChanges(const Comparison &cmp, QString *error) const
{
    const QStringList ws = cmp.options.whitespaceArgs();
    QByteArray out;
    if (cmp.pathsAreFiles) {
        // git difftool passes /dev/null for the missing side of an added or deleted file.
        const bool leftMissing = cmp.leftPath == QLatin1String("/dev/null");
        const bool rightMissing = cmp.rightPath == QLatin1String("/dev/null");
        FileChange fc;
        fc.path = QFileInfo(rightMissing ? cmp.leftPath : cmp.rightPath).fileName();
        fc.oldPath = QFileInfo(leftMissing ? cmp.rightPath : cmp.leftPath).fileName();
        if (!runDiff(QStringList{QStringLiteral("diff"), QStringLiteral("--no-index"), QStringLiteral("--numstat"),
                                 QStringLiteral("-z")}
                         + ws + QStringList{QStringLiteral("--"), cmp.leftPath, cmp.rightPath},
                     &out, error))
            return {};
        const auto st = parseNumstat(out);
        fc.status = leftMissing ? u'A' : rightMissing ? u'D' : st.isEmpty() ? u'=' : u'M';
        if (!st.isEmpty()) {
            fc.added = st.begin()->added;
            fc.removed = st.begin()->removed;
        } else {
            fc.added = fc.removed = 0;
        }
        return {fc};
    }

    const QString left = cmp.leftPath + u'/';
    const QString right = cmp.rightPath + u'/';
    auto strip = [&](const QString &p) -> QString {
        if (p.startsWith(left))
            return p.mid(left.size());
        if (p.startsWith(right))
            return p.mid(right.size());
        return p;
    };
    const QStringList tail{QStringLiteral("--"), cmp.leftPath, cmp.rightPath};
    if (!runDiff(QStringList{QStringLiteral("diff"), QStringLiteral("--no-index"), QStringLiteral("--name-status"),
                             QStringLiteral("-z"), QStringLiteral("-M")}
                     + ws + tail,
                 &out, error))
        return {};
    QVector<FileChange> result = parseNameStatus(out, ChangeGroup::Revisions);
    for (FileChange &fc : result) {
        fc.path = strip(fc.path);
        fc.oldPath = strip(fc.oldPath);
    }
    QByteArray statOut;
    if (runDiff(QStringList{QStringLiteral("diff"), QStringLiteral("--no-index"), QStringLiteral("--numstat"),
                            QStringLiteral("-z"), QStringLiteral("-M")}
                    + ws + tail,
                &statOut, nullptr))
        applyStats(result, parseNumstat(statOut, strip), ChangeGroup::Revisions, true);
    return result;
}

FileDiff GitRepo::loadDiff(const FileChange &fc, const Comparison &cmp, QString *error) const
{
    if (cmp.mode == Comparison::Mode::Paths)
        return loadPathDiff(fc, cmp, error);
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

    QStringList args = diffBaseArgs(cmp.options);
    if (cmp.mode == Comparison::Mode::Revisions) {
        args << cmp.rev1;
        if (!cmp.rev2.isEmpty())
            args << cmp.rev2;
    } else if (cmp.allChanges || fc.status == u'U') {
        args << headOrEmptyTree(); // working tree (with conflict markers, if any) against HEAD
    } else if (fc.group == ChangeGroup::Staged) {
        args << QStringLiteral("--cached") << headOrEmptyTree();
    } else if (cmp.unstagedAgainstHead) {
        args << headOrEmptyTree();
    }
    args += pathArgs(fc);

    QByteArray out;
    if (!run(args, &out, error))
        return {};
    FileDiff d = DiffModel::parseUnified(QString::fromUtf8(out), cmp.options.detectMoves);
    if (d.binary)
        d.message = QStringLiteral("Binary file differs");
    else if (d.lines.isEmpty() && fc.status == u'D')
        d.message = QStringLiteral("Empty file deleted");
    else if (d.lines.isEmpty() && (fc.status == u'A'))
        d.message = QStringLiteral("Empty file added");
    else if (d.lines.isEmpty() && cmp.options.ignoresWhitespace())
        d.message = QStringLiteral("No differences with the current whitespace options");
    else if (d.lines.isEmpty())
        d.message = QStringLiteral("No content differences (mode or metadata change only)");
    else if (d.chunks.isEmpty())
        d.message.clear();
    return d;
}

QByteArray GitRepo::fingerprint(const FileChange &fc, const Comparison &cmp) const
{
    // Blob id of `spec` (e.g. "HEAD:path" or ":0:path"), "-" when absent.
    auto blob = [&](const QString &spec) -> QByteArray {
        QByteArray out;
        if (run({QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("--quiet"), spec}, &out, nullptr))
            return out.trimmed();
        return "-";
    };
    auto workingFile = [&](const QString &path) -> QByteArray {
        QFile f(m_root + u'/' + path);
        if (!f.open(QIODevice::ReadOnly))
            return "-";
        return "wt:" + QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha1).toHex();
    };

    QByteArray oldSide, newSide;
    if (cmp.mode == Comparison::Mode::Paths) {
        const QString l = cmp.pathsAreFiles ? cmp.leftPath : cmp.leftPath + u'/' + fc.oldPath;
        const QString r = cmp.pathsAreFiles ? cmp.rightPath : cmp.rightPath + u'/' + fc.path;
        auto hashFile = [](const QString &p) -> QByteArray {
            QFile f(p);
            if (!f.open(QIODevice::ReadOnly))
                return "-";
            return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha1).toHex();
        };
        oldSide = hashFile(l);
        newSide = hashFile(r);
    } else if (fc.status == u'?') {
        oldSide = "-";
        newSide = workingFile(fc.path);
    } else if (cmp.mode == Comparison::Mode::Revisions) {
        oldSide = blob(cmp.rev1 + u':' + fc.oldPath);
        newSide = cmp.rev2.isEmpty() ? workingFile(fc.path) : blob(cmp.rev2 + u':' + fc.path);
    } else if (fc.group == ChangeGroup::Staged && !cmp.allChanges && fc.status != u'U') {
        oldSide = blob(headOrEmptyTree() + u':' + fc.oldPath);
        newSide = blob(QStringLiteral(":0:") + fc.path);
    } else {
        const bool vsIndex = !cmp.allChanges && !cmp.unstagedAgainstHead && fc.status != u'U';
        oldSide = vsIndex ? blob(QStringLiteral(":0:") + fc.oldPath) : blob(headOrEmptyTree() + u':' + fc.oldPath);
        newSide = workingFile(fc.path);
    }
    return QCryptographicHash::hash(fc.oldPath.toUtf8() + '\0' + fc.path.toUtf8() + '\0' + oldSide + '\0' + newSide,
                                    QCryptographicHash::Sha1)
        .toHex();
}

FileDiff GitRepo::loadPathDiff(const FileChange &fc, const Comparison &cmp, QString *error) const
{
    if (fc.status == u'=') {
        FileDiff d;
        d.message = QStringLiteral("The files are identical");
        return d;
    }
    QString left = cmp.pathsAreFiles ? cmp.leftPath : cmp.leftPath + u'/' + fc.oldPath;
    QString right = cmp.pathsAreFiles ? cmp.rightPath : cmp.rightPath + u'/' + fc.path;
    if (fc.status == u'A')
        left = QStringLiteral("/dev/null"); // git diff --no-index understands this on every platform
    if (fc.status == u'D')
        right = QStringLiteral("/dev/null");
    QStringList args = diffBaseArgs(cmp.options);
    args.insert(1, QStringLiteral("--no-index"));
    args << QStringLiteral("--") << left << right;
    QByteArray out;
    if (!runDiff(args, &out, error))
        return {};
    FileDiff d = DiffModel::parseUnified(QString::fromUtf8(out), cmp.options.detectMoves);
    if (d.binary)
        d.message = QStringLiteral("Binary file differs");
    else if (d.lines.isEmpty() && cmp.options.ignoresWhitespace())
        d.message = QStringLiteral("No differences with the current whitespace options");
    else if (d.lines.isEmpty())
        d.message = QStringLiteral("No content differences");
    return d;
}

QString GitRepo::diskPath(const FileChange &fc, const Comparison &cmp, bool newSide) const
{
    QString p;
    if (cmp.mode == Comparison::Mode::Paths) {
        if (cmp.pathsAreFiles)
            p = newSide ? cmp.rightPath : cmp.leftPath;
        else
            p = (newSide ? cmp.rightPath : cmp.leftPath) + u'/' + (newSide ? fc.path : fc.oldPath);
    } else if (newSide && !m_bare
               && (isAgainstWorkingTree(cmp) || fc.status == u'U'
                   || (cmp.mode == Comparison::Mode::WorkingTree && fc.group == ChangeGroup::Unstaged))) {
        p = m_root + u'/' + fc.path;
    }
    return !p.isEmpty() && QFileInfo(p).isFile() ? p : QString();
}

QVector<CommitInfo> GitRepo::log(int max, int skip, bool allRefs, QString *error) const
{
    QStringList args{QStringLiteral("log"), QStringLiteral("--date=format:%Y-%m-%d %H:%M"),
                     QStringLiteral("--format=%H%x1f%h%x1f%an%x1f%ad%x1f%D%x1f%s%x1f%P%x1e"),
                     QStringLiteral("-n"), QString::number(max), QStringLiteral("--skip=") + QString::number(skip)};
    if (allRefs)
        args << QStringLiteral("--all");
    QByteArray out;
    if (!run(args, &out, error))
        return {};
    QVector<CommitInfo> result;
    for (const QByteArray &rec : out.split('\x1e')) {
        const QList<QByteArray> f = rec.trimmed().split('\x1f');
        if (f.size() < 7)
            continue;
        CommitInfo c;
        c.oid = QString::fromUtf8(f[0]);
        c.shortOid = QString::fromUtf8(f[1]);
        c.author = QString::fromUtf8(f[2]);
        c.date = QString::fromUtf8(f[3]);
        c.refs = QString::fromUtf8(f[4]);
        c.subject = QString::fromUtf8(f[5]);
        c.parents = QString::fromUtf8(f[6]).split(u' ', Qt::SkipEmptyParts);
        result.push_back(c);
    }
    return result;
}

std::optional<QByteArray> GitRepo::fileContent(const FileChange &fc, const Comparison &cmp, bool newSide,
                                               qint64 maxBytes) const
{
    if ((newSide && fc.status == u'D') || (!newSide && (fc.status == u'A' || fc.status == u'?')))
        return std::nullopt;

    auto readFile = [&](const QString &path) -> std::optional<QByteArray> {
        QFile f(path);
        if (path == QLatin1String("/dev/null") || !f.open(QIODevice::ReadOnly) || f.size() > maxBytes)
            return std::nullopt;
        return f.readAll();
    };
    if (cmp.mode == Comparison::Mode::Paths) {
        if (cmp.pathsAreFiles)
            return readFile(newSide ? cmp.rightPath : cmp.leftPath);
        return readFile((newSide ? cmp.rightPath : cmp.leftPath) + u'/' + (newSide ? fc.path : fc.oldPath));
    }
    if (newSide) {
        if (const QString disk = diskPath(fc, cmp, true); !disk.isEmpty())
            return readFile(disk);
    }

    // Otherwise the side is a blob: name it the way `git cat-file` understands.
    QString spec;
    if (cmp.mode == Comparison::Mode::Revisions) {
        spec = newSide ? cmp.rev2 + u':' + fc.path : cmp.rev1 + u':' + fc.oldPath;
    } else if (newSide) {
        spec = QStringLiteral(":0:") + fc.path; // staged: the index
    } else if (fc.group == ChangeGroup::Unstaged && !cmp.allChanges && !cmp.unstagedAgainstHead && fc.status != u'U') {
        spec = QStringLiteral(":0:") + fc.oldPath;
    } else {
        spec = headOrEmptyTree() + u':' + fc.oldPath;
    }
    QByteArray size;
    if (!run({QStringLiteral("cat-file"), QStringLiteral("-s"), spec}, &size, nullptr) || size.trimmed().toLongLong() > maxBytes)
        return std::nullopt;
    QByteArray out;
    if (!run({QStringLiteral("cat-file"), QStringLiteral("blob"), spec}, &out, nullptr))
        return std::nullopt;
    return out;
}
