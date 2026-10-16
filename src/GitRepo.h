#pragma once

#include "DiffModel.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

enum class ChangeGroup { Staged, Unstaged, Revisions };

struct FileChange {
    QString path;       // repository-relative, '/'-separated (new side)
    QString oldPath;    // equals path unless renamed/copied
    QChar status;       // git status letter: M A D R C T U, or '?' for untracked
    ChangeGroup group = ChangeGroup::Revisions;
};

// What is being compared.
struct Comparison {
    enum class Mode { WorkingTree, Revisions };
    Mode mode = Mode::WorkingTree;

    // Revisions mode: resolved object ids. rev2 empty means "working tree".
    QString rev1, rev2;
    QString rev1Name, rev2Name; // as typed by the user, for display

    // WorkingTree mode: diff unstaged files against HEAD (true) or the index.
    bool unstagedAgainstHead = true;
};

class GitRepo
{
public:
    // Finds the repository containing `dir`. Returns nullopt and sets *error on failure.
    static std::optional<GitRepo> open(const QString &dir, QString *error);

    QString root() const { return m_root; }

    // Runs git in the repository root. Returns false on non-zero exit.
    bool run(const QStringList &args, QByteArray *out, QString *error, const QByteArray &input = {}) const;

    // Resolves a user-supplied revision to a commit id.
    std::optional<QString> resolveCommit(const QString &rev, QString *error) const;
    std::optional<QString> mergeBase(const QString &a, const QString &b, QString *error) const;

    // HEAD's commit id, or the empty tree when the repository has no commits yet.
    QString headOrEmptyTree() const;
    bool hasHead() const;

    QVector<FileChange> listChanges(const Comparison &cmp, QString *error) const;
    FileDiff loadDiff(const FileChange &change, const Comparison &cmp, QString *error) const;

private:
    explicit GitRepo(QString root) : m_root(std::move(root)) {}
    QString m_root;
};

QString statusDescription(QChar status);
