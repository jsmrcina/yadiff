#pragma once

#include "PrInbox.h"

#include <QDialog>

#include <atomic>
#include <memory>

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

// File > Open pull request: the pull requests waiting for the user's review, loaded in
// the background. Double-click one (or Open) to review it; a link can be pasted too.
class PrListDialog : public QDialog
{
    Q_OBJECT
public:
    // `fetch`: load the list at once (tests pass false and call showListing).
    explicit PrListDialog(QWidget *parent = nullptr, bool fetch = true);
    ~PrListDialog() override;

    // What to open: "ado" or "github", and the PR's link.
    QString host() const { return m_host; }
    QString link() const { return m_link; }
    // Tests: shows a listing as if it had been fetched.
    void showListing(const PrInbox::Listing &listing);
    bool isLoading() const { return m_loading; }
    QTreeWidget *tree() const { return m_tree; }

private:
    void load();
    void addProject();
    void applyFilter();
    void openSelected();
    void openLink();

    QTreeWidget *m_tree = nullptr;
    QLabel *m_status = nullptr;
    QLineEdit *m_filter = nullptr;
    QLineEdit *m_paste = nullptr;
    QPushButton *m_open = nullptr;
    QString m_host, m_link;
    bool m_loading = false;
    quint64 m_gen = 0;
    std::shared_ptr<std::atomic_bool> m_cancel;
};
