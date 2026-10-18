#pragma once

#include "DiffModel.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

enum class ChangeGroup { Staged, Unstaged, Revisions, Unchanged };

struct FileChange {
    QString path;       // repository-relative, '/'-separated (new side)
    QString oldPath;    // equals path unless renamed/copied
    QChar status;       // git status letter: M A D R C T U, '?' untracked, '=' identical
    ChangeGroup group = ChangeGroup::Revisions;
    int added = -1;     // line counts from git --numstat; -1 when unknown (binary)
    int removed = -1;
    bool blankOnly = false; // the only differences are blank lines
};

// One line of `git blame`.
struct BlameLine {
    QString hash;     // empty for lines not committed yet
    QString author;
    qint64 time = 0;  // seconds since the epoch
    QString summary;
};

// Which side(s) of a comparison a file exists on, for the file filters.
enum class FileCategory { LeftOnly, RightOnly, Different, Identical };
FileCategory categoryOf(const FileChange &fc, bool reversed = false);

struct CommitInfo {
    QString oid, shortOid, author, date, refs, subject;
    QStringList parents;
};

// Histogram to Minimal are git's; MyersLcs and Heckel are yadiff's own (LineDiff).
enum class DiffAlgorithm { Histogram, Patience, Myers, Minimal, MyersLcs, Heckel };
const QVector<DiffAlgorithm> &allAlgorithms(); // in menu order

struct DiffOptions {
    DiffAlgorithm algorithm = DiffAlgorithm::Patience;
    bool ignoreSpaceChange = false;   // git -b
    bool ignoreAllSpace = false;      // git -w
    bool ignoreBlankLines = false;
    bool ignoreCrAtEol = false;
    bool ignoreTrim = false;          // leading and trailing whitespace (yadiff)
    bool ignoreCase = false;          // (yadiff)
    bool detectMoves = true;

    QStringList gitArgs() const;
    QStringList whitespaceArgs() const; // the subset that also affects which files are listed
    bool ignoresWhitespace() const
    {
        return ignoreSpaceChange || ignoreAllSpace || ignoreBlankLines || ignoreCrAtEol || ignoreTrim;
    }
    bool ignoresSomething() const { return ignoresWhitespace() || ignoreCase; }
    // True when yadiff compares the lines itself instead of using git's diff output.
    bool inProcess() const;
    // A line as it is compared under these options.
    QString normalized(QString line) const;
};

QString algorithmName(DiffAlgorithm a);         // git's name: "histogram", ...
QString algorithmLabel(DiffAlgorithm a);        // UI label
QString algorithmDescription(DiffAlgorithm a);  // one-line explanation

// What is being compared.
struct Comparison {
    // Paths: two directories or two files compared directly (git diff --no-index),
    // inside or outside any repository.
    enum class Mode { WorkingTree, Revisions, Paths };
    Mode mode = Mode::WorkingTree;

    // Revisions mode: resolved object ids. rev2 empty means "working tree".
    QString rev1, rev2;
    QString rev1Name, rev2Name; // for display

    // WorkingTree mode: diff unstaged files against HEAD (true) or the index.
    bool unstagedAgainstHead = true;
    // WorkingTree mode: one list of everything, working tree against HEAD,
    // instead of separate staged and unstaged sections.
    bool allChanges = false;

    // Repository-relative paths (files or directories) to limit the comparison to.
    QStringList paths;

    // Paths mode: absolute paths of the two sides; both files or both directories.
    QString leftPath, rightPath;
    bool pathsAreFiles = false;

    // Pull request being reviewed (prNumber 0 when none). prHost is "github" or "ado".
    // GitHub: prOwner/prRepo. Azure DevOps: prCollectionUrl/prProject/prRepo.
    QString prHost;
    QString prOwner, prRepo;
    QString prCollectionUrl, prProject, prWebUrl;
    int prNumber = 0;

    // Swapped sides for comparisons that involve the working tree (git diff -R): the
    // working tree is on the left. (Other comparisons swap rev1/rev2 or the paths.)
    bool reversed = false;
    // Nothing to compare yet (yadiff started outside a repository): no files.
    bool empty = false;
    // Also list files that are the same on both sides (status '=').
    bool includeIdentical = false;
    // Files whose path matches one of these globs are left out (when useIgnoreGlobs).
    QStringList ignoreGlobs;
    bool useIgnoreGlobs = false;

    // Shown in the window title instead of the default description (e.g. a PR title).
    QString title;

    DiffOptions options;
};

class GitRepo
{
public:
    // Finds the repository containing `dir` (a working tree or a bare repository).
    static std::optional<GitRepo> open(const QString &dir, QString *error);
    // For Comparison::Mode::Paths outside a repository: git runs in `workDir`.
    static GitRepo forPaths(const QString &workDir) { return GitRepo(workDir, false, true); }

    QString root() const { return m_root; }
    bool isBare() const { return m_bare; }
    bool isRepository() const { return !m_noRepo; }

    // Runs git in the repository root. Returns false on non-zero exit.
    bool run(const QStringList &args, QByteArray *out, QString *error, const QByteArray &input = {}) const;
    // Like run(), but exit code 1 also counts as success (git diff --no-index reports differences that way).
    bool runDiff(const QStringList &args, QByteArray *out, QString *error) const;

    // Resolves a user-supplied revision to a commit id.
    std::optional<QString> resolveCommit(const QString &rev, QString *error) const;
    // The empty tree's id (accepted by resolveCommit as the parent of a root commit).
    QString emptyTree() const;
    std::optional<QString> mergeBase(const QString &a, const QString &b, QString *error) const;
    bool hasCommit(const QString &oid) const;

    // True when `path` (repository-relative) is tracked, or exists in HEAD.
    bool knowsPath(const QString &path) const;

    // HEAD's commit id, or the empty tree when the repository has no commits yet.
    QString headOrEmptyTree() const;
    bool hasHead() const;

    QVector<FileChange> listChanges(const Comparison &cmp, QString *error) const;
    FileDiff loadDiff(const FileChange &change, const Comparison &cmp, QString *error) const;

    // Identity of the two file versions being compared (blob ids, or a content hash for
    // working-tree files). Stays the same as long as neither side changes, whatever the
    // diff options are; used to remember which files were reviewed.
    QByteArray fingerprint(const FileChange &change, const Comparison &cmp) const;

    // Absolute path of one side of a change when that side is a file on disk that can be
    // edited (the working tree, or either side of a Paths comparison); empty otherwise.
    QString diskPath(const FileChange &change, const Comparison &cmp, bool newSide) const;

    // Raw bytes of one side of a change (nullopt when that side doesn't exist or is
    // larger than `maxBytes`). Used for image diffs and reports.
    std::optional<QByteArray> fileContent(const FileChange &change, const Comparison &cmp, bool newSide,
                                          qint64 maxBytes = 64 * 1024 * 1024) const;

    // Working-tree operations (on the actual files; `changes` as listed for `cmp`).
    bool stage(const QVector<FileChange> &changes, QString *error) const;
    bool unstage(const QVector<FileChange> &changes, QString *error) const;
    // Makes the right side of each change match its left side: restores files from the
    // left revision (or the index), deletes files the left side doesn't have (to the
    // trash when possible), and in directory comparisons copies the left file over.
    bool revert(const QVector<FileChange> &changes, const Comparison &cmp, QString *error) const;
    // `git blame` for one side of a change, one entry per line (empty when that side has
    // no history: an untracked file, a directory comparison, ...).
    QVector<BlameLine> blame(const FileChange &change, const Comparison &cmp, bool newSide) const;

    // The last commit that touched `path` (repository-relative), empty when unknown.
    QString lastCommit(const QString &path) const;

    // Recent commits, newest first. `allRefs` includes every branch and tag.
    QVector<CommitInfo> log(int max, int skip, bool allRefs, QString *error) const;

private:
    GitRepo(QString root, bool bare, bool noRepo = false) : m_root(std::move(root)), m_bare(bare), m_noRepo(noRepo) {}
    QStringList pathspecs(const Comparison &cmp) const;
    QVector<FileChange> listChangesCore(const Comparison &cmp, QString *error) const;
    QVector<FileChange> listPathChanges(const Comparison &cmp, QString *error) const;
    void appendIdentical(QVector<FileChange> &changes, const Comparison &cmp) const;
    QString diskPathPhysical(const FileChange &change, const Comparison &cmp, bool newSide) const;
    std::optional<QByteArray> fileContentPhysical(const FileChange &change, const Comparison &cmp, bool newSide,
                                                  qint64 maxBytes) const;
    FileDiff loadPathDiff(const FileChange &change, const Comparison &cmp, QString *error) const;
    FileDiff loadDiffInProcess(const FileChange &change, const Comparison &cmp, QString *error) const;
    QString m_root;
    bool m_bare = false;
    bool m_noRepo = false;
};

QString statusDescription(QChar status);
