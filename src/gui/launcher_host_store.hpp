#pragma once

#include "launcher_types.hpp"

#include <QByteArray>
#include <QList>
#include <QRect>
#include <QString>

#include <optional>
#include <string>

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
    void delete_host(const QString& id) const;

    void save_last_connected(const QString& host_id) const;
    QString load_last_connected() const;

    // Per-host viewer-window geometry, stored alongside the host record.
    void save_window_rect(const QString& host_id, const QRect& rect) const;
    std::optional<QRect> load_window_rect(const QString& host_id) const;

    // Per-host pinned certificate (SHA-256, canonical upper-hex, no separators),
    // stored alongside the host record. Created/replaced only by explicit user
    // consent; removed automatically when the host is deleted.
    void save_pinned_cert(const QString& host_id, const std::string& sha256_hex) const;
    std::optional<std::string> load_pinned_cert(const QString& host_id) const;

    const QString& root_path() const;

private:
    QString root_path_;
    CredentialProtector protector_;
};

} // namespace hitsc
