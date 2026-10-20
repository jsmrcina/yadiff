#pragma once

#include "PrOverview.h"

#include <QWidget>

class QLabel;
class QTextBrowser;
class QToolButton;

// The pull request's overview above the file list: its title and description, the
// reviewers and their votes, checks, work items and the comments on the PR itself.
// The header folds it away to one line that still says how the PR stands.
class PrOverviewPanel : public QWidget
{
    Q_OBJECT
public:
    explicit PrOverviewPanel(QWidget *parent = nullptr);

    void setLoading(const QString &hostName);
    void setError(const QString &error);
    // `canReply`: the comments' threads take replies and a status (Azure DevOps).
    void setOverview(const PrOverview::Overview &o, const QString &hostName, bool canReply);
    void clear();

    bool isFolded() const { return m_folded; }
    void setFolded(bool folded);
    QString summary() const; // the header's one line (tests)
    QTextBrowser *browser() const { return m_browser; }

signals:
    void reloadRequested();
    void foldedChanged(bool folded);
    void replyRequested(qint64 threadId);
    void resolveRequested(qint64 threadId, bool resolve);
    void commentRequested(); // a new comment on the pull request

private:
    void render();

    QToolButton *m_fold = nullptr;
    QLabel *m_title = nullptr;
    QToolButton *m_reload = nullptr;
    QTextBrowser *m_browser = nullptr;
    bool m_folded = false;
    std::optional<PrOverview::Overview> m_overview;
    QString m_host, m_message;
    bool m_canReply = false;
};
