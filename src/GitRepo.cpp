#include "GitRepo.h"
#include "LineDiff.h"

#include "Comments.h"
#include "FileFilter.h"
#include "PatchFile.h"
#include "StructuralDiff.h"

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

#include <algorithm>
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
    case DiffAlgorithm::Structural: return QStringLiteral("structural");
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
    case DiffAlgorithm::Structural: return QStringLiteral("Structural (tokens)");
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
    case DiffAlgorithm::Structural:
        return QStringLiteral("Compares tokens, not lines: re-wrapped, re-indented or reformatted code doesn't count, and "
                              "only the tokens that changed are highlighted.");
    }
    return {};
}

const QVector<DiffAlgorithm> &allAlgorithms()
{
    static const QVector<DiffAlgorithm> all{DiffAlgorithm::Patience, DiffAlgorithm::Histogram, DiffAlgorithm::Myers,
                                            DiffAlgorithm::Minimal,  DiffAlgorithm::MyersLcs,  DiffAlgorithm::Heckel,
                                            DiffAlgorithm::Structural};
    return all;
}

bool DiffOptions::inProcess() const
{
    return algorithm == DiffAlgorithm::MyersLcs || algorithm == DiffAlgorithm::Heckel
        || algorithm == DiffAlgorithm::Structural || ignoreCase || ignoreTrim
        || ignoreComments || !unimportantPatterns.isEmpty();
}

QStringList DiffOptions::normalizedLines(const QStringList &lines, const QString &fileName) const
{
    QStringList out = ignoreComments ? Comments::strip(lines, Comments::forFile(fileName)) : lines;
    QVector<QRegularExpression> patterns;
    for (const QString &p : unimportantPatterns)
        if (QRegularExpression re(p); !p.isEmpty() && re.isValid())
            patterns.push_back(re);
    for (QString &l : out) {
        for (const QRegularExpression &re : std::as_const(patterns))
            l.remove(re);
        l = normalized(l);
    }
    return out;
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
    const DiffAlgorithm gitAlgorithm = algorithm == DiffAlgorithm::MyersLcs || algorithm == DiffAlgorithm::Heckel
                                               || algorithm == DiffAlgorithm::Structural
                                           ? DiffAlgorithm::Myers
                                           : algorithm;
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
    if (cmp.includeIdentical && cmp.mode != Comparison::Mode::Patch)
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
    if (cmp.mode == Comparison::Mode::Patch)
        return listPatchChanges(cmp, error);

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
    if (cmp.mode == Comparison::Mode::Patch)
        return loadPatchDiff(fc, cmp, error);
    if (fc.status != u'?' && (cmp.options.inProcess() || !fc.anchors.isEmpty()))
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
    if (cmp.mode == Comparison::Mode::Patch) {
        oldSide = patchContent(fc, cmp, false).value_or("-");
        newSide = QCryptographicHash::hash(patchContent(fc, cmp, true).value_or("-"), QCryptographicHash::Sha1).toHex();
        oldSide = QCryptographicHash::hash(oldSide, QCryptographicHash::Sha1).toHex();
    } else if (cmp.mode == Comparison::Mode::Paths) {
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
    if (cmp.options.inProcess() || !fc.anchors.isEmpty())
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

namespace {
std::optional<PatchFile::File> patchFileFor(const FileChange &fc, const Comparison &cmp)
{
    QFile f(cmp.patchPath);
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    for (const PatchFile::File &pf : PatchFile::parse(QString::fromUtf8(f.readAll())))
        if (pf.path() == fc.path)
            return pf;
    return std::nullopt;
}
} // namespace

QVector<FileChange> GitRepo::listPatchChanges(const Comparison &cmp, QString *error) const
{
    QFile f(cmp.patchPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("%1: %2").arg(cmp.patchPath, f.errorString());
        return {};
    }
    QVector<FileChange> out;
    for (const PatchFile::File &pf : PatchFile::parse(QString::fromUtf8(f.readAll()))) {
        FileChange fc;
        fc.path = pf.path();
        fc.oldPath = pf.oldPath.isEmpty() ? fc.path : pf.oldPath;
        fc.status = pf.status;
        fc.added = pf.binary ? -1 : pf.added;
        fc.removed = pf.binary ? -1 : pf.removed;
        out << fc;
    }
    if (out.isEmpty() && error)
        *error = QStringLiteral("%1 has no file changes in it").arg(cmp.patchPath);
    return out;
}

std::optional<QByteArray> GitRepo::patchContent(const FileChange &fc, const Comparison &cmp, bool newSide) const
{
    // The file the patch was made from, under patchBase, and the patch applied to it.
    const std::optional<PatchFile::File> pf = patchFileFor(fc, cmp);
    if (!pf || pf->binary || (newSide && pf->status == u'D') || (!newSide && pf->status == u'A'))
        return std::nullopt;
    QByteArray base;
    if (pf->status != u'A') {
        QFile f(QDir(cmp.patchBase).filePath(pf->oldPath));
        if (!f.open(QIODevice::ReadOnly))
            return std::nullopt;
        base = f.readAll();
    }
    const QStringList baseLines = LineDiff::splitLines(QString::fromUtf8(base));
    const auto applied = PatchFile::apply(baseLines, *pf);
    if (!applied)
        return std::nullopt;
    if (!newSide)
        return base;
    QString out = applied->join(u'\n');
    if (!applied->isEmpty())
        out += u'\n';
    return out.toUtf8();
}

FileDiff GitRepo::loadPatchDiff(const FileChange &fc, const Comparison &cmp, QString *error) const
{
    const std::optional<PatchFile::File> pf = patchFileFor(fc, cmp);
    FileDiff d;
    if (!pf) {
        if (error)
            *error = QStringLiteral("%1 is no longer in the patch").arg(fc.path);
        return d;
    }
    if (pf->binary) {
        d.binary = true;
        d.message = QStringLiteral("Binary change (the patch can't be shown)");
        return d;
    }
    // When the patch fits the files it was made from, show whole files; otherwise its hunks.
    const bool fits = (pf->status == u'A' || patchContent(fc, cmp, false)) && (pf->status == u'D' || patchContent(fc, cmp, true));
    if (fits) {
        d = loadDiffInProcess(fc, cmp, error);
        if (!d.lines.isEmpty() || !d.message.isEmpty())
            return d;
    }
    d = DiffModel::parseUnified(pf->text, cmp.options.detectMoves);
    if (d.lines.isEmpty())
        d.message = QStringLiteral("No content changes in the patch (mode or name only)");
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
    LineDiff::Alignment anchors;
    for (const auto &[o1, n1] : fc.anchors)
        anchors.push_back({o1 - 1, n1 - 1});
    if (o.algorithm == DiffAlgorithm::Structural) {
        StructuralDiff::Options so;
        so.syntax = Comments::forFile(fc.path);
        if (!so.syntax.isValid())
            so.syntax.quotes = QStringLiteral("\"'"); // still keep strings together
        so.ignoreCase = o.ignoreCase;
        so.ignoreComments = o.ignoreComments;
        d = StructuralDiff::build(oldLines, newLines, so, anchors);
        if (d.chunks.isEmpty() && fc.anchors.isEmpty()) {
            d = FileDiff();
            d.message = QStringLiteral("No differences in the code's tokens (only whitespace or line breaks changed)");
        }
        return d;
    }
    const QStringList oldNorm = o.normalizedLines(oldLines, fc.oldPath);
    const QStringList newNorm = o.normalizedLines(newLines, fc.path);

    QString alignError;
    auto align = [&](const QStringList &oldPart, const QStringList &newPart) -> LineDiff::Alignment {
        if (oldPart.isEmpty() || newPart.isEmpty())
            return {};
        if (o.algorithm == DiffAlgorithm::MyersLcs || o.algorithm == DiffAlgorithm::Heckel) {
            const auto [a, b] = LineDiff::lineIds(oldPart, newPart);
            return o.algorithm == DiffAlgorithm::Heckel ? LineDiff::heckel(a, b) : LineDiff::myers(a, b);
        }
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
        if (!tmp.isValid() || !save(QStringLiteral("old"), oldPart) || !save(QStringLiteral("new"), newPart)) {
            alignError = QStringLiteral("Could not write temporary files");
            return {};
        }
        QByteArray out;
        if (!runDiff({QStringLiteral("diff"), QStringLiteral("--no-index"), QStringLiteral("--no-color"),
                      QStringLiteral("--no-ext-diff"), QStringLiteral("-U0"),
                      QStringLiteral("--diff-algorithm=") + algorithmName(o.algorithm), QStringLiteral("--"),
                      tmp.filePath(QStringLiteral("old")), tmp.filePath(QStringLiteral("new"))},
                     &out, &alignError))
            return {};
        return LineDiff::fromUnifiedZero(QString::fromUtf8(out), int(oldPart.size()), int(newPart.size()));
    };

    const LineDiff::Alignment alignment = LineDiff::alignAround(oldNorm, newNorm, anchors, align, &anchors);
    if (!alignError.isEmpty()) {
        if (error)
            *error = alignError;
        return d;
    }

    d = LineDiff::build(oldLines, newLines, alignment, false, anchors);
    // Added and removed lines that are blank once ignored text is taken out don't count;
    // blank lines themselves only with "ignore blank lines".
    const bool removesText = o.ignoreComments || !o.unimportantPatterns.isEmpty() || o.ignoreBlankLines;
    if (removesText) {
        for (DiffLine &l : d.lines) {
            if (l.kind == LineKind::Equal)
                continue;
            const QString &orig = l.kind == LineKind::Removed ? oldLines[l.oldNo - 1] : newLines[l.newNo - 1];
            const QString &norm = l.kind == LineKind::Removed ? oldNorm[l.oldNo - 1] : newNorm[l.newNo - 1];
            if (norm.trimmed().isEmpty() && (o.ignoreBlankLines || !orig.trimmed().isEmpty()))
                l.unimportant = true;
        }
    }
    for (DiffLine &l : d.lines) { // finalize again, keeping only the manual pairs
        l.pair = l.anchored && l.kind != LineKind::Equal ? l.pair : -1;
        l.highlights.clear();
    }
    DiffModel::finalize(d, o.detectMoves);
    if (d.chunks.isEmpty() && fc.anchors.isEmpty()) {
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

namespace {
const QString kLogFormat = QStringLiteral("--format=%H%x1f%h%x1f%an%x1f%ad%x1f%D%x1f%s%x1f%P%x1e");
const QString kLogDate = QStringLiteral("--date=format:%Y-%m-%d %H:%M");

QVector<CommitInfo> parseLog(const QByteArray &out)
{
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
} // namespace

QVector<CommitInfo> GitRepo::log(int max, int skip, bool allRefs, QString *error) const
{
    QStringList args{QStringLiteral("log"), kLogDate, kLogFormat, QStringLiteral("-n"), QString::number(max),
                     QStringLiteral("--skip=") + QString::number(skip)};
    if (allRefs)
        args << QStringLiteral("--all");
    QByteArray out;
    if (!run(args, &out, error))
        return {};
    return parseLog(out);
}

QVector<CommitInfo> GitRepo::commitsInRange(const QString &from, const QString &to, int max, const QStringList &paths) const
{
    QByteArray out;
    QStringList args{QStringLiteral("log"), kLogDate, kLogFormat, QStringLiteral("--topo-order"), QStringLiteral("-n"),
                     QString::number(max), from + QStringLiteral("..") + to};
    if (!paths.isEmpty()) {
        args << QStringLiteral("--");
        for (const QString &p : paths)
            args << QStringLiteral(":(top,literal)") + p;
    }
    if (!run(args, &out, nullptr))
        return {};
    QVector<CommitInfo> list = parseLog(out);
    std::reverse(list.begin(), list.end());
    return list;
}

QVector<CommitInfo> GitRepo::commitNeighbours(const QString &oid, int max, int *index) const
{
    // Older: the first-parent ancestors. Newer: commits on the way from it to HEAD.
    QByteArray out;
    run({QStringLiteral("log"), kLogDate, kLogFormat, QStringLiteral("--first-parent"), QStringLiteral("-n"),
         QString::number(max), oid},
        &out, nullptr);
    QVector<CommitInfo> older = parseLog(out);
    std::reverse(older.begin(), older.end());
    QVector<CommitInfo> newer;
    if (hasHead()
        && run({QStringLiteral("merge-base"), QStringLiteral("--is-ancestor"), oid, QStringLiteral("HEAD")}, nullptr, nullptr)) {
        out.clear();
        run({QStringLiteral("log"), kLogDate, kLogFormat, QStringLiteral("--first-parent"), QStringLiteral("--ancestry-path"),
             QStringLiteral("-n"), QString::number(max), oid + QStringLiteral("..HEAD")},
            &out, nullptr);
        newer = parseLog(out);
        std::reverse(newer.begin(), newer.end());
    }
    if (index)
        *index = int(older.size()) - 1;
    return older + newer;
}

QString GitRepo::commitMessage(const QString &oid) const
{
    QByteArray out;
    run({QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--format=%B"), oid}, &out, nullptr);
    return QString::fromUtf8(out).trimmed();
}

QVector<CommitInfo> GitRepo::refTips(QString *error) const
{
    QByteArray names;
    if (!run({QStringLiteral("for-each-ref"), QStringLiteral("--sort=-committerdate"), QStringLiteral("--format=%(refname)"),
              QStringLiteral("refs/heads"), QStringLiteral("refs/remotes"), QStringLiteral("refs/tags")},
             &names, error))
        return {};
    QStringList refs;
    for (const QByteArray &n : names.split('\n'))
        if (!n.isEmpty() && !n.endsWith("/HEAD"))
            refs << QString::fromUtf8(n) + QStringLiteral("^{commit}");
    if (refs.isEmpty())
        return {};
    QByteArray out;
    if (!run(QStringList{QStringLiteral("log"), kLogDate, kLogFormat, QStringLiteral("--no-walk=unsorted")} + refs, &out,
             error))
        return {};
    return parseLog(out);
}

QVector<CommitInfo> GitRepo::stashes(QString *error) const
{
    QByteArray out;
    if (!run({QStringLiteral("stash"), QStringLiteral("list"), kLogDate,
              QStringLiteral("--format=%H%x1f%h%x1f%an%x1f%ad%x1f%gd%x1f%s%x1f%P%x1e")},
             &out, error))
        return {};
    return parseLog(out);
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
    if (cmp.mode == Comparison::Mode::Patch)
        return patchContent(fc, cmp, newSide);
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

std::optional<QByteArray> GitRepo::patch(const Comparison &cmp, QString *error) const
{
    const QStringList base{QStringLiteral("diff"), QStringLiteral("--no-color"), QStringLiteral("--no-ext-diff"),
                           QStringLiteral("-M"), QStringLiteral("--binary")};
    QByteArray out;
    if (cmp.mode == Comparison::Mode::Patch) {
        QFile f(cmp.patchPath);
        if (!f.open(QIODevice::ReadOnly)) {
            if (error)
                *error = f.errorString();
            return std::nullopt;
        }
        return f.readAll();
    }
    if (cmp.mode == Comparison::Mode::Paths) {
        if (!runDiff(base + QStringList{QStringLiteral("--no-index"), QStringLiteral("--"), cmp.leftPath, cmp.rightPath}, &out,
                     error))
            return std::nullopt;
        // git names the sides by their full paths: make them relative to the compared folders.
        auto side = [&](const QString &p) {
            QString s = QDir::fromNativeSeparators(p);
            while (s.startsWith(u'/'))
                s.remove(0, 1);
            return s.toUtf8();
        };
        const QByteArray l = side(cmp.leftPath), r = side(cmp.rightPath);
        QList<QByteArray> lines = out.split('\n');
        for (QByteArray &line : lines) {
            if (!line.startsWith("diff --git ") && !line.startsWith("--- ") && !line.startsWith("+++ "))
                continue;
            if (cmp.pathsAreFiles) {
                line.replace("a/" + l, "a/" + QFileInfo(cmp.leftPath).fileName().toUtf8());
                line.replace("b/" + r, "b/" + QFileInfo(cmp.rightPath).fileName().toUtf8());
            } else {
                line.replace("a/" + l + '/', "a/");
                line.replace("b/" + r + '/', "b/");
            }
        }
        return lines.join('\n');
    }
    QStringList args = base;
    if (cmp.reversed)
        args << QStringLiteral("-R");
    if (cmp.mode == Comparison::Mode::Revisions) {
        args << cmp.rev1;
        if (!cmp.rev2.isEmpty())
            args << cmp.rev2;
    } else {
        args << headOrEmptyTree(); // staged and unstaged together: the working tree against HEAD
    }
    if (!run(args + pathspecs(cmp), &out, error))
        return std::nullopt;
    if (cmp.mode == Comparison::Mode::WorkingTree && !cmp.reversed) {
        // Untracked files too, as new files.
        Comparison c = cmp;
        c.includeIdentical = false;
        for (const FileChange &fc : listChanges(c, nullptr)) {
            if (fc.status != u'?')
                continue;
            QByteArray one;
            if (runDiff(base + QStringList{QStringLiteral("--no-index"), QStringLiteral("--"), QStringLiteral("/dev/null"), fc.path},
                        &one, nullptr))
                out += one;
        }
    }
    return out;
}

QVector<GitRepo::LineOp> GitRepo::lineOps(const FileChange &fc, const Comparison &cmp) const
{
    if (m_noRepo || m_bare || cmp.mode != Comparison::Mode::WorkingTree || cmp.reversed || fc.status == u'='
        || fc.status == u'U' || fc.status == u'T')
        return {};
    if (fc.group == ChangeGroup::Staged && !cmp.allChanges)
        return {LineOp::Unstage};
    if (fc.status == u'D')
        return {LineOp::Stage}; // nothing on disk to discard into
    return {LineOp::Stage, LineOp::Discard};
}

bool GitRepo::applyLines(LineOp op, const FileChange &fc, const Comparison &cmp, const QSet<int> &oldLines,
                         const QSet<int> &newLines, QString *error) const
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!lineOps(fc, cmp).contains(op))
        return fail(QStringLiteral("Lines of this file can't be changed that way here"));
    auto blob = [&](const QString &spec) -> std::optional<QByteArray> {
        QByteArray out;
        if (!run({QStringLiteral("cat-file"), QStringLiteral("-e"), spec}, nullptr, nullptr)
            || !run({QStringLiteral("cat-file"), QStringLiteral("blob"), spec}, &out, nullptr))
            return std::nullopt;
        return out;
    };
    const std::optional<QByteArray> index = blob(QStringLiteral(":0:") + fc.path);
    const std::optional<QByteArray> head = hasHead() ? blob(QStringLiteral("HEAD:") + fc.oldPath) : std::nullopt;
    const QString diskFile = m_root + u'/' + fc.path;
    std::optional<QByteArray> work;
    if (QFile f(diskFile); f.open(QIODevice::ReadOnly))
        work = f.readAll();

    using LineDiff::splitKeepingEnds;
    const QList<QByteArray> I = splitKeepingEnds(index.value_or(QByteArray()));
    const QList<QByteArray> H = splitKeepingEnds(head.value_or(QByteArray()));
    const QList<QByteArray> W = splitKeepingEnds(work.value_or(QByteArray()));
    // The shown diff's old side: the index, or HEAD (the default, and "all changes").
    const bool oldIsIndex = op != LineOp::Unstage && !cmp.allChanges && !cmp.unstagedAgainstHead && fc.status != u'?';
    const QList<QByteArray> &B = op == LineOp::Unstage ? H : oldIsIndex ? I : H;

    auto writeIndex = [&](const QList<QByteArray> &lines) {
        const QByteArray content = lines.join();
        QByteArray sha;
        QString err;
        if (!run({QStringLiteral("hash-object"), QStringLiteral("-w"), QStringLiteral("--stdin"),
                  QStringLiteral("--path=") + fc.path},
                 &sha, &err, content))
            return fail(err);
        QString mode = QStringLiteral("100644");
        QByteArray ls;
        if (run({QStringLiteral("ls-files"), QStringLiteral("-s"), QStringLiteral("--"), QStringLiteral(":(top,literal)") + fc.path},
                &ls, nullptr)
            && !ls.trimmed().isEmpty())
            mode = QString::fromUtf8(ls.left(ls.indexOf(' ')));
        else if (QFileInfo(diskFile).isExecutable())
            mode = QStringLiteral("100755");
        if (!run({QStringLiteral("update-index"), QStringLiteral("--add"), QStringLiteral("--cacheinfo"),
                  mode + u',' + QString::fromUtf8(sha.trimmed()) + u',' + fc.path},
                 nullptr, &err))
            return fail(err);
        return true;
    };

    switch (op) {
    case LineOp::Stage: {
        // Removed lines are numbered on the shown old side; find them in the index.
        QSet<int> drop;
        if (&B == &I) {
            drop = oldLines;
        } else {
            QStringList h, i;
            for (const QByteArray &l : H)
                h << QString::fromLatin1(l);
            for (const QByteArray &l : I)
                i << QString::fromLatin1(l);
            const auto [ih, ii] = LineDiff::lineIds(h, i);
            for (const LineDiff::Match &m : LineDiff::myers(ih, ii))
                if (oldLines.contains(m.first + 1))
                    drop.insert(m.second + 1); // lines already gone from the index stay gone
        }
        return writeIndex(LineDiff::applyPart(I, W, drop, newLines));
    }
    case LineOp::Unstage: {
        const QList<QByteArray> result = LineDiff::applyPart(I, H, newLines, oldLines);
        if (!head && result.isEmpty()) // nothing left of a new file: take it out of the index
            return run({QStringLiteral("update-index"), QStringLiteral("--force-remove"), QStringLiteral("--"), fc.path},
                       nullptr, error); // the working-tree file stays
        return writeIndex(result);
    }
    case LineOp::Discard: {
        if (!work)
            return fail(QStringLiteral("Could not read %1").arg(fc.path));
        const QByteArray content = LineDiff::applyPart(W, B, newLines, oldLines).join();
        QFile f(diskFile);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(content) != content.size())
            return fail(f.errorString());
        return true;
    }
    }
    return false;
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
    if (m_noRepo || displayedCmp.mode == Comparison::Mode::Paths || displayedCmp.mode == Comparison::Mode::Patch)
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
