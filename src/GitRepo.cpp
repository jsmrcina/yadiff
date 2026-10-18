#include "GitRepo.h"
#include "LineDiff.h"

#include "FileFilter.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
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
    case DiffAlgorithm::MyersLcs: return QStringLiteral("myers-lcs");
    case DiffAlgorithm::Heckel: return QStringLiteral("heckel");
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
    case DiffAlgorithm::MyersLcs: return QStringLiteral("Myers LCS");
    case DiffAlgorithm::Heckel: return QStringLiteral("Heckel isolation");
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
    case DiffAlgorithm::MyersLcs:
        return QStringLiteral("yadiff's own exact Myers: a longest common subsequence of lines, so the fewest changes.");
    case DiffAlgorithm::Heckel:
        return QStringLiteral("Heckel's algorithm: unique lines anchor the match and isolate the changes between them.");
    }
    return {};
}

const QVector<DiffAlgorithm> &allAlgorithms()
{
    static const QVector<DiffAlgorithm> all{DiffAlgorithm::Patience, DiffAlgorithm::Histogram, DiffAlgorithm::Myers,
                                            DiffAlgorithm::Minimal,  DiffAlgorithm::MyersLcs,  DiffAlgorithm::Heckel};
    return all;
}

bool DiffOptions::inProcess() const
{
    return algorithm == DiffAlgorithm::MyersLcs || algorithm == DiffAlgorithm::Heckel || ignoreCase || ignoreTrim;
}

QString DiffOptions::normalized(QString l) const
{
    if (ignoreCrAtEol && l.endsWith(u'\r'))
        l.chop(1);
    if (ignoreAllSpace) {
        l.removeIf([](QChar c) { return c.isSpace(); });
    } else {
        if (ignoreTrim)
            l = l.trimmed();
        if (ignoreSpaceChange) {
            // Like git -b: runs of whitespace are equal, and trailing whitespace is ignored.
            QString out;
            out.reserve(l.size());
            bool space = false;
            for (QChar c : std::as_const(l)) {
                if (c.isSpace()) {
                    space = true;
                    continue;
                }
                if (space)
                    out += u' ';
                space = false;
                out += c;
            }
            l = out;
        }
    }
    if (ignoreCase)
        l = l.toCaseFolded();
    return l;
}

QStringList DiffOptions::gitArgs() const
{
    // yadiff's own algorithms diff in-process; git lists the files with plain Myers.
    const DiffAlgorithm gitAlgorithm =
        algorithm == DiffAlgorithm::MyersLcs || algorithm == DiffAlgorithm::Heckel ? DiffAlgorithm::Myers : algorithm;
    return QStringList{QStringLiteral("--diff-algorithm=") + algorithmName(gitAlgorithm)} + whitespaceArgs();
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
    // The empty tree stands in for the parent of a root commit.
    if (rev == emptyTree())
        return rev;
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
    // No commits yet: compare against the empty tree.
    return emptyTree();
}

QString GitRepo::emptyTree() const
{
    // Works for SHA-1 and SHA-256 repos.
    QByteArray out;
    if (run({QStringLiteral("hash-object"), QStringLiteral("-t"), QStringLiteral("tree"), QStringLiteral("--stdin")},
            &out, nullptr))
        return QString::fromUtf8(out).trimmed();
    return QStringLiteral("4b825dc642cb6eb9a060e54bf8d69288fbee4904");
}

FileCategory categoryOf(const FileChange &fc, bool reversed)
{
    switch (fc.status.unicode()) {
    case 'D': return FileCategory::LeftOnly;
    case 'A': return FileCategory::RightOnly;
    case '?': return reversed ? FileCategory::LeftOnly : FileCategory::RightOnly; // untracked: working tree only
    case '=': return FileCategory::Identical;
    default: return FileCategory::Different;
    }
}

namespace {
bool matchesAnyGlob(const QString &path, const QStringList &globs)
{
    for (const QString &g : globs)
        if (globMatches(g, path))
            return true;
    return false;
}
} // namespace

QVector<FileChange> GitRepo::listChanges(const Comparison &cmp, QString *error) const
{
    if (cmp.empty)
        return {};
    QVector<FileChange> result = listChangesCore(cmp, error);
    if (error && !error->isEmpty())
        return result;
    if (cmp.includeIdentical)
        appendIdentical(result, cmp);
    if (cmp.useIgnoreGlobs && !cmp.ignoreGlobs.isEmpty()) {
        result.erase(std::remove_if(result.begin(), result.end(),
                                    [&](const FileChange &fc) {
                                        return matchesAnyGlob(fc.path, cmp.ignoreGlobs) && matchesAnyGlob(fc.oldPath, cmp.ignoreGlobs);
                                    }),
                     result.end());
    }
    return result;
}

QVector<FileChange> GitRepo::listChangesCore(const Comparison &cmp, QString *error) const
{
    if (cmp.mode == Comparison::Mode::Paths)
        return listPathChanges(cmp, error);

    QByteArray out;
    // With whitespace ignored, git leaves whitespace-only files out of --name-status.
    const QStringList ws = cmp.options.whitespaceArgs();
    const QStringList reverse = cmp.reversed ? QStringList{QStringLiteral("-R")} : QStringList();
    const QStringList nameStatus =
        QStringList{QStringLiteral("diff"), QStringLiteral("--name-status"), QStringLiteral("-z")} + ws + reverse;
    const QStringList numstat = QStringList{QStringLiteral("diff"), QStringLiteral("--numstat"), QStringLiteral("-z")} + ws + reverse;
    const QStringList specs = pathspecs(cmp);

    // Runs `numstat + extra` and copies the counts onto the matching changes.
    auto stats = [&](QVector<FileChange> &list, const QStringList &extra, ChangeGroup group, bool anyGroup) {
        QByteArray statOut;
        if (run(numstat + extra + specs, &statOut, nullptr))
            applyStats(list, parseNumstat(statOut), group, anyGroup);
        // Files that drop out once blank lines are ignored only changed blank lines.
        if (run(numstat + QStringList{QStringLiteral("--ignore-blank-lines")} + extra + specs, &statOut, nullptr)) {
            const auto nonBlank = parseNumstat(statOut);
            for (FileChange &fc : list)
                if ((anyGroup || fc.group == group) && fc.status == u'M' && !nonBlank.contains(fc.path) && fc.added >= 0)
                    fc.blankOnly = true;
        }
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
    if (runDiff(QStringList{QStringLiteral("diff"), QStringLiteral("--no-index"), QStringLiteral("--numstat"),
                            QStringLiteral("-z"), QStringLiteral("-M"), QStringLiteral("--ignore-blank-lines")}
                    + ws + tail,
                &statOut, nullptr)) {
        const auto nonBlank = parseNumstat(statOut, strip);
        for (FileChange &fc : result)
            if (fc.status == u'M' && !nonBlank.contains(fc.path) && fc.added >= 0)
                fc.blankOnly = true;
    }
    return result;
}

FileDiff GitRepo::loadDiff(const FileChange &fc, const Comparison &cmp, QString *error) const
{
    if (fc.status == u'=') {
        // Identical: show the file itself (every line unchanged).
        FileDiff d;
        const auto content = fileContent(fc, cmp, true, 32 * 1024 * 1024);
        if (!content) {
            d.message = QStringLiteral("The files are identical");
        } else if (content->left(8000).contains('\0')) {
            d.binary = true;
            d.message = QStringLiteral("Identical binary file");
        } else {
            d = DiffModel::allEqual(QString::fromUtf8(*content));
            if (d.lines.isEmpty())
                d.message = QStringLiteral("The files are identical (both empty)");
        }
        return d;
    }
    if (cmp.mode == Comparison::Mode::Paths)
        return loadPathDiff(fc, cmp, error);
    if (fc.status != u'?' && cmp.options.inProcess())
        return loadDiffInProcess(fc, cmp, error);
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
        if (cmp.reversed)
            d = DiffModel::reversed(d); // swapped sides: the untracked file is on the left
        if (d.lines.isEmpty())
            d.message = QStringLiteral("Empty file (untracked)");
        return d;
    }

    QStringList args = diffBaseArgs(cmp.options);
    if (cmp.reversed)
        args << QStringLiteral("-R");
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
    else if (d.lines.isEmpty() && cmp.options.ignoresSomething())
        d.message = QStringLiteral("No differences with the current whitespace options");
    else if (d.lines.isEmpty())
        d.message = QStringLiteral("No content differences (mode or metadata change only)");
    else if (d.chunks.isEmpty())
        d.message.clear();
    return d;
}

namespace {
// The change as it is on disk when sides are swapped (git diff -R shows new paths as old).
FileChange physicalChange(const FileChange &fc, const Comparison &cmp)
{
    if (!cmp.reversed)
        return fc;
    FileChange p = fc;
    std::swap(p.path, p.oldPath);
    if (fc.status == u'A')
        p.status = u'D';
    else if (fc.status == u'D')
        p.status = u'A';
    return p;
}
Comparison physicalComparison(const Comparison &cmp)
{
    Comparison c = cmp;
    c.reversed = false;
    return c;
}
} // namespace

QByteArray GitRepo::fingerprint(const FileChange &displayed, const Comparison &displayedCmp) const
{
    // Marks follow the files, not the direction they're compared in.
    const FileChange fc = physicalChange(displayed, displayedCmp);
    const Comparison cmp = physicalComparison(displayedCmp);
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
    if (cmp.options.inProcess())
        return loadDiffInProcess(fc, cmp, error);
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
    else if (d.lines.isEmpty() && cmp.options.ignoresSomething())
        d.message = QStringLiteral("No differences with the current whitespace options");
    else if (d.lines.isEmpty())
        d.message = QStringLiteral("No content differences");
    return d;
}

FileDiff GitRepo::loadDiffInProcess(const FileChange &fc, const Comparison &cmp, QString *error) const
{
    // Both sides' content, compared line by line after normalising each line.
    const std::optional<QByteArray> oldData = fileContent(fc, cmp, false);
    const std::optional<QByteArray> newData = fileContent(fc, cmp, true);
    FileDiff d;
    if (!oldData && !newData) {
        if (error)
            *error = QStringLiteral("Could not read either side of %1").arg(fc.path);
        return d;
    }
    if (oldData.value_or(QByteArray()).left(8000).contains('\0') || newData.value_or(QByteArray()).left(8000).contains('\0')) {
        d.binary = true;
        d.message = QStringLiteral("Binary file differs");
        return d;
    }
    const QStringList oldLines = LineDiff::splitLines(QString::fromUtf8(oldData.value_or(QByteArray())));
    const QStringList newLines = LineDiff::splitLines(QString::fromUtf8(newData.value_or(QByteArray())));
    const DiffOptions &o = cmp.options;
    QStringList oldNorm, newNorm;
    oldNorm.reserve(oldLines.size());
    newNorm.reserve(newLines.size());
    for (const QString &l : oldLines)
        oldNorm << o.normalized(l);
    for (const QString &l : newLines)
        newNorm << o.normalized(l);

    LineDiff::Alignment alignment;
    if (o.algorithm == DiffAlgorithm::MyersLcs || o.algorithm == DiffAlgorithm::Heckel) {
        const auto [a, b] = LineDiff::lineIds(oldNorm, newNorm);
        alignment = o.algorithm == DiffAlgorithm::Heckel ? LineDiff::heckel(a, b) : LineDiff::myers(a, b);
    } else {
        // One of git's algorithms, run on the normalised lines.
        QTemporaryDir tmp;
        auto save = [&tmp](const QString &name, const QStringList &lines) {
            QFile f(tmp.filePath(name));
            if (!f.open(QIODevice::WriteOnly))
                return false;
            for (const QString &l : lines) {
                f.write(l.toUtf8());
                f.write("\n");
            }
            return true;
        };
        if (!tmp.isValid() || !save(QStringLiteral("old"), oldNorm) || !save(QStringLiteral("new"), newNorm)) {
            if (error)
                *error = QStringLiteral("Could not write temporary files");
            return d;
        }
        QByteArray out;
        if (!runDiff({QStringLiteral("diff"), QStringLiteral("--no-index"), QStringLiteral("--no-color"),
                      QStringLiteral("--no-ext-diff"), QStringLiteral("-U0"),
                      QStringLiteral("--diff-algorithm=") + algorithmName(o.algorithm), QStringLiteral("--"),
                      tmp.filePath(QStringLiteral("old")), tmp.filePath(QStringLiteral("new"))},
                     &out, error))
            return d;
        alignment = LineDiff::fromUnifiedZero(QString::fromUtf8(out), int(oldLines.size()), int(newLines.size()));
    }

    d = LineDiff::build(oldLines, newLines, alignment, o.detectMoves);
    bool onlyBlank = o.ignoreBlankLines;
    for (int i = 0; onlyBlank && i < d.lines.size(); ++i)
        if (d.lines[i].kind != LineKind::Equal
            && !(d.lines[i].kind == LineKind::Removed ? oldNorm[d.lines[i].oldNo - 1] : newNorm[d.lines[i].newNo - 1])
                    .trimmed()
                    .isEmpty())
            onlyBlank = false;
    if (d.chunks.isEmpty() || onlyBlank) {
        // Nothing differs under the options: like git, show nothing.
        d = FileDiff();
        if (fc.status == u'D' && oldLines.isEmpty())
            d.message = QStringLiteral("Empty file deleted");
        else if (fc.status == u'A' && newLines.isEmpty())
            d.message = QStringLiteral("Empty file added");
        else
            d.message = o.ignoresSomething() ? QStringLiteral("No differences with the current options")
                                             : QStringLiteral("No content differences (mode or metadata change only)");
    }
    return d;
}

QString GitRepo::diskPath(const FileChange &fc, const Comparison &cmp, bool newSide) const
{
    if (!cmp.reversed)
        return diskPathPhysical(fc, cmp, newSide);
    return diskPathPhysical(physicalChange(fc, cmp), physicalComparison(cmp), !newSide);
}

QString GitRepo::diskPathPhysical(const FileChange &fc, const Comparison &cmp, bool newSide) const
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
    if (!cmp.reversed)
        return fileContentPhysical(fc, cmp, newSide, maxBytes);
    return fileContentPhysical(physicalChange(fc, cmp), physicalComparison(cmp), !newSide, maxBytes);
}

std::optional<QByteArray> GitRepo::fileContentPhysical(const FileChange &fc, const Comparison &cmp, bool newSide,
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
        if (const QString disk = diskPathPhysical(fc, cmp, true); !disk.isEmpty())
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

void GitRepo::appendIdentical(QVector<FileChange> &changes, const Comparison &cmp) const
{
    QSet<QString> listed;
    for (const FileChange &fc : changes) {
        listed.insert(fc.path);
        listed.insert(fc.oldPath);
    }
    auto add = [&](const QString &path, ChangeGroup group) {
        if (path.isEmpty() || listed.contains(path))
            return;
        FileChange fc;
        fc.path = fc.oldPath = path;
        fc.status = u'=';
        fc.group = group;
        fc.added = fc.removed = 0;
        changes.push_back(fc);
        listed.insert(path);
    };
    if (cmp.mode == Comparison::Mode::Paths) {
        if (cmp.pathsAreFiles)
            return; // a two-file comparison already reports '=' itself
        const QString right = cmp.rightPath + u'/';
        QDirIterator it(cmp.rightPath, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        int n = 0;
        while (it.hasNext() && n++ < 200000) {
            const QString abs = it.next();
            const QString rel = abs.mid(right.size());
            if (rel.startsWith(QLatin1String(".git/")))
                continue;
            if (QFileInfo::exists(cmp.leftPath + u'/' + rel))
                add(rel, ChangeGroup::Revisions);
        }
        return;
    }
    QByteArray out;
    QStringList args;
    if (cmp.mode == Comparison::Mode::Revisions && !cmp.rev2.isEmpty())
        args = {QStringLiteral("ls-tree"), QStringLiteral("-r"), QStringLiteral("--name-only"), QStringLiteral("-z"), cmp.rev2};
    else if (!m_bare)
        args = {QStringLiteral("ls-files"), QStringLiteral("-z")};
    else
        return;
    if (!run(args + pathspecs(cmp), &out, nullptr))
        return;
    const ChangeGroup group = cmp.mode == Comparison::Mode::WorkingTree && !cmp.allChanges ? ChangeGroup::Unchanged
                                                                                           : ChangeGroup::Revisions;
    for (const QByteArray &p : out.split('\0'))
        add(QString::fromUtf8(p), group);
}

bool GitRepo::stage(const QVector<FileChange> &changes, QString *error) const
{
    QStringList args{QStringLiteral("add"), QStringLiteral("-A"), QStringLiteral("--")};
    for (const FileChange &fc : changes) {
        args << QStringLiteral(":(top,literal)") + fc.path;
        if (fc.oldPath != fc.path)
            args << QStringLiteral(":(top,literal)") + fc.oldPath;
    }
    return run(args, nullptr, error);
}

bool GitRepo::unstage(const QVector<FileChange> &changes, QString *error) const
{
    QStringList paths;
    for (const FileChange &fc : changes) {
        paths << QStringLiteral(":(top,literal)") + fc.path;
        if (fc.oldPath != fc.path)
            paths << QStringLiteral(":(top,literal)") + fc.oldPath;
    }
    if (hasHead())
        return run(QStringList{QStringLiteral("restore"), QStringLiteral("--staged"), QStringLiteral("--")} + paths, nullptr, error);
    return run(QStringList{QStringLiteral("rm"), QStringLiteral("--cached"), QStringLiteral("-q"), QStringLiteral("--")} + paths,
               nullptr, error);
}

namespace {
bool removeToTrash(const QString &path, QString *error)
{
    if (!QFileInfo::exists(path))
        return true;
    // Tests set YADIFF_NO_TRASH so they don't fill the desktop's trash.
    if (qEnvironmentVariableIsEmpty("YADIFF_NO_TRASH") && QFile::moveToTrash(path))
        return true;
    if (QFile::remove(path))
        return true;
    if (error)
        *error = QStringLiteral("Could not delete %1").arg(path);
    return false;
}
} // namespace

bool GitRepo::revert(const QVector<FileChange> &changes, const Comparison &displayedCmp, QString *error) const
{
    for (const FileChange &displayed : changes) {
        if (displayed.status == u'=')
            continue;
        if (displayedCmp.mode == Comparison::Mode::Paths) {
            // Directory or file comparison: make the right file a copy of the left one.
            const QString left = displayedCmp.pathsAreFiles ? displayedCmp.leftPath
                                                            : displayedCmp.leftPath + u'/' + displayed.oldPath;
            const QString right = displayedCmp.pathsAreFiles ? displayedCmp.rightPath
                                                             : displayedCmp.rightPath + u'/' + displayed.path;
            if (!removeToTrash(right, error))
                return false;
            if (QFileInfo(left).isFile()) {
                QDir().mkpath(QFileInfo(right).absolutePath());
                if (!QFile::copy(left, right)) {
                    if (error)
                        *error = QStringLiteral("Could not copy %1 to %2").arg(left, right);
                    return false;
                }
            }
            continue;
        }
        if (displayedCmp.reversed) {
            if (error)
                *error = QStringLiteral("Swap the sides back first: reverting writes the working tree, which is on the left.");
            return false;
        }
        const FileChange &fc = displayed;
        const Comparison &cmp = displayedCmp;
        const QString lit = QStringLiteral(":(top,literal)");
        if (cmp.mode == Comparison::Mode::WorkingTree && !cmp.allChanges && fc.group == ChangeGroup::Staged) {
            // Staged: the right side is the index, so put the index back to HEAD.
            if (!unstage({fc}, error))
                return false;
            continue;
        }
        if (cmp.mode == Comparison::Mode::Revisions && !cmp.rev2.isEmpty()) {
            if (error)
                *error = QStringLiteral("Both sides are commits; there is no file on disk to revert.");
            return false;
        }
        // The left side's source: HEAD (or the chosen revision), or the index.
        QString source;
        if (cmp.mode == Comparison::Mode::Revisions)
            source = cmp.rev1;
        else if (cmp.allChanges || cmp.unstagedAgainstHead || fc.status == u'U')
            source = hasHead() ? QStringLiteral("HEAD") : QString();
        const bool toHead = !source.isEmpty() && (cmp.mode == Comparison::Mode::Revisions || cmp.allChanges);
        if (fc.status == u'?' || fc.status == u'A') {
            // Not on the left: remove it (and its staged copy).
            if (fc.status == u'A')
                run({QStringLiteral("rm"), QStringLiteral("--cached"), QStringLiteral("-q"), QStringLiteral("--"), lit + fc.path}, nullptr, nullptr);
            if (!removeToTrash(m_root + u'/' + fc.path, error))
                return false;
            continue;
        }
        if (fc.oldPath != fc.path) {
            // Renamed: bring back the old name and drop the new one.
            if (!removeToTrash(m_root + u'/' + fc.path, error))
                return false;
            run({QStringLiteral("rm"), QStringLiteral("--cached"), QStringLiteral("-q"), QStringLiteral("--ignore-unmatch"),
                 QStringLiteral("--"), lit + fc.path},
                nullptr, nullptr);
        }
        QStringList args{QStringLiteral("restore")};
        if (!source.isEmpty())
            args << QStringLiteral("--source=") + source;
        if (toHead)
            args << QStringLiteral("--staged"); // "all changes": the staged part goes too
        args << QStringLiteral("--worktree") << QStringLiteral("--") << lit + fc.oldPath;
        if (!run(args, nullptr, error))
            return false;
    }
    return true;
}

QString GitRepo::lastCommit(const QString &path) const
{
    QByteArray out;
    if (!run({QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--format=%H"), QStringLiteral("--"),
              QStringLiteral(":(top,literal)") + path},
             &out, nullptr))
        return {};
    return QString::fromUtf8(out).trimmed();
}

namespace {
// Parses `git blame --porcelain`: a header per line (hash, original line, final line),
// commit details the first time a commit appears, then the line itself after a tab.
QVector<BlameLine> parseBlame(const QByteArray &out)
{
    QHash<QString, BlameLine> commits;
    QVector<BlameLine> lines;
    QString current;
    int finalLine = 0;
    for (const QByteArray &raw : out.split('\n')) {
        if (raw.startsWith('\t')) {
            if (finalLine > 0) {
                if (lines.size() < finalLine)
                    lines.resize(finalLine);
                lines[finalLine - 1] = commits.value(current);
            }
            continue;
        }
        const QList<QByteArray> parts = raw.split(' ');
        if (parts.size() >= 3 && parts[0].size() == 40) {
            current = QString::fromLatin1(parts[0]);
            finalLine = parts[2].toInt();
            if (!commits.contains(current)) {
                BlameLine b;
                if (current != QLatin1String("0000000000000000000000000000000000000000"))
                    b.hash = current;
                commits.insert(current, b);
            }
            continue;
        }
        BlameLine &b = commits[current];
        if (raw.startsWith("author "))
            b.author = QString::fromUtf8(raw.mid(7));
        else if (raw.startsWith("author-time "))
            b.time = raw.mid(12).toLongLong();
        else if (raw.startsWith("summary "))
            b.summary = QString::fromUtf8(raw.mid(8));
    }
    return lines;
}
} // namespace

QVector<BlameLine> GitRepo::blame(const FileChange &displayed, const Comparison &displayedCmp, bool displayedNewSide) const
{
    if (m_noRepo || displayedCmp.mode == Comparison::Mode::Paths)
        return {};
    const FileChange fc = physicalChange(displayed, displayedCmp);
    const Comparison cmp = physicalComparison(displayedCmp);
    const bool newSide = displayedCmp.reversed ? !displayedNewSide : displayedNewSide;
    if ((newSide && (fc.status == u'D')) || (!newSide && fc.status == u'A') || fc.status == u'?')
        return {};
    const QString path = newSide ? fc.path : fc.oldPath;
    QStringList args{QStringLiteral("blame"), QStringLiteral("--porcelain")};
    QByteArray input;
    if (cmp.mode == Comparison::Mode::Revisions) {
        if (newSide && cmp.rev2.isEmpty()) {
            // The working tree: no revision, uncommitted lines show as such.
        } else {
            args << (newSide ? cmp.rev2 : cmp.rev1);
        }
    } else if (newSide && (cmp.allChanges || fc.group != ChangeGroup::Staged || fc.status == u'U')) {
        // The working tree.
    } else if (newSide || (fc.group == ChangeGroup::Unstaged && !cmp.allChanges && !cmp.unstagedAgainstHead)) {
        // The index: blame its content (from stdin) against the history.
        QByteArray out;
        if (!run({QStringLiteral("cat-file"), QStringLiteral("blob"), QStringLiteral(":0:") + path}, &out, nullptr))
            return {};
        input = out;
        args << QStringLiteral("--contents") << QStringLiteral("-");
    } else {
        if (!hasHead())
            return {};
        args << QStringLiteral("HEAD");
    }
    args << QStringLiteral("--") << path;
    QByteArray out;
    if (!run(args, &out, nullptr, input))
        return {};
    return parseBlame(out);
}
