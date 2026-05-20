#pragma once

#include "launcher_types.hpp"

#include <QByteArray>
#include <QList>
#include <QString>

namespace hitsc {

class CredentialProtector {
public:
    QByteArray protect(const QByteArray& plaintext) const;
    QByteArray unprotect(const QByteArray& ciphertext) const;
};

class HostStore {
public:
    explicit HostStore(QString root_path = QStringLiteral("Software\\hitsc"));

    QList<SavedHost> load_hosts() const;
    void save_host(const SavedHost& host) const;

    const QString& root_path() const;

private:
    QString root_path_;
    CredentialProtector protector_;
};

} // namespace hitsc
