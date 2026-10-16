#pragma once

#include "DiffModel.h"
#include "GitRepo.h"

#include <QByteArray>
#include <QString>
#include <QVector>

#include <functional>
#include <optional>

// A self-contained HTML report of a whole comparison: summary table plus every
// file's diff, colour-coded, with images embedded. Opens in any browser.
namespace Report {

struct File {
    FileChange change;
    FileDiff diff;
    bool reviewed = false;
    std::optional<QByteArray> oldImage, newImage; // only for image files
};

struct Input {
    QString title;       // e.g. "yadiff — myrepo"
    QString description; // e.g. "staged and unstaged changes"
    QString leftName, rightName;
    QVector<File> files;
    int context = 3;     // unchanged lines kept around each change
};

QString html(const Input &in);

// Loads every change (in the given order) for a report. `isReviewed` says which are
// ticked; `progress(done, total)` returning false cancels (then nullopt is returned).
std::optional<Input> gather(const GitRepo &repo, const Comparison &cmp, const QVector<FileChange> &changes,
                            const std::function<bool(const FileChange &)> &isReviewed,
                            const std::function<bool(int, int)> &progress = {});

// MIME type for embedding, from the file name; empty when not an image yadiff embeds.
QString imageMimeType(const QString &path);

} // namespace Report
