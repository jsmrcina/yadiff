#pragma once

#include <QString>

#include <optional>

// Secrets in the operating system's keyring: the freedesktop Secret Service on Linux
// (KDE Wallet, GNOME Keyring) through `secret-tool`, the Credential Manager on Windows.
// For tests, YADIFF_SECRET_STORE_FILE redirects everything to a plain file instead.
namespace SecretStore {

bool available();
QString backendName();

std::optional<QString> read(const QString &key, QString *error = nullptr);
bool write(const QString &key, const QString &label, const QString &secret, QString *error = nullptr);
bool remove(const QString &key, QString *error = nullptr);

} // namespace SecretStore
