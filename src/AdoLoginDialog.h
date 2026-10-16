#pragma once

#include "AdoAuth.h"

#include <QDialog>

#include <optional>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

// Signs in to an Azure DevOps organisation: a personal access token (checked against
// the server, optionally remembered in the system keyring), or the Azure CLI.
class AdoLoginDialog : public QDialog
{
    Q_OBJECT
public:
    // `reason` explains why sign-in is needed (shown at the top); may be empty.
    AdoLoginDialog(const QUrl &collectionUrl, const QString &reason, QWidget *parent = nullptr);

    std::optional<AdoAuth::Credential> credential() const { return m_cred; }
    QUrl collectionUrl() const;
    QString signedInAs() const { return m_user; }
    bool remembered() const;

private:
    void signInWithToken();
    void signInWithAzureCli();
    void updateHelp();
    bool finish(const AdoAuth::Credential &cred);

    QLineEdit *m_org = nullptr;
    QLineEdit *m_pat = nullptr;
    QCheckBox *m_remember = nullptr;
    QLabel *m_help = nullptr;
    QLabel *m_status = nullptr;
    std::optional<AdoAuth::Credential> m_cred;
    QString m_user;
};
