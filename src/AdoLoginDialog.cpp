#include "AdoLoginDialog.h"

#include "SecretStore.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

AdoLoginDialog::AdoLoginDialog(const QUrl &collectionUrl, const QString &reason, QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Sign in to Azure DevOps"));
    setMinimumWidth(560);

    auto *intro = new QLabel(reason.isEmpty() ? tr("yadiff needs access to Azure DevOps to read pull requests.") : reason);
    intro->setWordWrap(true);

    m_org = new QLineEdit(collectionUrl.isEmpty() ? QString() : AdoAuth::organizationUrl(collectionUrl).toString());
    m_org->setPlaceholderText(QStringLiteral("https://dev.azure.com/your-organisation"));
    m_org->setReadOnly(!collectionUrl.isEmpty());
    m_pat = new QLineEdit;
    m_pat->setEchoMode(QLineEdit::Password);
    m_pat->setPlaceholderText(tr("Paste a personal access token"));
    m_remember = new QCheckBox(tr("Remember it in the %1").arg(SecretStore::backendName()));
    m_remember->setChecked(SecretStore::available());
    m_remember->setEnabled(SecretStore::available());
    if (!SecretStore::available())
        m_remember->setToolTip(tr("No keyring available (install libsecret's secret-tool)"));

    m_help = new QLabel;
    m_help->setWordWrap(true);
    m_help->setOpenExternalLinks(true);
    m_help->setTextFormat(Qt::RichText);
    m_status = new QLabel;
    m_status->setWordWrap(true);

    auto *form = new QFormLayout;
    form->addRow(tr("Organization:"), m_org);
    form->addRow(tr("Token:"), m_pat);
    form->addRow(QString(), m_remember);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    QPushButton *signIn = buttons->addButton(tr("Sign in"), QDialogButtonBox::AcceptRole);
    signIn->setDefault(true);
    connect(signIn, &QPushButton::clicked, this, &AdoLoginDialog::signInWithToken);
    if (AdoAuth::azureCliAvailable()) {
        QPushButton *az = buttons->addButton(tr("Sign in with Azure CLI (browser)"), QDialogButtonBox::ActionRole);
        connect(az, &QPushButton::clicked, this, &AdoLoginDialog::signInWithAzureCli);
    }
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_org, &QLineEdit::textChanged, this, &AdoLoginDialog::updateHelp);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(form);
    layout->addWidget(m_help);
    layout->addWidget(m_status);
    layout->addWidget(buttons);
    updateHelp();
}

QUrl AdoLoginDialog::collectionUrl() const
{
    return QUrl(m_org->text().trimmed());
}

bool AdoLoginDialog::remembered() const
{
    return m_remember->isChecked() && m_cred && m_cred->kind == AdoAuth::Credential::Kind::Pat;
}

void AdoLoginDialog::updateHelp()
{
    const QUrl org = AdoAuth::organizationUrl(collectionUrl());
    const QString tokens = org.isValid() && !org.host().isEmpty()
                               ? org.toString(QUrl::StripTrailingSlash) + QStringLiteral("/_usersSettings/tokens")
                               : QString();
    QString text = tr("Create a token under <i>User settings &rarr; Personal access tokens</i>");
    if (!tokens.isEmpty())
        text = tr("Create a token at <a href=\"%1\">%1</a>").arg(tokens.toHtmlEscaped());
    text += tr(" with the <b>Code: Read &amp; write</b> scope. Read is enough to view pull requests; "
               "write is needed to comment and vote.");
    if (AdoAuth::azureCliAvailable())
        text += tr("<br>Or sign in with the Azure CLI, which uses your Microsoft account in the browser.");
    m_help->setText(text);
}

bool AdoLoginDialog::finish(const AdoAuth::Credential &cred)
{
    const QUrl org = collectionUrl();
    if (!org.isValid() || org.host().isEmpty()) {
        m_status->setText(tr("Enter the organization URL."));
        return false;
    }
    m_status->setText(tr("Checking…"));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const auto user = AdoAuth::validate(org, cred, &error);
    QApplication::restoreOverrideCursor();
    if (!user) {
        m_status->setText(tr("<span style=\"color:#d13b3b\">%1</span>").arg(error.toHtmlEscaped()));
        return false;
    }
    m_cred = cred;
    m_user = *user;
    if (cred.kind == AdoAuth::Credential::Kind::Pat && m_remember->isChecked()) {
        if (!AdoAuth::savePat(org, cred.secret, &error)) {
            m_status->setText(tr("Signed in, but the token could not be saved: %1").arg(error.toHtmlEscaped()));
            m_remember->setChecked(false);
        }
    }
    accept();
    return true;
}

void AdoLoginDialog::signInWithToken()
{
    const QString pat = m_pat->text().trimmed();
    if (pat.isEmpty()) {
        m_status->setText(tr("Paste a personal access token first."));
        return;
    }
    finish({AdoAuth::Credential::Kind::Pat, pat, tr("token")});
}

void AdoLoginDialog::signInWithAzureCli()
{
    m_status->setText(tr("Finish signing in in your browser…"));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const bool ok = AdoAuth::azureCliLogin(&error);
    auto cred = ok ? AdoAuth::azureCliCredential() : std::nullopt;
    QApplication::restoreOverrideCursor();
    if (!cred) {
        m_status->setText(error.isEmpty() ? tr("The Azure CLI did not return a token.") : error);
        return;
    }
    finish(*cred);
}
